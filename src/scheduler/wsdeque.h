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
 * Memory model: bottom is a plain field (only the owner writes it, stealers
 * read it with an acquire load), top is an atomic decremented... rather, both
 * are read/written through the atomic helpers so a stealer that reads a stale
 * bottom sees the store-release ordering the owner publishes. The node array
 * is a fixed ring; when it would overflow the caller falls back to the
 * global overflow pool (see rt.c), which is vanishingly rare.
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
   hot-path deque tiny in cache while making overflow essentially unreachable. */
#define WSDEQUE_CAPACITY 4096u
#define WSDEQUE_MASK     (WSDEQUE_CAPACITY - 1u)

typedef struct wsdeque {
    rt_node*     cells[WSDEQUE_CAPACITY]; /* ring: worker-owned writes. */
    uint64_t     top;    /**< public end index (atomics: CAS target). */
    uint64_t     bottom; /**< private end index (owner-only writes). */
} wsdeque;

/** wsdeque_init -- reset a deque to empty. Called once by the owner. */
void wsdeque_init(wsdeque* d);

/** wsdeque_push -- owner-only push on the private end. Never blocks. */
void wsdeque_push(wsdeque* d, rt_node* n);

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