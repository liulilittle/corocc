/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/scheduler/sched.c -- single-thread run loop over coroutines.
 *
 * The scheduler owns three things per coroutine: its place in the runnable
 * queue, its deadline when it sleeps, and the wait chain of coroutines blocked
 * on it. Everything else -- the state machine, the credentials, the delivery
 * path -- is the coroutine layer, and this file only drives it.
 *
 * Two rules shape the loop:
 *   1. A coroutine that becomes runnable is *queued*, never jumped to. The
 *      thread that holds the credential drives it, which is this thread, so a
 *      timer firing or a waiter being released always ends in the queue and is
 *      picked up by the loop.
 *   2. The loop never spins. With nothing runnable it sleeps until either a
 *      cross-thread delivery arrives or the nearest timer expires, using the
 *      runtime's work signal.
 */
#include "scheduler/sched.h"

#include "arch/coro_atomic.h"
#include "coroutine/coro_internal.h"
#include "coroutine/coro_runtime.h"
#include "scheduler/timer.h"

#include <stddef.h>
#include <stdlib.h>

sched_node* sched_node_of(coro_t* c) {
    return (sched_node*)coro_get_sched_tag(c);
}

static int index_add(coro_sched_t* s, sched_node* n) {
    if (s->index_count == s->index_cap) {
        size_t cap = s->index_cap ? s->index_cap * 2 : 16;
        sched_node** grown =
            (sched_node**)realloc(s->index, cap * sizeof *grown);
        if (!grown) return 0; /* caller turns this into a failed spawn */
        s->index = grown;
        s->index_cap = cap;
    }
    s->index[s->index_count++] = n;
    return 1;
}

static void index_drop(coro_sched_t* s, sched_node* n) {
    for (size_t i = 0; i < s->index_count; ++i) {
        if (s->index[i] == n) {
            s->index[i] = s->index[s->index_count - 1];
            --s->index_count;
            return;
        }
    }
}

coro_status coro_sched_spawn_h(coro_sched_t* s, const coro_attr_t* attr,
                               coro_fn fn, void* arg, coro_t** out) {
    if (!s || !fn || !out) return CORO_EINVAL;

    sched_node* n = (sched_node*)calloc(1, sizeof *n);
    if (!n) return CORO_ENOMEM;

    coro_status st = coro_create(&n->coro, attr, fn, arg);
    if (st != CORO_OK) {
        free(n);
        return st;
    }
    coro_set_sched_tag(n->coro, n);
    n->tok = CORO_TOKEN_NONE;
    n->done = 0;
    n->waiter_next = NULL;
    n->timer.deadline = 0;
    /* Register before queueing: the node must be counted the moment it is
       runnable, or destroy() could tear the scheduler down under a node that
       is already on the run queue. A failed registration is a clean spawn
       failure -- nothing has been queued yet. */
    if (!index_add(s, n)) {
        coro_destroy(n->coro);
        free(n);
        return CORO_ENOMEM;
    }
    sched_enqueue(s, n);
    *out = n->coro;
    return CORO_OK;
}

coro_status coro_sched_spawn(coro_sched_t* s, const coro_attr_t* attr,
                             coro_fn fn, void* arg) {
    coro_t* ignore = NULL;
    return coro_sched_spawn_h(s, attr, fn, arg, &ignore);
}

void sched_enqueue(coro_sched_t* s, sched_node* n) {
    n->next = NULL;
    if (s->runq_tail) {
        s->runq_tail->next = n;
    } else {
        s->runq_head = n;
    }
    s->runq_tail = n;
}

static sched_node* runq_take(coro_sched_t* s) {
    sched_node* n = s->runq_head;
    if (!n) return NULL;
    s->runq_head = n->next;
    if (!s->runq_head) s->runq_tail = NULL;
    n->next = NULL;
    return n;
}

/* Put a node back on the run queue and retire its one-shot park marker. A
   node that is queued is runnable again, so the park (a sleep deadline or a
   join) it was in is over: leaving the marker set would make the settle step
   below drop it after its next plain yield, stranding the coroutine. Every
   requeue that ends a park goes through here; a plain yield (marker already
   clear) uses sched_enqueue directly. */
static void sched_make_runnable(coro_sched_t* s, sched_node* n) {
    coro_at_store(&n->waiting, 0);
    sched_enqueue(s, n);
}

/* Unlink a node from the owner's wake list before it leaves the scheduler's
   ownership (a retire or a lost claim frees it). Runs on the owner thread
   under wake_lock, mutually exclusive with sched_drain_wakes' pop. The tail
   is rolled back to its predecessor when the removed node was the tail, so
   wake_tail never dangles. Idempotent: a node not on the list is untouched. */
static void sched_wake_unlink(coro_sched_t* s, sched_node* n) {
    coro_plat_mutex_lock(&s->wake_lock);
    if (n->wake_pending) {
        sched_node** p = &s->wake_head;
        sched_node* prev = NULL;
        while (*p) {
            if (*p == n) {
                *p = n->wake_next;
                if (s->wake_tail == n) s->wake_tail = prev;
                break;
            }
            prev = *p;
            p = &(*p)->wake_next;
        }
        n->wake_next = NULL;
        n->wake_pending = 0;
    }
    coro_plat_mutex_unlock(&s->wake_lock);
}

/* Drive a node to its next park (or its end). The first turn of a coroutine
   starts it: the credential that claims a never-started coroutine is the
   implicit "never started" one, which coro_resume rejects as invalid. */
coro_status sched_claim(sched_node* n, coro_token* next) {
    if (n->started) return coro_resume(n->coro, n->tok, next);
    n->started = 1;
    return coro_start(n->coro, next);
}

void sched_collect_expired(coro_sched_t* s) {
    uint64_t now = coro_monotonic_ns();
    for (;;) {
        coro_timer_node* t;
        sched_node* n;
        coro_plat_mutex_lock(&s->timer_lock);
        t = coro_timer_heap_top(&s->sleepers);
        if (!t || t->deadline > now) {
            coro_plat_mutex_unlock(&s->timer_lock);
            return;
        }
        t = coro_timer_heap_pop(&s->sleepers);
        coro_plat_mutex_unlock(&s->timer_lock);
        /* the timer node is embedded in the bookkeeping node, so the address
           of the owning node is the timer's address minus that offset */
        n = (sched_node*)(void*)((char*)t - offsetof(sched_node, timer));
        sched_make_runnable(s, n); /* queue it, never jump to it */
    }
}

/* A node whose coroutine finished: release the wait chain and drop it. */
static void retire_node(coro_sched_t* s, sched_node* n) {
    sched_node* waiter = n->waiter_next;
    /* A finishing coroutine may still hold a deadline if it was cancelled
       between the timer firing and its next drive; leaving it in the heap
       would later hand the loop a pointer to this freed node. */
    coro_plat_mutex_lock(&s->timer_lock);
    (void)coro_timer_heap_remove(&s->sleepers, &n->timer);
    coro_plat_mutex_unlock(&s->timer_lock);
    /* Unlink the whole chain first: each waiter is resumed exactly once, no
       matter how many of them were waiting. */
    n->waiter_next = NULL;
    while (waiter) {
        sched_node* next = waiter->waiter_next;
        waiter->waiter_next = NULL;
        sched_make_runnable(s, waiter); /* it is runnable again */
        waiter = next;
    }
    n->done = 1;
    index_drop(s, n);
    /* A cancel may have filed a wake request for this node while it was
       running (the request outlives the drive): take it off the wake list
       before the node is freed, or the next drain would pop freed memory. */
    sched_wake_unlink(s, n);
    coro_destroy(n->coro);
    free(n);
}

/* Park the running node on a target's wait chain. */
coro_status sched_join(coro_sched_t* s, sched_node* target) {
    sched_node* self = s ? s->running : NULL;
    if (!s || !target) return CORO_EINVAL;
    if (!self) return CORO_ESTALE; /* not running on this scheduler */
    if (target == self) return CORO_EINVAL; /* waiting on oneself deadlocks */

    if (target->done) return CORO_EDESTROYED;

    /* Link at the head; the retire path unlinks the whole chain. The chain
       is only ever mutated by this thread's run loop, so the link is atomic
       with respect to any concurrent retire. */
    self->waiter_next = target->waiter_next;
    target->waiter_next = self;
    coro_at_store(&self->waiting, 1); /* a join is a wait, so no round-robin requeue */
    coro_status st = coro_yield();
    if (st != CORO_OK) {
        /* Cancelled before or at the park: the yield consumed the request
           without switching out, so this node is still linked on the
           target's chain. Unlink it -- a later retire would walk this node
           after it was freed -- and clear the wait flag so a following
           plain yield requeues normally. A retire that ran in the same
           instant has already unlinked the whole chain; detaching what is
           not there is a no-op. */
        sched_node** p = &target->waiter_next;
        while (*p) {
            if (*p == self) {
                *p = self->waiter_next;
                self->waiter_next = NULL;
                break;
            }
            p = &(*p)->waiter_next;
        }
        coro_at_store(&self->waiting, 0);
        return st;
    }
    return CORO_OK;
}

coro_status coro_sched_join(coro_sched_t* s, coro_t* c) {
    sched_node* target;
    if (!s || !c) return CORO_EINVAL;
    target = sched_node_of(c);
    if (!target) return CORO_EINVAL;
    /* A target that already finished reports that instead of parking. */
    if (target->done) return CORO_EDESTROYED;
    return sched_join(s, target);
}

coro_status coro_sched_wait_all(coro_sched_t* s) {
    return coro_sched_run(s);
}

coro_status sched_sleep(coro_sched_t* s, uint64_t deadline) {
    sched_node* n = s ? s->running : NULL;
    if (!n) return CORO_ESTALE; /* not inside a scheduler-run coroutine */
    if (!s) return CORO_EINVAL;

    n->timer.deadline = deadline;
    coro_plat_mutex_lock(&s->timer_lock);
    int armed = (coro_timer_heap_push(&s->sleepers, &n->timer) == 0);
    coro_plat_mutex_unlock(&s->timer_lock);
    if (!armed) {
        /* Out of memory arming the deadline: parking now would strand the
           node forever (nothing would ever wake it). Report the failure and
           leave the node runnable -- the settle step requeues it. */
        return CORO_ENOMEM;
    }
    /* A sleep is a wait, not a voluntary yield: the node stays off the run
       queue until its deadline fires, so the loop must not requeue it the way
       it requeues a plain yield. */
    coro_at_store(&n->waiting, 1);
    coro_status st = coro_yield();
    if (st != CORO_OK) {
        /* Cancelled at the pre-park safe point: coro_yield returned without
           parking, so the deadline pushed just above is still in the heap.
           Take it out -- the next sleep would otherwise push the same embedded
           node a second time and corrupt the heap -- and retire the park
           marker so a following plain yield round-robins again. When the
           cancellation instead landed after the park, the wake path already
           removed the deadline and cleared the marker, so both steps are
           refused no-ops here. */
        coro_plat_mutex_lock(&s->timer_lock);
        (void)coro_timer_heap_remove(&s->sleepers, &n->timer);
        coro_plat_mutex_unlock(&s->timer_lock);
        coro_at_store(&n->waiting, 0);
    }
    return st;
}

void sched_wake_request(coro_sched_t* s, sched_node* n) {
    if (!s || !n) return;
    /* Only a parked node (sleeping on a deadline, or waiting on a join) needs
       a wake request: a running or already-runnable node observes the
       cancellation at its next safe point on its own, and filing a request for
       it would only leave a stale wake-list entry to be discarded. waiting is
       read atomically -- it is written by the owner. */
    if (coro_at_load(&n->waiting) == 0) return;
    /* Cross-thread entry point: it must not touch the run queue or the timer
       heap (the owner writes them without a common lock). It only appends the
       node to the owner-private wake list, de-duplicating a node already
       queued, and signals the loop -- which then does the removal and requeue
       on its own thread. */
    coro_plat_mutex_lock(&s->wake_lock);
    if (!n->wake_pending) {
        n->wake_pending = 1;
        n->wake_next = NULL;
        if (s->wake_tail) {
            s->wake_tail->wake_next = n;
        } else {
            s->wake_head = n;
        }
        s->wake_tail = n;
    }
    coro_plat_mutex_unlock(&s->wake_lock);
    /* The run loop may be sleeping until this deadline; the deadline is about
       to be cancelled, so wake the thread that runs the loop. The runtime
       pointer is read through the pointer-width atomic helpers because it is
       published by the owner (coro_sched_run) and read here from any thread. */
    coro_runtime_signal_work(
        (coro_runtime*)coro_atp_load((const volatile uintptr_t*)&s->rt));
}

void sched_drain_wakes(coro_sched_t* s) {
    for (;;) {
        sched_node* n;
        coro_plat_mutex_lock(&s->wake_lock);
        n = s->wake_head;
        if (n) {
            s->wake_head = n->wake_next;
            if (!s->wake_head) s->wake_tail = NULL;
            n->wake_next = NULL;
            n->wake_pending = 0;
        }
        coro_plat_mutex_unlock(&s->wake_lock);
        if (!n) return;

        /* Owner side: remove the deadline (if any) and requeue. Removal
           refuses a node that is not in the heap, which is exactly the state
           of a node whose deadline already fired, one already requeued, or one
           parked on a join chain: such a node is either runnable already or
           stays parked for its own releaser, and is left untouched. */
        coro_plat_mutex_lock(&s->timer_lock);
        if (coro_timer_heap_remove(&s->sleepers, &n->timer) == 0) {
            sched_make_runnable(s, n);
        }
        coro_plat_mutex_unlock(&s->timer_lock);
    }
}

coro_status coro_sched_cancel(coro_sched_t* s, coro_t* c) {
    if (!s || !c) return CORO_EINVAL;
    sched_node* n = sched_node_of(c);
    if (!n) return CORO_EINVAL;
    /* The request lands where any other cancellation lands; the coroutine
       observes it at its next safe point. If it is asleep, the loop's thread
       takes its deadline out of the heap and requeues it so that safe point
       comes now; this thread only files the request. */
    coro_status st = coro_cancel(c);
    if (st == CORO_OK) sched_wake_request(s, n);
    return st;
}

coro_status coro_sched_sleep(coro_sched_t* s, uint64_t deadline) {
    if (!s) return CORO_EINVAL;
    return sched_sleep(s, deadline);
}

coro_sched_t* coro_sched_create(void) {
    coro_sched_t* s = (coro_sched_t*)calloc(1, sizeof *s);
    if (!s) return NULL;
#if !defined(_WIN32)
    /* SRWLOCK (Windows) is ready zero-initialized; POSIX needs the init. */
    pthread_mutex_init(&s->timer_lock, NULL);
    pthread_mutex_init(&s->wake_lock, NULL);
#endif
    coro_timer_heap_init(&s->sleepers);
    return s;
}

coro_status coro_sched_run(coro_sched_t* s) {
    if (!s) return CORO_EINVAL;
    /* The thread that runs the loop. Published through the pointer-width
       atomic helpers because a cross-thread cancel reads it to signal work. */
    coro_atp_store((volatile uintptr_t*)&s->rt,
                   (uintptr_t)coro_runtime_current());

    for (;;) {
        /* Cross-thread wakeups addressed to this thread are served here, which
           is where the coroutine layer says they must be handled. */
        (void)coro_pump();

        /* Cross-thread cancellation requests are served here too, on the
           owner thread, so the run queue and the timer heap stay
           single-writer. */
        sched_drain_wakes(s);

        sched_collect_expired(s);

        sched_node* n = runq_take(s);
        if (n) {
            coro_token next = CORO_TOKEN_NONE;
            coro_status st;
            s->running = n;
            st = sched_claim(n, &next);
            s->running = NULL;
            if (st == CORO_OK) {
                if (next == CORO_TOKEN_DEAD) {
                    retire_node(s, n);
                } else {
                    /* A yield is a voluntary giving-up of the CPU: the
                       coroutine goes back to the tail of the run queue, which
                       makes the loop round-robin. Waiting -- sleeping on a
                       timer or joining another coroutine -- does NOT requeue;
                       whatever was waited on re-enqueues it when it is ready,
                       which is what the waiting flag records. */
                    n->tok = next;
                    if (!coro_at_load(&n->waiting)) sched_enqueue(s, n);
                }
            } else {
                /* The claim was lost -- the coroutine was cancelled or driven
                   elsewhere. It is no longer ours to park. Drop any pending
                   wake request before the node is freed. */
                index_drop(s, n);
                sched_wake_unlink(s, n);
                coro_destroy(n->coro);
                free(n);
            }
            continue;
        }

        /* Nothing runnable. A coroutine parked at a yield with no wakeup
           pending is idle by design, and a sleeping one has a deadline in the
           heap: sleep until the nearest deadline or a delivery, then sweep. */
        coro_timer_node* t;
        coro_plat_mutex_lock(&s->timer_lock);
        t = coro_timer_heap_top(&s->sleepers);
        coro_plat_mutex_unlock(&s->timer_lock);
        if (t) {
            coro_runtime_wait_work_until(s->rt, t->deadline);
            continue;
        }
        return CORO_OK;
    }
}

size_t coro_sched_pending(const coro_sched_t* s) {
    return s ? s->index_count : 0;
}

coro_status coro_sched_destroy(coro_sched_t* s) {
    if (!s) return CORO_OK;
    if (s->index_count > 0) return CORO_EBUSY;
    coro_timer_heap_destroy(&s->sleepers);
    free(s->index);
    free(s);
    return CORO_OK;
}
