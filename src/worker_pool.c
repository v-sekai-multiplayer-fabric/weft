/*
 * Bespoke H2O worker pool implementation.
 *
 * SPDX-License-Identifier: MIT
 *
 * Each worker thread runs a tight loop:
 *   1. Pop work_item from SPSC ring
 *   2. Execute the handler (libpq async queries)
 *   3. Push result back to H2O via h2o_multithread_send
 *   4. Repeat
 *
 * The H2O network thread dispatches via round-robin across worker SPSC rings.
 * No mutexes on the hot path -- only atomic load/store in the SPSC ring.
 */

#include "worker_pool.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>

#include "error.h"

/* Message sent back from worker to H2O thread. */
typedef struct {
    h2o_multithread_message_t super;
    h2o_req_t *req;
    int status_code;
    const char *body;
    size_t body_len;
} return_message_t;

static void on_return_message(h2o_multithread_receiver_t *receiver,
                              h2o_linklist_t *messages)
{
    while (!h2o_linklist_is_empty(messages)) {
        h2o_linklist_t *node = messages->next;
        h2o_linklist_unlink(node);
        return_message_t *msg = (return_message_t *)node;
        if (msg->status_code == 200)
            h2o_send_inline(msg->req, msg->body, (size_t)msg->body_len);
        else
            h2o_send_error_generic(msg->req, msg->status_code,
                          "Error", msg->body, 0);
        free(msg);
    }
}

static void *worker_thread(void *arg)
{
    worker_t *w = (worker_t *)arg;

    while (atomic_load_explicit(&w->running, memory_order_acquire)) {
        work_item_t *item = (work_item_t *)spsc_ring_pop(&w->ring);
        if (item) {
            /* Execute the handler -- this is where libpq work happens */
            item->handler(item->req, item->worker_data);
            free(item);
        } else {
            /* Ring empty -- yield to avoid busy-spin */
            /* In production: use futex or eventfd for blocking wake-up */
            sched_yield();
        }
    }

    return NULL;
}

void worker_pool_init(worker_pool_t *pool, size_t num_workers,
                      h2o_loop_t *loop)
{
    assert(num_workers > 0);
    pool->workers = calloc(num_workers, sizeof(worker_t));
    pool->num_workers = num_workers;
    atomic_store(&pool->next_worker, 0);

    for (size_t i = 0; i < num_workers; i++) {
        spsc_ring_init(&pool->workers[i].ring,
                       pool->workers[i].slots, SPSC_CAPACITY);
        atomic_store(&pool->workers[i].running, true);
        pool->workers[i].return_queue = h2o_multithread_create_queue(loop);

        h2o_multithread_register_receiver(pool->workers[i].return_queue,
                                          &pool->workers[i].receiver,
                                          on_return_message);

        int rc = pthread_create(&pool->workers[i].thread, NULL,
                               worker_thread, &pool->workers[i]);
        if (rc != 0) {
            LIBRARY_ERROR("pthread_create", "Failed to create worker thread");
            abort();
        }
    }
}

void worker_pool_destroy(worker_pool_t *pool)
{
    for (size_t i = 0; i < pool->num_workers; i++) {
        atomic_store_explicit(&pool->workers[i].running, false,
                             memory_order_release);
        pthread_join(pool->workers[i].thread, NULL);
        h2o_multithread_unregister_receiver(pool->workers[i].return_queue,
                                           &pool->workers[i].receiver);
        h2o_multithread_destroy_queue(pool->workers[i].return_queue);
    }
    free(pool->workers);
}

bool worker_pool_dispatch(worker_pool_t *pool, h2o_req_t *req,
                          void (*handler)(h2o_req_t *req, void *worker_data),
                          void *worker_data)
{
    /* Round-robin worker selection */
    size_t idx = atomic_fetch_add_explicit(&pool->next_worker, 1,
                                           memory_order_relaxed) % pool->num_workers;
    worker_t *w = &pool->workers[idx];

    work_item_t *item = malloc(sizeof(work_item_t));
    if (!item) {
        STANDARD_ERROR("malloc");
        return false;
    }

    item->req = req;
    item->handler = handler;
    item->worker_data = worker_data;

    if (!spsc_ring_push(&w->ring, item)) {
        /* Ring full -- back-pressure */
        free(item);
        return false;
    }

    return true;
}

void worker_pool_process_returns(worker_pool_t *pool)
{
    /* Process return messages from all workers */
    for (size_t i = 0; i < pool->num_workers; i++)
        h2o_multithread_send_message(&pool->workers[i].receiver, NULL);
}
