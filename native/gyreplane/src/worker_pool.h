#ifndef WORKER_POOL_H_
#define WORKER_POOL_H_

#include <h2o.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "spsc_ring.h"

/*
 * Bespoke H2O worker pool ("actor-lite").
 *
 * The H2O network thread receives HTTP requests and dispatches them to
 * worker threads via per-worker SPSC ring buffers. Each worker thread
 * owns one libpq connection and processes requests from its ring buffer.
 * Results are sent back to the H2O thread via h2o_multithread_send().
 *
 * Architecture:
 *
 *   H2O network thread (event loop)
 *     |  spsc_ring_push(req, worker[i] ring)
 *     v
 *   Worker thread i (tight loop)
 *     |  spsc_ring_pop -> execute libpq -> h2o_multithread_send
 *     v
 *   H2O network thread
 *     |  h2o_multithread_register_queue -> dequeue response -> send HTTP
 *
 * This avoids all mutexes on the hot path. Each worker has its own
 * SPSC ring (single producer = H2O thread, single consumer = worker).
 */

#define SPSC_CAPACITY 1024

/* A work item dispatched from H2O to a worker. */
typedef struct {
    h2o_req_t *req;
    void (*handler)(h2o_req_t *req, void *worker_data);
    void *worker_data;
} work_item_t;

/* Per-worker state. */
typedef struct {
    spsc_ring_t ring;
    void *slots[SPSC_CAPACITY];
    pthread_t thread;
    h2o_multithread_receiver_t receiver;
    h2o_multithread_queue_t *return_queue;
    _Atomic bool running;
} worker_t;

/* The worker pool. */
typedef struct {
    worker_t *workers;
    size_t num_workers;
    _Atomic size_t next_worker;  /* round-robin counter */
    h2o_multithread_queue_t **return_queues;
} worker_pool_t;

void worker_pool_init(worker_pool_t *pool, size_t num_workers,
                      h2o_loop_t *loop);
void worker_pool_destroy(worker_pool_t *pool);
bool worker_pool_dispatch(worker_pool_t *pool, h2o_req_t *req,
                          void (*handler)(h2o_req_t *req, void *worker_data),
                          void *worker_data);
void worker_pool_process_returns(worker_pool_t *pool);

#endif
