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
#include <corocc/coro_rt_driver.h>
#include <corocc/coro.h>
#include <corocc/coro_stack.h>

#include "platform/coro_plat_locks.h"

#include "coroutine/coro_runtime.h"
#include "scheduler/wsdeque.h"
#include "scheduler/mpsc.h"
#include "scheduler/timer.h"
#include "platform/threads.h"

/* The node type is forward-declared by scheduler/wsdeque.h and
   scheduler/mpsc.h (included above) under the same guard, so this declaration
   is skipped whenever either of them came first -- which they always do here.
   One guarded declaration keeps the name defined exactly once for strict
   C11-only toolchains (NDK clang -Wtypedef-redefinition). */
#ifndef COROCC_FWD_RT_NODE
#define COROCC_FWD_RT_NODE
typedef struct rt_node rt_node;
#endif
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
#define RT_TIMED     (1u << 6)  /* BT: only a deadline park carries it */
#define RT_QUEUED    (1u << 7)  /* BT: one live delivery reference per node */
#define RT_DYING     (1u << 8)  /* BT: retire is purging the node's queues */
#define RT_WAKE_PENDING (1u << 9) /* BT: a collapsed wake is still owed a drive */

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
    uintptr_t       sleepers_lock; /**< 0 free, 1 held: guards `sleepers`. A stale
                                        delivery can move a node to another
                                        worker, so a removal issued there must be
                                        serialized against the owner's
                                        collect_expired. Zero-init is the whole
                                        contract (rt_worker is calloc'd). */
    rt_node*        running;      /**< node being driven right now. */
    uint64_t        stop;         /**< shutdown requested (atomic). */
    /* B2: this worker's alarm, owned by the attached driver. All of these are
       written only under coro_rt.drv_lock (by the worker, or by attach
       re-arming it), so a driver switch can migrate the alarm safely. The token
       is an opaque value the driver hands back; the runtime uses the worker
       pointer itself. */
    void*           alarm_req;      /**< opaque alarm token handed to the driver. */
    void*           alarm_driver;   /**< driver that armed alarm_req (may be stale). */
    int             alarm_armed;    /**< 1 while an alarm is outstanding. */
    uint64_t        alarm_deadline; /**< the deadline alarm_req was armed for. */
    uint64_t        alarm_gen;      /**< drv_gen the alarm was armed under. */
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
/* Returns 1 when the wake was delivered (pushed or collapsed into a
   live delivery), 0 when the node refused it (retired/dying, or no
   longer waiting): a releaser must then hand its wake to the next
   waiter instead of letting it evaporate on the node. */
int rt_node_wake(coro_rt_t* rt, rt_node* n);

/**
 * rt_node_wake_locked -- rt_node_wake with the activity lock already held.
 *
 * Same wake, but the caller already holds rt->act_lock and must call
 * rt_signal_activity itself after releasing it. A sync release path that pops
 * a waiter and then wakes it uses this so the popped node cannot be resumed,
 * retired and freed between the pop and the wake (retire_node publishes
 * RT_DYING under the same lock, so holding it keeps the node alive).
 */
int rt_node_wake_locked(rt_node* n);

/**
 * rt_signal_activity -- raise the runtime's activity signal.
 *
 * Bumps the activity generation and broadcasts so idle workers rescan, and
 * raises the attached driver's doorbell. A locked wake (rt_node_wake_locked)
 * leaves this to the caller, which calls it after releasing act_lock. Also
 * declared by rt_driver.h for the driver services.
 */
void rt_signal_activity(coro_rt_t* rt);

/* Consume a collapsed wake's pending drive at a park publication: if
   RT_WAKE_PENDING is set, clear it and re-arm the drive into the owner
   worker's inbox (the owner is the current driver, so it drains right
   after this drive). Returns 1 when a drive was re-armed. */
int rt_pending_requeue(rt_node* n);

/* The type name coro_rt_t lives in the public header <corocc/coro_rt.h>
   (typedef struct coro_rt coro_rt_t, line 40), included above; repeating the
   typedef here would be a typedef redefinition under strict C11-only
   toolchains, so the internal structure only completes the public tag. */
/* Bounded per-runtime stack reclaim pool. A coroutine that finishes has its
   stack parked here instead of unmapped on the retire path, so bursty
   spawn/retire traffic pays neither the unmap nor the next mmap/mprotect;
   a spawn pops a parked stack and builds the coroutine around it. Entries
   are default-size (>= CORO_DEFAULT_STACK_SIZE) stacks of the built-in
   allocator only. Bounded by entry count -- the pool defers at most this
   many mappings, and coro_rt_destroy releases every parked stack. Override
   with -DCOROCC_RT_STACK_CACHE_MAX=n. */
#ifndef COROCC_RT_STACK_CACHE_MAX
#define COROCC_RT_STACK_CACHE_MAX 256
#endif

struct coro_rt {
    rt_worker*      workers;
    size_t          nworkers;
    coro_plat_mutex_t act_lock;   /**< guards act_gen and the idle/settled waits. */
    uint64_t        act_gen;      /**< bumped whenever a node becomes runnable. */
    coro_plat_cond_t act_cond;    /**< idle workers sleep here (monotone). */
    coro_plat_cond_t settled;     /**< raises when live == 0 (wait_all). */
    uint64_t        live;         /**< coroutines not yet finished (retired); a
                                       finished retained handle no longer
                                       counts (atomic). */
    size_t          spawn_seq;    /**< round-robin spawn distribution. */
    coro_plat_mutex_t cache_lock; /**< guards the stack reclaim pool below. */
    coro_stack      cache[COROCC_RT_STACK_CACHE_MAX]; /**< parked stacks. */
    size_t          cache_n;      /**< parked entries in use. */
    coro_rt_driver_t* drv;        /**< attached driver, or NULL for the built-in
                                       EDSM backend (B1 leaves it NULL: the
                                       attach/detach machine lands in B2). */
    /* B2 driver state. drv_kind is RT_DRV_*; drv_lock serializes attach/detach
       against a worker arming its own alarm, and guards the workers' alarm_*
       fields. drv_gen is bumped on every switch (an alarm armed under an older
       generation is stale, so the worker re-arms on the new one). A same-family
       switch migrates alarms in place and never parks a worker. A cross-class
       switch (LOOP vs. worker mode) quiesces every worker on the barrier below:
       drv_barrier parks them at the top of the loop and drv_parked counts them,
       drv_started counts the workers that are inside the loop (the start
       handshake the quiesce wait keys on), and drv_threads records whether the
       worker threads were ever created (a LOOP-created runtime starts them only
       when it switches back to a threaded family). pump_active is a re-entrancy
       guard for coro_rt_pump. All the counters are atomic; drv_threads is only
       written under drv_lock. */
    coro_plat_mutex_t drv_lock;
    unsigned          drv_kind;
    uint64_t          drv_gen;
    uint64_t          drv_barrier;   /**< atomic: 1 = workers hold at the barrier. */
    uint64_t          drv_parked;    /**< atomic: workers parked at the barrier. */
    uint64_t          drv_started;   /**< atomic: workers inside their loop. */
    uint64_t          drv_threads;   /**< atomic: worker threads were created. */
    uint64_t          pump_active;   /**< atomic: a pump is inside rt_run_some. */
};

/* Driver families (coro_rt.drv_kind). */
#define RT_DRV_BUILTIN 0u   /**< no driver: the built-in EDSM backend. */
#define RT_DRV_SIGNAL  1u   /**< worker threads stay; driver doorbell/clock/alarm. */
#define RT_DRV_LOOP    2u   /**< no worker threads; the host calls coro_rt_pump. */

/**
 * rt_run_some -- drive up to @p budget runnable nodes for worker @p w.
 *
 * One round of the worker loop: drain expirations and the inbox, then pop (or
 * steal) and drive runnable nodes until @p budget is spent or the runtime has
 * nothing runnable. Never blocks, never touches the timer heap's deadlines
 * except through expiration. Returns the number of nodes taken off the run
 * queue (0 = nothing was runnable). worker_main calls it with budget 1 to keep
 * its one-node-per-turn shape; coro_rt_pump calls it with the host's budget.
 */
size_t rt_run_some(coro_rt_t* rt, rt_worker* w, size_t budget);

/**
 * rt_loop_sweep -- collect and drain every worker but the pump's own.
 *
 * In LOOP mode the host calls coro_rt_pump, which drives worker[0] by hand
 * while the other worker threads hold at the barrier. Their timer heaps and
 * inboxes still carry work from before the switch, so the pump first moves
 * every expired deadline and pending inbox entry of workers[1..] onto their own
 * run queues; rt_run_some then steals them for worker[0]. Safe only when the
 * owning workers are parked (which the LOOP barrier guarantees), because the
 * sweep acts as the owner.
 */
void rt_loop_sweep(coro_rt_t* rt);

/**
 * rt_start_workers -- create the worker threads.
 *
 * Allocated worker slots are started via worker_main. Returns 1 when every
 * worker started, 0 when a thread could not be created (the ones already
 * started are stopped and joined, and their stop flag is reset so a later
 * retry starts cleanly). On success drv_threads is set. Used by coro_rt_create
 * and by a LOOP-to-threaded driver switch on a runtime whose workers were never
 * started.
 */
int rt_start_workers(coro_rt_t* rt);

#endif /* COROCC_SCHEDULER_RT_H */
