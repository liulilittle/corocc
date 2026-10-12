#ifndef COROCC_SCHEDULER_SCHED_H
#define COROCC_SCHEDULER_SCHED_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/scheduler/sched.h -- internal shape of the single-thread scheduler.
 *
 * The scheduler owns three intrusive lists (runnable queue, sleeping timers,
 * wait chains) and reuses the coroutine object for everything else: a runnable
 * coroutine is simply one whose credential this thread holds, and a sleeping
 * one is a coroutine whose deadline sits in the timer heap.
 *
 * This header is INTERNAL: it is not installed and is not part of the public
 * API or ABI.
 */
#include <stddef.h>

#include <corocc/coro_sched.h>
#include "coroutine/coro_runtime.h"
#include "scheduler/timer.h"

typedef struct sched_node {
    struct sched_node* next;
    coro_t*            coro;
    coro_token         tok;         /**< credential held for a parked coroutine */
    coro_timer_node    timer;       /**< deadline, when the node is sleeping */
    struct sched_node* waiter_next; /**< wait chain: coroutines waiting on this one */
    int                done;        /**< the coroutine finished (join fast path) */
    int                started;     /**< its entry has been entered at least once */
} sched_node;

struct coro_sched {
    sched_node*  runq_head;   /**< runnable, in spawn order */
    sched_node*  runq_tail;
    coro_timer_heap sleepers; /**< deadlines of the sleeping nodes */
    sched_node** index;       /**< every live node, for wait_all */
    size_t       index_count;
    size_t       index_cap;
};

/** sched_node_of -- the bookkeeping node a coroutine carries. */
sched_node* sched_node_of(coro_t* c);

/** sched_claim -- drive a node whose credential we hold to its next park. */
coro_status sched_claim(sched_node* n, coro_token* next);

/** sched_enqueue -- make a parked node runnable again. */
void sched_enqueue(coro_sched_t* s, sched_node* n);

/** sched_collect_expired -- move every node whose deadline has passed. */
void sched_collect_expired(coro_sched_t* s);

#endif /* COROCC_SCHEDULER_SCHED_H */
