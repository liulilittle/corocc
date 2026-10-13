/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/scheduler/wsdeque.c -- Chase-Lev work-stealing deque.
 *
 * The classic Chase-Lev deque (PLDI 2005, "Dynamic Circular Work-Stealing
 * Deque") shaped for a fixed ring. The owner thread operates the bottom end
 * with no synchronization: pushes and pops touch only the ring and the bottom
 * index. A thief operates the top end with one compare-and-swap, and the
 * single-owner case is handled by the owner participating in the CAS when the
 * deque has exactly one node, exactly as the original paper prescribes.
 *
 * Memory order: the owner's cell write is ordered before its bottom store by
 * coro_at_store (release); a thief that loads top, then loads bottom (acquire),
 * then loads the cell therefore sees the node the owner published. The owner's
 * pop is ordered against the thief's top CAS by the owner first reloading top
 * with an acquire load before touching the cell. This is the order the paper
 * proves correct for the WMM model.
 *
 * The ring slots are never cleared on take. Chase-Lev's correctness relies on
 * the top/bottom pair alone: a slot that was consumed is simply out of range
 * and a later push overwrites it. Clearing it would race with a concurrent
 * thief that already read the top index but not yet the slot, making the thief
 * observe NULL and drop a node that was actually still in the deque.
 */
#include "scheduler/wsdeque.h"

#include "scheduler/rt.h"

#include <string.h>

void wsdeque_init(wsdeque* d) {
    d->top = 0;
    d->bottom = 0;
    (void)memset(d->cells, 0, sizeof d->cells);
}

void wsdeque_push(wsdeque* d, rt_node* n) {
    uint64_t b = d->bottom;
    d->cells[b & WSDEQUE_MASK] = n;
    /* Publish the node before advancing bottom: a thief that sees the new
       bottom must be able to read the cell it points at. */
    coro_at_store((volatile uint64_t*)&d->bottom, b + 1);
}

rt_node* wsdeque_pop(wsdeque* d) {
    if (d->bottom == 0) return NULL;
    uint64_t old_bottom = d->bottom;
    uint64_t b = d->bottom - 1;
    coro_at_store((volatile uint64_t*)&d->bottom, b); /* reserve the slot */
    /* Order the bottom store before the top load (paper's take barrier): a
       thief that sees the reserved bottom must not race us for the same
       single slot without our knowing. */
    coro_at_fence();
    uint64_t t = coro_at_load((volatile uint64_t*)&d->top);
    if (t <= b) {
        /* At least one node on our end: take it. */
        rt_node* n = d->cells[b & WSDEQUE_MASK];
        if (t == b) {
            /* Exactly one node left: race the thieves for it. */
            if (!coro_at_cas((volatile uint64_t*)&d->top, &t, t + 1)) {
                /* A thief took it. The slot is now consumed by the thief; the
                   deque is empty with top == bottom (both t+1 after the CAS
                   updated t), so report a lost race. */
                coro_at_store((volatile uint64_t*)&d->bottom, t);
                return NULL;
            }
            /* We won the single-element race and advanced top to t+1. The
               bottom must follow, or the deque is left with top == bottom+1,
               a hole that makes the next push overwrite the consumed slot. */
            coro_at_store((volatile uint64_t*)&d->bottom, t + 1);
        } else {
            d->bottom = b; /* keep the reservation: we consumed the slot */
        }
        return n;
    }
    /* Nothing on our end: restore the reservation and report empty. */
    coro_at_store((volatile uint64_t*)&d->bottom, old_bottom);
    return NULL;
}

rt_node* wsdeque_steal(wsdeque* d) {
    uint64_t t = coro_at_load((volatile uint64_t*)&d->top);
    /* Order the top load before the bottom load (paper's steal barrier): if
       the owner empties the deque after we read top, we must observe the
       new bottom and back off instead of stealing a stale slot. */
    coro_at_fence();
    /* The owner's store-release on bottom orders the cell write before it, so
       acquiring the bottom makes the cell read below observe the node. */
    uint64_t b = coro_at_load((volatile uint64_t*)&d->bottom);
    if (t >= b) return NULL; /* nothing published yet */
    rt_node* n = d->cells[t & WSDEQUE_MASK];
    uint64_t cur = t;
    if (!coro_at_cas((volatile uint64_t*)&d->top, &cur, t + 1)) {
        return NULL; /* someone else took it; the race is lost */
    }
    return n;
}

size_t wsdeque_size(const wsdeque* d) {
    uint64_t b = coro_at_load((volatile uint64_t*)&d->bottom);
    uint64_t t = coro_at_load((volatile uint64_t*)&d->top);
    return b >= t ? (size_t)(b - t) : 0;
}

int wsdeque_empty(const wsdeque* d) {
    return wsdeque_size(d) == 0;
}