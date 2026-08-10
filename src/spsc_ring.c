/*
 * Lock-free SPSC ring buffer implementation.
 *
 * SPDX-License-Identifier: MIT
 *
 * Memory model notes:
 *
 * The producer pushes by writing the slot at head, then releasing head.
 * The consumer pops by acquiring head (seeing the slot data), reading the slot,
 * then releasing tail to signal the slot is free.
 *
 * tail chases head:  tail <= head <= tail + capacity
 * The buffer is full when head - tail == capacity.
 * The buffer is empty when head == tail.
 *
 * This is the classic DPDK/Linux kernel SPSC pattern -- no CAS needed because
 * only one thread writes head and only one thread writes tail.
 */

#include "spsc_ring.h"

#include <assert.h>

void spsc_ring_init(spsc_ring_t *ring, void **slots, size_t capacity)
{
    /* Capacity must be a power of two for mask-based indexing */
    assert(capacity > 0 && (capacity & (capacity - 1)) == 0);
    ring->slots = slots;
    ring->mask = capacity - 1;
    atomic_store_explicit(&ring->head, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->tail, 0, memory_order_relaxed);
}

bool spsc_ring_push(spsc_ring_t *ring, void *item)
{
    size_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

    if (head - tail >= ring->mask + 1)
        return false; /* full */

    ring->slots[head & ring->mask] = item;

    /* Release: make the slot write visible before head advances */
    atomic_store_explicit(&ring->head, head + 1, memory_order_release);
    return true;
}

void *spsc_ring_pop(spsc_ring_t *ring)
{
    size_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&ring->head, memory_order_acquire);

    if (tail == head)
        return NULL; /* empty */

    void *item = ring->slots[tail & ring->mask];

    /* Release: make the slot free before tail advances */
    atomic_store_explicit(&ring->tail, tail + 1, memory_order_release);
    return item;
}
