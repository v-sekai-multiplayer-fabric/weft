#ifndef ZONE_ABI_H_
#define ZONE_ABI_H_

/*
 * The zone guest ABI: the complete set of host operations a sandboxed
 * guest can reach. Included by BOTH the host (src/sandbox/) and every
 * guest program, so the two can never disagree about a number.
 *
 * This is a DOMAIN ABI, not a Linux emulation. A guest does not open
 * files, seek, or stat. The pattern comes from godot-sandbox,
 * libriscv's own production integration, whose guests likewise call
 * domain operations (ECALL_VCALL, ECALL_GET_NODE) and never a POSIX
 * syscall.
 *
 * TWO STORES, and the split is deliberate -- neither one virtualizes a
 * filesystem, because a filesystem is what forced an earlier revision
 * of this host to grow a fake /dev, a fake working directory, and a
 * fake process table:
 *
 *   ZONE_KV_*   FoundationDB, used as what it is: a LINEARIZABLE
 *               datastore. Small mutable state, transactional, hot.
 *               Values are capped well under FDB's own limits so no
 *               chunking layer is needed or wanted.
 *
 *   ZONE_OBJ_*  The object store: casync/desync content-defined
 *               chunking, .caibx indexes, zstd chunks, over a local
 *               cache and an S3-shaped store. Immutable and
 *               deduplicated across every asset that shares chunks --
 *               the property that matters most for user-generated
 *               content, where a thousand uploads are mostly the same
 *               bytes.
 *
 *               Two implementations of that format already exist and
 *               neither is written here: aria-storage (Elixir, the
 *               upload/publish side) and fabric-godot-core's
 *               modules/multiplayer_fabric_asset (C++, the fetch and
 *               verify side, on branch feat/module-multiplayer-fabric-
 *               asset). The C++ one is the reference for this host,
 *               and its header is explicit that its constants are
 *               canonical: SHA-512/256 chunk ids, 16 KB to 256 KB
 *               chunks, AES-128-GCM with a 24 h key TTL, and Uro for
 *               the ACL check. This ABI must not restate any of them.
 *
 * Putting content in FDB would pay transaction cost, replication, and
 * quota for bytes that never change and are identical in every zone.
 * Putting hot state in an object store would give up linearizability.
 * Each store does the one job it is good at.
 *
 * Numbering starts at 600:
 *   0..~450   Linux syscalls (libriscv's setup_minimal_syscalls uses a
 *             handful: write, exit, brk, close, lseek, fstat, ebreak)
 *   500..599  godot-sandbox's own GAME_API_BASE block, left free so a
 *             guest can speak both ABIs if it ever needs to
 *   600..     this ABI
 *
 * Calling convention, uniform across every call below:
 *   a0..a3  arguments as documented per call
 *   a0      return value: >= 0 on success, negative errno on failure
 *
 * Guest pointers are guest-virtual addresses. The host bounds-checks
 * every one through libriscv's memory API, so a bad pointer is a guest
 * fault, never a host read.
 */

#define ZONE_API_BASE 600

/*
 * ZONE_KV_GET(key_ptr, key_len, buf_ptr, buf_len) -> value length
 *
 * Reads one key. Returns the FULL value length even when it exceeds
 * buf_len (POSIX read semantics would truncate silently; this lets a
 * guest resize and retry). Returns -ENOENT when the key is absent.
 */
#define ZONE_KV_GET (ZONE_API_BASE + 0)

/*
 * ZONE_KV_SET(key_ptr, key_len, val_ptr, val_len) -> bytes written
 *
 * Writes one key, one FDB transaction, no chunking. A value over
 * ZONE_KV_MAX_VALUE is -E2BIG rather than something the host quietly
 * splits: at that size the data is content, and content belongs in
 * the object store where it is deduplicated and cached instead of
 * replicated through a transaction log.
 *
 * A write past the zone's storage quota BLOCKS until space frees,
 * rather than failing -- storage pressure is latency, not an error a
 * guest must code around.
 */
#define ZONE_KV_SET (ZONE_API_BASE + 1)

/* Comfortably under FoundationDB's 100,000-byte hard value limit, and
 * near its own ~10 KB recommendation. This is a design boundary, not
 * a workaround: it is the line where state ends and content begins. */
#define ZONE_KV_MAX_VALUE 32768

/* ZONE_KV_DEL(key_ptr, key_len) -> 0, or -ENOENT */
#define ZONE_KV_DEL (ZONE_API_BASE + 2)

/*
 * ZONE_KV_LIST(prefix_ptr, prefix_len, buf_ptr, buf_len) -> count
 *
 * Writes the matching keys into buf as NUL-separated strings and
 * returns how many were written. Truncates at buf_len (the last entry
 * is always NUL-terminated), so a guest with a small buffer gets a
 * short list rather than an error.
 */
#define ZONE_KV_LIST (ZONE_API_BASE + 3)

/*
 * ZONE_PRINT(str_ptr, str_len) -> 0
 *
 * One log line, tagged with the zone id on the host side. This is
 * observability, not a capability: a guest cannot choose a
 * destination, and cannot read anything back.
 */
#define ZONE_PRINT (ZONE_API_BASE + 4)

/*
 * ZONE_ENTROPY(buf_ptr, len) -> len
 *
 * Random bytes from the host.
 *
 * This exists for correctness, not only to avoid emulating
 * /dev/urandom. A zone server that replays a journal (rfd/0083's
 * shape) cannot have guests drawing entropy from the host RNG behind
 * its back: the replay would diverge from the recorded run. Routing
 * every guest random byte through one host call is what makes a
 * guest's randomness reproducible, because the host owns the seed.
 */
#define ZONE_ENTROPY (ZONE_API_BASE + 5)

/*
 * ZONE_OBJ_GET(id_ptr, id_len, buf_ptr, buf_len, offset) -> length
 *
 * Reads assembled object bytes. The id names a casync index
 * (.caibx/.caidx), NOT a whole-blob hash: the host resolves that
 * index's chunks through aria-storage, from the local chunk cache
 * first and the S3 backend second, and assembles the range the guest
 * asked for. Returns the FULL object length, so a guest can size a
 * buffer from one call and page a large object with `offset`.
 *
 * Naming an index rather than a blob is what buys deduplication: two
 * assets that differ slightly share nearly all their chunks, and the
 * host stores and transfers each chunk once.
 *
 * Immutability is what makes this cheap: the host can cache an object
 * on local disk forever, share one copy across every zone and every
 * guest, and never coordinate. That is the whole reason content does
 * not live in FDB.
 */
#define ZONE_OBJ_GET (ZONE_API_BASE + 6)

/*
 * ZONE_OBJ_PUT(buf_ptr, len, id_out_ptr, id_out_len) -> id length
 *
 * Publishes an object and writes its casync index id into id_out.
 * The host chunks the bytes, stores only chunks the store does not
 * already hold, and writes the index -- so republishing a small edit
 * of a large asset costs the edit, not the asset.
 *
 * A guest does not hold this capability itself. Publishing is an
 * admin-plane action (rfd/0092's CAN_GRANT plane), and a guest never
 * reaches that plane directly. What a guest can hold is a DELEGATION:
 * a ReBAC edge from the guest subject to a principal that does hold
 * admin capability, permitting the guest to publish on that
 * principal's authority.
 *
 * So the host answers this call by checking for that delegation edge,
 * not by checking the guest. Without one the answer is -EPERM. With
 * one the publish happens, attributed to the delegating principal,
 * because authority stays with whoever actually holds it.
 *
 * That check is rebac_check() in src/gen/rebac.h, and it is the
 * decision point -- not multiplayer_fabric_asset's acl_check(). The
 * two are not rival models: acl_check POSTs an (object, relation,
 * subject) tuple to Uro's /acl/check, so it is ReBAC too. They split
 * by job. Uro resolves the relation graph and owns it. This host takes
 * the resolved relation set and decides the action, from the generated
 * table, so there is exactly one decision procedure to keep correct.
 *
 * Resolution happens once, when the host loads a guest. A guest ecall
 * must never turn into an HTTP round trip to Uro.
 *
 * This is the mechanism that lets UGC scripting produce content --
 * a guest that authors an asset publishes it through the principal
 * who authorized it, rather than the host either refusing every guest
 * write or granting guests admin capability.
 */
#define ZONE_OBJ_PUT (ZONE_API_BASE + 7)

#define ZONE_ABI_CALL_COUNT 8

/*
 * Guest-side inline wrappers. Compiled only in a guest build (the
 * host has no `ecall` instruction), so they are guarded on the
 * riscv target rather than on a hand-set macro nobody remembers.
 */
#if defined(__riscv) && !defined(ZONE_ABI_NO_GUEST_STUBS)

static inline long zone_ecall4(long num, long a0, long a1, long a2, long a3)
{
	register long r_a0 __asm__("a0") = a0;
	register long r_a1 __asm__("a1") = a1;
	register long r_a2 __asm__("a2") = a2;
	register long r_a3 __asm__("a3") = a3;
	register long r_a7 __asm__("a7") = num;
	__asm__ __volatile__("ecall"
	                     : "+r"(r_a0)
	                     : "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a7)
	                     : "memory");
	return r_a0;
}

static inline long zone_kv_get(const char *key, unsigned long key_len,
                               void *buf, unsigned long buf_len)
{
	return zone_ecall4(ZONE_KV_GET, (long)key, (long)key_len, (long)buf, (long)buf_len);
}

static inline long zone_kv_set(const char *key, unsigned long key_len,
                               const void *val, unsigned long val_len)
{
	return zone_ecall4(ZONE_KV_SET, (long)key, (long)key_len, (long)val, (long)val_len);
}

static inline long zone_kv_del(const char *key, unsigned long key_len)
{
	return zone_ecall4(ZONE_KV_DEL, (long)key, (long)key_len, 0, 0);
}

static inline long zone_kv_list(const char *prefix, unsigned long prefix_len,
                                void *buf, unsigned long buf_len)
{
	return zone_ecall4(ZONE_KV_LIST, (long)prefix, (long)prefix_len, (long)buf, (long)buf_len);
}

static inline long zone_print(const char *str, unsigned long len)
{
	return zone_ecall4(ZONE_PRINT, (long)str, (long)len, 0, 0);
}

static inline long zone_entropy(void *buf, unsigned long len)
{
	return zone_ecall4(ZONE_ENTROPY, (long)buf, (long)len, 0, 0);
}

static inline long zone_obj_get(const char *id, unsigned long id_len,
                                void *buf, unsigned long buf_len,
                                unsigned long offset)
{
	register long r_a0 __asm__("a0") = (long)id;
	register long r_a1 __asm__("a1") = (long)id_len;
	register long r_a2 __asm__("a2") = (long)buf;
	register long r_a3 __asm__("a3") = (long)buf_len;
	register long r_a4 __asm__("a4") = (long)offset;
	register long r_a7 __asm__("a7") = ZONE_OBJ_GET;
	__asm__ __volatile__("ecall"
	                     : "+r"(r_a0)
	                     : "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a4), "r"(r_a7)
	                     : "memory");
	return r_a0;
}

#endif /* __riscv */

#endif /* ZONE_ABI_H_ */
