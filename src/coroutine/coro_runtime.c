/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/coroutine/coro_runtime.c -- per-thread coroutine runtime.
 *
 * One runtime per thread, created on first use and released by a thread-exit
 * hook. The identity stored in it is a monotone number rather than the
 * runtime's own address: an address into freed thread storage would dangle the
 * moment the thread exits, while a number never does.
 *
 * The runtime also owns the thread's delivery queue -- a multi-producer,
 * single-consumer list of wakeup requests that the owner drains inside
 * coro_pump. Producers append under the runtime's lock, the consumer removes
 * under the same lock, and nothing else does. The lock covers a handful of
 * pointer assignments, is never held across a context switch, and never waits:
 * both sides finish in a few instructions, which is what the zero-spin
 * contract allows.
 */
#include "coroutine/coro_runtime.h"

#include "arch/coro_atomic.h"
#include "arch/coro_tls.h"
#include "platform/threads.h"

#include <pthread.h>
#include <stdlib.h>

/* Threads are numbered in the order they first touch the library. The counter
   is monotone and never reused, so an identity stays unique for the life of
   the process -- which is what the owner rule compares. */
static uint64_t g_next_id;

/* The runtime of the calling thread, NULL until first use. */
static CORO_TLS coro_runtime* tls_runtime;

/* Queue access. Every field is read and written through the atomic helpers so
   the lock provides mutual exclusion without the accessor being a data race;
   the fields themselves stay plain pointers. */
static coro_delivery* q_load(coro_delivery* const* field) {
    return (coro_delivery*)(uintptr_t)coro_at_load((volatile uint64_t*)field);
}
static void q_store(coro_delivery** field, coro_delivery* value) {
    coro_at_store((volatile uint64_t*)field, (uint64_t)(uintptr_t)value);
}

coro_runtime* coro_runtime_current(void) {
    coro_runtime* r = tls_runtime;
    if (r) return r;

    r = (coro_runtime*)calloc(1, sizeof *r);
    if (!r) {
        /* No runtime means no coroutine driving, and the drive path has no
           status code that describes it. Failing hard here is the documented
           contract for an unrecoverable condition. */
        abort();
    }
    r->id = coro_at_fetch_add(&g_next_id, 1) + 1;
    pthread_mutex_init(&r->lock, NULL);
    coro_wait_init(&r->work);
    tls_runtime = r;
    coro_runtime_register(r);
    coro_thread_exit_hook(coro_runtime_thread_exit);
    return r;
}

void coro_runtime_enqueue(coro_runtime* r, coro_delivery* node) {
    if (!r) return;
    node->next = NULL;

    pthread_mutex_lock(&r->lock);
    if (q_load(&r->tail) == NULL) {
        q_store(&r->head, node);
    } else {
        q_store(&r->tail->next, node);
    }
    q_store(&r->tail, node);
    pthread_mutex_unlock(&r->lock);
    coro_wait_raise(&r->work); /* wake the owner out of coro_pump_wait */
}

coro_delivery* coro_runtime_pop(coro_runtime* r) {
    if (!r) return NULL;

    pthread_mutex_lock(&r->lock);
    coro_delivery* first = (coro_delivery*)q_load(&r->head);
    if (first) {
        coro_delivery* next = q_load(&first->next);
        if (next == NULL) {
            q_store(&r->head, NULL);
            q_store(&r->tail, NULL);
        } else {
            q_store(&r->head, next);
        }
    }
    pthread_mutex_unlock(&r->lock);
    return first;
}

/* ---- registry: identity to runtime ----
 *
 * The delivery path has to find the queue of the thread that owns a coroutine,
 * and a coroutine only records that thread's numeric identity -- a pointer
 * would dangle once the thread leaves. The registry maps the identity to the
 * runtime for as long as the runtime lives; the exit hook removes the entry
 * before the runtime is freed.
 *
 * Lock order is always registry first, runtime second, and the registry lock is
 * held for the whole enqueue in the delivery path, so a thread cannot unregister
 * while a delivery is appending to its queue. */
struct registry_entry {
    uint64_t       id;
    coro_runtime*  rt;
};

static pthread_mutex_t     g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct registry_entry* g_reg;
static size_t              g_reg_count;
static size_t              g_reg_cap;

static int registry_grow(void) {
    if (g_reg_count < g_reg_cap) return 0;
    size_t cap = g_reg_cap ? g_reg_cap * 2 : 16;
    struct registry_entry* grown = (struct registry_entry*)realloc(
        g_reg, cap * sizeof *grown);
    if (!grown) return -1;
    g_reg = grown;
    g_reg_cap = cap;
    return 0;
}

void coro_runtime_register(coro_runtime* r) {
    if (!r) return;
    pthread_mutex_lock(&g_reg_lock);
    if (registry_grow() == 0) {
        g_reg[g_reg_count].id = r->id;
        g_reg[g_reg_count].rt = r;
        ++g_reg_count;
    }
    pthread_mutex_unlock(&g_reg_lock);
}

void coro_runtime_unregister(coro_runtime* r) {
    if (!r) return;
    pthread_mutex_lock(&g_reg_lock);
    for (size_t i = 0; i < g_reg_count; ++i) {
        if (g_reg[i].rt == r) {
            g_reg[i] = g_reg[g_reg_count - 1];
            --g_reg_count;
            break;
        }
    }
    pthread_mutex_unlock(&g_reg_lock);
}

void coro_runtime_wait_work(coro_runtime* r) {
    if (!r) return;
    /* Reset first, then look: a signal raised before the reset is covered by
       the queue being non-empty, and one raised after the check still sets the
       signal, so the block cannot miss a request. */
    coro_wait_reset(&r->work);
    pthread_mutex_lock(&r->lock);
    int empty = (q_load(&r->head) == NULL);
    pthread_mutex_unlock(&r->lock);
    if (empty) coro_wait_block(&r->work);
}

void coro_runtime_signal_work(coro_runtime* r) {
    if (r) coro_wait_raise(&r->work);
}

void coro_runtime_wait_work_until(coro_runtime* r, uint64_t deadline) {
    if (!r) return;
    /* Reset first, then look: a signal raised before the reset is covered by
       the queue being non-empty, and one raised after the check still sets the
       signal, so the wait cannot miss a request. */
    coro_wait_reset(&r->work);
    pthread_mutex_lock(&r->lock);
    int empty = (q_load(&r->head) == NULL);
    pthread_mutex_unlock(&r->lock);
    if (empty) coro_wait_block_until(&r->work, deadline);
}

coro_runtime* coro_runtime_by_id(uint64_t id) {
    coro_runtime* found = NULL;
    pthread_mutex_lock(&g_reg_lock);
    for (size_t i = 0; i < g_reg_count; ++i) {
        if (g_reg[i].id == id) {
            found = g_reg[i].rt;
            break;
        }
    }
    pthread_mutex_unlock(&g_reg_lock);
    return found;
}

size_t coro_runtime_pending(const coro_runtime* r) {
    if (!r) return 0;
    /* The queue is only mutated under its lock; a snapshot is enough for a
       diagnostic count. */
    pthread_mutex_t* lock = (pthread_mutex_t*)&r->lock;
    pthread_mutex_lock(lock);
    size_t n = 0;
    for (coro_delivery* d = (coro_delivery*)q_load(&r->head); d;
         d = q_load(&d->next)) {
        ++n;
    }
    pthread_mutex_unlock(lock);
    return n;
}

void coro_runtime_drain(coro_runtime* r) {
    if (!r) return;
    for (;;) {
        coro_delivery* d = coro_runtime_pop(r);
        if (!d) return;
        d->status = CORO_EDESTROYED;
        d->done = 1;
        if (d->signal) coro_wait_raise(d->signal);
    }
}

void coro_runtime_thread_exit(void* arg) {
    (void)arg;
    coro_runtime* r = tls_runtime;
    tls_runtime = NULL;
    if (r) {
        /* Remove the registry entry before anything else so no delivery can
           find this runtime afterwards, then serve or fail what is queued. */
        coro_runtime_unregister(r);
        coro_runtime_drain(r);
        coro_wait_destroy(&r->work);
        pthread_mutex_destroy(&r->lock);
        free(r);
    }
}
