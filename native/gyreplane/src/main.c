/*
 * The zone plane entry point.
 *
 * Status: basic-ZoneTick spike. It boots the FoundationDB scaffold inherited from
 * weftspun/h2o-bench-tpcc and drives a bare `position += velocity * dt` ZoneTick.
 *
 * weft note: this process is a plane, and a plane has no networking. The transport it
 * used to bind moved to native/edge, which splits it in two: an ingest edge for player
 * input datagrams and a gateway edge for control streams.
 *
 * h2o is gone with it. It survived one change longer than the transport did, for its
 * event loop, and that loop drove nothing: fdb_thread_init stored it and no code ever
 * read it back. A worker thread ran h2o_evloop_run on a loop with no handle registered.
 * So this build links no h2o, and the worker threads wait rather than poll.
 *
 * Nothing feeds the ZoneTick until iceoryx2 carries the decoded input from an edge. That
 * is the next thing to build, and ../harness/README.md holds the bus it will use.
 *
 * Usage:
 *   zone-server-h2o -a<thread_count> -c<cluster_file> -z<zone_id>
 */

#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "fdb_database.h"
#include "global_data.h"

typedef struct {
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

    /* Nothing to poll. The transport is at the edge, and the bus that will replace it is
     * not wired, so this thread has no source of work. It waits until main clears
     * `running` rather than spinning on an empty event loop, which is what it did before
     * h2o came out. A busy wait here would burn a core to do nothing. */
    while (tctx->running) {
        struct timespec idle = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&idle, NULL);
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

        fdb_thread_init(&fdb_global, &t->fdb_state);

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
