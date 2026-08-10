#ifndef GLOBAL_DATA_H_
#define GLOBAL_DATA_H_

/*
 * What one zone-server process was told on the command line.
 *
 * weft note: this held h2o_globalconf_t, an h2o_context_t per thread, and
 * the TPC-C fields this repo inherited from h2o-bench-tpcc (warehouses,
 * MAX_QUERIES, prepared statements, request_handler_data). None of them
 * had a reader once the transport moved to the edge -- h2o's config and
 * context exist to route HTTP requests, and this process routes none.
 * The one h2o type that is still load-bearing is h2o_loop_t, and it lives
 * in fdb_database.h where the FDB futures that need it live.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *fdb_cluster_file;
    size_t worker_count;
} config_t;

#endif
