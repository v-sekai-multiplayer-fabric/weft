#ifndef ZF_GUEST_KV_H_
#define ZF_GUEST_KV_H_

#define FDB_API_VERSION 730

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <foundationdb/fdb_c.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * FoundationDB-backed guest key-value store -- the storage half of
 * zone_abi.h's ZONE_KV_* calls.
 *
 * This layer used to present a POSIX filesystem (open/read/write/
 * lseek/stat, plus a working directory and device nodes). That
 * abstraction is gone: FoundationDB is a key-value store, the guests
 * only ever wanted get and put, and every POSIX concept in between was
 * invented here and then had to be defended here.
 *
 * What survives is the part that was never bespoke -- the rules for
 * putting arbitrary guest data into FDB safely:
 *
 *   1. Hard limits. Every ceiling is a compile-visible constant in
 *      zf_guest_kv_limits_t, enforced in this layer, not in callers.
 *      rfd/0092 makes budget *extension* a ReBAC-authorized relation
 *      later; these are the floor values a guest gets before any grant.
 *
 *   2. Offline illusion. The guest believes it talks to local storage.
 *      Every call here is really a networked FDB transaction. The
 *      guest never learns which, and never touches the h2o event loop
 *      -- every FDB call BLOCKS on its own future, which is only safe
 *      because sandbox_guest.cpp runs this on a dedicated guest
 *      thread, never on a worker loop (libfdb_c's network thread is
 *      separate and keeps running; see fdb_database.h's threading
 *      notes).
 *
 *   3. Quota pressure becomes latency, not an error. A write that
 *      would push the zone's usage past max_total_bytes BLOCKS and
 *      polls until space frees. Consequence, stated plainly: with a
 *      single guest and nothing else freeing space, that block does
 *      not resolve until an admin-plane actor deletes keys or raises
 *      the quota -- which is rfd/0092's
 *      budget-extension-as-ReBAC-relation, not an error path here.
 *
 *   4. No chunking, and no value large enough to need it. A value
 *      over max_value_bytes is -E2BIG. At that size the data is
 *      content, and content goes to the object store
 *      (zf_guest_obj.h), where it is deduplicated, cached, and never
 *      replicated through a transaction log. An earlier revision
 *      chunked megabyte values across transactions; that was
 *      filesystem virtualization wearing a key-value coat.
 *
 * Keyspace (matches zf_kv.h's prefix + big-endian style):
 *   "zf/guestkv/{z_id}/{key}"  -> value bytes, verbatim
 *   "zf/guestkv/{z_id}!u"      -> u64 total-bytes usage
 *
 * Guest keys are normalized before use (lexical "." and ".."
 * resolution inside a closed namespace, the way WASI resolves paths
 * before lookup) so one logical key is always exactly one key family
 * and the usage counter cannot drift.
 */

/* Matches zone_abi.h's ZONE_KV_MAX_VALUE: the line where state ends
 * and content begins. Well under FDB's 100,000-byte hard limit. */
#define ZF_GUEST_KV_MAX_VALUE_BYTES_DEFAULT 32768
#define ZF_GUEST_KV_MAX_TOTAL_BYTES_DEFAULT (8 * 1024 * 1024)
#define ZF_GUEST_KV_MAX_KEY_LEN_DEFAULT     512

typedef struct {
    uint64_t max_value_bytes; /* per value (policy, not an FDB limit) */
    uint64_t max_total_bytes; /* per zone guest namespace */
    uint32_t max_key_len;     /* bytes, excluding NUL */
} zf_guest_kv_limits_t;

typedef struct zf_guest_kv zf_guest_kv_t;

/* Creates its own FDBDatabase from cluster_file -- deliberately NOT a
 * borrowed fdb_thread_state_t: the guest thread must never share the
 * event-loop adapter. Requires fdb_setup_network()/fdb_run_network()
 * to already be live (main.c does both before guests start). */
zf_guest_kv_t *zf_guest_kv_create(const char *cluster_file, uint32_t z_id,
                                  const zf_guest_kv_limits_t *limits);
void zf_guest_kv_destroy(zf_guest_kv_t *kv);

/* All calls below return >= 0 on success or a negative errno value
 * (-ENOENT, -ENAMETOOLONG, -E2BIG, -EIO), ready to hand to
 * machine.set_result() unchanged. Each is one FDB transaction. On
 * quota pressure they block and poll until space frees, rather than
 * returning a capacity error. */

/* Returns the FULL value length, even if it exceeds buf_len (the
 * caller can resize and retry). -ENOENT if absent. */
int64_t zf_guest_kv_get(zf_guest_kv_t *kv, const char *key,
                        uint8_t *buf, uint64_t buf_len);

int64_t zf_guest_kv_set(zf_guest_kv_t *kv, const char *key,
                        const uint8_t *val, uint64_t val_len);

int zf_guest_kv_del(zf_guest_kv_t *kv, const char *key);

/* Writes matching keys into buf as NUL-separated strings, truncating
 * at buf_len on a whole-entry boundary. Returns the count written. */
int64_t zf_guest_kv_list(zf_guest_kv_t *kv, const char *prefix,
                         char *buf, uint64_t buf_len);

#ifdef __cplusplus
}
#endif

#endif
