/*
 * ZoneTick: FDB-backed read-tick-write. See zf_zonetick.h.
 */

#include "zf_zonetick.h"

#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "zf_kv.h"

typedef struct {
    FDBTransaction *tr;
    fdb_thread_state_t *fdb_state;
    uint32_t z_id;
    uint32_t tick_count;
    zf_zonetick_done_cb done_cb;
    void *user_ctx;
} zonetick_ctx_t;

/* vel_i16's physical meaning, per lean-entity-packet's README: scaled to
 * +/-XR_PACKET_V_MAX_PHYSICAL_DEFAULT_UM_PER_TICK at +/-INT16_MAX. The
 * codec (xr_grid_entity_packet.c) intentionally does not do this
 * conversion itself -- see its header comment -- so it lives here, at
 * the one call site that needs physical micrometers instead of the raw
 * wire integer. */
static int64_t vel_to_um_per_tick(int16_t v)
{
    return ((int64_t)v * XR_PACKET_V_MAX_PHYSICAL_DEFAULT_UM_PER_TICK) / INT16_MAX;
}

static void zt_on_error_retry(FDBFuture *future, void *arg);
static void zt_on_range_read(FDBFuture *future, void *arg);
static void zt_on_commit(FDBFuture *future, void *arg);

static void zt_finish(zonetick_ctx_t *ctx, bool ok)
{
    if (ctx->tr != NULL) {
        fdb_transaction_destroy(ctx->tr);
    }
    if (ctx->done_cb != NULL) {
        ctx->done_cb(ctx->user_ctx, ok);
    }
    free(ctx);
}

static void zt_start_range_read(zonetick_ctx_t *ctx)
{
    uint8_t begin[64], end[64];
    size_t begin_len = zf_kv_entity_range_begin(begin, ctx->z_id);
    size_t end_len = zf_kv_entity_range_end(end, ctx->z_id);

    if (fdb_async_get_range(ctx->fdb_state, ctx->tr,
            begin, (int)begin_len, end, (int)end_len,
            zt_on_range_read, ctx) != 0) {
        zt_finish(ctx, false);
    }
}

static void zt_on_error_retry(FDBFuture *future, void *arg)
{
    zonetick_ctx_t *ctx = (zonetick_ctx_t *)arg;
    (void)future;
    /* fdb_handle_error already reset the transaction on a retryable
     * error (matches tpcc_new_order.c's on_error_retry usage); start
     * this zone's tick over from the range read. */
    zt_start_range_read(ctx);
}

static void zt_on_range_read(FDBFuture *future, void *arg)
{
    zonetick_ctx_t *ctx = (zonetick_ctx_t *)arg;

    fdb_error_t err = fdb_future_get_error(future);
    if (err) {
        fdb_handle_error(ctx->fdb_state, ctx->tr, err, zt_on_error_retry, ctx);
        return;
    }

    FDBKeyValue const *kvs;
    int count;
    fdb_bool_t more;
    err = fdb_future_get_keyvalue_array(future, &kvs, &count, &more);
    if (err) {
        zt_finish(ctx, false);
        return;
    }

    /* RFD 0002 caps zones at 200 entities and reads the whole range in
     * one FDB_STREAMING_MODE_WANT_ALL call (see fdb_async_get_range's
     * implementation in fdb_database.c) -- `more` is not expected to be
     * true at this scale. If it ever is, this tick still applies what it
     * got and commits; the next tick picks up the rest on its own range
     * read rather than paginating mid-tick. */
    for (int i = 0; i < count; i++) {
        zf_entity_val_t ev;
        zf_kv_decode_entity(kvs[i].value, kvs[i].value_length, &ev);

        int64_t dx = vel_to_um_per_tick(ev.vel_x) * ctx->tick_count;
        int64_t dy = vel_to_um_per_tick(ev.vel_y) * ctx->tick_count;
        int64_t dz = vel_to_um_per_tick(ev.vel_z) * ctx->tick_count;
        ev.pos_um_x += dx;
        ev.pos_um_y += dy;
        ev.pos_um_z += dz;

        uint8_t val_buf[ZF_ENTITY_VAL_SIZE];
        zf_kv_encode_entity(val_buf, &ev);
        fdb_sync_set(ctx->tr, kvs[i].key, kvs[i].key_length, val_buf, ZF_ENTITY_VAL_SIZE);
    }

    fdb_async_commit(ctx->fdb_state, ctx->tr, zt_on_commit, ctx);
}

static void zt_on_commit(FDBFuture *future, void *arg)
{
    zonetick_ctx_t *ctx = (zonetick_ctx_t *)arg;
    fdb_error_t err = fdb_future_get_error(future);
    if (err) {
        fdb_handle_error(ctx->fdb_state, ctx->tr, err, zt_on_error_retry, ctx);
        return;
    }
    zt_finish(ctx, true);
}

int zf_zonetick_run(fdb_thread_state_t *fdb_state, uint32_t z_id, uint32_t tick_count,
                     zf_zonetick_done_cb done_cb, void *ctx_arg)
{
    zonetick_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return -1;
    }
    ctx->fdb_state = fdb_state;
    ctx->z_id = z_id;
    ctx->tick_count = tick_count;
    ctx->done_cb = done_cb;
    ctx->user_ctx = ctx_arg;

    if (fdb_create_transaction(fdb_state, &ctx->tr) != 0) {
        free(ctx);
        return -1;
    }

    zt_start_range_read(ctx);
    return 0;
}
