#ifndef FDB_DATABASE_H_
#define FDB_DATABASE_H_

#define FDB_API_VERSION 730

#include <h2o.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <foundationdb/fdb_c.h>

#include "list.h"

/*
 * Async FDB database adapter, ported from h2o-bench-tpcc.
 *
 * Replaces the libpq connection pool (database.c) with FDB's C API.
 * FDB futures are integrated into h2o's event loop via
 * fdb_future_set_callback + h2o_timer_t for timeout handling.
 *
 * Each worker thread gets its own FDBDatabase* handle (thread-safe
 * after fdb_setup_network). Transactions are created per-request.
 *
 * FDB API version: 730 (FDB 7.3.x)
 */

#define FDB_CLUSTER_FILE_DEFAULT "/etc/foundationdb/fdb.cluster"
#define FDB_DB_ERROR "FDB error"
#define FDB_DB_TIMEOUT_ERROR "FDB transaction timeout"

typedef enum { FDB_DONE = 1 } fdb_result_return_t;

/* A single FDB operation submitted to the adapter. */
typedef struct fdb_op_param {
    /* The transaction this operation belongs to. */
    FDBTransaction *tr;

    /* Callbacks */
    void (*on_result)(struct fdb_op_param *param, FDBFuture *future);
    void (*on_error)(struct fdb_op_param *param, fdb_error_t err);
    void (*on_timeout)(struct fdb_op_param *param);

    /* User data */
    void *ctx;

    /* For callback chaining */
    list_t l;
} fdb_op_param_t;

/* Per-thread FDB state. */
typedef struct {
    FDBDatabase *db;
    h2o_loop_t *loop;
    h2o_timer_t timer;
    size_t active_transactions;
    size_t max_transactions;
} fdb_thread_state_t;

/* Global FDB state. */
typedef struct {
    const char *cluster_file;
    fdb_thread_state_t *thread_states;
    size_t num_threads;
    bool network_started;
} fdb_global_t;

/* Initialize the global FDB network (call once at startup). */
int fdb_global_init(fdb_global_t *fdb, const char *cluster_file, size_t num_threads);

/* Initialize per-thread FDB state. */
int fdb_thread_init(fdb_global_t *fdb, h2o_loop_t *loop, fdb_thread_state_t *state);

/* Create a transaction on the given thread's FDB database. */
int fdb_create_transaction(fdb_thread_state_t *state, FDBTransaction **tr);

/*
 * CONTRACT FOR EVERY `cb` PASSED TO THE FOUR ASYNC FUNCTIONS BELOW
 * (fdb_async_get, fdb_async_get_range, fdb_async_commit,
 * fdb_handle_error) -- read this before writing a new callback:
 *
 *   The callback MUST NOT call fdb_future_destroy() on the FDBFuture it
 *   receives. It does not own that future. fdb_database.c's own
 *   future_callback() wrapper always destroys it exactly once, on every
 *   path, immediately after the callback returns.
 *
 * A callback that destroys the future itself causes a double-free that
 * segfaults inside libfdb_c.so, usually not at the call site -- this is
 * not theoretical, it shipped once (the MUD prototype's own
 * on_mud_turn_write_commit, back when that code lived in this repo's
 * src/mud/mud_http.c; see zone-guest-middleham's history for it now)
 * and crashed the server on every MUD command.
 *
 * The callback still owns everything else it allocated: destroy the
 * transaction, free your own context, send your own response.
 *
 * Correct reference example: zf_zonetick.c's zt_on_commit(), which reads
 * fdb_future_get_error(future) and then never touches `future` again.
 *
 * Values read out of the future (e.g. the FDBKeyValue array from
 * fdb_future_get_keyvalue_array) point into the future's own buffer and
 * stay valid for the duration of the callback only -- copy anything that
 * must outlive it before returning.
 */

/* Execute a get operation asynchronously. */
int fdb_async_get(fdb_thread_state_t *state, FDBTransaction *tr,
                  const uint8_t *key, int key_len,
                  void (*cb)(FDBFuture *, void *), void *ctx);

/* Execute a set operation (synchronous within the transaction). */
void fdb_sync_set(FDBTransaction *tr,
                  const uint8_t *key, int key_len,
                  const uint8_t *value, int value_len);

/* Execute a range get operation asynchronously. */
int fdb_async_get_range(fdb_thread_state_t *state, FDBTransaction *tr,
                        const uint8_t *begin, int begin_len,
                        const uint8_t *end, int end_len,
                        void (*cb)(FDBFuture *, void *), void *ctx);

/* Commit a transaction asynchronously. */
int fdb_async_commit(fdb_thread_state_t *state, FDBTransaction *tr,
                     void (*cb)(FDBFuture *, void *), void *ctx);

/* Handle a transaction error (retry or fail). */
int fdb_handle_error(fdb_thread_state_t *state, FDBTransaction *tr,
                     fdb_error_t err,
                     void (*cb)(FDBFuture *, void *), void *ctx);

/* Cleanup. */
void fdb_thread_cleanup(fdb_thread_state_t *state);
void fdb_global_cleanup(fdb_global_t *fdb);

#endif
