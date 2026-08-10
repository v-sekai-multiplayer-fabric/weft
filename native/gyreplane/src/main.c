/*
 * zone-server-h2o main entry point.
 *
 * Status: basic-ZoneTick spike (plan step 0 / task #11). Boots an event
 * loop and the FDB scaffold inherited from weftspun/h2o-bench-tpcc, and
 * drives a bare `position += velocity * dt` ZoneTick.
 *
 * weft note: this process is a plane, and a plane has no networking. The
 * QUIC/UDP listener that thread 0 used to bind moved to
 * native/edge/transport, which is the edge. Nothing feeds the
 * ZoneTick until iceoryx carries the decoded input from the edge.
 *
 * h2o is down to one job here: h2o_evloop_create() and h2o_evloop_run(),
 * so that fdb_future_set_callback has a loop to fire on and h2o_timer_t
 * has one to time out against. h2o's config, its per-thread context, and
 * its host table are gone -- they exist to route HTTP requests, and this
 * process routes none. Removing h2o_context_init() also removed the
 * dummy "default" host that only existed to satisfy its assertion.
 *
 * That last event-loop job is what the thread-per-core harness over
 * iceoryx takes over. When it does, h2o leaves this build entirely. See
 * WEFT.md.
 *
 * The libriscv guest sandbox went too. A plane runs one runtime model,
 * and a second sandbox inside it is not that model.
 *
 * Usage:
 *   zone-server-h2o -a<thread_count> -c<cluster_file> -z<zone_id>
 */

#include <h2o.h>

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "fdb_database.h"
#include "global_data.h"

typedef struct {
    h2o_loop_t *loop;
    fdb_thread_state_t fdb_state;
    pthread_t tid;
    config_t *config;
    bool running;
    uint32_t z_id;
} thread_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -a<thread_count> -c<cluster_file> -z<zone_id>\n"
            "\n"
            "  -a  Number of worker threads\n"
            "  -c  FoundationDB cluster file path\n"
            "  -z  This process's zone ID -- a zone fabric is multiple\n"
            "      processes, each one zone (1 process : 1 zone). The\n"
            "      one-UDP-port-per-zone-instance half of that deployment\n"
            "      shape is the edge's job now. Required.\n",
            prog);
}

static void *worker_main(void *arg)
{
    thread_ctx_t *tctx = (thread_ctx_t *)arg;

    while (tctx->running) {
        h2o_evloop_run(tctx->loop, INT32_MAX);
    }

    return NULL;
}

static fdb_global_t fdb_global;

int main(int argc, char *argv[])
{
    config_t config = {0};
    config.fdb_cluster_file = "/etc/foundationdb/fdb.cluster";
    config.worker_count = 1;
    long z_id = -1;
    bool have_z_id = false;

    int opt;
    while ((opt = getopt(argc, argv, "a:c:z:h")) != -1) {
        switch (opt) {
        case 'a': config.worker_count = (size_t)atoi(optarg); break;
        case 'c': config.fdb_cluster_file = optarg; break;
        case 'z': z_id = atol(optarg); have_z_id = true; break;
        case 'h':
        default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }

    if (!have_z_id || z_id < 0) {
        fprintf(stderr, "zone-server-h2o: -z<zone_id> is required "
                        "(a zone fabric is multiple processes, each one zone)\n");
        usage(argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    if (fdb_global_init(&fdb_global, config.fdb_cluster_file, config.worker_count)) {
        fprintf(stderr, "Failed to initialize FoundationDB\n");
        return 1;
    }

    thread_ctx_t *threads = calloc(config.worker_count, sizeof(*threads));

    for (size_t i = 0; i < config.worker_count; i++) {
        thread_ctx_t *t = &threads[i];
        t->config = &config;
        t->running = true;
        t->z_id = (uint32_t)z_id;
        t->loop = h2o_evloop_create();

        fdb_thread_init(&fdb_global, t->loop, &t->fdb_state);

        pthread_create(&t->tid, NULL, worker_main, t);
    }

    fprintf(stderr, "zone-server-h2o: zone %u, %zu worker(s), no transport (a plane has no networking)\n",
            (uint32_t)z_id, config.worker_count);

    fdb_run_network();

    for (size_t i = 0; i < config.worker_count; i++) {
        threads[i].running = false;
        pthread_join(threads[i].tid, NULL);
        fdb_thread_cleanup(&threads[i].fdb_state);
    }

    free(threads);
    /* Not a cleanup_fdb_global() helper -- src/thread.c held one that
     * operated on its own separate static fdb_global_t, not this file's
     * own fdb_global above. That file is deleted now, but the trap it
     * set is worth remembering: call fdb_global_cleanup() on your own
     * variable. Found while writing tools/zonetick_throughput.c for
     * task #21 and hitting the same mismatch there. */
    fdb_global_cleanup(&fdb_global);

    return 0;
}
