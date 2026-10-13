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

#include "coroutine/coro_internal.h"

#include <stddef.h>
#include <stdlib.h>
#include <time.h>

#if defined(__linux__)
#include <unistd.h>
#endif

#if defined(_WIN32)
#error "corocc: the M:N runtime arrives on Windows with milestone M9"
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

/* ---- live count (atomic) ---- */

static void live_add(coro_rt_t* rt, long d) {
    (void)coro_at_fetch_add((volatile uint64_t*)&rt->live, (uint64_t)(long long)d);
}

static void live_notify(coro_rt_t* rt) {
    if (coro_at_load((volatile uint64_t*)&rt->live) == 0) {
        pthread_mutex_lock(&rt->act_lock);
        ++rt->act_gen;
        pthread_cond_broadcast(&rt->act_cond);
        pthread_cond_broadcast(&rt->settled);
        pthread_mutex_unlock(&rt->act_lock);
    }
}

/* Wake every idle worker: bump the generation under the activity lock and
   broadcast, so a producer that raises work always wakes the sleepers. */
static void signal_activity(coro_rt_t* rt) {
    pthread_mutex_lock(&rt->act_lock);
    ++rt->act_gen;
    pthread_cond_broadcast(&rt->act_cond);
    pthread_mutex_unlock(&rt->act_lock);
}

/* Requeue a parked node onto its owner worker and raise the idle signal. The
   node must be off any wait chain; the caller owns that invariant. The WAITING
   bit is the single-requeue gate against the worker that drove the node into
   the park: exactly one of the two enqueues the node, so it can never be
   queued twice and driven by two workers at once. */
void rt_node_wake(coro_rt_t* rt, rt_node* n) {
    if (node_cas_clear(n, RT_WAITING)) {
        mpsc_push(&n->worker->inbox, n);
    }
    /* Else the driver already requeued the node locally; waking is done. */
    signal_activity(rt);
}

/* ---- worker queues ---- */

/* A worker that owns an inbox turns every drained node into a deque entry. */
static void drain_inbox(rt_worker* w) {
    rt_node* n;
    while ((n = mpsc_pop(&w->inbox)) != NULL) {
        if (node_test(n, RT_WAKE_REQ)) {
            node_clear(n, RT_WAKE_REQ);
            if (coro_timer_heap_remove(&w->sleepers, &n->timer) == 0) {
                /* Single-requeue gate: a head-of-timer expiration may win the
                   same bit for the very node. */
                if (node_cas_clear(n, RT_WAITING)) {
                    wsdeque_push(&w->queue, n);
                }
            }
            continue;
        }
        wsdeque_push(&w->queue, n);
    }
}

/* Move every node whose deadline has passed from this worker's timer heap to
   its runnable deque. Owner-only. */
static void collect_expired(rt_worker* w) {
    uint64_t now = coro_monotonic_ns();
    for (;;) {
        coro_timer_node* t = coro_timer_heap_top(&w->sleepers);
        if (!t || t->deadline > now) return;
        t = coro_timer_heap_pop(&w->sleepers);
        rt_node* n = (rt_node*)(void*)((char*)t - offsetof(rt_node, timer));
        /* Single-requeue gate against a concurrent cancel/wake of the node. */
        if (node_cas_clear(n, RT_WAITING)) {
            wsdeque_push(&w->queue, n);
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
    rt_node* waiter = n->waiter_next;
    n->waiter_next = NULL;
    while (waiter) {
        rt_node* next = waiter->waiter_next;
        waiter->waiter_next = NULL;
        rt_node_wake(rt, waiter);
        waiter = next;
    }
    node_set(n, RT_DONE);
    live_add(rt, -1);
    live_notify(rt);
    if (!node_test(n, RT_RETAINED)) {
        coro_destroy(n->coro);
        free(n);
    }
}

/* ---- worker loop ---- */

static void worker_main(void* pv) {
    rt_worker* w = (rt_worker*)pv;
    coro_rt_t* rt = w->rt;

    for (;;) {
        rt_node* n;
        if (coro_at_load((volatile uint64_t*)&w->stop)) break;

        collect_expired(w);
        drain_inbox(w);

        n = wsdeque_pop(&w->queue);
        if (!n) {
            size_t first = (size_t)(w - rt->workers);
            for (size_t i = 1; i < rt->nworkers; ++i) {
                rt_worker* v = &rt->workers[(first + i) % rt->nworkers];
                n = wsdeque_steal(&v->queue);
                if (n) break;
            }
        }

        if (n) {
            coro_token next = CORO_TOKEN_NONE;
            coro_status st;
            n->worker = w;
            w->running = n;
            st = rt_claim(n, &next);
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
                           on the queue immediately. */
                        node_clear(n, RT_PARKED);
                    } else {
                        wsdeque_push(&w->queue, n);
                    }
                }
            }
            /* The claim was lost (CORO_EBUSY busy, CORO_ESTALE parked at a
               newer token, or CORO_EDESTROYED already dead): the driver that
               holds the coroutine owns its requeue and its reclaim, so this
               worker must not touch the node -- destroying it here would free
               a live coroutine. */
            continue;
        }

        /* Nothing runnable anywhere. Sleep until work appears or the nearest
           deadline in this worker's own heap passes. The generation is read
           under the activity lock, the same lock a producer bumps under, so a
           wake that lands between the scan and this check moves the
           generation and sends us back to scan instead of blocking forever. */
        uint64_t deadline = 0;
        coro_timer_node* t = coro_timer_heap_top(&w->sleepers);
        if (t) deadline = t->deadline;

        pthread_mutex_lock(&rt->act_lock);
        if (!coro_at_load((volatile uint64_t*)&w->stop) &&
            wsdeque_empty(&w->queue) && mpsc_empty(&w->inbox)) {
            struct timespec ts;
            if (deadline) {
                ts.tv_sec = (time_t)(deadline / 1000000000ull);
                ts.tv_nsec = (long)(deadline % 1000000000ull);
                pthread_cond_timedwait(&rt->act_cond, &rt->act_lock, &ts);
            } else {
                pthread_cond_wait(&rt->act_cond, &rt->act_lock);
            }
        }
        pthread_mutex_unlock(&rt->act_lock);
    }
}

/* ---- public API ---- */

static void w_stop(rt_worker* w) {
    coro_at_store((volatile uint64_t*)&w->stop, 1);
}

coro_rt_t* coro_rt_create(size_t nthreads) {
    coro_rt_t* rt = (coro_rt_t*)calloc(1, sizeof *rt);
    if (!rt) return NULL;

    if (nthreads == 0) {
        long n = 4;
#if defined(__linux__)
        n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
        if (n < 1) n = 1;
        if (n > 64) n = 64;
        nthreads = (size_t)n;
    }

    rt->nworkers = nthreads;
    rt->workers = (rt_worker*)calloc(nthreads, sizeof *rt->workers);
    if (!rt->workers) {
        free(rt);
        return NULL;
    }
    pthread_mutex_init(&rt->act_lock, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&rt->act_cond, &ca);
    pthread_cond_init(&rt->settled, &ca);
    pthread_condattr_destroy(&ca);

    size_t started = 0;
    for (; started < nthreads; ++started) {
        rt_worker* w = &rt->workers[started];
        w->rt = rt;
        wsdeque_init(&w->queue);
        mpsc_init(&w->inbox);
        coro_timer_heap_init(&w->sleepers);
        if (coro_thread_create(&w->thread, worker_main, w) != 0) break;
    }
    if (started != nthreads) {
        for (size_t j = 0; j < started; ++j) w_stop(&rt->workers[j]);
        signal_activity(rt);
        for (size_t j = 0; j < started; ++j) {
            coro_thread_join(rt->workers[j].thread);
            coro_timer_heap_destroy(&rt->workers[j].sleepers);
        }
        pthread_cond_destroy(&rt->act_cond);
        pthread_cond_destroy(&rt->settled);
        pthread_mutex_destroy(&rt->act_lock);
        free(rt->workers);
        free(rt);
        return NULL;
    }
    return rt;
}

/* The worker a fresh spawn goes to: round-robin across the pool. */
static rt_worker* spawn_worker(coro_rt_t* rt) {
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

    coro_status st = coro_create(&n->coro, attr, fn, arg);
    if (st != CORO_OK) {
        free(n);
        return st;
    }
    coro_set_sched_tag(n->coro, n);
    n->tok = CORO_TOKEN_NONE;
    n->flags = retained ? RT_RETAINED : 0;
    n->worker = spawn_worker(rt);
    live_add(rt, 1);
    mpsc_push(&n->worker->inbox, n);
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
    if (coro_get_state(c) != CORO_DEAD) return CORO_EBUSY;
    node_set(n, RT_DONE);
    live_add(rt, -1);
    live_notify(rt);
    coro_set_sched_tag(c, NULL);
    coro_destroy(c);
    free(n);
    return CORO_OK;
}

coro_status coro_rt_wait_all(coro_rt_t* rt) {
    if (!rt) return CORO_EINVAL;
    pthread_mutex_lock(&rt->act_lock);
    while (coro_at_load((volatile uint64_t*)&rt->live) > 0) {
        pthread_cond_wait(&rt->settled, &rt->act_lock);
    }
    pthread_mutex_unlock(&rt->act_lock);
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
    self->waiter_next = target->waiter_next;
    target->waiter_next = self;
    node_set(self, RT_WAITING);
    node_set(self, RT_PARKED);
    return coro_yield();
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
    node_set(n, RT_WAITING);
    node_set(n, RT_PARKED);
    /* The heap belongs to the worker driving us; it is the only writer. */
    coro_timer_heap_push(&n->worker->sleepers, &n->timer);
    return coro_yield();
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
       removes the heap entry under its own ownership. */
    if (node_test(n, RT_WAITING) && !node_test(n, RT_WAKE_REQ)) {
        node_set(n, RT_WAKE_REQ);
        mpsc_push(&n->worker->inbox, n);
        signal_activity(rt);
    }
    return CORO_OK;
}

coro_status coro_rt_destroy(coro_rt_t* rt) {
    if (!rt) return CORO_OK;
    if (coro_at_load((volatile uint64_t*)&rt->live) > 0) return CORO_EBUSY;

    for (size_t i = 0; i < rt->nworkers; ++i) w_stop(&rt->workers[i]);
    signal_activity(rt);
    for (size_t i = 0; i < rt->nworkers; ++i) {
        coro_thread_join(rt->workers[i].thread);
        coro_timer_heap_destroy(&rt->workers[i].sleepers);
    }
    pthread_cond_destroy(&rt->act_cond);
    pthread_cond_destroy(&rt->settled);
    pthread_mutex_destroy(&rt->act_lock);
    free(rt->workers);
    free(rt);
    return CORO_OK;
}