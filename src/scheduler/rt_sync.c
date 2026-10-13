/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/scheduler/rt_sync.c -- coroutine synchronization primitives (L2).
 *
 * A runtime schedules M workers over N coroutines; the primitives here let
 * coroutines wait for each other *inside* the runtime instead of blocking a
 * worker thread. Every park is a coroutine yield: the waiting coroutine
 * leaves the run queue, its worker keeps driving other coroutines, and the
 * waiter is requeued (via its worker's MPSC inbox) by the coroutine that
 * releases the resource -- an idle worker never spins and a single-core
 * runtime cannot deadlock.
 *
 * The chains are FIFO and every chain mutation happens under the runtime's
 * activity lock, the same lock that guards the idle/settled condition and the
 * wake generation. Handing a node to its worker is done with rt_node_wake,
 * which shares the exact wake path the scheduler uses for timed and join
 * wakeups, so a notified waiter is resumed exactly once.
 *
 * Waiting protocol. A node's RT_WAITING bit doubles as its "on a chain"
 * marker: a waiter appends itself at most once, then yields; the releaser
 * pops the FIFO head and wakes it; the woken coroutine clears the bit, re-
 * attempts the take (mutex lock / semaphore decrement) and re-parks if it
 * loses the race. Because the chain pop and the wake share the timetable with
 * the append, no wake is lost and no resource is double-handed.
 *
 * Uncontended acquires are CAS-fast (mutex::lock, sem::trywait, sem::wait on
 * a positive count); only the contended path parks.
 */
#include "scheduler/rt.h"

#include "coroutine/coro_internal.h"

/* ---- local helpers ---------------------------------------------------- */

/* RT_WAITING bit double marker: test/set/clear on the node's flag word. */
static void node_set(rt_node* n, unsigned bit) {
    (void)coro_at_or((volatile uint64_t*)&n->flags, bit);
}
static void node_clear(rt_node* n, unsigned bit) {
    (void)coro_at_and((volatile uint64_t*)&n->flags, ~(uint64_t)bit);
}

/* Append n to the FIFO chain q (assumes the activity lock is held). */
static void q_append(void** q, rt_node* n) {
    n->waiter_next = NULL;
    if (!*q) {
        *q = (void*)n;
        return;
    }
    rt_node* t = (rt_node*)*q;
    while (t->waiter_next) t = t->waiter_next;
    t->waiter_next = n;
}

/* Pop the FIFO head of q (assumes the activity lock is held). Clears the
   popped node's link so a stale chain tail cannot be walked again (retire_node
   traverses waiter_next; a leftover link would let a sync chain bleed into a
   join chain and wake a phantom). */
static rt_node* q_pop(void** q) {
    rt_node* n = (rt_node*)*q;
    if (n) {
        *q = (void*)n->waiter_next;
        n->waiter_next = NULL;
    }
    return n;
}

/* The current coroutine's scheduler node, or NULL outside the runtime. */
static rt_node* rt_current_node(void) {
    coro_t* cur = coro_runtime_current()->current;
    if (!cur) return NULL;
    return (rt_node*)coro_get_sched_tag(cur);
}

/* Append self to *chain and mark it parked; the caller must hold act_lock.
   RT_PARKED tells the driver to leave the node off the run queue (the releaser
   owns the requeue); RT_WAITING is the releaser's single-requeue gate. */
static void rt_chain_park(void** chain, rt_node* self) {
    q_append(chain, self);
    node_set(self, RT_WAITING);
    node_set(self, RT_PARKED);
}

/* Detach self from the FIFO chain, if still present (assumes act_lock held
   by the caller). Returns whether self was found on the chain. */
static void rt_detach(void** chain, rt_node* self) {
    rt_node** p = (rt_node**)chain;
    while (*p) {
        if (*p == self) {
            *p = self->waiter_next;
            self->waiter_next = NULL;
            node_clear(self, RT_WAITING);
            return;
        }
        p = &(*p)->waiter_next;
    }
}

/* ---- mutex ------------------------------------------------------------- */

void coro_rt_mutex_init(coro_rt_mutex_t* m) {
    if (!m) return;
    m->locked = 0;
    m->waitq = NULL;
}

coro_status coro_rt_mutex_trylock(coro_rt_t* rt, coro_rt_mutex_t* m) {
    if (!rt || !m) return CORO_EINVAL;
    uint64_t cur = 0;
    if (coro_at_cas(&m->locked, &cur, 1)) return CORO_OK;
    return CORO_EBUSY;
}

coro_status coro_rt_mutex_lock(coro_rt_t* rt, coro_rt_mutex_t* m) {
    if (!rt || !m) return CORO_EINVAL;
    rt_node* self = rt_current_node();
    if (!self) return CORO_ESTALE;

    for (;;) {
        uint64_t cur = 0;
        if (coro_at_cas(&m->locked, &cur, 1)) return CORO_OK;

        /* Slow path. unlock() clears `locked` and drains the wait chain while
           holding the activity lock; if our failed CAS and our append were not
           serialized against it, the unlock could run in between and strand us
           on the chain with a free lock, never woken. Re-check and append under
           that same lock: either we observe the free lock here and take it, or
           we are queued before the pop and are guaranteed a wake. */
        pthread_mutex_lock(&rt->act_lock);
        cur = 0;
        if (coro_at_cas(&m->locked, &cur, 1)) {
            pthread_mutex_unlock(&rt->act_lock);
            return CORO_OK;
        }
        rt_chain_park(&m->waitq, self);
        pthread_mutex_unlock(&rt->act_lock);

        if (coro_yield() != CORO_OK) {
            /* Cancelled while parked. If we were still queued a later unlock
               would wake a phantom: try once more to take the lock (the
               unlocker that woke us may have freed it), and detach whether
               or not we won. */
            uint64_t c2 = 0;
            int won = coro_at_cas(&m->locked, &c2, 1);
            pthread_mutex_lock(&rt->act_lock);
            rt_detach(&m->waitq, self);
            pthread_mutex_unlock(&rt->act_lock);
            return won ? CORO_OK : CORO_ECANCELED;
        }
        /* Woken normally: loop and re-attempt the take (park again if
           beaten; RT_WAITING was cleared by the pop+wake in unlock). */
        node_clear(self, RT_WAITING);
    }
}

coro_status coro_rt_mutex_unlock(coro_rt_t* rt, coro_rt_mutex_t* m) {
    if (!rt || !m) return CORO_EINVAL;
    if (coro_at_load(&m->locked) == 0) return CORO_ESTATE;

    pthread_mutex_lock(&rt->act_lock);
    rt_node* w = q_pop(&m->waitq);
    coro_at_store(&m->locked, 0);
    pthread_mutex_unlock(&rt->act_lock);

    if (w) rt_node_wake(rt, w);
    return CORO_OK;
}

/* ---- condition variable ---------------------------------------------- */

void coro_rt_cond_init(coro_rt_cond_t* c) {
    if (!c) return;
    c->waitq = NULL;
}

coro_status coro_rt_cond_wait(coro_rt_t* rt, coro_rt_cond_t* c,
                              coro_rt_mutex_t* m) {
    if (!rt || !c || !m) return CORO_EINVAL;
    rt_node* self = rt_current_node();
    if (!self) return CORO_ESTALE;
    if (coro_at_load(&m->locked) == 0) return CORO_ESTATE;

    /* Release the mutex and park, atomically with respect to notifiers: the
       release and the cond-append both happen under the activity lock. */
    pthread_mutex_lock(&rt->act_lock);
    rt_node* mw = q_pop(&m->waitq);
    coro_at_store(&m->locked, 0);
    rt_chain_park(&c->waitq, self);
    pthread_mutex_unlock(&rt->act_lock);
    if (mw) rt_node_wake(rt, mw);

    if (coro_yield() != CORO_OK) {
        /* Cancelled while on the cond chain. */
        pthread_mutex_lock(&rt->act_lock);
        rt_detach(&c->waitq, self);
        node_clear(self, RT_WAITING);
        pthread_mutex_unlock(&rt->act_lock);
        /* Re-acquire the mutex so the caller may clean up consistently. */
        return coro_rt_mutex_lock(rt, m);
    }
    node_clear(self, RT_WAITING);
    /* Notified: re-acquire the mutex (parks again if contended). */
    return coro_rt_mutex_lock(rt, m);
}

coro_status coro_rt_cond_notify(coro_rt_t* rt, coro_rt_cond_t* c) {
    if (!rt || !c) return CORO_EINVAL;
    pthread_mutex_lock(&rt->act_lock);
    rt_node* w = q_pop(&c->waitq);
    pthread_mutex_unlock(&rt->act_lock);
    if (w) {
        rt_node_wake(rt, w);
    }
    return CORO_OK;
}

coro_status coro_rt_cond_broadcast(coro_rt_t* rt, coro_rt_cond_t* c) {
    if (!rt || !c) return CORO_EINVAL;
    for (;;) {
        pthread_mutex_lock(&rt->act_lock);
        rt_node* w = q_pop(&c->waitq);
        pthread_mutex_unlock(&rt->act_lock);
        if (!w) return CORO_OK;
        rt_node_wake(rt, w);
    }
}

/* ---- counting semaphore ---------------------------------------------- */

void coro_rt_sem_init(coro_rt_sem_t* s, size_t count) {
    if (!s) return;
    s->count = count;
    s->waitq = NULL;
}

coro_status coro_rt_sem_trywait(coro_rt_t* rt, coro_rt_sem_t* s) {
    if (!rt || !s) return CORO_EINVAL;
    for (;;) {
        uint64_t cur = coro_at_load(&s->count);
        if (cur == 0) return CORO_EBUSY;
        if (coro_at_cas(&s->count, &cur, cur - 1)) return CORO_OK;
    }
}

coro_status coro_rt_sem_wait(coro_rt_t* rt, coro_rt_sem_t* s) {
    if (!rt || !s) return CORO_EINVAL;
    rt_node* self = rt_current_node();
    if (!self) return CORO_ESTALE;

    for (;;) {
        if (coro_rt_sem_trywait(rt, s) == CORO_OK) return CORO_OK;

        /* Same check-and-suspend atomicity as the mutex: post() bumps the
           count and pops the chain under the activity lock, so re-check the
           token and append the waiter under that same lock. A post landing
           between the failed trywait and our append is then either consumed
           here or observed by the pop as a queued waiter -- never lost. */
        pthread_mutex_lock(&rt->act_lock);
        if (coro_rt_sem_trywait(rt, s) == CORO_OK) {
            pthread_mutex_unlock(&rt->act_lock);
            return CORO_OK;
        }
        rt_chain_park(&s->waitq, self);
        pthread_mutex_unlock(&rt->act_lock);

        if (coro_yield() != CORO_OK) {
            /* Cancelled while parked: detach; a racing post already bumped
               the count and will not be double-counted. */
            pthread_mutex_lock(&rt->act_lock);
            rt_detach(&s->waitq, self);
            node_clear(self, RT_WAITING);
            pthread_mutex_unlock(&rt->act_lock);
            return CORO_ECANCELED;
        }
        node_clear(self, RT_WAITING);
    }
}

coro_status coro_rt_sem_post(coro_rt_t* rt, coro_rt_sem_t* s) {
    if (!rt || !s) return CORO_EINVAL;
    /* The open-operation must always succeed: a compare-and-swap could lose
       to a concurrent waiter decrement and drop the token. A fetch-add is
       atomic and cannot lose. */
    (void)coro_at_fetch_add((volatile uint64_t*)&s->count, 1);

    pthread_mutex_lock(&rt->act_lock);
    rt_node* w = q_pop(&s->waitq);
    pthread_mutex_unlock(&rt->act_lock);
    if (w) {
        rt_node_wake(rt, w);
    }
    return CORO_OK;
}

size_t coro_rt_sem_count(const coro_rt_sem_t* s) {
    if (!s) return 0;
    return (size_t)coro_at_load(&s->count);
}