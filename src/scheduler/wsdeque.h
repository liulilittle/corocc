#ifndef COROCC_SCHEDULER_WSDEQUE_H
#define COROCC_SCHEDULER_WSDEQUE_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/scheduler/wsdeque.h -- Chase-Lev work-stealing deque (internal).
 *
 * The work-stealing runtime's runnable queue must never cost the hot path
 * anything: a worker that pushes and pops its own coroutines does so with no
 * lock, no atomics and no system call, and another worker steals from one
 * deque with a single compare-and-swap on the public end. The Chase-Lev
 * deque is the shape that gives exactly that: the private end (bottom) is
 * owned by the worker, the public end (top) is shared.
 *
 * Memory model: the owner accesses bottom through the relaxed atomic helpers
 * (a new value is published to stealers with a release store) and reads top
 * with an acquire load; a stealer reads top (acquire), then bottom, so the
 * publish/release ordering makes the node the cell points at visible. top is
 * the CAS target, bottom the owner's private index. The node array is a fixed
 * ring (WSDEQUE_CAPACITY): the owner must never queue more than the ring can
 * hold, so a push that would overflow is refused (wsdeque_push returns 0) and
 * the caller defers the node instead of overwriting a live slot.
 *
 * This header is INTERNAL and compiles as C99.
 */
#include <stddef.h>
#include <stdint.h>

#include "arch/coro_atomic.h"

/* The node type belongs to the runtime layer (scheduler/rt.h defines it);
   this header only needs the name. The guard keeps the forward declaration
   single-source among its users (rt.h, mpsc.h and this header), which is
   also what strict C11-only toolchains (NDK clang -Wtypedef-redefinition)
   require. */
#ifndef COROCC_FWD_RT_NODE
#define COROCC_FWD_RT_NODE
typedef struct rt_node rt_node;
#endif

/* Capacity must be a power of two so the mask drops to &; 4096 keeps the
   hot-path deque tiny in cache. Overflow is not assumed away: wsdeque_push
   refuses a full ring (see below) rather than wrapping over a live slot. */
#define WSDEQUE_CAPACITY 4096u
#define WSDEQUE_MASK     (WSDEQUE_CAPACITY - 1u)

typedef struct wsdeque {
    rt_node*     cells[WSDEQUE_CAPACITY]; /* ring: worker-owned writes. */
    uint64_t     top;    /**< public end index (atomics: CAS target). */
    uint64_t     bottom; /**< private end index (owner-only writes). */
} wsdeque;

/** wsdeque_init -- reset a deque to empty. Called once by the owner. */
void wsdeque_init(wsdeque* d);

/**
 * wsdeque_push -- owner-only push on the private end; never blocks.
 *
 * Returns 1 when the node was stored, 0 when the fixed ring is full. A full
 * ring is refused, never wrapped: overwriting a live slot would strand that
 * entry's delivery reference and let a later pop read the covering node twice.
 * On 0 the caller defers the node (its own inbox or its timer heap) until a
 * pop frees a slot.
 */
int wsdeque_push(wsdeque* d, rt_node* n);

/**
 * wsdeque_full -- owner-only full test for the fixed ring.
 *
 * Returns 1 when the ring cannot take another node (bottom - top >=
 * WSDEQUE_CAPACITY). A stale top snapshot only makes this conservative: it
 * may report full while a concurrent steal just freed a slot, which merely
 * defers a push and never overwrites a live cell. Owner only, like push/pop.
 */
int wsdeque_full(const wsdeque* d);

/** wsdeque_pop -- owner-only pop from the private end; returns NULL if empty. */
rt_node* wsdeque_pop(wsdeque* d);

/**
 * wsdeque_steal -- pop the oldest node from the public end.
 *
 * Safe to call from any thread. Returns NULL when the deque is empty or the
 * single-task race is lost (another thief took the last node). Never spins.
 */
rt_node* wsdeque_steal(wsdeque* d);

/**
 * wsdeque_size -- approximate live fill level for diagnostics.
 *
 * Caller must be the owner; the two indices are read without syncing.
 */
size_t wsdeque_size(const wsdeque* d);

/** wsdeque_empty -- approximate empty test; owner only. */
int wsdeque_empty(const wsdeque* d);

#endif /* COROCC_SCHEDULER_WSDEQUE_H */
