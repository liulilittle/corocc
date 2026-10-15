/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/scheduler/mpsc.c -- Vyukov intrusive MPSC queue.
 *
 * Producers append by CAS-ing the head; the owner consumer pops one node by
 * draining the whole batch and reversing it. The tail field is the owner's
 * cache of the last node it reversed, so the common case (one node) costs one
 * CAS and one pointer read. This is the queue shape the work-stealing runtime
 * uses for cross-thread wakeups, which never touch the private Chase-Lev end.
 */
#include "scheduler/mpsc.h"

#include "scheduler/rt.h"

#include <stddef.h>

static rt_node* load_link(rt_node* volatile* p) {
    return (rt_node*)coro_atp_load((const volatile uintptr_t*)p);
}
static void store_link(rt_node* volatile* p, rt_node* v) {
    coro_atp_store((volatile uintptr_t*)p, (uintptr_t)v);
}

void mpsc_init(mpsc* q) {
    q->head = NULL;
    q->tail = NULL;
}

void mpsc_push(mpsc* q, rt_node* n) {
    /* The node's next is the producer-private link: relink it to the head we
       are about to replace, then swing the head with one CAS. On a lost race
       the CAS leaves the current head in `old`, so the next turn relinks to
       the fresh head before retrying. Relinking inside the loop is what keeps
       the chain acyclic: a link captured before an internal retry can no
       longer survive (the old code set n->next from a stale `old` while an
       inner retry loop advanced the head, so a later turn linked n to itself
       -- head==n made n->next=n, a self-loop that handed the same node out
       twice). */
    rt_node* old = (rt_node*)coro_atp_load((const volatile uintptr_t*)&q->head);
    for (;;) {
        n->next = old;
        if (coro_atp_cas((volatile uintptr_t*)&q->head, (uintptr_t*)&old,
                         (uintptr_t)n)) {
            break;
        }
    }
}

rt_node* mpsc_pop(mpsc* q) {
    /* Drain the batch only when our cached tail ran out. */
    rt_node* t = q->tail;
    if (t) {
        rt_node* next = load_link(&t->next);
        q->tail = next; /* may be NULL: this is the last node of the batch */
        return t;
    }

    rt_node* head = (rt_node*)coro_atp_exchange(
        (volatile uintptr_t*)&q->head, 0);
    if (!head) return NULL;
    /* The producers appended in reverse order (each new node became the
       head), so the chain head -> ... -> oldest reads newest-first. Reverse
       it to oldest-first; `oldest` is the first node to hand out, its next is
       the cached tail for the following pop. */
    rt_node* prev = NULL;
    rt_node* cur = head;
    while (cur) {
        rt_node* next = load_link(&cur->next);
        store_link(&cur->next, prev);
        prev = cur;
        cur = next;
    }
    rt_node* oldest = prev;
    q->tail = load_link(&oldest->next); /* the rest of the reversed chain */
    return oldest;
}

int mpsc_empty(const mpsc* q) {
    return coro_atp_load((const volatile uintptr_t*)&q->head) == 0;
}
