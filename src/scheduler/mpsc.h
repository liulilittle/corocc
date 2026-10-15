#ifndef COROCC_SCHEDULER_MPSC_H
#define COROCC_SCHEDULER_MPSC_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/scheduler/mpsc.h -- Vyukov intrusive MPSC single-consumer queue.
 *
 * Cross-thread wakeups (a join target retiring on another worker, a cancel
 * from a non-owner thread) must hand a runnable node to the worker whose
 * deque it now belongs on. That path can never touch the worker's private
 * Chase-Lev end, so it lands here: a lock-free multi-producer, single-consumer
 * FIFO. Producers append with one compare-and-swap against the head; the
 * owner consumer pops an entire batch by swapping the head to NULL and
 * reversing the link chain. Never spins: a failed CAS means another producer
 * won the slot and the retry is bounded to a couple of instructions.
 *
 * This header is INTERNAL and compiles as C99.
 */
#include <stddef.h>
#include <stdint.h>

#include "arch/coro_atomic.h"

/* Guarded forward declaration: see scheduler/wsdeque.h for the rationale
   (single definition among rt.h / wsdeque.h / this header). */
#ifndef COROCC_FWD_RT_NODE
#define COROCC_FWD_RT_NODE
typedef struct rt_node rt_node;
#endif

typedef struct mpsc {
    rt_node*          head; /**< PRIVATE: CAS target, NULL when empty. */
    rt_node*          tail; /**< PRIVATE: last node the owner drained, cached. */
} mpsc;

/** mpsc_init -- reset the queue to empty. Called by the owner before start. */
void mpsc_init(mpsc* q);

/**
 * mpsc_push -- append a node; safe from any thread.
 *
 * The node's mpsc.link (rt_node.next) is written here; the caller must not
 * reuse that field meanwhile. Never blocks, never spins unboundedly.
 */
void mpsc_push(mpsc* q, rt_node* n);

/**
 * mpsc_pop -- take the oldest buffered node, or NULL when drained.
 *
 * Owner (single consumer) only. Drains exactly one batch per call; repeated
 * calls drain a full burst. Amortized O(1) per node.
 */
rt_node* mpsc_pop(mpsc* q);

/** mpsc_peek -- non-empty test the owner can poll cheaply. */
int mpsc_empty(const mpsc* q);

#endif /* COROCC_SCHEDULER_MPSC_H */