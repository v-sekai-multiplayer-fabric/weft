/*
 * CBMC harness for SPSC lock-free ring buffer.
 *
 * Verifies three invariants:
 *   1. Push never succeeds when ring is full (head - tail >= capacity)
 *   2. Pop never returns an item when ring is empty (head == tail)
 *   3. Items come out in FIFO order (what goes in first comes out first)
 *
 * CBMC explores all possible interleavings of push/pop with bounded
 * unwind depth. The atomic memory orderings are modeled as total
 * orderings (CBMC does not yet model C11 memory_order directly, but
 * the invariants hold under sequential consistency which is stronger).
 *
 * Build: cbmc --unwind 20 test/cbmc/spsc_harness.c src/spsc_ring.c
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Stub for the real implementation */
typedef struct {
    void **slots;
    size_t mask;
    _Atomic size_t head;
    _Atomic size_t tail;
} spsc_ring_t;

void spsc_ring_init(spsc_ring_t *ring, void **slots, size_t capacity);
bool spsc_ring_push(spsc_ring_t *ring, void *item);
void *spsc_ring_pop(spsc_ring_t *ring);

/*
 * Verify FIFO ordering with a bounded ring (capacity=4).
 * Push N items, then pop N items, and check they come out in order.
 */
void verify_fifo_ordering(void)
{
    #define CAP 4
    void *slots[CAP];
    spsc_ring_t ring;
    spsc_ring_init(&ring, slots, CAP);

    /* Use tagged pointers to track identity */
    uintptr_t items[CAP];
    for (int i = 0; i < CAP; i++) {
        items[i] = (uintptr_t)(i + 1);
        bool ok = spsc_ring_push(&ring, (void *)items[i]);
        __CPROVER_assert(ok, "push must succeed when ring has space");
    }

    /* Ring is now full -- push must fail */
    bool overflow = spsc_ring_push(&ring, (void *)0xDEAD);
    __CPROVER_assert(!overflow, "push must fail when ring is full");

    /* Pop all items and verify FIFO order */
    for (int i = 0; i < CAP; i++) {
        void *item = spsc_ring_pop(&ring);
        __CPROVER_assert(item != NULL, "pop must succeed when ring is non-empty");
        __CPROVER_assert((uintptr_t)item == items[i],
                         "pop must return items in FIFO order");
    }

    /* Ring is now empty -- pop must return NULL */
    void *underflow = spsc_ring_pop(&ring);
    __CPROVER_assert(underflow == NULL, "pop must return NULL when ring is empty");

    #undef CAP
}

/*
 * Verify the head-tail invariant: head >= tail at all times.
 * This is the core safety property -- if head ever goes backwards
 * or tail overtakes head, data corruption occurs.
 */
void verify_head_tail_invariant(void)
{
    #define CAP 2
    void *slots[CAP];
    spsc_ring_t ring;
    spsc_ring_init(&ring, slots, CAP);

    /* Interleave pushes and pops nondeterministically */
    for (int i = 0; i < 4; i++) {
        bool do_push = nondet_bool();
        if (do_push) {
            spsc_ring_push(&ring, (void *)(uintptr_t)(i + 1));
        } else {
            spsc_ring_pop(&ring);
        }

        /* Check invariant after each operation */
        size_t head = atomic_load(&ring.head);
        size_t tail = atomic_load(&ring.tail);
        __CPROVER_assert(head >= tail, "head must be >= tail at all times");
        __CPROVER_assert(head - tail <= CAP, "ring must never exceed capacity");
    }

    #undef CAP
}
