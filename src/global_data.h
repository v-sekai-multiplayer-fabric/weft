#ifndef GLOBAL_DATA_H_
#define GLOBAL_DATA_H_

#include <h2o.h>
#include <stdbool.h>
#include <stdint.h>

#include "fdb_database.h"
#include "list.h"

#define DEFAULT_PORT 8080
#define MAX_QUERIES 500

typedef struct {
    const char *fdb_cluster_file;
    size_t warehouses;
    size_t worker_count;
    size_t duration_ms;
    h2o_globalconf_t h2o_config;
} config_t;

typedef struct {
    fdb_thread_state_t fdb_state;
} request_handler_thread_data_t;

typedef struct {
    list_t *prepared_statements;
} request_handler_data_t;

typedef struct {
    config_t *config;
    request_handler_data_t request_handler_data;
} global_thread_data_t;

typedef struct {
    h2o_context_t h2o_ctx;
    h2o_loop_t *loop;
    list_t local_messages;
    global_thread_data_t *global_thread_data;
    request_handler_thread_data_t request_handler_data;
    unsigned int random_seed;
} thread_context_t;

#endif
