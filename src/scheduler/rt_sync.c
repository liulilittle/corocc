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
 * wake generation. Handing a node to its worker is done with the locked wake
 * (rt_node_wake_locked, issued while the activity lock is held, with the idle
 * signal raised after the unlock), so a node popped/detached from a chain
 * cannot be resumed by a stale delivery, retired and freed before the wake
 * touches it; the scheduler's timed and join wakeups share the same wake core.
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

#include <stdlib.h>   /* calloc / free / realloc: the channel ring buffer */

#ifdef COROCC_WAKE_DBG
#define RT_DBG_INC(f) (++rt_dbg.f)
#else
#define RT_DBG_INC(f) ((void)0)
#endif

/* ---- local helpers ---------------------------------------------------- */

/* RT_WAITING bit double marker: test/set/clear on the node's flag word. */
static void node_set(rt_node* n, unsigned bit) {
    (void)coro_at_or((volatile uint64_t*)&n->flags, bit);
}
static void node_clear(rt_node* n, unsigned bit) {
    (void)coro_at_and((volatile uint64_t*)&n->flags, ~(uint64_t)bit);
}

/* Head-pointer accesses go through the atomic helpers. The mutex unlock and
   the semaphore post fast paths read the head outside the activity lock
   (ordered against an enqueue by the same-word barrier handshake, see the
   lock path); a mixed plain/atomic access to the same object would be a
   data race under the C model even though the logical mutation of the chain
   stays protected by act_lock. The link fields are only ever touched while
   act_lock is held and stay plain. */
static rt_node* head_load(void** q) {
    return (rt_node*)coro_atp_load((const volatile uintptr_t*)q);
}
static void head_store(void** q, rt_node* n) {
    coro_atp_store((volatile uintptr_t*)q, (uintptr_t)n);
}

/* Append n to the FIFO chain q (assumes the activity lock is held). */
static void q_append(void** q, rt_node* n) {
    /* BT hygiene: unlink n from this chain first, if it is linked (a stale
       delivery can resume a node whose chain entry is still in place). */
    rt_node* head = head_load(q);
    if (head == n) {
        head_store(q, n->waiter_next);
    } else {
        for (rt_node* t = head; t; t = t->waiter_next) {
            if (t->waiter_next == n) {
                t->waiter_next = n->waiter_next;
                break;
            }
        }
    }
    n->waiter_next = NULL;
    head = head_load(q);
    if (!head) {
        head_store(q, n);
        return;
    }
    rt_node* t = head;
    while (t->waiter_next) t = t->waiter_next;
    t->waiter_next = n;
}

/* Pop the FIFO head of q (assumes the activity lock is held). Clears the
   popped node's link so a stale chain tail cannot be walked again (retire_node
   traverses waiter_next; a leftover link would let a sync chain bleed into a
   join chain and wake a phantom). */
static int node_test(const rt_node* n, unsigned bit) {
    return (coro_at_load((const volatile uint64_t*)&n->flags) & bit) != 0;
}

static rt_node* q_pop(void** q) {
    rt_node* n = head_load(q);
    if (n) {
        head_store(q, n->waiter_next);
        n->waiter_next = NULL;
    }
    return n;
}

/* Pop the first head that can actually be woken: a head that is already
   retired/dying or no longer waiting is discarded (its requeue is already
   owned elsewhere -- a corpse's wake would be refused by the delivery claim
   and the handoff would evaporate on it). Bounded: every pop removes one
   node. The caller must hold the activity lock. */
static rt_node* q_pop_wakeable(void** q) {
    rt_node* w;
    while ((w = q_pop(q)) != NULL) {
        if (node_test(w, RT_DONE | RT_DYING)) { RT_DBG_INC(pop_discard); continue; }
        if (!node_test(w, RT_WAITING)) { RT_DBG_INC(pop_discard); continue; }
        break;
    }
    return w;
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
    RT_DBG_INC(park);
    /* BT minimal handshake: a collapsed wake left a drive owed to this
       flow; the park publication is its deliverer (re-armed into the
       owner's inbox; the possible spurious wake is re-checked by the
       site loop / genuineness checks). */
    (void)rt_pending_requeue(self);
}

/* Detach self from the FIFO chain, if still present (assumes act_lock held
   by the caller), and retire the park marker with it. Every caller is a
   cancel branch whose node is the running coroutine leaving a park: a
   stale RT_PARKED would make the driver drop the node off the run queue at
   its next plain yield, with no releaser left to requeue it. On a node the
   releaser already woke (not found here) the clear is a no-op -- that
   path's driver settle already retired the marker. */
static int rt_detach(void** chain, rt_node* self) {
    rt_node* head = head_load(chain);
    if (head == self) {
        head_store(chain, self->waiter_next);
        self->waiter_next = NULL;
        node_clear(self, RT_WAITING);
        node_clear(self, RT_PARKED);
        return 1;
    }
    for (rt_node* t = head; t; t = t->waiter_next) {
        if (t->waiter_next == self) {
            t->waiter_next = self->waiter_next;
            self->waiter_next = NULL;
            node_clear(self, RT_WAITING);
            node_clear(self, RT_PARKED);
            return 1;
        }
    }
    node_clear(self, RT_PARKED);
    return 0;
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

        /* Slow path: enqueue FIRST, then retry the take inside the same
           activity-lock section. The unlock fast path (release exchange,
           acquire read of the chain head) and this section form a
           release/acquire handshake over the lock word itself: both sides
           run an RMW on `locked` (the waiter a zero-delta barrier RMW, the
           unlocker the release exchange), so the two are totally ordered
           by the word's coherence and no plain store can break the chain.
           If the waiter's RMW is ordered first,
           its earlier enqueue is published to the unlocker's head read and
           the pop finds this node; if the unlocker's RMW is ordered first,
           its release is visible to the retry below, which then wins
           without parking. The waiting bits are set only on the parking
           branch, so a node that won the retry is never seen as a waiter by
           a pop that already claimed the link (such a pop wakes only parked
           nodes). */
        coro_plat_mutex_lock(&rt->act_lock);
        q_append(&m->waitq, self);
        coro_at_fence_rmw(&m->locked);
        cur = 0;
        if (coro_at_cas(&m->locked, &cur, 1)) {
            /* Won while queued: unlink and return; never parked. */
            rt_detach(&m->waitq, self);
            coro_plat_mutex_unlock(&rt->act_lock);
            return CORO_OK;
        }
        node_set(self, RT_WAITING);
        node_set(self, RT_PARKED);
        RT_DBG_INC(park);
        (void)rt_pending_requeue(self);   /* round-6: park consumes pending */
        coro_plat_mutex_unlock(&rt->act_lock);
        /* Round-7 handshake: an unlock's fast path can miss this freshly
           appended waiter; if the lock is free right now, take it instead of
           parking (the classic publish-then-recheck). */
        {
            uint64_t z0 = 0;
            if (coro_at_load(&m->locked) == 0 && coro_at_cas(&m->locked, &z0, 1)) {
                coro_plat_mutex_lock(&rt->act_lock);
                (void)rt_detach(&m->waitq, self);
                coro_plat_mutex_unlock(&rt->act_lock);
                return CORO_OK;
            }
        }

        if (coro_yield() != CORO_OK) {
            /* Cancelled while parked. If we were still queued a later unlock
               would wake a phantom: try once more to take the lock (the
               unlocker that woke us may have freed it), and detach whether
               or not we won. */
            uint64_t c2 = 0;
            int won = coro_at_cas(&m->locked, &c2, 1);
            coro_plat_mutex_lock(&rt->act_lock);
            rt_detach(&m->waitq, self);
            coro_plat_mutex_unlock(&rt->act_lock);
            return won ? CORO_OK : CORO_ECANCELED;
        }
        /* Woken: a stale delivery may have resumed us while still linked;
           detach our chain entry (no-op when the unlocker popped it). */
        coro_plat_mutex_lock(&rt->act_lock);
        (void)rt_detach(&m->waitq, self);
        coro_plat_mutex_unlock(&rt->act_lock);
        node_clear(self, RT_WAITING);
    }
}

coro_status coro_rt_mutex_unlock(coro_rt_t* rt, coro_rt_mutex_t* m) {
    if (!rt || !m) return CORO_EINVAL;
    if (coro_at_load(&m->locked) == 0) return CORO_ESTATE;

    /* Release, then decide: with no queued waiter the activity lock is
       skipped entirely. The release is an EXCHANGE, not a store: the lock
       word must never be written by a plain store, or an intervening plain
       store could sit between the waiting side's barrier RMW and this one
       in the word's modification order and terminate the release sequence
       that carries the enqueue (C++20 [atomics.order]/1: a plain store by
       another thread ends the sequence). With every write to the word an
       RMW (CAS/barrier/exchange), the two sides' RMWs are adjacent-in-chain
       whatever the order: the later one reads the earlier one's write, a
       same-location release/acquire chain every supported target gives.
       So either this exchange reads the waiter's barrier RMW (its enqueue
       is published to the head read below and the slow path pops it), or
       the waiter's barrier RMW reads this exchange (its retry sees the
       release and wins without parking). A stale non-empty hit only costs
       the slow path: the pop re-reads the head under the activity lock,
       and it wakes only a node whose waiting bit is set (a lock winner's
       node never has it). */
    (void)coro_at_exchange(&m->locked, 0);
    if (coro_atp_load((const volatile uintptr_t*)&m->waitq) == 0) {
        return CORO_OK;
    }

    /* The handoff must reach a live waiter: a waiter that is cancel-bound or
       already mid-exit cannot take the lock (its resume unwinds at the safe
       point), so waking it would only reap it. Serve the next live waiter
       instead of evaporating the token on it; popping unlinks, so each node is
       served at most once and the loop is bounded by the chain length.
       Pop, classify and wake under ONE act_lock hold: a popped waiter can
       still be resumed by an independent stale delivery and run to completion,
       and once it retires retire_node frees it -- reading w->coro (or handing
       w to the wake) after dropping the lock is a use-after-free (observed:
       heap-use-after-free in this loop). retire_node publishes RT_DYING under
       the same lock, so holding it keeps w alive for the classification and
       the delivery; the idle signal is raised after the unlock. */
    for (;;) {
        rt_node* w;
        int live;
        int accepted;
        coro_plat_mutex_lock(&rt->act_lock);
        w = q_pop_wakeable(&m->waitq);
        if (!w) {
            coro_plat_mutex_unlock(&rt->act_lock);
            break;
        }
        live = !coro_cancel_pending(w->coro) &&
               coro_get_state(w->coro) != CORO_RUNNING &&
               coro_get_state(w->coro) != CORO_DEAD;
        accepted = rt_node_wake_locked(w);
        coro_plat_mutex_unlock(&rt->act_lock);
        rt_signal_activity(rt);
        if (live && accepted) break;   /* accepted handoff */
    }
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
    coro_plat_mutex_lock(&rt->act_lock);
    rt_node* mw = q_pop_wakeable(&m->waitq);
    /* Exchange, not a plain store: after init the lock word must only ever
       be written by an RMW. A plain store here would sit between a queued
       waiter's barrier RMW and a later unlock's exchange in the word's
       modification order and terminate the release sequence that carries
       that waiter's enqueue to the unlock's fast-path head read (see the
       closure proof at coro_rt_mutex_unlock). */
    (void)coro_at_exchange(&m->locked, 0);
    rt_chain_park(&c->waitq, self);
    if (mw) (void)rt_node_wake_locked(mw);   /* wake under the lock (no UAF) */
    coro_plat_mutex_unlock(&rt->act_lock);
    if (mw) rt_signal_activity(rt);

    for (;;) {
        if (coro_yield() != CORO_OK) {
            /* Cancelled while on the cond chain. */
            coro_plat_mutex_lock(&rt->act_lock);
            rt_detach(&c->waitq, self);
            node_clear(self, RT_WAITING);
            coro_plat_mutex_unlock(&rt->act_lock);
            /* Re-acquire the mutex so the caller may clean up consistently. */
            return coro_rt_mutex_lock(rt, m);
        }
        /* BT genuineness: the notifier pops the chain entry before waking;
           still being linked means a stale delivery resumed us, so re-park
           (the mutex is not held here; it was released before parking). */
        {
            int still;
            coro_plat_mutex_lock(&rt->act_lock);
            still = rt_detach(&c->waitq, self);
            if (still) rt_chain_park(&c->waitq, self);
            coro_plat_mutex_unlock(&rt->act_lock);
            if (still) continue;
        }
        node_clear(self, RT_WAITING);
        /* Notified: re-acquire the mutex (parks again if contended). */
        return coro_rt_mutex_lock(rt, m);
    }
}

coro_status coro_rt_cond_notify(coro_rt_t* rt, coro_rt_cond_t* c) {
    if (!rt || !c) return CORO_EINVAL;
    /* Hold act_lock across pop+wake (see coro_rt_mutex_unlock's note): a node
       popped here can otherwise be resumed by a stale delivery, retired and
       freed before the wake touches it. */
    for (;;) {
        coro_plat_mutex_lock(&rt->act_lock);
        rt_node* w = q_pop_wakeable(&c->waitq);
        int woken = w ? rt_node_wake_locked(w) : 0;
        coro_plat_mutex_unlock(&rt->act_lock);
        if (!w) return CORO_OK;
        rt_signal_activity(rt);
        if (woken) return CORO_OK;
    }
}

coro_status coro_rt_cond_broadcast(coro_rt_t* rt, coro_rt_cond_t* c) {
    if (!rt || !c) return CORO_EINVAL;
    for (;;) {
        coro_plat_mutex_lock(&rt->act_lock);
        rt_node* w = q_pop_wakeable(&c->waitq);
        if (w) (void)rt_node_wake_locked(w);
        coro_plat_mutex_unlock(&rt->act_lock);
        if (!w) return CORO_OK;
        rt_signal_activity(rt);
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

        /* Same append-first handshake as the mutex lock, over the count
           word: post() bumps the count and runs its barrier on the same
           word, so either this enqueue is visible to the post's head read
           (and the post pops this node) or this retry -- ordered after
           this side's barrier -- consumes the posted token without
           parking. */
        coro_plat_mutex_lock(&rt->act_lock);
        q_append(&s->waitq, self);
        coro_at_fence_rmw(&s->count);
        if (coro_rt_sem_trywait(rt, s) == CORO_OK) {
            rt_detach(&s->waitq, self);
            coro_plat_mutex_unlock(&rt->act_lock);
            return CORO_OK;
        }
        node_set(self, RT_WAITING);
        node_set(self, RT_PARKED);
        (void)rt_pending_requeue(self);   /* round-6: park consumes pending */
        coro_plat_mutex_unlock(&rt->act_lock);
        /* Round-7 handshake: a post can miss the freshly appended waiter; if
           a token is available right now, consume it instead of parking. */
        if (coro_rt_sem_trywait(rt, s) == CORO_OK) {
            coro_plat_mutex_lock(&rt->act_lock);
            (void)rt_detach(&s->waitq, self);
            coro_plat_mutex_unlock(&rt->act_lock);
            return CORO_OK;
        }

        if (coro_yield() != CORO_OK) {
            /* Cancelled while parked: detach; a racing post already bumped
               the count and will not be double-counted. */
            coro_plat_mutex_lock(&rt->act_lock);
            rt_detach(&s->waitq, self);
            node_clear(self, RT_WAITING);
            coro_plat_mutex_unlock(&rt->act_lock);
            return CORO_ECANCELED;
        }
        coro_plat_mutex_lock(&rt->act_lock);
        (void)rt_detach(&s->waitq, self);
        coro_plat_mutex_unlock(&rt->act_lock);
        node_clear(self, RT_WAITING);
    }
}

coro_status coro_rt_sem_post(coro_rt_t* rt, coro_rt_sem_t* s) {
    if (!rt || !s) return CORO_EINVAL;
    /* The open-operation must always succeed: a compare-and-swap could lose
       to a concurrent waiter decrement and drop the token. A fetch-add is
       atomic and cannot lose. */
    (void)coro_at_fetch_add((volatile uint64_t*)&s->count, 1);

    /* Fast path: with no queued waiter the activity lock is skipped. The
       barrier is a zero-delta RMW on the count word, pairing with the wait
       path's RMW on the same word: a retry that does not observe this add
       is exactly the case where the head read below observes the enqueue
       (see the wait path). */
    coro_at_fence_rmw(&s->count);
    if (coro_atp_load((const volatile uintptr_t*)&s->waitq) == 0) {
        return CORO_OK;
    }

    /* Same pop/classify/wake discipline as coro_rt_mutex_unlock: hold
       act_lock across the whole handoff so a popped waiter cannot be resumed
       by a stale delivery, retired and freed between the pop and the wake
       (see the long note there). */
    for (;;) {
        rt_node* w;
        int live;
        int accepted;
        coro_plat_mutex_lock(&rt->act_lock);
        w = q_pop_wakeable(&s->waitq);
        if (!w) {
            coro_plat_mutex_unlock(&rt->act_lock);
            break;
        }
        live = !coro_cancel_pending(w->coro) &&
               coro_get_state(w->coro) != CORO_RUNNING &&
               coro_get_state(w->coro) != CORO_DEAD;
        accepted = rt_node_wake_locked(w);
        coro_plat_mutex_unlock(&rt->act_lock);
        rt_signal_activity(rt);
        if (live && accepted) break;   /* accepted handoff */
    }
    return CORO_OK;
}

size_t coro_rt_sem_count(const coro_rt_sem_t* s) {
    if (!s) return 0;
    return (size_t)coro_at_load(&s->count);
}

/* ---- read/write lock ------------------------------------------------ */

/* State word layout (single 64-bit word, one CAS per transition):
 *   bit0  RW_W   a writer holds the lock
 *   bit1  RW_WW  a writer holds the lock or is queued for it
 *   bits2..63    active reader count, right-shifted by RW_READER_SHIFT
 *
 * Invariant: RW_WW is set exactly when (RW_W is held) or (wr_waitq is
 * non-empty). Readers acquire only while neither bit is clear of them, so a
 * queued writer blocks new readers (writer preference) while a writer still
 * has to wait for the active readers to drain.
 *
 * Every acquire is ONE full-word compare-and-swap between two legal states
 * (no fetch-add, no mask/partial CAS), so the reader counter can never carry
 * into the writer bits: readers count up to 2^62-1, and past that the acquire
 * reports CORO_ESTATE.
 */
#define RW_W   ((uint64_t)1u << 0)
#define RW_WW  ((uint64_t)1u << 1)
#define RW_READER_SHIFT 2u
#define RW_READER_ONE   ((uint64_t)1u << RW_READER_SHIFT)
#define RW_READERS_MASK (~(uint64_t)0 >> RW_READER_SHIFT)
#define RW_READERS_MAX  RW_READERS_MASK
#define RW_WRITER_BITS  (RW_W | RW_WW)

void coro_rt_rwlock_init(coro_rt_rwlock_t* l) {
    if (!l) return;
    coro_at_store(&l->state, 0);
    l->rd_waitq = NULL;
    l->wr_waitq = NULL;
}

/* Recompute the WW bit from the single invariant after a queue mutation.
   The caller must hold act_lock. A CAS is used (not a store) so a concurrent
   reader fast-path cannot be lost. */
static void rw_recompute_ww(coro_rt_rwlock_t* l) {
    for (;;) {
        uint64_t cur = coro_at_load(&l->state);
        uint64_t want = ((cur & RW_W) || l->wr_waitq) ? RW_WW : 0;
        if ((cur & RW_WW) == want) return;
        uint64_t expect = cur;
        if (coro_at_cas(&l->state, &expect, (cur & ~RW_WW) | want)) return;
    }
}

/* Try to become a reader in ONE CAS. Returns CORO_OK when it succeeded,
   CORO_EBUSY when a writer holds or is queued, CORO_ESTATE on overflow. */
static coro_status rw_try_read(coro_rt_rwlock_t* l) {
    uint64_t cur = coro_at_load(&l->state);
    if (cur & RW_WRITER_BITS) return CORO_EBUSY;
    if ((cur >> RW_READER_SHIFT) >= RW_READERS_MAX) return CORO_ESTATE;
    uint64_t expect = cur;
    if (coro_at_cas(&l->state, &expect, cur + RW_READER_ONE)) return CORO_OK;
    return CORO_EBUSY;
}

/* Try to become the writer in ONE CAS. Returns CORO_OK when it succeeded,
   CORO_EBUSY when readers are active or a writer holds the lock. The writer
   also latches WW (the invariant wants it set while a writer holds). */
static coro_status rw_try_write(coro_rt_rwlock_t* l) {
    uint64_t cur = coro_at_load(&l->state);
    if ((cur & RW_W) || (cur >> RW_READER_SHIFT)) return CORO_EBUSY;
    /* The grant from the queue is RW_WW: the woken writer claims it by CASing
       RW_WW -> RW_W in one step, so readers never slip through. */
    uint64_t want = cur | RW_W | RW_WW;
    uint64_t expect = cur;
    if (coro_at_cas(&l->state, &expect, want)) return CORO_OK;
    return CORO_EBUSY;
}

/* Release one reader count in ONE CAS; returns the count that was removed. */
static coro_status rw_reader_leave(coro_rt_rwlock_t* l) {
    for (;;) {
        uint64_t cur = coro_at_load(&l->state);
        if ((cur >> RW_READER_SHIFT) == 0) return CORO_ESTATE;
        uint64_t expect = cur;
        if (coro_at_cas(&l->state, &expect, cur - RW_READER_ONE)) return CORO_OK;
    }
}

/* Drop the WRITER hold in ONE CAS (RW_W -> 0), keeping WW for a queued
   writer. Returns CORO_ESTATE when no writer held the lock. */
static coro_status rw_writer_leave(coro_rt_rwlock_t* l) {
    for (;;) {
        uint64_t cur = coro_at_load(&l->state);
        if (!(cur & RW_W)) return CORO_ESTATE;
        uint64_t expect = cur;
        if (coro_at_cas(&l->state, &expect, cur & ~RW_W)) return CORO_OK;
    }
}

/* Pop the longest-waiting writer (act_lock held). WW is NOT recomputed here:
   the popped writer is in flight and claims its grant with one CAS from the
   WW state, so the bit must stay set until it has either taken the lock (its
   own transition) or re-parked (which sets it again). Clearing it here would
   let a new reader slip in and strand the writers already queued. */
static rt_node* rw_pop_writer(coro_rt_rwlock_t* l) {
    return (rt_node*)q_pop_wakeable(&l->wr_waitq);
}

coro_status coro_rt_rwlock_tryrdlock(coro_rt_t* rt, coro_rt_rwlock_t* l) {
    if (!rt || !l) return CORO_EINVAL;
    return rw_try_read(l);
}

coro_status coro_rt_rwlock_trywrlock(coro_rt_t* rt, coro_rt_rwlock_t* l) {
    if (!rt || !l) return CORO_EINVAL;
    return rw_try_write(l);
}

coro_status coro_rt_rwlock_rdlock(coro_rt_t* rt, coro_rt_rwlock_t* l) {
    if (!rt || !l) return CORO_EINVAL;
    rt_node* self = rt_current_node();
    if (!self) return CORO_ESTALE;

    for (;;) {
        coro_status fast = rw_try_read(l);
        if (fast == CORO_OK) return CORO_OK;
        if (fast == CORO_ESTATE) return CORO_ESTATE;

        /* Slow path: park under the activity lock so the release path (which
           drains the reader chain under the same lock) cannot run between the
           failed attempt and the append. */
        coro_plat_mutex_lock(&rt->act_lock);
        coro_status again = rw_try_read(l);
        if (again == CORO_ESTATE) {
            coro_plat_mutex_unlock(&rt->act_lock);
            return CORO_ESTATE;
        }
        if (again == CORO_OK) {
            coro_plat_mutex_unlock(&rt->act_lock);
            return CORO_OK;
        }
        rt_chain_park(&l->rd_waitq, self);
        coro_plat_mutex_unlock(&rt->act_lock);

        if (coro_yield() != CORO_OK) {
            /* Cancelled while parked: finish the operation if it is now
               possible (the wake may have already freed the lock), else
               detach and report the cancellation. */
            coro_plat_mutex_lock(&rt->act_lock);
            coro_status retry = rw_try_read(l);
            rt_detach(&l->rd_waitq, self);
            node_clear(self, RT_WAITING);
            coro_plat_mutex_unlock(&rt->act_lock);
            return retry == CORO_OK ? CORO_OK : CORO_ECANCELED;
        }
        coro_plat_mutex_lock(&rt->act_lock);
        (void)rt_detach(&l->rd_waitq, self);
        coro_plat_mutex_unlock(&rt->act_lock);
        node_clear(self, RT_WAITING);
        /* Woken: loop and retry the take. */
    }
}

coro_status coro_rt_rwlock_rdunlock(coro_rt_t* rt, coro_rt_rwlock_t* l) {
    if (!rt || !l) return CORO_EINVAL;
    coro_status st = rw_reader_leave(l);
    if (st != CORO_OK) return st;

    /* The read hold is gone. Three cases, decided under the activity lock so
       the queue mutation and the wake cannot be split:
         - readers are still active: nothing to hand over;
         - last reader out with a writer queued: hand it the lock (WW stays
           set; the woken writer completes the grant with its own CAS);
         - last reader out with nobody queued: the lock is free, so clear WW
           and release every parked reader. The drain is not optional: a
           cancelled writer clears WW, and a reader that parked behind it must
           still be woken, or it hangs forever. */
    coro_plat_mutex_lock(&rt->act_lock);
    int last = (coro_at_load(&l->state) >> RW_READER_SHIFT) == 0;
    rt_node* w = last ? rw_pop_writer(l) : NULL;
    if (last && !w) {
        for (;;) {
            uint64_t cur = coro_at_load(&l->state);
            uint64_t expect = cur;
            if (coro_at_cas(&l->state, &expect, cur & ~RW_WW)) break;
        }
        for (;;) {
            rt_node* r = (rt_node*)q_pop_wakeable(&l->rd_waitq);
            if (!r) break;
            (void)rt_node_wake_locked(r);
            coro_plat_mutex_unlock(&rt->act_lock);
            rt_signal_activity(rt);
            coro_plat_mutex_lock(&rt->act_lock);
        }
    }
    /* Hold the lock across the writer handoff's pop+wake; a popped writer can
       otherwise be resumed, retired and freed before the wake (UAF). */
    int woken = w ? rt_node_wake_locked(w) : 0;
    coro_plat_mutex_unlock(&rt->act_lock);
    if (w) rt_signal_activity(rt);
    while (w && !woken) {
        coro_plat_mutex_lock(&rt->act_lock);
        w = rw_pop_writer(l);
        woken = w ? rt_node_wake_locked(w) : 0;
        coro_plat_mutex_unlock(&rt->act_lock);
        if (w) rt_signal_activity(rt);
    }
    return CORO_OK;
}

coro_status coro_rt_rwlock_wrlock(coro_rt_t* rt, coro_rt_rwlock_t* l) {
    if (!rt || !l) return CORO_EINVAL;
    rt_node* self = rt_current_node();
    if (!self) return CORO_ESTALE;

    for (;;) {
        coro_status fast = rw_try_write(l);
        if (fast == CORO_OK) return CORO_OK;

        /* Slow path: queue behind any waiting writer (writer preference) and
           latch WW under the activity lock so new readers are locked out. */
        coro_plat_mutex_lock(&rt->act_lock);
        if (rw_try_write(l) == CORO_OK) {
            coro_plat_mutex_unlock(&rt->act_lock);
            return CORO_OK;
        }
        rt_chain_park(&l->wr_waitq, self);
        rw_recompute_ww(l);
        coro_plat_mutex_unlock(&rt->act_lock);

        if (coro_yield() != CORO_OK) {
            /* Cancelled while queued: take the grant if it was handed to us
               (the CAS RW_WW -> RW_W completes in one step), else detach and
               keep the WW invariant by recomputing it. */
            coro_plat_mutex_lock(&rt->act_lock);
            coro_status retry = rw_try_write(l);
            if (retry == CORO_OK) {
                rt_detach(&l->wr_waitq, self);
                node_clear(self, RT_WAITING);
                coro_plat_mutex_unlock(&rt->act_lock);
                return CORO_OK;
            }
            rt_detach(&l->wr_waitq, self);
            node_clear(self, RT_WAITING);
            rw_recompute_ww(l);
            coro_plat_mutex_unlock(&rt->act_lock);
            return CORO_ECANCELED;
        }
        coro_plat_mutex_lock(&rt->act_lock);
        (void)rt_detach(&l->wr_waitq, self);
        coro_plat_mutex_unlock(&rt->act_lock);
        node_clear(self, RT_WAITING);
        /* Woken: loop and retry the take. */
    }
}

coro_status coro_rt_rwlock_wrunlock(coro_rt_t* rt, coro_rt_rwlock_t* l) {
    if (!rt || !l) return CORO_EINVAL;
    coro_status st = rw_writer_leave(l);
    if (st != CORO_OK) return st;

    /* Hand the permission to the longest-waiting writer if one is queued;
       otherwise release every parked reader (bounded by the reader count). */
    coro_plat_mutex_lock(&rt->act_lock);
    rt_node* w = rw_pop_writer(l);
    if (!w) {
        /* No writer waiting: clear WW and let readers in. */
        for (;;) {
            uint64_t cur = coro_at_load(&l->state);
            uint64_t expect = cur;
            if (coro_at_cas(&l->state, &expect, cur & ~RW_WW)) break;
        }
        for (;;) {
            rt_node* r = (rt_node*)q_pop_wakeable(&l->rd_waitq);
            if (!r) break;
            (void)rt_node_wake_locked(r);
            coro_plat_mutex_unlock(&rt->act_lock);
            rt_signal_activity(rt);
            coro_plat_mutex_lock(&rt->act_lock);
        }
    }
    int woken = w ? rt_node_wake_locked(w) : 0;
    coro_plat_mutex_unlock(&rt->act_lock);
    if (w) rt_signal_activity(rt);
    while (w && !woken) {
        coro_plat_mutex_lock(&rt->act_lock);
        w = rw_pop_writer(l);
        woken = w ? rt_node_wake_locked(w) : 0;
        coro_plat_mutex_unlock(&rt->act_lock);
        if (w) rt_signal_activity(rt);
    }
    return CORO_OK;
}

/* ---- channel ------------------------------------------------------------- */

/* A bounded or unbounded FIFO of opaque void* values between coroutines of
   one runtime. The ring is guarded by the runtime's activity lock, exactly
   like the semaphore, so the park/wake protocol is the one already proven
   there: every mutation happens under the lock, and each wake is issued under
   the same lock (the locked wake variant), with the idle signal raised only
   after the unlock. Waking under the lock is required: a node popped/detached
   from a chain can otherwise be resumed by a stale delivery, retired and freed
   before the wake touches it (use-after-free).

   The handshake is 1:1: a successful send wakes at most one parked receiver
   and a successful receive wakes at most one parked sender, so a queued value
   and a parked waiter can never both wait for each other. A cancelled waiter
   that is woken retries its own operation first (the same rule the mutex
   uses), so a grant handed to it is never lost. */

/* Wake every node on the local chain `head` (act_lock held). The nodes were
   detached from the channel's chain under the lock; waking them while the lock
   is still held keeps each from being resumed, retired and freed between the
   detach and the wake. The caller raises the idle signal after unlocking. */
static void ch_wake_all_locked(rt_node* head) {
    while (head) {
        rt_node* next = (rt_node*)head->waiter_next;
        head->waiter_next = NULL;
        (void)rt_node_wake_locked(head);
        head = next;
    }
}

static int ch_is_unbounded(const coro_rt_channel_t* c) {
    return c->limit == 0;
}

/* Room for one more item? (unbounded channels always have room) */
static int ch_has_room(const coro_rt_channel_t* c) {
    return ch_is_unbounded(c) || c->len < c->limit;
}

/* Grow the ring to at least `want` slots, preserving the FIFO order.
   Returns 0 on success, -1 on failure or overflow. */
static int ch_grow(coro_rt_channel_t* c, size_t want) {
    size_t ncap = c->cap ? c->cap : 4u;
    while (ncap < want) {
        if (ncap > (size_t)-1 / 2u) return -1;
        ncap *= 2u;
    }
    void** slots = (void**)realloc(c->slots, ncap * sizeof(void*));
    if (!slots) return -1;
    /* Compact: the ring may be wrapped, so the open slot index is not simply
       c->head + c->len when it exceeds the old capacity. Layout in order. */
    if (c->len) {
        void** tmp = (void**)malloc(c->len * sizeof(void*));
        if (!tmp) { free(slots); return -1; }
        for (size_t i = 0; i < c->len; ++i)
            tmp[i] = slots[(c->head + i) % c->cap];
        for (size_t i = 0; i < c->len; ++i) slots[i] = tmp[i];
        free(tmp);
    }
    c->head = 0;
    c->slots = slots;
    c->cap = ncap;
    return 0;
}

static coro_status ch_init(coro_rt_channel_t* ch, size_t cap) {
    free(ch->slots);
    ch->slots = NULL;
    ch->cap = 0;
    ch->len = 0;
    ch->head = 0;
    ch->limit = cap;
    coro_at_store(&ch->closed, 0);
    ch->sendq = NULL;
    ch->recvq = NULL;
    if (cap == 0) return CORO_OK;              /* unbounded: lazily grown */
    if (ch_grow(ch, cap) != 0) return CORO_ENOMEM;
    ch->limit = cap;
    return CORO_OK;
}

coro_status coro_rt_channel_init(coro_rt_channel_t* ch, size_t cap) {
    if (!ch) return CORO_EINVAL;
    return ch_init(ch, cap);
}


/* Reset a drained channel so a second destroy is a no-op. */
static void ch_reset(coro_rt_channel_t* ch) {
    free(ch->slots);
    ch->slots = NULL;
    ch->cap = 0;
    ch->len = 0;
    ch->head = 0;
    ch->limit = 0;
    coro_at_store(&ch->closed, 0);
    ch->sendq = NULL;
    ch->recvq = NULL;
}

coro_status coro_rt_channel_destroy(coro_rt_channel_t* ch) {
    if (!ch) return CORO_EINVAL;
    if (ch->sendq || ch->recvq) return CORO_EBUSY;   /* a waiter remains */
    if (ch->len > 0) return CORO_EBUSY;              /* not drained */
    ch_reset(ch);
    return CORO_OK;
}

coro_status coro_rt_channel_close(coro_rt_t* rt, coro_rt_channel_t* ch) {
    if (!rt || !ch) return CORO_EINVAL;
    coro_plat_mutex_lock(&rt->act_lock);
    coro_at_store(&ch->closed, 1);
    /* Collect both chains under the lock and wake them under the same lock so
       a detached waiter cannot be freed before its wake. */
    rt_node* senders = (rt_node*)ch->sendq;
    rt_node* receivers = (rt_node*)ch->recvq;
    ch->sendq = NULL;
    ch->recvq = NULL;
    ch_wake_all_locked(senders);
    ch_wake_all_locked(receivers);
    coro_plat_mutex_unlock(&rt->act_lock);
    rt_signal_activity(rt);
    return CORO_OK;   /* idempotent */
}

size_t coro_rt_channel_size(const coro_rt_channel_t* ch) {
    /* len is a size_t, so it travels through the pointer-width atomic
       helpers: a 64-bit access would write 8 bytes over 4 on ILP32. */
    return ch ? (size_t)coro_atp_load((const volatile uintptr_t*)&ch->len) : 0;
}

int coro_rt_channel_closed(const coro_rt_channel_t* ch) {
    return ch && coro_at_load(&ch->closed) != 0;
}

coro_status coro_rt_channel_trysend(coro_rt_t* rt, coro_rt_channel_t* ch,
                                    void* v) {
    if (!rt || !ch) return CORO_EINVAL;
    if (coro_at_load(&ch->closed)) return CORO_ECLOSED;

    coro_plat_mutex_lock(&rt->act_lock);
    if (coro_at_load(&ch->closed)) {
        coro_plat_mutex_unlock(&rt->act_lock);
        return CORO_ECLOSED;
    }
    if (!ch_has_room(ch)) {
        coro_plat_mutex_unlock(&rt->act_lock);
        return CORO_EBUSY;
    }
    if (ch_is_unbounded(ch) && ch->len == ch->cap && ch_grow(ch, ch->len + 1) != 0) {
        coro_plat_mutex_unlock(&rt->act_lock);
        return CORO_ENOMEM;
    }
    ch->slots[(ch->head + ch->len) % ch->cap] = v;
    ++ch->len;
    rt_node* r = (rt_node*)q_pop_wakeable(&ch->recvq);
    if (r) (void)rt_node_wake_locked(r);
    coro_plat_mutex_unlock(&rt->act_lock);
    if (r) rt_signal_activity(rt);
    return CORO_OK;
}

coro_status coro_rt_channel_send(coro_rt_t* rt, coro_rt_channel_t* ch, void* v) {
    if (!rt || !ch) return CORO_EINVAL;
    rt_node* self = rt_current_node();
    if (!self) return CORO_ESTALE;

    for (;;) {
        coro_status fast = coro_rt_channel_trysend(rt, ch, v);
        if (fast == CORO_OK || fast == CORO_ENOMEM) return fast;
        /* EBUSY (full) or ECLOSED handled: park only when merely full. */
        if (fast == CORO_ECLOSED) return CORO_ECLOSED;

        /* Full: park under the activity lock and re-check atomically. */
        coro_plat_mutex_lock(&rt->act_lock);
        if (coro_at_load(&ch->closed)) {
            coro_plat_mutex_unlock(&rt->act_lock);
            return CORO_ECLOSED;
        }
        int roomed = 0;
        if (ch_has_room(ch)) {
            if (ch_is_unbounded(ch) && ch->len == ch->cap && ch_grow(ch, ch->len + 1) != 0) {
                coro_plat_mutex_unlock(&rt->act_lock);
                return CORO_ENOMEM;
            }
            roomed = 1;
        }
        if (roomed) {
            ch->slots[(ch->head + ch->len) % ch->cap] = v;
            ++ch->len;
            rt_node* r = (rt_node*)q_pop_wakeable(&ch->recvq);
            if (r) (void)rt_node_wake_locked(r);
            coro_plat_mutex_unlock(&rt->act_lock);
            if (r) rt_signal_activity(rt);
            return CORO_OK;
        }
        rt_chain_park(&ch->sendq, self);
        coro_plat_mutex_unlock(&rt->act_lock);

        if (coro_yield() != CORO_OK) {
            /* Cancelled while parked. The wake that released us may have
               freed a slot for us, so retry the send first (the same rule
               the mutex uses): if it completes, the grant is not lost.
               Detach either way so no phantom waiter remains. */
            coro_plat_mutex_lock(&rt->act_lock);
            rt_detach(&ch->sendq, self);
            node_clear(self, RT_WAITING);
            int room = !coro_at_load(&ch->closed) && ch_has_room(ch);
            if (room) {
                if (ch_is_unbounded(ch) && ch->len == ch->cap && ch_grow(ch, ch->len + 1) != 0)
                    room = 0;
            }
            if (room) {
                ch->slots[(ch->head + ch->len) % ch->cap] = v;
                ++ch->len;
                rt_node* r = (rt_node*)q_pop_wakeable(&ch->recvq);
                if (r) (void)rt_node_wake_locked(r);
                coro_plat_mutex_unlock(&rt->act_lock);
                if (r) rt_signal_activity(rt);
                return CORO_OK;
            }
            coro_plat_mutex_unlock(&rt->act_lock);
            return CORO_ECANCELED;
        }
        coro_plat_mutex_lock(&rt->act_lock);
        (void)rt_detach(&ch->sendq, self);
        coro_plat_mutex_unlock(&rt->act_lock);
        node_clear(self, RT_WAITING);
    }
}

coro_status coro_rt_channel_tryrecv(coro_rt_t* rt, coro_rt_channel_t* ch,
                                    void** out) {
    if (!rt || !ch) return CORO_EINVAL;
    coro_plat_mutex_lock(&rt->act_lock);
    if (ch->len > 0) {
        void* v = ch->slots[ch->head];
        ch->head = (ch->head + 1) % ch->cap;
        --ch->len;
        rt_node* s = (rt_node*)q_pop_wakeable(&ch->sendq);
        if (s) (void)rt_node_wake_locked(s);
        coro_plat_mutex_unlock(&rt->act_lock);
        if (s) rt_signal_activity(rt);   /* its slot is free again */
        if (out) *out = v;
        return CORO_OK;
    }
    if (coro_at_load(&ch->closed)) {
        coro_plat_mutex_unlock(&rt->act_lock);
        return CORO_ECLOSED;
    }
    coro_plat_mutex_unlock(&rt->act_lock);
    return CORO_EBUSY;
}

coro_status coro_rt_channel_recv(coro_rt_t* rt, coro_rt_channel_t* ch,
                                 void** out) {
    if (!rt || !ch) return CORO_EINVAL;
    rt_node* self = rt_current_node();
    if (!self) return CORO_ESTALE;

    for (;;) {
        coro_status fast = coro_rt_channel_tryrecv(rt, ch, out);
        if (fast == CORO_OK || fast == CORO_ECLOSED) return fast;

        /* Empty and open: park under the activity lock and re-check. */
        coro_plat_mutex_lock(&rt->act_lock);
        if (ch->len > 0) {
            void* v = ch->slots[ch->head];
            ch->head = (ch->head + 1) % ch->cap;
            --ch->len;
            rt_node* s = (rt_node*)q_pop_wakeable(&ch->sendq);
            if (s) (void)rt_node_wake_locked(s);
            coro_plat_mutex_unlock(&rt->act_lock);
            if (s) rt_signal_activity(rt);
            if (out) *out = v;
            return CORO_OK;
        }
        if (coro_at_load(&ch->closed)) {
            coro_plat_mutex_unlock(&rt->act_lock);
            return CORO_ECLOSED;
        }
        rt_chain_park(&ch->recvq, self);
        coro_plat_mutex_unlock(&rt->act_lock);

        if (coro_yield() != CORO_OK) {
            /* Cancelled: retry the receive first, then detach. */
            coro_plat_mutex_lock(&rt->act_lock);
            rt_detach(&ch->recvq, self);
            node_clear(self, RT_WAITING);
            coro_plat_mutex_unlock(&rt->act_lock);
            coro_status retry = coro_rt_channel_tryrecv(rt, ch, out);
            return retry == CORO_OK || retry == CORO_ECLOSED ? retry
                                                             : CORO_ECANCELED;
        }
        coro_plat_mutex_lock(&rt->act_lock);
        (void)rt_detach(&ch->recvq, self);
        coro_plat_mutex_unlock(&rt->act_lock);
        node_clear(self, RT_WAITING);
    }
}
