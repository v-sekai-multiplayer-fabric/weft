/*
 * Guest ELF loader. See sandbox_guest.h for the isolation contract,
 * zone_abi.h for the guest ABI, zf_guest_kv.h for the storage rules.
 *
 * This host does NOT emulate Linux. libriscv's minimal syscall set
 * gives a guest its heap (brk), its console (write), and exit; every
 * other host operation is an explicit ecall from zone_abi.h. An
 * earlier revision of this file went the other way, installing
 * openat/read/lseek/fstat/getcwd/chdir/mkdirat/unlinkat plus a
 * synthetic /dev -- 17 handlers -- chasing a whole game engine's
 * expectations. That list has no end short of a Linux kernel, so the
 * design changed instead of the list growing: engine-class guests now
 * run under bubblewrap as ordinary processes (see rfd/0095), and
 * in-process guests are scripts that speak this ABI.
 */

#include "sandbox_guest.h"
#include "zf_guest_kv.h"
#include "zone_abi.h"

extern "C" {
#include "gen/rebac.h"
}

#include <libriscv/machine.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <pthread.h>
#include <string>
#include <vector>

static constexpr int W = 8; /* riscv64 */
using SandboxMachine = riscv::Machine<W>;

/* Per-machine context, reached via machine.get_userdata(). Never a
 * global: libriscv's syscall handler table is static per machine
 * width, and rfd/0094's composition puts several guests in one zone. */
struct sandbox_ctx {
    zf_guest_kv_t *kv;
    uint32_t       z_id;
};

static sandbox_ctx *ctx_of(SandboxMachine &m)
{
    return m.template get_userdata<sandbox_ctx>();
}

/* Guest string argument -> std::string, bounds-checked by libriscv. */
static std::string guest_str(SandboxMachine &m, uint64_t addr, uint64_t len)
{
    if (len == 0) return std::string();
    std::vector<uint8_t> tmp(len);
    m.copy_from_guest(tmp.data(), addr, len);
    return std::string(reinterpret_cast<const char *>(tmp.data()), len);
}

/* --- the ABI ---------------------------------------------------------- */

static void ecall_kv_get(SandboxMachine &m)
{
    auto *ctx = ctx_of(m);
    const auto g_key = m.sysarg(0);
    const uint64_t key_len = m.sysarg(1);
    const auto g_buf = m.sysarg(2);
    const uint64_t buf_len = m.sysarg(3);

    std::string key = guest_str(m, g_key, key_len);
    std::vector<uint8_t> tmp(buf_len ? buf_len : 1);
    int64_t n = zf_guest_kv_get(ctx->kv, key.c_str(), tmp.data(), buf_len);
    if (n > 0 && buf_len > 0) {
        m.copy_to_guest(g_buf, tmp.data(), (size_t)(n < (int64_t)buf_len ? n : buf_len));
    }
    m.set_result(n); /* full length, even when the guest's buffer was short */
}

static void ecall_kv_set(SandboxMachine &m)
{
    auto *ctx = ctx_of(m);
    const auto g_key = m.sysarg(0);
    const uint64_t key_len = m.sysarg(1);
    const auto g_val = m.sysarg(2);
    const uint64_t val_len = m.sysarg(3);

    std::string key = guest_str(m, g_key, key_len);
    std::vector<uint8_t> val(val_len ? val_len : 1);
    if (val_len) m.copy_from_guest(val.data(), g_val, val_len);
    m.set_result(zf_guest_kv_set(ctx->kv, key.c_str(), val.data(), val_len));
}

static void ecall_kv_del(SandboxMachine &m)
{
    auto *ctx = ctx_of(m);
    std::string key = guest_str(m, m.sysarg(0), m.sysarg(1));
    m.set_result(zf_guest_kv_del(ctx->kv, key.c_str()));
}

static void ecall_kv_list(SandboxMachine &m)
{
    auto *ctx = ctx_of(m);
    const auto g_prefix = m.sysarg(0);
    const uint64_t prefix_len = m.sysarg(1);
    const auto g_buf = m.sysarg(2);
    const uint64_t buf_len = m.sysarg(3);

    std::string prefix = guest_str(m, g_prefix, prefix_len);
    std::vector<char> tmp(buf_len ? buf_len : 1);
    int64_t n = zf_guest_kv_list(ctx->kv, prefix.c_str(), tmp.data(), buf_len);
    if (n > 0 && buf_len > 0) m.copy_to_guest(g_buf, tmp.data(), buf_len);
    m.set_result(n);
}

static void ecall_print(SandboxMachine &m)
{
    auto *ctx = ctx_of(m);
    const uint64_t len = m.sysarg(1);
    if (len == 0) { m.set_result(0); return; }
    std::string s = guest_str(m, m.sysarg(0), len);
    fprintf(stderr, "zone %u guest: %.*s\n", ctx->z_id, (int)s.size(), s.data());
    m.set_result(0);
}

/*
 * Host entropy. Routed through one call so a journal replay can seed
 * it deterministically (see zone_abi.h's ZONE_ENTROPY note). Fails
 * closed: a guest never receives predictable bytes it believes are
 * random.
 */
static void ecall_entropy(SandboxMachine &m)
{
    const auto g_buf = m.sysarg(0);
    const uint64_t len = m.sysarg(1);
    if (len == 0) { m.set_result(0); return; }

    static FILE *urandom = nullptr;
    if (!urandom) urandom = fopen("/dev/urandom", "rb");

    std::vector<uint8_t> tmp(len);
    if (!urandom || fread(tmp.data(), 1, len, urandom) != len) {
        fprintf(stderr, "sandbox: host entropy unavailable, guest request denied\n");
        m.set_result(-EIO);
        return;
    }
    m.copy_to_guest(g_buf, tmp.data(), len);
    m.set_result((int64_t)len);
}

/*
 * Object store, declared in the ABI and not yet wired.
 *
 * Deliberately NOT implemented here. fabric-godot-core's
 * modules/multiplayer_fabric_asset already implements this exact
 * format in C++ -- .caibx parsing, chunk fetch, SHA-512/256
 * verification, zstd, AES-128-GCM, local cache, and the Uro ACL
 * check. Writing a second casync client in this file would repeat
 * the mistake this whole rewrite corrected: a bespoke reimplementation
 * of something that exists.
 *
 * ZONE_OBJ_PUT additionally is not a guest capability. A guest
 * reaches it only through a ReBAC delegation edge to a principal that
 * holds admin capability, and the publish is attributed to that
 * principal (rfd/0095).
 *
 * Both answer -ENOSYS until that module is extracted from the Godot
 * build and linked here. Stated explicitly rather than left to the
 * catch-all, so a guest developer reading the log sees "not wired
 * yet" instead of "wrong syscall number".
 */
static void ecall_obj_unimplemented(SandboxMachine &m)
{
    m.set_result(-ENOSYS);
}

/* --- guest console ---------------------------------------------------- */

/*
 * setup_minimal_syscalls installs libriscv's own write(), which goes
 * to the host's stdout untagged. Replacing it keeps every guest line
 * attributed to its zone, and keeps a guest's printf working without
 * the guest needing ZONE_PRINT for ordinary output.
 */
static void sys_write(SandboxMachine &m)
{
    auto *ctx = ctx_of(m);
    const uint64_t len = m.sysarg(2);
    if (len == 0) { m.set_result(0); return; }
    std::string s = guest_str(m, m.sysarg(1), len);
    fprintf(stderr, "zone %u guest: %.*s", ctx->z_id, (int)s.size(), s.data());
    m.set_result((int64_t)len);
}

/* --- thread body ------------------------------------------------------- */

/*
 * There is no content-seeding path here, deliberately.
 *
 * An earlier revision copied a host directory into the KV store so a
 * guest could read its content pack back out. That paid FDB write
 * cost, storage quota, and replication for bytes that are immutable,
 * identical in every zone, and already addressable by content hash
 * from the CDN. Content does not belong in a transactional database.
 *
 * The split this loader now keeps:
 *   content (immutable, shared)  -> the guest ELF itself for script
 *                                   guests; a read-only bind mount for
 *                                   bubblewrap engine guests
 *   state   (mutable, per-zone)  -> zf_guest_kv, which is what FDB's
 *                                   transactions and durability are
 *                                   actually for
 */

struct guest_thread_arg {
    sandbox_guest_config_t cfg;
    std::string            elf_path;
    std::string            cluster_file;
};

static void *guest_thread_main(void *varg)
{
    auto *arg = static_cast<guest_thread_arg *>(varg);
    const uint32_t z_id = arg->cfg.z_id;

    /*
     * Admin-plane gate (rfd/0092, rfd/0094): loading a guest is a
     * MODIFY action, owner-only. Identity is the documented gap
     * (README: TLS cert/key still NULL/NULL), so until a real subject
     * exists the claim is the process operator's own OWNER relation --
     * the operator started this binary with -g, which IS ownership of
     * the process. The rebac_check call is real and stays on this path
     * so the wiring never needs to move when identity lands.
     */
    const rebac_relation_t operator_claim[] = { REBAC_RELATION_OWNER };
    if (!rebac_check(operator_claim, 1, REBAC_ACTION_MODIFY)) {
        fprintf(stderr, "zone %u: guest load DENIED by rebac_check\n", z_id);
        delete arg;
        return nullptr;
    }

    std::ifstream f(arg->elf_path, std::ios::binary);
    if (!f) {
        fprintf(stderr, "zone %u: guest ELF unreadable: %s\n", z_id,
                arg->elf_path.c_str());
        delete arg;
        return nullptr;
    }
    std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

    zf_guest_kv_limits_t limits = {
        ZF_GUEST_KV_MAX_VALUE_BYTES_DEFAULT,
        ZF_GUEST_KV_MAX_TOTAL_BYTES_DEFAULT,
        ZF_GUEST_KV_MAX_KEY_LEN_DEFAULT,
    };
    zf_guest_kv_t *kv = zf_guest_kv_create(arg->cluster_file.c_str(), z_id, &limits);
    if (!kv) {
        fprintf(stderr, "zone %u: zf_guest_kv_create failed\n", z_id);
        delete arg;
        return nullptr;
    }

    try {
        riscv::MachineOptions<W> options;
        options.memory_max = arg->cfg.memory_max;
        SandboxMachine machine{elf, options};

        sandbox_ctx ctx{kv, z_id};
        machine.set_userdata(&ctx);

        /*
         * Minimal syscalls only: close, lseek, write, fstat, exit,
         * brk, ebreak. No filesystem, no sockets, no process table --
         * not because those are switched off, but because they were
         * never installed. There is no FileDescriptors object, no
         * network namespace to reach, and nothing to escape from.
         */
        machine.setup_minimal_syscalls();
        machine.setup_linux({"guest"}, {"LC_ALL=C"});

        SandboxMachine::install_syscall_handler(64, sys_write);
        SandboxMachine::install_syscall_handler(ZONE_KV_GET, ecall_kv_get);
        SandboxMachine::install_syscall_handler(ZONE_KV_SET, ecall_kv_set);
        SandboxMachine::install_syscall_handler(ZONE_KV_DEL, ecall_kv_del);
        SandboxMachine::install_syscall_handler(ZONE_KV_LIST, ecall_kv_list);
        SandboxMachine::install_syscall_handler(ZONE_PRINT, ecall_print);
        SandboxMachine::install_syscall_handler(ZONE_ENTROPY, ecall_entropy);
        SandboxMachine::install_syscall_handler(ZONE_OBJ_GET, ecall_obj_unimplemented);
        SandboxMachine::install_syscall_handler(ZONE_OBJ_PUT, ecall_obj_unimplemented);

        /*
         * Anything else returns -ENOSYS instead of killing the guest.
         * libriscv's default throws a machine exception, which ends a
         * guest at whatever unimplemented call it happens to reach
         * first. A real kernel answers an unknown syscall with an
         * errno, and libc code handles that everywhere, so a guest
         * built against a fuller libc degrades instead of dying.
         * Logged once per number, so an unexpected capability need
         * shows up in the zone log.
         */
        SandboxMachine::on_unhandled_syscall = [](SandboxMachine &mm, size_t num) {
            static bool seen[1024] = {false};
            if (num < 1024 && !seen[num]) {
                seen[num] = true;
                fprintf(stderr, "zone guest: syscall %zu unimplemented, -ENOSYS\n", num);
            }
            mm.set_result(-ENOSYS);
        };

        fprintf(stderr, "zone %u: guest booting (%zu byte ELF, %llu MB mem, %llu Minstr budget)\n",
                z_id, elf.size(),
                (unsigned long long)(arg->cfg.memory_max >> 20),
                (unsigned long long)(arg->cfg.max_instructions / 1000000));

        machine.simulate(arg->cfg.max_instructions);

        fprintf(stderr, "zone %u: guest exited, status %d, %llu instructions\n",
                z_id, machine.return_value<int>(),
                (unsigned long long)machine.instruction_counter());
    } catch (const std::exception &e) {
        fprintf(stderr, "zone %u: guest fault: %s\n", z_id, e.what());
    }

    zf_guest_kv_destroy(kv);
    delete arg;
    return nullptr;
}

extern "C" int sandbox_guest_start(const sandbox_guest_config_t *cfg)
{
    auto *arg = new guest_thread_arg{};
    arg->cfg = *cfg;
    arg->elf_path = cfg->elf_path;
    arg->cluster_file = cfg->cluster_file;
    if (arg->cfg.memory_max == 0)
        arg->cfg.memory_max = SANDBOX_GUEST_MEMORY_MAX_DEFAULT;
    if (arg->cfg.max_instructions == 0)
        arg->cfg.max_instructions = SANDBOX_GUEST_MAX_INSTR_DEFAULT;

    pthread_t tid;
    if (pthread_create(&tid, nullptr, guest_thread_main, arg) != 0) {
        delete arg;
        return -1;
    }
    pthread_detach(tid);
    return 0;
}
