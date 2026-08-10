/*
 * Real zone-tick throughput measurement against a live FDB cluster,
 * task #21 (verify throughput before cutover). Not a mock, not a
 * timed sleep loop: seeds real entities via fdb_c directly, then
 * calls the same zf_zonetick_run() src/main.c's own worker threads
 * call, back to back, for real, against the FDB cluster given on the
 * command line.
 *
 * RFD 0002's target: ~500 zone ticks/sec/core with a real FDB commit,
 * <10us per entity update within a tick (that second number is not
 * separately measured here -- it is implied by ticks/sec once entity
 * count per zone is fixed, per RFD 0002's own "1 range read (200 KV
 * pairs) + 1 commit (200 sets) per zone per tick" scope).
 *
 * Usage: zonetick_throughput -c<cluster_file> -z<zone_id> [-n<entities>] [-d<seconds>]
 */

#include <h2o.h>

#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/fdb_database.h"
#include "../src/zf_kv.h"
#include "../src/zf_zonetick.h"

static void *fdb_run_network_thread(void *unused)
{
    (void)unused;
    fdb_error_t err = fdb_run_network();
    if (err) {
        fprintf(stderr, "fdb_run_network: %s\n", fdb_get_error(err));
    }
    return NULL;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* One-shot synchronous seed of `n` entities under zone z_id -- outside
 * the timed loop below, so it does not skew the throughput number.
 * Uses fdb_c directly (fdb_future_block_until_ready), not the async
 * h2o-integrated path zf_zonetick_run() itself uses, since this only
 * runs once before the timing starts. */
static int seed_entities(FDBDatabase *db, uint32_t z_id, uint32_t n)
{
    FDBTransaction *tr = NULL;
    fdb_error_t err = fdb_database_create_transaction(db, &tr);
    if (err) {
        fprintf(stderr, "seed: fdb_database_create_transaction: %s\n", fdb_get_error(err));
        return -1;
    }

    for (uint32_t e_id = 0; e_id < n; e_id++) {
        uint8_t key[32];
        size_t key_len = zf_kv_entity_key(key, z_id, e_id);

        zf_entity_val_t val = {0};
        val.global_id = e_id;
        val.pos_um_x = (int64_t)e_id * 1000;
        val.pos_um_y = 0;
        val.pos_um_z = 0;
        val.vel_x = 100;
        val.vel_y = 0;
        val.vel_z = 0;

        uint8_t value[256];
        zf_kv_encode_entity(value, &val);
        fdb_transaction_set(tr, key, (int)key_len, value, (int)sizeof(zf_entity_val_t));
    }

    FDBFuture *commit_f = fdb_transaction_commit(tr);
    fdb_future_block_until_ready(commit_f);
    err = fdb_future_get_error(commit_f);
    fdb_future_destroy(commit_f);
    fdb_transaction_destroy(tr);

    if (err) {
        fprintf(stderr, "seed: commit failed: %s\n", fdb_get_error(err));
        return -1;
    }
    return 0;
}

/* Reads entity e_id's current stored position, synchronously, without
 * seeding or ticking anything -- for task #21's crash-recovery check:
 * run with -P before restarting zone-server-h2o, run again with -P
 * after, and confirm the two printed positions match (proof the
 * entity moved by earlier ticks, then survived the process restart
 * untouched, since nothing else writes to it in between). */
static int probe_entity(FDBDatabase *db, uint32_t z_id, uint32_t e_id)
{
    FDBTransaction *tr = NULL;
    fdb_error_t err = fdb_database_create_transaction(db, &tr);
    if (err) {
        fprintf(stderr, "probe: fdb_database_create_transaction: %s\n", fdb_get_error(err));
        return -1;
    }

    uint8_t key[32];
    size_t key_len = zf_kv_entity_key(key, z_id, e_id);
    FDBFuture *get_f = fdb_transaction_get(tr, key, (int)key_len, 0);
    fdb_future_block_until_ready(get_f);

    fdb_bool_t present = 0;
    uint8_t const *value = NULL;
    int value_len = 0;
    err = fdb_future_get_value(get_f, &present, &value, &value_len);
    if (err) {
        fprintf(stderr, "probe: get failed: %s\n", fdb_get_error(err));
        fdb_future_destroy(get_f);
        fdb_transaction_destroy(tr);
        return -1;
    }
    if (!present) {
        printf("probe: zone=%u entity=%u not found\n", z_id, e_id);
    } else {
        zf_entity_val_t val;
        zf_kv_decode_entity(value, value_len, &val);
        printf("probe: zone=%u entity=%u pos_um=(%" PRId64 ",%" PRId64 ",%" PRId64 ") hlc=%u\n",
               z_id, e_id, val.pos_um_x, val.pos_um_y, val.pos_um_z, val.hlc);
    }
    fdb_future_destroy(get_f);
    fdb_transaction_destroy(tr);
    return 0;
}

typedef struct {
    uint32_t z_id;
    uint32_t ticks_done;
    uint32_t ticks_failed;
    bool in_flight;
    double deadline;
} tick_loop_ctx_t;

static fdb_thread_state_t *g_fdb_state;

static void on_tick_done(void *ctx, bool ok)
{
    tick_loop_ctx_t *loop_ctx = (tick_loop_ctx_t *)ctx;
    loop_ctx->in_flight = false;
    if (ok) {
        loop_ctx->ticks_done++;
    } else {
        loop_ctx->ticks_failed++;
    }
}

int main(int argc, char *argv[])
{
    const char *cluster_file = FDB_CLUSTER_FILE_DEFAULT;
    long z_id = -1;
    uint32_t n_entities = 200; /* RFD 0002's "200 KV pairs" per-zone scope */
    double duration = 5.0;
    bool probe_only = false;

    int opt;
    while ((opt = getopt(argc, argv, "c:z:n:d:Ph")) != -1) {
        switch (opt) {
        case 'c': cluster_file = optarg; break;
        case 'z': z_id = atol(optarg); break;
        case 'n': n_entities = (uint32_t)atoi(optarg); break;
        case 'd': duration = atof(optarg); break;
        case 'P': probe_only = true; break;
        default:
            fprintf(stderr, "Usage: %s -c<cluster_file> -z<zone_id> [-n<entities>] [-d<seconds>] [-P]\n"
                            "  -P  Probe only: print entity 0's current stored position and\n"
                            "      exit. No seeding, no ticking -- for confirming state survived\n"
                            "      a zone-server-h2o restart (task #21's crash-recovery check).\n", argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }
    if (z_id < 0) {
        fprintf(stderr, "zonetick_throughput: -z<zone_id> is required\n");
        return 1;
    }

    fdb_global_t fdb_global;
    if (fdb_global_init(&fdb_global, cluster_file, 1)) {
        fprintf(stderr, "zonetick_throughput: fdb_global_init failed\n");
        return 1;
    }

    /* fdb_global_init() only calls fdb_setup_network() -- the FDB C
     * client library never actually processes network I/O (futures
     * never resolve, fdb_future_block_until_ready() blocks forever)
     * until fdb_run_network() is also running, on some thread, per
     * FDB's own documented model. main.c calls it on its own main
     * thread, blocking, while its h2o worker threads do the real work
     * concurrently; this tool is single-threaded application logic, so
     * it runs fdb_run_network() on a dedicated background thread
     * instead. Confirmed by hitting exactly this hang (seed_entities()
     * never returning) before adding it, not guessed. */
    pthread_t fdb_net_thread;
    pthread_create(&fdb_net_thread, NULL, fdb_run_network_thread, NULL);

    h2o_loop_t *loop = h2o_evloop_create();
    fdb_thread_state_t fdb_state;
    fdb_thread_init(&fdb_global, loop, &fdb_state);
    g_fdb_state = &fdb_state;

    if (probe_only) {
        int rc = probe_entity(fdb_state.db, (uint32_t)z_id, 0);
        fdb_thread_cleanup(&fdb_state);
        fdb_global_cleanup(&fdb_global);
        pthread_join(fdb_net_thread, NULL);
        return rc;
    }

    fprintf(stderr, "zonetick_throughput: seeding %u entities in zone %u...\n", n_entities, (uint32_t)z_id);
    if (seed_entities(fdb_state.db, (uint32_t)z_id, n_entities) != 0) {
        return 1;
    }

    tick_loop_ctx_t loop_ctx = {.z_id = (uint32_t)z_id};
    double t0 = now_sec();
    loop_ctx.deadline = t0 + duration;

    fprintf(stderr, "zonetick_throughput: running for %.1fs against zone %u...\n", duration, (uint32_t)z_id);

    while (now_sec() < loop_ctx.deadline) {
        if (!loop_ctx.in_flight) {
            loop_ctx.in_flight = true;
            zf_zonetick_run(&fdb_state, loop_ctx.z_id, 1, on_tick_done, &loop_ctx);
        }
        h2o_evloop_run(loop, 10 /* ms */);
    }
    /* Drain the one possibly-in-flight tick so it is not silently dropped
     * from the count. */
    while (loop_ctx.in_flight && now_sec() < loop_ctx.deadline + 2.0) {
        h2o_evloop_run(loop, 10);
    }

    double elapsed = now_sec() - t0;
    double rate = (double)loop_ctx.ticks_done / elapsed;

    printf("zonetick_throughput: zone=%u entities=%u elapsed=%.2fs ticks_ok=%u ticks_failed=%u rate=%.1f ticks/sec\n",
           (uint32_t)z_id, n_entities, elapsed, loop_ctx.ticks_done, loop_ctx.ticks_failed, rate);
    printf("RFD 0002 target: ~500 ticks/sec/core -- this run: %s\n",
           rate >= 500.0 ? "MEETS target" : "below target");

    fdb_thread_cleanup(&fdb_state);
    /* Not main.c's own cleanup_fdb_global() -- that function (thread.c)
     * operates on thread.c's own separate static fdb_global_t, not the
     * one actually initialized via fdb_global_init() above. Calling
     * fdb_global_cleanup() directly on our own variable instead. */
    fdb_global_cleanup(&fdb_global); /* calls fdb_stop_network(), which makes
                                       * fdb_run_network_thread's blocking call return */
    pthread_join(fdb_net_thread, NULL);
    return 0;
}
