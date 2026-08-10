#ifndef SANDBOX_GUEST_H_
#define SANDBOX_GUEST_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Guest ELF loader -- RFD 0094's item 1, the host side of the minimum
 * UGC game loop. C-callable so main.c can start a guest without
 * touching C++; the implementation (sandbox_guest.cpp) links
 * thirdparty/libriscv and src/sandbox/zf_guestfs.
 *
 * Isolation contract (rfd/0092's use plane, rfd/0094's constraints):
 *   - setup_minimal_syscalls() only: close, lseek, write, fstat, exit,
 *     brk, ebreak. No filesystem and no sockets exist to deny, because
 *     neither was ever installed.
 *   - Every host operation beyond that is an explicit ecall from
 *     zone_abi.h. Storage looks local to the guest and is really FDB.
 *   - Hard limits: guest memory (memory_max), an instruction budget,
 *     and zf_guest_kv's own storage ceilings.
 *
 * This loader takes SCRIPT-class guests. Engine-class guests (a whole
 * Godot build) run as ordinary processes under bubblewrap instead --
 * emulating enough Linux for one in-process is unbounded work. See
 * rfd/0095.
 */

typedef struct {
    const char *elf_path;      /* guest ELF on the host disk (CDN-fetched) */
    const char *cluster_file;  /* FDB cluster file, for zf_guest_kv */
    uint32_t    z_id;
    uint64_t    memory_max;    /* guest memory ceiling, bytes */
    uint64_t    max_instructions; /* per sandbox_guest_run() call */
} sandbox_guest_config_t;

#define SANDBOX_GUEST_MEMORY_MAX_DEFAULT (512ull << 20)
#define SANDBOX_GUEST_MAX_INSTR_DEFAULT  (16ull * 1000 * 1000 * 1000)

/*
 * Spawns the dedicated guest pthread: ReBAC-gates the load (admin
 * plane, modify/owner -- see the call site's identity note), loads the
 * ELF, installs the VFS syscall layer, runs the guest to completion or
 * budget exhaustion, logs the outcome. Returns 0 if the thread
 * started, -1 otherwise. Fire-and-forget: the thread detaches, since
 * a guest's lifetime is independent of any one request.
 */
int sandbox_guest_start(const sandbox_guest_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif
