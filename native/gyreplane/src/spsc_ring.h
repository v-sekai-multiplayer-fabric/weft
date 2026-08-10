#ifndef SPSC_RING_H_
#define SPSC_RING_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Lock-free single-producer single-consumer ring buffer.
 *
 * One producer thread enqueues, one consumer thread dequeues.
 * No locks, no CAS — just atomic load/store with acquire/release fences.
 *
 * Capacity must be a power of two (mask = capacity - 1).
 *
 * Memory ordering:
 *   Producer: release on head (makes enqueued data visible)
 *   Consumer: acquire on head (sees enqueued data before dequeuing)
 *
 * Invariant: head - tail <= capacity (never overwrites unread data)
 * Invariant: tail <= head (never reads uninitialized slots)
 *
 * See test/cbmc/spsc_harness.c and test/verification/ZoneVerification/Spsc.lean
 * for formal verification of these invariants.
 */

typedef struct {
    void **slots;
    size_t mask;              /* capacity - 1 (capacity is power of two) */
    _Atomic size_t head;      /* producer writes, consumer reads */
    _Atomic size_t tail;      /* consumer writes, producer reads */
} spsc_ring_t;

/* Initialize a ring buffer with a power-of-two capacity. Caller owns slots[]. */
void spsc_ring_init(spsc_ring_t *ring, void **slots, size_t capacity);

/* Returns true if enqueue succeeded, false if full. Producer-only. */
bool spsc_ring_push(spsc_ring_t *ring, void *item);

/* Returns item or NULL if empty. Consumer-only. */
void *spsc_ring_pop(spsc_ring_t *ring);

/* Returns the number of items currently in the ring. */
static inline size_t spsc_ring_size(spsc_ring_t *ring)
{
    size_t head = atomic_load_explicit(&ring->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);
    return head - tail;
}

/* Returns true if the ring is empty. */
static inline bool spsc_ring_empty(spsc_ring_t *ring)
{
    return spsc_ring_size(ring) == 0;
}

#endif
