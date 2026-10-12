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

static void index_add(coro_sched_t* s, sched_node* n) {
    if (s->index_count == s->index_cap) {
        size_t cap = s->index_cap ? s->index_cap * 2 : 16;
        sched_node** grown =
            (sched_node**)realloc(s->index, cap * sizeof *grown);
        if (!grown) return; /* tracked in pending(); the caller notices */
        s->index = grown;
        s->index_cap = cap;
    }
    s->index[s->index_count++] = n;
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

coro_status coro_sched_spawn(coro_sched_t* s, const coro_attr_t* attr,
                             coro_fn fn, void* arg) {
    if (!s || !fn) return CORO_EINVAL;

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
    index_add(s, n);
    sched_enqueue(s, n);
    return CORO_OK;
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
        coro_timer_node* t = coro_timer_heap_top(&s->sleepers);
        if (!t || t->deadline > now) return;
        t = coro_timer_heap_pop(&s->sleepers);
        /* the timer node is embedded in the bookkeeping node, so the address
           of the owning node is the timer's address minus that offset */
        sched_node* n = (sched_node*)(void*)((char*)t - offsetof(sched_node, timer));
        sched_enqueue(s, n); /* queue it, never jump to it */
    }
}

/* A node whose coroutine finished: release the wait chain and drop it. */
static void retire_node(coro_sched_t* s, sched_node* n) {
    sched_node* waiter = n->waiter_next;
    while (waiter) {
        sched_node* next = waiter->waiter_next;
        waiter->waiter_next = NULL;
        sched_enqueue(s, waiter); /* wake: the join returns on its next turn */
        waiter = next;
    }
    n->waiter_next = NULL;
    n->done = 1;
    index_drop(s, n);
    coro_destroy(n->coro);
    free(n);
}

coro_sched_t* coro_sched_create(void) {
    coro_sched_t* s = (coro_sched_t*)calloc(1, sizeof *s);
    if (!s) return NULL;
    coro_timer_heap_init(&s->sleepers);
    return s;
}

coro_status coro_sched_run(coro_sched_t* s) {
    if (!s) return CORO_EINVAL;

    for (;;) {
        /* Cross-thread wakeups addressed to this thread are served here, which
           is where the coroutine layer says they must be handled. */
        (void)coro_pump();

        sched_collect_expired(s);

        sched_node* n = runq_take(s);
        if (n) {
            coro_token next = CORO_TOKEN_NONE;
            coro_status st = sched_claim(n, &next);
            if (st == CORO_OK) {
                if (next == CORO_TOKEN_DEAD) {
                    retire_node(s, n);
                } else {
                    /* A yield is a voluntary giving-up of the CPU: the
                       coroutine goes back to the tail of the run queue, which
                       makes the loop round-robin. Waiting -- sleeping on a
                       timer or joining another coroutine -- does NOT requeue;
                       whatever was waited on re-enqueues it when it is ready. */
                    n->tok = next;
                    sched_enqueue(s, n);
                }
            } else {
                /* The claim was lost -- the coroutine was cancelled or driven
                   elsewhere. It is no longer ours to park. */
                index_drop(s, n);
                coro_destroy(n->coro);
                free(n);
            }
            continue;
        }

        /* Nothing runnable. A coroutine parked at a yield with no wakeup
           pending is idle by design, and a sleeping one has a deadline in the
           heap: sleep until the nearest deadline or a delivery, then sweep. */
        coro_timer_node* t = coro_timer_heap_top(&s->sleepers);
        if (t) {
            coro_runtime_wait_work_until(coro_runtime_current(), t->deadline);
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
