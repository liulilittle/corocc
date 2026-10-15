#ifndef COROCC_SCHEDULER_RT_H
#define COROCC_SCHEDULER_RT_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/scheduler/rt.h -- internal shape of the M:N work-stealing runtime.
 *
 * A runtime owns N worker threads. Each worker owns one Chase-Lev deque (its
 * private runnable end: pushes and pops cost nothing), one Vyukov MPSC inbox
 * (cross-thread wakeups land here, never on the private end) and one timer
 * heap (single-threaded, owner only). A worker whose deque runs dry steals
 * from a peer's public end with a single compare-and-swap; when even that
 * yields nothing it sleeps on the runtime's activity condition until work
 * appears or its nearest deadline passes.
 *
 * The hot path -- a coroutine driven, yielding, and driven again by the SAME
 * worker -- touches no lock and no system call: the deque push and pop are
 * plain stores on the private end, and the timer heap is touched by the
 * worker that owns it alone. Cross-thread state (spawn, steal, cancel, wait
 * chains) is carried through the MPSC inbox, the Chase-Lev public end, and a
 * small set of atomics -- never through a mutex.
 *
 * Single-core safety: a worker never spins. With one core and one thread the
 * queue is the private Chase-Lev end and a yield is an enqueue-dequeue -- no
 * deadlock. With one core and several threads, idle workers block on the
 * activity condition that any producer raises, so stolen work is always
 * witnessed.
 *
 * This header is INTERNAL: it is not installed and is not part of the public
 * API or ABI.
 */
#include <stddef.h>

#include <corocc/coro_rt.h>
#include <corocc/coro.h>

#if !defined(_WIN32)
#include <pthread.h>
#endif

#include "coroutine/coro_runtime.h"
#include "scheduler/wsdeque.h"
#include "scheduler/mpsc.h"
#include "scheduler/timer.h"
#include "platform/threads.h"

typedef struct rt_node rt_node;
typedef struct rt_worker rt_worker;

/* Node state bits, kept in one atomic word so cross-thread readers (cancel)
   and the worker agree without a lock. The worker itself uses relaxed reads
   where ordering is already given by the transfer protocol. */
#define RT_STARTED   (1u << 0)
#define RT_WAITING   (1u << 1)
#define RT_RETAINED  (1u << 2)
#define RT_DONE      (1u << 3)
#define RT_WAKE_REQ  (1u << 4)
#define RT_PARKED    (1u << 5)

struct rt_node {
    struct rt_node* next;         /**< MPSC inbox link. */
    coro_t*         coro;
    coro_token      tok;          /**< credential held for a parked coroutine. */
    coro_timer_node timer;        /**< deadline, when sleeping. */
    struct rt_node* waiter_next;  /**< wait chain: coroutines waiting on this one. */
    rt_worker*      worker;       /**< worker that owns the heap this node sleeps in. */
    uint64_t        flags;        /**< RT_* state bits (atomic). */
};

struct rt_worker {
    struct coro_rt* rt;
    coro_thread_t   thread;
    wsdeque         queue;        /**< runnable; private end owned by this worker. */
    mpsc            inbox;        /**< cross-thread wakeups, lock-free. */
    coro_timer_heap sleepers;     /**< deadlines; this worker is the only writer. */
    rt_node*        running;      /**< node being driven right now. */
    uint64_t        stop;         /**< shutdown requested (atomic). */
};

/* ---- synchronization primitives (rt_sync.c) ---- */

/* A wait chain is an owner-protected singly linked list of rt_node waiters
   connected through waiter_next. The runtime's own chains are protected by the
   activity lock; user-facing primitive chains carry their own guard. */

/**
 * rt_node_wake -- requeue a parked node for execution.
 *
 * Clears RT_WAITING and pushes the node to its owner worker's inbox, then
 * raises the activity signal so an idle worker notices. Only the chain owner
 * may call this; the node must be off any chain when it returns.
 */
void rt_node_wake(coro_rt_t* rt, rt_node* n);

typedef struct coro_rt {
    rt_worker*      workers;
    size_t          nworkers;
    pthread_mutex_t act_lock;     /**< guards act_gen and the idle/settled waits. */
    uint64_t        act_gen;      /**< bumped whenever a node becomes runnable. */
    pthread_cond_t  act_cond;     /**< idle workers sleep here (monotone). */
    pthread_cond_t  settled;      /**< raises when live == 0 (wait_all). */
    uint64_t        live;         /**< coroutines not yet finished (retired); a
                                       finished retained handle no longer
                                       counts (atomic). */
    size_t          spawn_seq;    /**< round-robin spawn distribution. */
} coro_rt_t;

#endif /* COROCC_SCHEDULER_RT_H */