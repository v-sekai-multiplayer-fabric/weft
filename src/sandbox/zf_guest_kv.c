/*
 * FoundationDB-backed guest key-value store. See zf_guest_kv.h for the
 * contract: small linearizable state only, no chunking, quota
 * pressure blocks instead of failing, guest-thread-only blocking.
 * Content goes to the object store, never here.
 *
 * Every FDB call blocks via fdb_future_block_until_ready. That is safe
 * here and only here: sandbox_guest.cpp calls this from the dedicated
 * guest pthread, never from an h2o worker loop. libfdb_c's own network
 * thread (fdb_run_network in main) services the futures meanwhile.
 */

#include "zf_guest_kv.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GKV_PREFIX "zf/guestkv/"
#define GKV_KEY_MAX 700
#define GKV_QUOTA_POLL_NS (100 * 1000 * 1000) /* 100 ms */

/* FoundationDB gives a transaction 5 seconds. Both constants below
 * exist to keep every transaction here inside that budget, rather than
 * discovering the limit as error 1007 at run time.
 *
 * The 5 s applies to ONE transaction, not to a call. The quota
 * block-and-poll is deliberately unbounded (rfd/0092 makes budget
 * extension an admin-plane action), and that is safe because the sleep
 * happens BETWEEN transactions: gkv_txn resets before it sleeps, so a
 * stalled guest never holds a read version across the wait. */
#define GKV_TXN_TIMEOUT_MS 5000
#define GKV_LIST_MAX_ROWS  1024

struct zf_guest_kv {
    FDBDatabase         *db;
    uint32_t             z_id;
    zf_guest_kv_limits_t limits;
};

/*
 * Key normalization, WASI-style. The guest namespace is closed (every
 * key lands under this zone's prefix, never a host path), so escape is
 * structurally impossible -- but without normalization, "a/../b" and
 * "b" would be two different key families for what the guest believes
 * is one entry, and the usage counter would drift. WASI's filesystem
 * capability model resolves exactly this class before lookup, so this
 * follows that model.
 *
 * Output: components joined by '/', no leading '/', no "." or "..".
 * Returns length, or -1 if ".." would climb above the root or the
 * result exceeds max_len.
 */
static int gkv_normalize(const char *in, char *out, size_t max_len)
{
    size_t out_len = 0;
    size_t comp_starts[64];
    size_t n_comps = 0;

    const char *p = in;
    while (*p == '/') p++;

    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t clen = (size_t)(p - start);
        while (*p == '/') p++;

        if (clen == 0 || (clen == 1 && start[0] == '.')) continue;
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            if (n_comps == 0) return -1; /* would climb above the root */
            n_comps--;
            out_len = comp_starts[n_comps];
            continue;
        }
        if (n_comps >= 64) return -1;
        if (out_len + clen + 2 > max_len) return -1;
        comp_starts[n_comps++] = out_len;
        if (out_len > 0) out[out_len++] = '/';
        memcpy(out + out_len, start, clen);
        out_len += clen;
    }
    out[out_len] = '\0';
    return (int)out_len;
}

/* --- keys ------------------------------------------------------------- */

/* "zf/guestkv/{z_id}/{key}" -- z_id big-endian, matching zf_kv.h. */
static size_t gkv_data_key(const zf_guest_kv_t *kv, uint8_t *buf, const char *key)
{
    size_t n = 0;
    memcpy(buf, GKV_PREFIX, sizeof(GKV_PREFIX) - 1);
    n += sizeof(GKV_PREFIX) - 1;
    buf[n++] = (uint8_t)(kv->z_id >> 24);
    buf[n++] = (uint8_t)(kv->z_id >> 16);
    buf[n++] = (uint8_t)(kv->z_id >> 8);
    buf[n++] = (uint8_t)(kv->z_id);
    buf[n++] = '/';
    size_t klen = strlen(key);
    memcpy(buf + n, key, klen);
    return n + klen;
}

static size_t gkv_usage_key(const zf_guest_kv_t *kv, uint8_t *buf)
{
    size_t n = 0;
    memcpy(buf, GKV_PREFIX, sizeof(GKV_PREFIX) - 1);
    n += sizeof(GKV_PREFIX) - 1;
    buf[n++] = (uint8_t)(kv->z_id >> 24);
    buf[n++] = (uint8_t)(kv->z_id >> 16);
    buf[n++] = (uint8_t)(kv->z_id >> 8);
    buf[n++] = (uint8_t)(kv->z_id);
    buf[n++] = '!';
    buf[n++] = 'u';
    return n;
}

/* --- blocking FDB helpers --------------------------------------------- */

/* Sentinel: the txn function asks the outer loop to sleep and retry
 * the whole transaction (quota pressure -- a stall, not an error). */
#define GKV_EAGAIN_QUOTA (-1000000)

typedef int (*gkv_txn_fn)(zf_guest_kv_t *kv, FDBTransaction *tr, void *ctx);

static int gkv_txn(zf_guest_kv_t *kv, gkv_txn_fn fn, void *ctx)
{
    FDBTransaction *tr = NULL;
    fdb_error_t err = fdb_database_create_transaction(kv->db, &tr);
    if (err) return -EIO;

    /* State the 5 s budget in code instead of inheriting it. FDB then
     * fails a too-slow transaction as 1007 on its own schedule, which
     * fdb_transaction_on_error below retries against a fresh read
     * version. */
    int64_t tmo = GKV_TXN_TIMEOUT_MS;
    fdb_transaction_set_option(tr, FDB_TR_OPTION_TIMEOUT,
                               (const uint8_t *)&tmo, sizeof(tmo));

    for (;;) {
        int rc = fn(kv, tr, ctx);
        if (rc == GKV_EAGAIN_QUOTA) {
            /* Reset BEFORE sleeping. The wait is unbounded by design,
             * and a 100 ms sleep on a live read version would burn the
             * 5 s budget of a transaction we are about to discard --
             * turning a stall into a 1007 storm. Reset first, so every
             * retry starts a clean 5 s window. */
            fdb_transaction_reset(tr);
            fdb_transaction_set_option(tr, FDB_TR_OPTION_TIMEOUT,
                                       (const uint8_t *)&tmo, sizeof(tmo));
            struct timespec ts = { 0, GKV_QUOTA_POLL_NS };
            nanosleep(&ts, NULL);
            continue;
        }
        if (rc < 0) {
            fdb_transaction_destroy(tr);
            return rc;
        }

        FDBFuture *cf = fdb_transaction_commit(tr);
        fdb_future_block_until_ready(cf);
        err = fdb_future_get_error(cf);
        fdb_future_destroy(cf);
        if (!err) {
            fdb_transaction_destroy(tr);
            return 0;
        }

        FDBFuture *rf = fdb_transaction_on_error(tr, err);
        fdb_future_block_until_ready(rf);
        fdb_error_t rerr = fdb_future_get_error(rf);
        fdb_future_destroy(rf);
        if (rerr) { /* not retryable */
            fdb_transaction_destroy(tr);
            return -EIO;
        }
        /* retryable: tr is reset by on_error; loop again */
    }
}

/* Blocking point-get inside tr. Copies up to buf_len bytes, reports the
 * FULL length in *out_len. Returns 1 present, 0 absent, -EIO. */
static int gkv_get_raw(FDBTransaction *tr, const uint8_t *key, int key_len,
                       uint8_t *buf, int buf_len, int *out_len)
{
    FDBFuture *f = fdb_transaction_get(tr, key, key_len, 0);
    fdb_future_block_until_ready(f);
    if (fdb_future_get_error(f)) { fdb_future_destroy(f); return -EIO; }

    fdb_bool_t present = 0;
    const uint8_t *val = NULL;
    int len = 0;
    if (fdb_future_get_value(f, &present, &val, &len)) {
        fdb_future_destroy(f);
        return -EIO;
    }
    if (present && buf && buf_len > 0) {
        int n = len < buf_len ? len : buf_len;
        memcpy(buf, val, (size_t)n);
    }
    if (out_len) *out_len = len;
    fdb_future_destroy(f);
    return present ? 1 : 0;
}

static uint64_t gkv_decode_u64(const uint8_t *v, int len)
{
    uint64_t x = 0;
    if (len == 8) memcpy(&x, v, 8);
    return x;
}

/* --- lifecycle -------------------------------------------------------- */

zf_guest_kv_t *zf_guest_kv_create(const char *cluster_file, uint32_t z_id,
                                  const zf_guest_kv_limits_t *limits)
{
    zf_guest_kv_t *kv = calloc(1, sizeof(*kv));
    if (!kv) return NULL;
    kv->z_id = z_id;
    kv->limits = *limits;
    if (kv->limits.max_value_bytes == 0)
        kv->limits.max_value_bytes = ZF_GUEST_KV_MAX_VALUE_BYTES_DEFAULT;
    if (kv->limits.max_key_len == 0)
        kv->limits.max_key_len = ZF_GUEST_KV_MAX_KEY_LEN_DEFAULT;

    if (fdb_create_database(cluster_file, &kv->db)) {
        free(kv);
        return NULL;
    }
    return kv;
}

void zf_guest_kv_destroy(zf_guest_kv_t *kv)
{
    if (!kv) return;
    fdb_database_destroy(kv->db);
    free(kv);
}

/* --- get -------------------------------------------------------------- */

typedef struct {
    const char *key;
    uint8_t    *buf;
    uint64_t    buf_len;
    int64_t     len;    /* full stored length */
    bool        found;
} gkv_get_ctx_t;

static int gkv_get_txn(zf_guest_kv_t *kv, FDBTransaction *tr, void *vctx)
{
    gkv_get_ctx_t *ctx = vctx;
    uint8_t key[GKV_KEY_MAX];
    size_t klen = gkv_data_key(kv, key, ctx->key);
    int len = 0;
    int rc = gkv_get_raw(tr, key, (int)klen, ctx->buf, (int)ctx->buf_len, &len);
    if (rc < 0) return rc;
    ctx->found = rc == 1;
    ctx->len = len;
    return 0;
}

int64_t zf_guest_kv_get(zf_guest_kv_t *kv, const char *key,
                        uint8_t *buf, uint64_t buf_len)
{
    char norm[ZF_GUEST_KV_MAX_KEY_LEN_DEFAULT + 1];
    if (gkv_normalize(key, norm, sizeof(norm)) < 0) return -ENAMETOOLONG;
    if (strlen(norm) > kv->limits.max_key_len) return -ENAMETOOLONG;

    gkv_get_ctx_t ctx = {
        .key = norm, .buf = buf, .buf_len = buf_len, .len = 0, .found = false,
    };
    int rc = gkv_txn(kv, gkv_get_txn, &ctx);
    if (rc < 0) return rc;
    if (!ctx.found) return -ENOENT;
    return ctx.len; /* full length, even when the caller's buffer was short */
}

/* --- set -------------------------------------------------------------- */

typedef struct {
    const char    *key;
    const uint8_t *val;
    uint64_t       val_len;
} gkv_set_ctx_t;

/*
 * One transaction: read the old size, check the quota, write the value
 * and the new usage together. FDB's serializable isolation is what
 * keeps the counter and the data from ever disagreeing. That is the
 * linearizable-datastore job, and the reason state lives here while
 * content does not.
 */
static int gkv_set_txn(zf_guest_kv_t *kv, FDBTransaction *tr, void *vctx)
{
    gkv_set_ctx_t *ctx = vctx;

    uint8_t key[GKV_KEY_MAX];
    size_t klen = gkv_data_key(kv, key, ctx->key);
    int old_len = 0;
    int rc = gkv_get_raw(tr, key, (int)klen, NULL, 0, &old_len);
    if (rc < 0) return rc;
    uint64_t old_size = rc == 1 ? (uint64_t)old_len : 0;

    uint8_t ukey[GKV_KEY_MAX];
    size_t ulen = gkv_usage_key(kv, ukey);
    uint8_t uv[8]; int uvlen = 0;
    rc = gkv_get_raw(tr, ukey, (int)ulen, uv, 8, &uvlen);
    if (rc < 0) return rc;
    uint64_t usage = rc == 1 ? gkv_decode_u64(uv, uvlen) : 0;

    uint64_t new_usage = usage - old_size + ctx->val_len;
    if (new_usage > kv->limits.max_total_bytes) {
        /* Over quota: the outer loop sleeps and retries. Storage
         * pressure stalls the guest instead of failing its write. */
        return GKV_EAGAIN_QUOTA;
    }

    fdb_transaction_set(tr, key, (int)klen, ctx->val, (int)ctx->val_len);
    fdb_transaction_set(tr, ukey, (int)ulen, (const uint8_t *)&new_usage, 8);
    return 0;
}

int64_t zf_guest_kv_set(zf_guest_kv_t *kv, const char *key,
                        const uint8_t *val, uint64_t val_len)
{
    char norm[ZF_GUEST_KV_MAX_KEY_LEN_DEFAULT + 1];
    if (gkv_normalize(key, norm, sizeof(norm)) < 0) return -ENAMETOOLONG;
    if (strlen(norm) > kv->limits.max_key_len) return -ENAMETOOLONG;
    /* Past this size the data is content, not state: the object store
     * takes it, deduplicated and cached, instead of the transaction
     * log replicating it. See zone_abi.h's ZONE_OBJ_GET. */
    if (val_len > kv->limits.max_value_bytes) return -E2BIG;

    gkv_set_ctx_t ctx = { .key = norm, .val = val, .val_len = val_len };
    int rc = gkv_txn(kv, gkv_set_txn, &ctx);
    return rc < 0 ? rc : (int64_t)val_len;
}

/* --- del -------------------------------------------------------------- */

typedef struct { const char *key; } gkv_del_ctx_t;

static int gkv_del_txn(zf_guest_kv_t *kv, FDBTransaction *tr, void *vctx)
{
    gkv_del_ctx_t *ctx = vctx;

    uint8_t key[GKV_KEY_MAX];
    size_t klen = gkv_data_key(kv, key, ctx->key);
    int len = 0;
    int rc = gkv_get_raw(tr, key, (int)klen, NULL, 0, &len);
    if (rc < 0) return rc;
    if (rc == 0) return -ENOENT;

    uint8_t ukey[GKV_KEY_MAX];
    size_t ulen = gkv_usage_key(kv, ukey);
    uint8_t uv[8]; int uvlen = 0;
    rc = gkv_get_raw(tr, ukey, (int)ulen, uv, 8, &uvlen);
    if (rc < 0) return rc;
    uint64_t usage = rc == 1 ? gkv_decode_u64(uv, uvlen) : 0;
    uint64_t new_usage = usage > (uint64_t)len ? usage - (uint64_t)len : 0;

    fdb_transaction_clear(tr, key, (int)klen);
    fdb_transaction_set(tr, ukey, (int)ulen, (const uint8_t *)&new_usage, 8);
    return 0;
}

int zf_guest_kv_del(zf_guest_kv_t *kv, const char *key)
{
    char norm[ZF_GUEST_KV_MAX_KEY_LEN_DEFAULT + 1];
    if (gkv_normalize(key, norm, sizeof(norm)) < 0) return -ENAMETOOLONG;
    gkv_del_ctx_t ctx = { .key = norm };
    return gkv_txn(kv, gkv_del_txn, &ctx);
}

/* --- list ------------------------------------------------------------- */

typedef struct {
    const char *prefix;
    char       *buf;
    uint64_t    buf_len;
    uint64_t    used;
    int64_t     count;
} gkv_list_ctx_t;

static int gkv_list_txn(zf_guest_kv_t *kv, FDBTransaction *tr, void *vctx)
{
    gkv_list_ctx_t *ctx = vctx;

    uint8_t begin[GKV_KEY_MAX], end[GKV_KEY_MAX];
    size_t blen = gkv_data_key(kv, begin, ctx->prefix);
    memcpy(end, begin, blen);
    size_t elen = blen;
    end[elen++] = 0xff; /* strict upper bound over the prefix */

    /* The stored-key prefix ahead of every guest key, so a scanned key
     * can be turned back into the guest's own name. */
    uint8_t root[GKV_KEY_MAX];
    size_t rootlen = gkv_data_key(kv, root, "");

    /* Bounded on purpose. WANT_ALL with no row limit asks FDB for the
     * whole range in one transaction, and a guest with many keys can
     * push that past FDB's 5 s transaction limit. That surfaces as
     * error 1007 (transaction_too_old), which fdb_transaction_on_error
     * reports as RETRYABLE -- so gkv_txn would retry a read that cannot
     * ever fit, forever. A livelock, not an error.
     *
     * The row cap makes the read fit by construction. It costs nothing
     * the caller did not already accept: zone_abi.h documents that
     * ZONE_KV_LIST truncates, and the buf_len check below truncates on
     * a whole-entry boundary anyway. EXACT matches the cap, where
     * WANT_ALL would ignore it. */
    FDBFuture *f = fdb_transaction_get_range(
        tr,
        FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(begin, (int)blen),
        FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(end, (int)elen),
        GKV_LIST_MAX_ROWS, 0, FDB_STREAMING_MODE_EXACT, 0, 0, 0);
    fdb_future_block_until_ready(f);
    if (fdb_future_get_error(f)) { fdb_future_destroy(f); return -EIO; }

    const FDBKeyValue *kvs = NULL;
    int count = 0;
    fdb_bool_t more = 0;
    if (fdb_future_get_keyvalue_array(f, &kvs, &count, &more)) {
        fdb_future_destroy(f);
        return -EIO;
    }

    for (int i = 0; i < count; i++) {
        int klen = kvs[i].key_length;
        if (klen <= (int)rootlen) continue;
        size_t guest_len = (size_t)klen - rootlen;
        if (ctx->used + guest_len + 1 > ctx->buf_len) break; /* whole entries only */
        memcpy(ctx->buf + ctx->used, kvs[i].key + rootlen, guest_len);
        ctx->used += guest_len;
        ctx->buf[ctx->used++] = '\0';
        ctx->count++;
    }
    fdb_future_destroy(f);
    return 0;
}

int64_t zf_guest_kv_list(zf_guest_kv_t *kv, const char *prefix,
                         char *buf, uint64_t buf_len)
{
    char norm[ZF_GUEST_KV_MAX_KEY_LEN_DEFAULT + 1];
    /* An empty prefix is legal and means "everything in this zone". */
    if (prefix[0] != '\0') {
        if (gkv_normalize(prefix, norm, sizeof(norm)) < 0) return -ENAMETOOLONG;
    } else {
        norm[0] = '\0';
    }

    gkv_list_ctx_t ctx = {
        .prefix = norm, .buf = buf, .buf_len = buf_len, .used = 0, .count = 0,
    };
    int rc = gkv_txn(kv, gkv_list_txn, &ctx);
    return rc < 0 ? rc : ctx.count;
}
