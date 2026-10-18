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

#include "platform/coro_plat_locks.h"

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
    uint64_t           waiting;     /**< parked on a deadline, not a plain yield;
                                         atomic: cross-thread cancel probes it
                                         before filing a wake request */
    struct sched_node* wake_next;   /**< PRIVATE: wake-request list link */
    int                wake_pending;/**< PRIVATE: already queued on the wake list */
} sched_node;

struct coro_sched {
    sched_node*  runq_head;   /**< runnable, in spawn order */
    sched_node*  runq_tail;
    coro_timer_heap sleepers; /**< deadlines of the sleeping nodes */
    coro_plat_mutex_t timer_lock; /**< guards the heap; never held across a switch */
    coro_runtime* rt;        /**< runtime of the thread that runs the loop */
    sched_node** index;       /**< every live node, for wait_all */
    size_t       index_count;
    size_t       index_cap;
    sched_node*  running;     /**< node the loop is driving right now */
    /* Cross-thread cancellation is a request only: it appends the node to
       this owner-private list and signals the loop, which is the only thread
       that touches the run queue or the timer heap (see sched_wake_request). */
    sched_node*  wake_head;   /**< PRIVATE: pending cross-thread wake requests */
    sched_node*  wake_tail;
    coro_plat_mutex_t wake_lock; /**< guards the wake-request list */
};

/** sched_node_of -- the bookkeeping node a coroutine carries. */
sched_node* sched_node_of(coro_t* c);

/** sched_claim -- drive a node whose credential we hold to its next park. */
coro_status sched_claim(sched_node* n, coro_token* next);

/** sched_enqueue -- make a parked node runnable again. */
void sched_enqueue(coro_sched_t* s, sched_node* n);

/** sched_sleep -- park the running node until its deadline (internal). */
coro_status sched_sleep(coro_sched_t* s, uint64_t deadline);

/** sched_join -- park the running node on a target's wait chain. */
coro_status sched_join(coro_sched_t* s, sched_node* target);

/** sched_wake_request -- ask the loop's thread to wake a parked node.
 *
 * Cross-thread safe: it only appends the node to the scheduler's owner-private
 * wake list and signals the loop. The owning thread performs the actual
 * deadline removal and requeue in sched_drain_wakes, so the run queue and the
 * timer heap stay single-writer. */
void sched_wake_request(coro_sched_t* s, sched_node* n);

/** sched_drain_wakes -- owner-side half of sched_wake_request.
 *
 * Called from the run loop: for every requested node, drop its deadline (when
 * one is pending) and requeue it, retiring the park marker so a following
 * plain yield round-robins normally. */
void sched_drain_wakes(coro_sched_t* s);

/** sched_collect_expired -- move every node whose deadline has passed. */
void sched_collect_expired(coro_sched_t* s);

#endif /* COROCC_SCHEDULER_SCHED_H */
