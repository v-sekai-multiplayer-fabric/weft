/*
 * zone-server-h2o main entry point.
 *
 * Status: basic-ZoneTick spike (plan step 0 / task #11). Boots the h2o
 * event-loop + worker-pool + FDB scaffold inherited from
 * weftspun/h2o-bench-tpcc, and drives a bare `position += velocity * dt`
 * ZoneTick.
 *
 * weft note: this process is a plane, and a plane has no networking. The
 * QUIC/UDP listener that thread 0 used to bind moved to
 * native/gyreedge/transport, which is the edge. h2o stays, for its event
 * loop only -- this process serves no HTTP and opens no socket. Nothing
 * feeds the ZoneTick until iceoryx carries the decoded input from the
 * edge. See ../../docs/reference/gyre_plane.md.
 *
 * Usage:
 *   zone-server-h2o -a<thread_count> -c<cluster_file> -z<zone_id>
 */

#include <h2o.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "error.h"
#include "global_data.h"
#include "sandbox/sandbox_guest.h"

typedef struct {
    h2o_context_t h2o_ctx;
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
            "      processes, each one zone (1 process : 1 zone), matching\n"
            "      zone-server/AGENTS.md's one-UDP-port-per-zone-instance\n"
            "      deployment shape. Required.\n"
            "  -g  Script-guest ELF path: boot this riscv64 guest in the\n"
            "      libriscv sandbox on a dedicated thread (rfd/0092, 0094).\n"
            "      The guest gets the zone_abi.h ecalls and nothing else:\n"
            "      no filesystem, no sockets, no event-loop access. Its\n"
            "      content ships inside the ELF; its state lives in FDB.\n"
            "      Engine-class guests run under bubblewrap instead, not\n"
            "      here -- see rfd/0095. Optional.\n",
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
    const char *guest_elf = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "a:c:z:g:h")) != -1) {
        switch (opt) {
        case 'a': config.worker_count = (size_t)atoi(optarg); break;
        case 'c': config.fdb_cluster_file = optarg; break;
        case 'z': z_id = atol(optarg); have_z_id = true; break;
        case 'g': guest_elf = optarg; break;
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

    h2o_config_init(&config.h2o_config);

    /* h2o_context_init() asserts config->hosts[0] != NULL unconditionally,
     * even though this process serves no HTTP requests through h2o's own
     * routing -- and now serves nothing at all, since the transport moved
     * to the edge. h2o's context object is still coupled to its
     * config for virtual-host lookups internally, so at least one host
     * must exist regardless. This is h2o's own documented minimal-usage
     * pattern (see h2o's examples/simple.c), not a guess: confirmed by
     * running the actual built binary for the first time (task #20) and
     * hitting this exact assertion, "h2o_context_init: Assertion
     * `config->hosts[0] != NULL' failed" -- every previous check of this
     * repo was a build/link/unit-test pass, never an actual process
     * start, so this had never been exercised before. No path/handler is
     * registered under it since nothing serves HTTP through this host. */
    h2o_config_register_host(&config.h2o_config, h2o_iovec_init(H2O_STRLIT("default")), 65535);

    thread_ctx_t *threads = calloc(config.worker_count, sizeof(*threads));

    for (size_t i = 0; i < config.worker_count; i++) {
        thread_ctx_t *t = &threads[i];
        t->config = &config;
        t->running = true;
        t->z_id = (uint32_t)z_id;
        t->loop = h2o_evloop_create();

        h2o_context_init(&t->h2o_ctx, t->loop, &config.h2o_config);
        fdb_thread_init(&fdb_global, t->loop, &t->fdb_state);

        pthread_create(&t->tid, NULL, worker_main, t);
    }

    fprintf(stderr, "zone-server-h2o: zone %u, %zu worker(s), no transport (a plane has no networking)\n",
            (uint32_t)z_id, config.worker_count);

    /* Guest boot happens after the worker threads exist but the guest
     * thread itself is fully independent of them: it blocks on FDB
     * futures directly (zf_guestfs), which requires the FDB network to
     * be live -- fdb_run_network() below runs it on this main thread,
     * and fdb_global_init already called fdb_setup_network. The
     * ordering is safe because fdb futures created before the network
     * runs simply wait for it. */
    if (guest_elf != NULL) {
        sandbox_guest_config_t guest_cfg = {
            .elf_path = guest_elf,
            .cluster_file = config.fdb_cluster_file,
            .z_id = (uint32_t)z_id,
            .memory_max = 0,       /* defaults, see sandbox_guest.h */
            .max_instructions = 0,
        };
        if (sandbox_guest_start(&guest_cfg) != 0) {
            fprintf(stderr, "zone-server-h2o: guest thread failed to start\n");
        }
    }

    fdb_run_network();

    for (size_t i = 0; i < config.worker_count; i++) {
        threads[i].running = false;
        pthread_join(threads[i].tid, NULL);
        h2o_context_dispose(&threads[i].h2o_ctx);
        fdb_thread_cleanup(&threads[i].fdb_state);
    }

    free(threads);
    /* Not thread.h's cleanup_fdb_global() -- that function operates on
     * thread.c's own separate static fdb_global_t, not this file's own
     * fdb_global above (confirmed by reading thread.c directly: it
     * calls fdb_global_cleanup(&fdb_global) against ITS OWN static of
     * that name, never touching this one). Calling fdb_global_cleanup()
     * directly on our own variable instead -- found while writing
     * tools/zonetick_throughput.c for task #21 and hitting the same
     * mismatch there. */
    fdb_global_cleanup(&fdb_global);

    return 0;
}
