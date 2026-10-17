/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/scheduler/rt.c -- M:N work-stealing runtime.
 *
 * The runtime is a pool of worker threads over one set of coroutines. Each
 * worker drives coroutines until they park, then takes the next runnable one;
 * when its own deque runs dry it steals the oldest runnable coroutine from a
 * peer's public end. Because a runnable node carries the coroutine's
 * credential, whoever picks the node up may resume it -- the coroutine
 * migrates freely between workers.
 *
 * The hot path (a coroutine driven, yielding, and driven again by the same
 * worker) touches no lock and no system call: the Chase-Lev private end is
 * plain stores, and the worker's timer heap is owned by that worker alone.
 * Cross-thread events -- spawn, steal, wake, cancel -- move through the MPSC
 * inboxes and the atomics only. An idle worker sleeps on the activity
 * condition until work appears or its nearest deadline passes; the generation
 * counter is bumped under the same lock the sleeper waits on, so a producer
 * that raises work always wakes the sleepers and a single-core runtime cannot
 * deadlock.
 */
#include "scheduler/rt.h"
#include "scheduler/rt_driver.h"

#include "coroutine/coro_internal.h"

#include <stddef.h>
#include <stdlib.h>
#include <time.h>
#if !defined(_WIN32)
#include <sched.h>    /* sched_yield: the retire purge loop's relax */
#endif

#if defined(__linux__)
#include <unistd.h>   /* sysconf: default worker count */
#endif
#if defined(_WIN32)
#include <windows.h>  /* GetSystemInfo: default worker count */
#endif

/* ---- atomic node state ---- */

static int node_test(rt_node* n, unsigned bit) {
    return (coro_at_load((volatile uint64_t*)&n->flags) & bit) != 0;
}

static void node_set(rt_node* n, unsigned bit) {
    (void)coro_at_or((volatile uint64_t*)&n->flags, bit);
}

static void node_clear(rt_node* n, unsigned bit) {
    (void)coro_at_and((volatile uint64_t*)&n->flags, ~(uint64_t)bit);
}

/* Publish form: a RELEASE fetch_or. Unlike node_set (relaxed), every write
   this thread made before the OR -- the wait-chain detach and the retained
   reading in retire_node's critical section -- is visible to the thread
   whose ACQUIRE load observes the bit. Used for exactly one write: the
   retire's RT_DONE publication, which must be the last write to the node. */
static void node_set_rel(rt_node* n, unsigned bit) {
    (void)coro_at_or_release((volatile uint64_t*)&n->flags, bit);
}

/* Atomically clear one bit. Returns 1 when this call cleared it, 0 when it
   was already clear. Used as the single-requeue gate: a parked node -- parked
   inside a sync primitive or in the timer heap or in a join chain -- is
   enqueued exactly once, by whichever of its driver (handing it back after a
   park) or its releaser (waking it) wins the bit. */
static int node_cas_clear(rt_node* n, unsigned bit) {
    uint64_t cur = coro_at_load((volatile uint64_t*)&n->flags);
    for (;;) {
        if (!(cur & bit)) return 0;
        uint64_t exp = cur;
        if (coro_at_cas((volatile uint64_t*)&n->flags, &exp,
                        cur & ~(uint64_t)bit)) {
            return 1;
        }
        cur = exp; /* lost: exp now holds the observed value */
    }
}

/* Portable relax for the retire purge loop: let a pusher that is between its
   claim and its push run. */
static void rt_cpu_relax(void) {
#if defined(_WIN32)
    SwitchToThread();
#else
    (void)sched_yield();
#endif
}

#ifdef COROCC_WAKE_DBG
struct rt_wake_dbg rt_dbg;
#define RT_DBG_INC(f, v) \
    (void)coro_at_fetch_add((volatile uint64_t*)&rt_dbg.f, (uint64_t)(v))
#else
#define RT_DBG_INC(f, v) ((void)0)
#endif

/* Claim the node's single delivery reference (BT): at most one live inbox
   entry / deque entry exists per node at any time. A delivery that loses the
   claim is collapsed into the live one; the claim is released by the worker
   that consumes the entry (pop/steal), by the retire purge, or by a drop.
   The claim refuses a node that is retiring or already retired, so no new
   reference can be created after RT_DYING/RT_DONE is visible. */
static int rt_queue_claim(rt_node* n) {
    uint64_t cur = coro_at_load((volatile uint64_t*)&n->flags);
    for (;;) {
        if (cur & (RT_QUEUED | RT_DYING | RT_DONE)) return 0;
        uint64_t exp = cur;
        if (coro_at_cas((volatile uint64_t*)&n->flags, &exp, cur | RT_QUEUED)) {
            return 1;
        }
        cur = exp;
    }
}

/* BT: deliver a collapsed wake's pending drive (B' minimal state machine:
   PENDING means one drive is owed to the flow; the park publication is the
   deliverer). Claim the delivery slot FIRST and clear PENDING only when the
   claim succeeds: the very situation that set PENDING (a live entry in
   flight) makes the claim fail, and clearing first would destroy the
   promise exactly when it is owed. On failure PENDING stays set for the
   next park publication, the gate re-arm, or the in-flight drive to
   discharge. */
int rt_pending_requeue(rt_node* n) {
    if (!node_test(n, RT_WAKE_PENDING)) return 0;
    if (!rt_queue_claim(n)) return 0;   /* still owed: keep PENDING */
    node_clear(n, RT_WAKE_PENDING);
    mpsc_push(&n->worker->inbox, n);
    return 1;
}

/* ---- live count (atomic) ---- */

static void live_add(coro_rt_t* rt, long d) {
    (void)coro_at_fetch_add((volatile uint64_t*)&rt->live, (uint64_t)(long long)d);
}

static void live_notify(coro_rt_t* rt) {
    if (coro_at_load((volatile uint64_t*)&rt->live) == 0) {
        coro_plat_mutex_lock(&rt->act_lock);
        ++rt->act_gen;
        coro_plat_cond_broadcast(&rt->act_cond);
        coro_plat_cond_broadcast(&rt->settled);
        coro_plat_mutex_unlock(&rt->act_lock);
    }
}

/* Wake every idle worker: bump the generation under the activity lock and
   broadcast, so a producer that raises work always wakes the sleepers. When a
   driver is attached its doorbell runs first, outside the activity lock (the
   wake contract forbids re-entering the runtime synchronously, and a driver
   may lock its own state here). The built-in broadcast always follows, because
   in the SIGNAL family the worker threads still sleep on the activity
   condition (invariant I5). With no driver attached the body is unchanged. */
static void signal_activity(coro_rt_t* rt) {
#if COROCC_RT_DRIVER
    coro_rt_driver_t* d =
        (coro_rt_driver_t*)coro_atp_load((const volatile uintptr_t*)&rt->drv);
    if (d) {
        d->ops.wake(d->ctx);
    }
#endif
    coro_plat_mutex_lock(&rt->act_lock);
    ++rt->act_gen;
    coro_plat_cond_broadcast(&rt->act_cond);
    coro_plat_mutex_unlock(&rt->act_lock);
}

void rt_signal_activity(coro_rt_t* rt) { signal_activity(rt); }

/* The driver alarm callback: invoked on the driver's thread when an armed
   deadline fires. Waking the activity condition lets the worker whose deadline
   passed re-collect its heap; the doorbell is raised by signal_activity too. */
void rt_alarm_wake(void* arg) { signal_activity((coro_rt_t*)arg); }

/* Requeue a parked node onto its owner worker and raise the idle signal. The
   node must be off any wait chain; the caller owns that invariant. The WAITING
   bit is the single-requeue gate against the worker that drove the node into
   the park: exactly one of the two enqueues the node, so it can never be
   queued twice and driven by two workers at once. */
int rt_node_wake_locked(rt_node* n) {
    /* Consume the park WAITING atomically with a corpse check: a node that
       retired between the releaser's pop and this wake refuses the handoff
       (return 0), so the releaser serves the next waiter instead of letting
       the handoff evaporate on a corpse. On a live park the wake either
       claims the delivery (push) or collapses into the live entry and
       records RT_WAKE_PENDING, which the consuming gate discharges by
       re-arming if that drive is spent while the flow is running. The caller
       holds act_lock, so the node cannot be retired and freed underneath this
       read (retire_node publishes RT_DYING under the same lock); the caller
       raises the idle signal itself after releasing the lock. */
    uint64_t cur = coro_at_load((volatile uint64_t*)&n->flags);
    for (;;) {
        if (cur & (RT_DONE | RT_DYING)) {
            RT_DBG_INC(wake_refused, 1);
            return 0;
        }
        if (!(cur & RT_WAITING)) {
            RT_DBG_INC(wake_refused, 1);
            return 0;
        }
        uint64_t exp = cur;
        if (coro_at_cas((volatile uint64_t*)&n->flags, &exp,
                        cur & ~(uint64_t)RT_WAITING)) {
            if (rt_queue_claim(n)) {
                RT_DBG_INC(wake_claimed, 1);
                node_clear(n, RT_WAKE_PENDING);
                mpsc_push(&n->worker->inbox, n);
            } else {
                RT_DBG_INC(wake_absorbed, 1);
                node_set(n, RT_WAKE_PENDING);
            }
            return 1;
        }
        cur = exp; /* lost: exp now holds the observed value */
    }
}

int rt_node_wake(coro_rt_t* rt, rt_node* n) {
    int woken = rt_node_wake_locked(n);
    signal_activity(rt);
    return woken;
}

/* ---- stack reclaim pool ---- */

/* Park a retired stack; when the pool is full the stack is released the
   normal way. Only stacks that can serve a default-size spawn are parked
   (smaller custom sizes are freed outright), matching the request side,
   which pops only for default-size spawns. */
static void rt_stack_push(coro_rt_t* rt, coro_stack* stk) {
    int stored = 0;
    if (stk->usable < CORO_DEFAULT_STACK_SIZE) {
        coro_stack_free(stk);
        return;
    }
    coro_plat_mutex_lock(&rt->cache_lock);
    if (rt->cache_n < COROCC_RT_STACK_CACHE_MAX) {
        rt->cache[rt->cache_n++] = *stk;
        stored = 1;
    }
    coro_plat_mutex_unlock(&rt->cache_lock);
    if (!stored) coro_stack_free(stk);
}

/* Take one parked stack, or report the pool empty. */
static int rt_stack_pop(coro_rt_t* rt, coro_stack* out) {
    int got = 0;
    coro_plat_mutex_lock(&rt->cache_lock);
    if (rt->cache_n > 0) {
        *out = rt->cache[--rt->cache_n];
        got = 1;
    }
    coro_plat_mutex_unlock(&rt->cache_lock);
    return got;
}

/* ---- worker queues ---- */

/* A worker that owns an inbox turns every drained node into a deque entry.
   BT: an entry is the node's single delivery reference (RT_QUEUED); it is
   routed to the deque unconditionally -- a stale entry resumes the node
   wherever it is parked and the woken paths re-check (genuineness) and
   detach from any chain they were still linked on. A dying node's entry is
   dropped and its reference released. */
static void drain_inbox(rt_worker* w) {
    rt_node* n;
    int routed = 0;
    while ((n = mpsc_pop(&w->inbox)) != NULL) {
        if (node_test(n, RT_DYING | RT_DONE)) {
            node_clear(n, RT_QUEUED);
            continue;
        }
        if (node_test(n, RT_WAKE_REQ)) {
            node_clear(n, RT_WAKE_REQ);
            (void)coro_timer_heap_remove(&w->sleepers, &n->timer);
        }
        wsdeque_push(&w->queue, n);
        routed = 1;
    }
    /* A routed entry can outlive the wake's activity signal (the worker was
       busy driving a hot coroutine at that moment); re-raise it so an idle
       peer can steal the entry instead of it starving at the deque top. */
    if (routed) signal_activity(w->rt);
}

/* Move every node whose deadline has passed from this worker's timer heap to
   its runnable deque. Owner-only. */
static void collect_expired(rt_worker* w) {
    /* Empty-heap short circuit: with no deadline pending there is nothing to
       expire, so the run loop must not pay the monotone-clock read (a virtual-
       syscall on Linux) just to learn that. The heap is worker-private, so the
       count load needs no synchronization. */
    if (w->sleepers.count == 0) return;
    uint64_t now = rt_now(w->rt);
    for (;;) {
        coro_timer_node* t = coro_timer_heap_top(&w->sleepers);
        if (!t || t->deadline > now) return;
        t = coro_timer_heap_pop(&w->sleepers);
        rt_node* n = (rt_node*)(void*)((char*)t - offsetof(rt_node, timer));
        /* BT: route only if this side owns the single delivery reference; a
           live cancel entry routes the node itself. */
        if (!(node_test(n, RT_DYING | RT_DONE)) &&
            node_cas_clear(n, RT_WAITING) && rt_queue_claim(n)) {
            wsdeque_push(&w->queue, n);
            signal_activity(w->rt);
        }
    }
}

/* ---- drive ---- */

/* Drive a node to its next park (or its end). The first turn starts the
   coroutine: the credential for a never-started coroutine is the implicit
   "never started" one, which coro_resume rejects. */
static coro_status rt_claim(rt_node* n, coro_token* next) {
    if (node_test(n, RT_STARTED)) return coro_resume(n->coro, n->tok, next);
    node_set(n, RT_STARTED);
    return coro_start(n->coro, next);
}

/* A node whose coroutine finished: release the wait chain and reclaim memory
   unless the caller keeps the handle. */
static void retire_node(coro_rt_t* rt, rt_node* n) {
    /* Detach the wait chain under the activity lock, the same lock a joiner
       holds while appending to it: without it a join that linked itself
       between the check and the append could land on a chain this walk is
       consuming, and its waiter would never be woken. The wake then runs
       under the lock (a locked wake; the idle signal is raised after the
       unlock), so a detached waiter cannot be resumed by a stale delivery,
       retired and freed between the detach and the wake. The retained reading
       belongs
       in that same critical section, and the publication is the RELEASE
       fetch_or that follows the detach, still inside the lock: it is the
       last write to the node, so whoever observes RT_DONE -- the collector
       via an acquire load, or a joiner re-checking under this same lock --
       observes the chain already detached and the retained reading already
       made. A retained node is left completely untouched from here on; the
       collector that observed RT_DONE owns the reclaim. A non-retained node
       has no external handle and cannot be collected concurrently, so this
       worker still frees it exclusively. */
    rt_node* waiter;
    int retained;
    coro_plat_mutex_lock(&rt->act_lock);
    retained = node_test(n, RT_RETAINED);
    node_set(n, RT_DYING);      /* no new delivery reference from now on */
    waiter = n->waiter_next;
    n->waiter_next = NULL;
    coro_plat_mutex_unlock(&rt->act_lock);
    /* BT death window: an entry pushed before RT_DYING must be consumed here
       (the owner worker drains its own inbox; a push that claimed the slot
       just before RT_DYING lands within the loop). The reference cannot be
       in a deque: a routed entry is consumed by the claim that drives the
       node, and this worker is the one driving it now. */
    while (node_test(n, RT_QUEUED)) {
        RT_DBG_INC(retire_spin, 1);
        drain_inbox(n->worker);
        if (node_test(n, RT_QUEUED)) rt_cpu_relax();
    }
    coro_plat_mutex_lock(&rt->act_lock);
    node_set_rel(n, RT_DONE);   /* publish: retire's LAST write to n */
    coro_plat_mutex_unlock(&rt->act_lock);
    /* Wake the detached join waiters under the lock: a waiter can be resumed
       by an independent stale delivery and retire (freeing itself) between the
       detach and a deliberate wake, so the wake must run while act_lock still
       keeps the node alive. The idle signal is raised after the unlock. */
    if (waiter) {
        coro_plat_mutex_lock(&rt->act_lock);
        while (waiter) {
            rt_node* next = waiter->waiter_next;
            waiter->waiter_next = NULL;
            (void)rt_node_wake_locked(waiter);
            waiter = next;
        }
        coro_plat_mutex_unlock(&rt->act_lock);
        signal_activity(rt);
    }
    live_add(rt, -1);
    live_notify(rt);
    if (!retained) {
        /* Park the stack instead of unmapping it: the pool serves the next
           spawn and coro_rt_destroy releases what is left. A handle with a
           custom allocator is released in full (and yields no stack). */
        coro_stack stk;
        if (coro_destroy_to_stack(n->coro, &stk) == CORO_OK && stk.base) {
            rt_stack_push(rt, &stk);
        }
        free(n);
    }
}

/* ---- worker loop ---- */

/* Drive up to budget runnable nodes: one round of collect + drain, then pop
   (or steal) and drive. Returns the number of nodes taken off the run queue. */
size_t rt_run_some(coro_rt_t* rt, rt_worker* w, size_t budget) {
    size_t done = 0;
    collect_expired(w);
    drain_inbox(w);

    while (done < budget) {
        rt_node* n = wsdeque_pop(&w->queue);
        if (!n) {
            size_t first = (size_t)(w - rt->workers);
            for (size_t i = 1; i < rt->nworkers; ++i) {
                rt_worker* v = &rt->workers[(first + i) % rt->nworkers];
                n = wsdeque_steal(&v->queue);
                if (n) break;
            }
        }
        if (!n) break;
        ++done;

        {
            coro_token next = CORO_TOKEN_NONE;
            coro_status st;
            /* BT: consuming this deque entry releases the node's single
               delivery reference; a dying/dead node is never driven. */
            node_clear(n, RT_QUEUED);
            if (node_test(n, RT_DYING | RT_DONE)) continue;
            n->worker = w;
            w->running = n;
            st = rt_claim(n, &next);
            if (st == CORO_OK) {
                node_clear(n, RT_WAKE_PENDING);   /* the drive fulfils the promise */
            }
            w->running = NULL;
            if (st == CORO_OK) {
                if (next == CORO_TOKEN_DEAD) {
                    retire_node(rt, n);
                } else {
                    n->tok = next;
                    if (node_test(n, RT_PARKED)) {
                        /* Parked: the releaser (wake, timer expiration or
                           cancel drain) owns the requeue, so the driver only
                           retires the park marker and leaves the node off the
                           run queue. A plain yield (no park marker) goes back
                           on the queue immediately -- tracked by the same
                           single QUEUED reference, so "an entry exists" stays
                           equivalent to "QUEUED is held" for deque entries
                           too (a pending stale entry already owes the drive). */
                        node_clear(n, RT_PARKED);
                    } else if (rt_queue_claim(n)) {
                        /* Self-requeue of a plain yield. n lands on this
                           worker's owner (bottom) end, so the next pop takes n
                           straight back; any OLDER entry already in the deque
                           sits at the far (steal) end and can only be reached
                           by a PEER. If every peer is asleep that older entry
                           starves forever -- observed as a woken waiter whose
                           delivery entry is never consumed, so its park is
                           never fulfilled and live never reaches zero (the
                           SV wind-down STALL: the holder re-yields while a
                           victim's wake entry waits below). Raise the activity
                           signal when the deque was ALREADY non-empty, exactly
                           as drain_inbox does after routing, so an idle peer
                           steals the older entry. An empty deque takes no
                           signal: the common single-coroutine yield stays free
                           of any lock/broadcast. */
                        int had_older = !wsdeque_empty(&w->queue);
                        wsdeque_push(&w->queue, n);
                        if (had_older) signal_activity(rt);
                    }
                }
            } else {
                /* BT: the claim was lost (EBUSY/SWITCHING/ESTALE) -- the flow
                   is being driven by its owner right now. Discharge the
                   collapse promise (RT_WAKE_PENDING) and re-arm into the
                   owner's inbox: the owner drains only after its current
                   drive ends, so the re-armed entry then meets a suspended
                   flow and rt_claim succeeds. Dead/dying nodes are refused by
                   the claim and purged by the retire drain. */
                node_clear(n, RT_WAKE_PENDING);
                if (rt_queue_claim(n)) {
                    mpsc_push(&n->worker->inbox, n);
                    signal_activity(rt);
                }
            }
        }
    }
    return done;
}

/* LOOP pump helper: collect the expired deadlines and drain the inbox of every
   worker but worker[0]. The host drives worker[0] through coro_rt_pump while the
   peer workers hold at the barrier, so the pump thread may act as their owner
   for this one pass; rt_run_some then steals the routed entries for worker[0].
   Worker 0's own heap and inbox are handled inside its rt_run_some call. */
void rt_loop_sweep(coro_rt_t* rt) {
    for (size_t i = 1; i < rt->nworkers; ++i) {
        collect_expired(&rt->workers[i]);
        drain_inbox(&rt->workers[i]);
    }
}

/* Put a worker to sleep until work appears or its nearest deadline passes.
   With a driver that arms alarms the deadline is handed to the driver and the
   worker waits on the activity condition without a timeout (the driver's alarm
   wakes it); otherwise the built-in partial wait is used. The generation
   snapshot makes a concurrent driver switch re-evaluate instead of sleeping
   under a mechanism the switch just retired. */
static void worker_sleep(coro_rt_t* rt, rt_worker* w, uint64_t deadline) {
    int wait_untimed = 0;
#if COROCC_RT_DRIVER
    uint64_t gen;
    int use_driver;
    coro_plat_mutex_lock(&rt->drv_lock);
    gen = coro_at_load((volatile uint64_t*)&rt->drv_gen);
    coro_rt_driver_t* d =
        (coro_rt_driver_t*)coro_atp_load((const volatile uintptr_t*)&rt->drv);
    use_driver = (d && d->ops.timer_arm) ? 1 : 0;
    if (!use_driver) {
        if (w->alarm_armed) {
            coro_rt_driver_t* od = (coro_rt_driver_t*)w->alarm_driver;
            if (od && od->ops.timer_cancel) {
                (void)od->ops.timer_cancel(od->ctx, w->alarm_req);
            }
            w->alarm_armed = 0;
        }
    } else if (deadline == 0) {
        if (w->alarm_armed) {
            if (d->ops.timer_cancel) {
                (void)d->ops.timer_cancel(d->ctx, w->alarm_req);
            }
            w->alarm_armed = 0;
        }
    } else if (!w->alarm_armed || w->alarm_deadline != deadline ||
               w->alarm_gen != gen) {
        if (w->alarm_armed) {
            coro_rt_driver_t* od = (coro_rt_driver_t*)w->alarm_driver;
            if (od && od->ops.timer_cancel) {
                (void)od->ops.timer_cancel(od->ctx, w->alarm_req);
            }
            w->alarm_armed = 0;
        }
        if (d->ops.timer_arm(d->ctx, w->alarm_req, deadline, rt_alarm_wake,
                             rt) == 0) {
            w->alarm_armed = 1;
            w->alarm_deadline = deadline;
            w->alarm_gen = gen;
            w->alarm_driver = d;
        } else {
            use_driver = 0; /* no alarm: fall back to the built-in timed wait */
        }
    }
    wait_untimed = use_driver;
    coro_plat_mutex_unlock(&rt->drv_lock);
#endif

    coro_plat_mutex_lock(&rt->act_lock);
    if (!coro_at_load((volatile uint64_t*)&w->stop) &&
        wsdeque_empty(&w->queue) && mpsc_empty(&w->inbox) &&
        /* A raised barrier must not be slept through: without this the worker
           could evaluate the condition just before the switch broadcasts and
           then miss it, leaving it asleep while the runtime waits for it to
           park. Returning here lets worker_main observe the barrier. */
        !coro_at_load((volatile uint64_t*)&rt->drv_barrier)
#if COROCC_RT_DRIVER
        && coro_at_load((volatile uint64_t*)&rt->drv_gen) == gen
        && !(deadline != 0 && rt_now(rt) >= deadline)
#endif
        ) {
        if (wait_untimed) {
            (void)coro_plat_cond_wait(&rt->act_cond, &rt->act_lock);
        } else if (deadline) {
            (void)coro_plat_cond_timedwait_ns(&rt->act_cond, &rt->act_lock,
                                              deadline);
        } else {
            (void)coro_plat_cond_wait(&rt->act_cond, &rt->act_lock);
        }
    }
    coro_plat_mutex_unlock(&rt->act_lock);
}

/* Park a worker on the cross-class barrier. Kept in its own noinline function
   on purpose: inlined into worker_main, this body (mutex/cond calls) perturbed
   clang -O3's register allocation in the per-node hot loop and measurably
   slowed it -- under CPU oversubscription that pushed rt_cancel_park's SV case
   over its watchdog. The barrier test in the loop stays a plain atomic load. */
#if defined(_MSC_VER)
# define COROCC_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
# define COROCC_NOINLINE __attribute__((noinline))
#else
# define COROCC_NOINLINE
#endif
static COROCC_NOINLINE void worker_barrier_park(coro_rt_t* rt, rt_worker* w) {
    coro_plat_mutex_lock(&rt->act_lock);
    (void)coro_at_fetch_add((volatile uint64_t*)&rt->drv_parked, 1);
    coro_plat_cond_broadcast(&rt->act_cond);
    while (coro_at_load((volatile uint64_t*)&rt->drv_barrier) &&
           !coro_at_load((volatile uint64_t*)&w->stop)) {
        (void)coro_plat_cond_wait(&rt->act_cond, &rt->act_lock);
    }
    (void)coro_at_fetch_add((volatile uint64_t*)&rt->drv_parked,
                            (uint64_t)-1);
    coro_plat_mutex_unlock(&rt->act_lock);
}

static void worker_main(void* pv) {
    rt_worker* w = (rt_worker*)pv;
    coro_rt_t* rt = w->rt;

    /* Start handshake: the cross-class quiesce wait keys on drv_started so a
       worker the OS has not scheduled yet cannot look like a worker that
       refused to park. Paired with the decrement on the way out. */
    coro_plat_mutex_lock(&rt->act_lock);
    (void)coro_at_fetch_add((volatile uint64_t*)&rt->drv_started, 1);
    coro_plat_cond_broadcast(&rt->act_cond);
    coro_plat_mutex_unlock(&rt->act_lock);

    for (;;) {
        if (coro_at_load((volatile uint64_t*)&w->stop)) break;

        /* A cross-class driver switch (or the whole LOOP phase) parks every
           worker here while it migrates the runtime between execution models.
           The worker stays off the run queue and out of the timer heap until
           the switch clears the barrier; in LOOP it stays parked for as long as
           the host pumps worker[0] by hand. */
        if (coro_at_load((volatile uint64_t*)&rt->drv_barrier)) {
            worker_barrier_park(rt, w);
            continue;
        }

        /* One node per turn keeps the hot path's shape; the budget form is for
           the host-driven pump. */
        if (rt_run_some(rt, w, 1) > 0) continue;

        /* Nothing runnable anywhere. Sleep until work appears or the nearest
           deadline in this worker's own heap passes. */
        uint64_t deadline = 0;
        coro_timer_node* t = coro_timer_heap_top(&w->sleepers);
        if (t) deadline = t->deadline;
        worker_sleep(rt, w, deadline);
    }

    coro_plat_mutex_lock(&rt->act_lock);
    (void)coro_at_fetch_add((volatile uint64_t*)&rt->drv_started, (uint64_t)-1);
    coro_plat_mutex_unlock(&rt->act_lock);
}

/* ---- public API ---- */

static void w_stop(rt_worker* w) {
    coro_at_store((volatile uint64_t*)&w->stop, 1);
}

int rt_start_workers(coro_rt_t* rt) {
    size_t started = 0;
    for (; started < rt->nworkers; ++started) {
        if (coro_thread_create(&rt->workers[started].thread, worker_main,
                               &rt->workers[started]) != 0) {
            break;
        }
    }
    if (started != rt->nworkers) {
        /* Roll the partial start back so a caller that retries starts clean:
           stop and join what did start, then clear the stop flags. A started
           thread that reached worker_main also decremented drv_started as it
           left, so the handshake counter returns to rest. */
        for (size_t j = 0; j < started; ++j) w_stop(&rt->workers[j]);
        signal_activity(rt);
        for (size_t j = 0; j < started; ++j) {
            coro_thread_join(rt->workers[j].thread);
            coro_at_store((volatile uint64_t*)&rt->workers[j].stop, 0);
        }
        return 0;
    }
    coro_at_store((volatile uint64_t*)&rt->drv_threads, 1);
    return 1;
}

coro_rt_t* coro_rt_create(size_t nthreads) {
    coro_rt_t* rt = (coro_rt_t*)calloc(1, sizeof *rt);
    if (!rt) return NULL;

    if (nthreads == 0) {
        long n = 4;
#if defined(__linux__)
        n = sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_WIN32)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        n = (long)si.dwNumberOfProcessors;
#endif
        if (n < 1) n = 1;
        if (n > 64) n = 64;
        nthreads = (size_t)n;
    }

    /* The process default selects the execution family. A LOOP default means
       the runtime owns no worker threads: it has one pump context (worker[0])
       and the host drives it through coro_rt_pump. Built-in and SIGNAL keep the
       worker pool. Read once, here, before any thread starts. */
    coro_rt_driver_t* def = coro_rt_driver_default();
    unsigned kind = RT_DRV_BUILTIN;
    if (def) {
        kind = (def->ops.flags & CORO_DRIVER_FLAG_LOOP) ? RT_DRV_LOOP
                                                        : RT_DRV_SIGNAL;
    }
    size_t nworkers = (kind == RT_DRV_LOOP) ? 1 : nthreads;

    rt->nworkers = nworkers;
    rt->workers = (rt_worker*)calloc(nworkers, sizeof *rt->workers);
    if (!rt->workers) {
        free(rt);
        return NULL;
    }
    /* The Windows lock/condition pair (SRWLOCK/CONDITION_VARIABLE) is usable
       in its zero-initialized state, so no mutex init exists there; the
       condition init keeps the POSIX monotone-clock binding, which is what
       makes the absolute deadlines below share the timer heap's scale. */
#if !defined(_WIN32)
    pthread_mutex_init(&rt->act_lock, NULL);
    pthread_mutex_init(&rt->cache_lock, NULL);
    pthread_mutex_init(&rt->drv_lock, NULL);
#endif
    rt->cache_n = 0;
    coro_atp_store((volatile uintptr_t*)&rt->drv, (uintptr_t)def);
    rt->drv_kind = kind;
    rt->drv_gen = 0;
    coro_plat_cond_init(&rt->act_cond);
    coro_plat_cond_init(&rt->settled);

    for (size_t j = 0; j < nworkers; ++j) {
        rt_worker* w = &rt->workers[j];
        w->rt = rt;
        w->alarm_req = w; /* the opaque token the driver hands back */
        wsdeque_init(&w->queue);
        mpsc_init(&w->inbox);
        coro_timer_heap_init(&w->sleepers);
    }
    if (kind == RT_DRV_LOOP) return rt; /* host-driven; no worker threads */

    if (!rt_start_workers(rt)) {
        for (size_t j = 0; j < nworkers; ++j) {
            coro_timer_heap_destroy(&rt->workers[j].sleepers);
        }
#if !defined(_WIN32)
        pthread_cond_destroy(&rt->act_cond);
        pthread_cond_destroy(&rt->settled);
        pthread_mutex_destroy(&rt->act_lock);
        pthread_mutex_destroy(&rt->cache_lock);
        pthread_mutex_destroy(&rt->drv_lock);
#endif
        free(rt->workers);
        free(rt);
        return NULL;
    }
    return rt;
}

/* The worker a fresh spawn goes to: round-robin across the pool, or the single
   pump context in LOOP mode. */
static rt_worker* spawn_worker(coro_rt_t* rt) {
    if (rt->drv_kind == RT_DRV_LOOP) return &rt->workers[0];
    size_t i = rt->spawn_seq++ % rt->nworkers;
    return &rt->workers[i];
}

static coro_status rt_spawn_common(coro_rt_t* rt, const coro_attr_t* attr,
                                   coro_fn fn, void* arg, coro_t** out,
                                   int retained) {
    if (!rt || !fn) return CORO_EINVAL;
    if (out) *out = NULL;

    rt_node* n = (rt_node*)calloc(1, sizeof *n);
    if (!n) return CORO_ENOMEM;

    /* Default-allocator spawns may reuse a parked stack (and then skip the
       mmap/mprotect entirely); a custom allocator owns its memory and the
       pool never sees it, so those spawns go straight to coro_create. */
    coro_status st;
    coro_stack stk;
    if ((!attr || (!attr->allocator && attr->stack_size == 0)) &&
        rt_stack_pop(rt, &stk)) {
        st = coro_create_on_stack(&n->coro, attr, fn, arg, &stk);
        if (st != CORO_OK) rt_stack_push(rt, &stk); /* caller keeps it */
    } else {
        st = coro_create(&n->coro, attr, fn, arg);
    }
    if (st != CORO_OK) {
        free(n);
        return st;
    }
    coro_set_sched_tag(n->coro, n);
    n->tok = CORO_TOKEN_NONE;
    n->flags = retained ? RT_RETAINED : 0;
    n->worker = spawn_worker(rt);
    live_add(rt, 1);
    if (rt_queue_claim(n)) mpsc_push(&n->worker->inbox, n);
    signal_activity(rt);

    if (out) *out = n->coro;
    return CORO_OK;
}

coro_status coro_rt_spawn(coro_rt_t* rt, const coro_attr_t* attr, coro_fn fn,
                          void* arg) {
    return rt_spawn_common(rt, attr, fn, arg, NULL, 0);
}

coro_status coro_rt_spawn_h(coro_rt_t* rt, const coro_attr_t* attr, coro_fn fn,
                            void* arg, coro_t** out) {
    return rt_spawn_common(rt, attr, fn, arg, out, 1);
}

coro_status coro_rt_collect(coro_rt_t* rt, coro_t* c) {
    if (!rt || !c) return CORO_EINVAL;
    rt_node* n = (rt_node*)coro_get_sched_tag(c);
    if (!n) return CORO_EINVAL;
    /* The reclaim gate is an ACQUIRE load of RT_DONE. The retire
       published that bit with a RELEASE fetch_or as its LAST write to the
       node (inside act_lock, after the chain detach), so the acquire here
       is what makes the retire's earlier writes visible -- aarch64
       included. The gate never touches the runtime, so a collect that runs
       after coro_rt_destroy (a contract the public header keeps) has no
       freed-runtime dereference: only the coroutine handle and its node are
       read. The coroutine's own DEAD state is deliberately NOT a gate: the
        settle publishes it before the worker has retired the node, and
        freeing then raced the retire's own access to the node (a
        use-after-free). A coroutine that finished but whose retirement has
       not run (yet) therefore reports EBUSY; the caller retries -- from
       inside a coroutine, by yielding first, never by spinning -- and the
       retire completes promptly. */
    if (!node_test(n, RT_DONE)) return CORO_EBUSY;
    /* Pure reclamation: the live count was already decremented once, by the
       retire that ran when the coroutine finished. Decrementing here as well
       would count the same spawn twice and underflow the count, so collect
       only releases the handle, the coroutine and the bookkeeping. */
    coro_set_sched_tag(c, NULL);
    coro_destroy(c);
    free(n);
    return CORO_OK;
}

coro_status coro_rt_wait_all(coro_rt_t* rt) {
    if (!rt) return CORO_EINVAL;
    coro_plat_mutex_lock(&rt->act_lock);
    while (coro_at_load((volatile uint64_t*)&rt->live) > 0) {
        coro_plat_cond_wait(&rt->settled, &rt->act_lock);
    }
    coro_plat_mutex_unlock(&rt->act_lock);
    return CORO_OK;
}

coro_status coro_rt_join(coro_rt_t* rt, coro_t* c) {
    if (!rt || !c) return CORO_EINVAL;
    rt_node* target = (rt_node*)coro_get_sched_tag(c);
    if (!target) return CORO_EINVAL;
    if (node_test(target, RT_DONE)) return CORO_EDESTROYED;
    if (coro_get_state(c) == CORO_DEAD) return CORO_EDESTROYED;

    /* Called from inside a coroutine scheduled by this runtime: park on the
       target's wait chain and let the runtime keep making progress. */
    coro_t* cur = coro_runtime_current()->current;
    rt_node* self = cur ? (rt_node*)coro_get_sched_tag(cur) : NULL;
    if (!self || self == target) return CORO_EAGAIN;

    /* Append under the activity lock and re-check there, the slow-path shape
       the sync primitives use: a target that finished between the checks
       above and this append reports EDESTROYED instead of parking on a
       chain nobody will walk again, and the retire walk can never consume
       the chain between the check and the append. */
    coro_plat_mutex_lock(&rt->act_lock);
    if (node_test(target, RT_DONE) || coro_get_state(c) == CORO_DEAD) {
        coro_plat_mutex_unlock(&rt->act_lock);
        return CORO_EDESTROYED;
    }
    self->waiter_next = target->waiter_next;
    target->waiter_next = self;
    node_set(self, RT_WAITING);
    node_set(self, RT_PARKED);
    (void)rt_pending_requeue(self);   /* round-5: park consumes pending */
    coro_plat_mutex_unlock(&rt->act_lock);

    for (;;) {
    coro_status st = coro_yield();
    if (st != CORO_OK) {
        /* Cancelled before or at the park: the yield consumed the request
           without switching out, so this node is (or was a moment ago) on
           the target's chain. Detach it under the activity lock, exactly
           like every primitive park site does, or the target's retire would
           walk this node after the coroutine freed it. A retire that beat us
           has already unlinked the whole chain; detaching what is not there
           is a no-op. */
        coro_plat_mutex_lock(&rt->act_lock);
        rt_node** p = &target->waiter_next;
        while (*p) {
            if (*p == self) {
                *p = self->waiter_next;
                self->waiter_next = NULL;
                break;
            }
            p = &(*p)->waiter_next;
        }
        node_clear(self, RT_WAITING);
        /* RT_PARKED must go too: the driver reads it when this drive
           settles, and a stale marker makes it drop the node off the run
           queue with no releaser left to requeue it -- a following plain
           yield would strand the coroutine. The bit is only meaningful
           while the coroutine is between a park and its releaser's
           requeue; this branch leaves the node running and off every
           chain, so neither holds. (Same shape as sched_join's wait-flag
           clear in sched.c.) */
        node_clear(self, RT_PARKED);
        /* Retry: the target may have finished while the cancellation
           was in flight (it only ever surfaces here, at the safe point).
           The wait condition is then satisfied, so the join reports the
           outcome instead of the cancellation. */
        coro_status ret = st;
        if (node_test(target, RT_DONE) || coro_get_state(c) == CORO_DEAD) {
            ret = CORO_OK;
        }
        coro_plat_mutex_unlock(&rt->act_lock);
        return ret;
    }
    {
        /* BT genuineness: the retire walk unlinks a waiter before waking it;
           still being linked means a stale delivery resumed us, so re-park. */
        int still = 0;
        coro_plat_mutex_lock(&rt->act_lock);
        if (node_test(target, RT_DONE) || coro_get_state(c) == CORO_DEAD) {
            coro_plat_mutex_unlock(&rt->act_lock);
            return CORO_OK;
        }
        {
            rt_node** p = &target->waiter_next;
            while (*p) {
                if (*p == self) {
                    *p = self->waiter_next;
                    self->waiter_next = NULL;
                    still = 1;
                    break;
                }
                p = &(*p)->waiter_next;
            }
        }
        if (still) {
            self->waiter_next = target->waiter_next;
            target->waiter_next = self;
            node_set(self, RT_WAITING);
            node_set(self, RT_PARKED);
        }
        coro_plat_mutex_unlock(&rt->act_lock);
        if (!still) return CORO_OK;
    }
    }
}

size_t coro_rt_pending(const coro_rt_t* rt) {
    if (!rt) return 0;
    return (size_t)coro_at_load((volatile uint64_t*)&rt->live);
}

coro_status coro_rt_sleep(coro_rt_t* rt, uint64_t deadline) {
    if (!rt) return CORO_EINVAL;
    coro_t* cur = coro_runtime_current()->current;
    rt_node* n = cur ? (rt_node*)coro_get_sched_tag(cur) : NULL;
    if (!n) return CORO_ESTALE; /* not inside a runtime-scheduled coroutine */

    n->timer.deadline = deadline;
    for (;;) {
        node_set(n, RT_WAITING);
        node_set(n, RT_PARKED);
        node_set(n, RT_TIMED);
        (void)rt_pending_requeue(n);   /* round-5: park consumes pending */
        /* The heap belongs to the worker driving us; it is the only writer. */
        coro_timer_heap_push(&n->worker->sleepers, &n->timer);
        coro_status st = coro_yield();
        node_clear(n, RT_TIMED);
        if (st != CORO_OK) {
            /* Cancelled at the park's safe point: take the deadline out and
               retire the markers (checked no-op when the entry is gone). */
            coro_timer_heap_remove(&n->worker->sleepers, &n->timer);
            node_clear(n, RT_WAITING);
            node_clear(n, RT_PARKED);
            return st;
        }
        /* BT genuineness: a stale delivery can resume the sleep early; only
           a passed deadline makes the sleep complete. */
        if (rt_now(rt) >= deadline) {
            node_clear(n, RT_WAITING);
            node_clear(n, RT_PARKED);
            return CORO_OK;
        }
        coro_timer_heap_remove(&n->worker->sleepers, &n->timer);
        node_clear(n, RT_WAITING);
        node_clear(n, RT_PARKED);
    }
}

coro_status coro_rt_yield(void) {
    return coro_yield();
}

coro_status coro_rt_cancel(coro_rt_t* rt, coro_t* c) {
    if (!rt || !c) return CORO_EINVAL;
    rt_node* n = (rt_node*)coro_get_sched_tag(c);
    if (!n) return CORO_EINVAL;
    coro_status st = coro_cancel(c);
    if (st != CORO_OK) return st;
    /* If the coroutine is parked on a deadline, hand the node to its worker's
       inbox so it wakes now instead of at its deadline. The owner worker
       removes the heap entry under its own ownership. The claim is one atomic
       test-and-set on the flags word (the mirror of node_cas_clear): two
       concurrent cancels cannot both push the node, or the second push would
       relink mpsc next to a stale head and hang the worker's drain (the
       node's self-loop, n->next == n, hands it out over and over). */
    uint64_t cur = coro_at_load((volatile uint64_t*)&n->flags);
    for (;;) {
        /* BT: only a deadline park is requeued by cancellation (sync parks
           keep observing the request at their next releaser wake). */
        if (!(cur & RT_WAITING) || !(cur & RT_TIMED) || (cur & RT_WAKE_REQ)) {
            return CORO_OK;
        }
        uint64_t exp = cur;
        if (coro_at_cas((volatile uint64_t*)&n->flags, &exp,
                        cur | RT_WAKE_REQ)) {
            if (rt_queue_claim(n)) {
                mpsc_push(&n->worker->inbox, n);
                signal_activity(rt);
            } else {
                node_clear(n, RT_WAKE_REQ); /* live delivery absorbs it */
            }
            return CORO_OK;
        }
        cur = exp; /* lost: exp now holds the observed value */
    }
}

coro_status coro_rt_destroy(coro_rt_t* rt) {
    if (!rt) return CORO_OK;
    if (coro_at_load((volatile uint64_t*)&rt->live) > 0) return CORO_EBUSY;

    /* Stop and join the worker threads whenever they were created -- even when
       the runtime is currently in LOOP, where the workers are parked on the
       barrier rather than absent. Clearing the barrier and broadcasting releases
       a parked worker so it can observe its stop flag. A LOOP-created runtime
       never started threads, so this is skipped. */
    if (coro_at_load((volatile uint64_t*)&rt->drv_threads)) {
        for (size_t i = 0; i < rt->nworkers; ++i) w_stop(&rt->workers[i]);
        coro_at_store((volatile uint64_t*)&rt->drv_barrier, 0);
        signal_activity(rt);
        for (size_t i = 0; i < rt->nworkers; ++i) {
            coro_thread_join(rt->workers[i].thread);
        }
    }
    /* Withdraw any alarm still outstanding on an attached driver before the
       runtime -- which is the alarm callback's argument -- goes away. No lock
       is needed: every worker has stopped (or never started in LOOP mode). */
    for (size_t i = 0; i < rt->nworkers; ++i) {
        rt_worker* w = &rt->workers[i];
        if (w->alarm_armed) {
            coro_rt_driver_t* od = (coro_rt_driver_t*)w->alarm_driver;
            if (od && od->ops.timer_cancel) {
                (void)od->ops.timer_cancel(od->ctx, w->alarm_req);
            }
            w->alarm_armed = 0;
        }
        coro_timer_heap_destroy(&w->sleepers);
    }
    /* Workers are stopped, so no retire can be pushing: release the parked
       stacks the pool still holds. */
    coro_plat_mutex_lock(&rt->cache_lock);
    for (size_t i = 0; i < rt->cache_n; ++i) coro_stack_free(&rt->cache[i]);
    rt->cache_n = 0;
    coro_plat_mutex_unlock(&rt->cache_lock);
#if !defined(_WIN32)
    pthread_cond_destroy(&rt->act_cond);
    pthread_cond_destroy(&rt->settled);
    pthread_mutex_destroy(&rt->act_lock);
    pthread_mutex_destroy(&rt->cache_lock);
    pthread_mutex_destroy(&rt->drv_lock);
#endif
    free(rt->workers);
    free(rt);
    return CORO_OK;
}
