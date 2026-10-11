/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/coroutine/coro.c -- L1 coroutine object.
 *
 * A coro_t is a scheduling entity: a state machine over one execution context.
 * All state lives in a single atomic word (W) so that every transition -- start,
 * resume, yield, cancel, destroy -- is one compare-and-swap, which is what the
 * "exactly once" contract of the design spec (section 3.3) is built on.
 *
 * W encoding (one word, state and credential together):
 *
 *   value                 meaning
 *   --------------------  -----------------------------------------------
 *   CORO_W_NEW      (0)   created, never started
 *   CORO_W_RUNNING  (1)   executing; not resumable
 *   CORO_W_DEAD     (max) finished; not resumable
 *   T       (2 .. max-1) parked at a suspension point; T is the only token
 *                        that may resume this coroutine
 *
 * Tokens come from a monotone per-coroutine sequence, so a token is never
 * reused and a stale token can never be mistaken for the live one (no ABA).
 *
 * Settle-after-return: a resume publishes the new token only after
 * coro_ctx_swap has returned, i.e. once the coroutine's frame is fully saved.
 * Nothing can observe a half-stopped coroutine as parked.
 */
#include <corocc/coro.h>
#include <corocc/coro_ctx.h>
#include <corocc/coro_stack.h>

#include "arch/coro_atomic.h"
#include "arch/coro_tls.h"

#include <stdlib.h>
#include <string.h>

/* Reserved values of W. Everything below CORO_W_TOKEN_BASE is a state tag;
   everything at or above it is a valid token. */
#define CORO_W_NEW         ((uint64_t)0)
#define CORO_W_RUNNING     ((uint64_t)1)
#define CORO_W_DEAD        ((uint64_t)~(uint64_t)0)
#define CORO_W_TOKEN_BASE  ((uint64_t)2)

/* The coroutine object. Fields are private by design: the public contract is
   the entry points in coro.h, never this layout. */
struct coro {
    uint64_t    w;        /**< Single authority word: state + credential. */
    uint64_t    seq;      /**< Monotone token source; 0 before the first yield. */
    coro_fn     fn;       /**< Entry function. */
    void*       arg;      /**< Opaque argument forwarded to fn. */
    coro_stack  stack;    /**< Owned stack (guard page + usable bytes). */
    coro_ctx    self;     /**< Owned execution context. */
    coro_attr_t attr;     /**< Attribute snapshot for diagnostics. */
};

/* Placeholder entry. coro_ctx_init needs a target before any control transfer
   exists; the real trampoline (start/resume) arrives with the credential
   layer, and this function is unreachable until then because a coroutine runs
   nothing until it is started. */
static void coro_entry_pending(void* arg) {
    (void)arg;
}

/* The coroutine this thread is currently running, or NULL. Set by the resume
   path once it exists; destroy uses it to refuse self-destruction from day
   one, so the guard never has to be retrofitted. */
static CORO_TLS coro_t* tls_current;

static coro_t* coro_current(void) {
    return tls_current;
}

coro_attr_t coro_attr_default(void) {
    coro_attr_t a;
    a.stack_size = 0;
    a.name = NULL;
    a.flags = CORO_F_NONE;
    return a;
}

coro_status coro_create(coro_t** out, const coro_attr_t* attr, coro_fn fn,
                        void* arg) {
    if (!out || !fn) return CORO_EINVAL;

    coro_attr_t a = attr ? *attr : coro_attr_default();
    if (a.flags != CORO_F_NONE) return CORO_EINVAL;

    size_t stk_size = a.stack_size ? a.stack_size : CORO_DEFAULT_STACK_SIZE;

    coro_t* c = (coro_t*)calloc(1, sizeof *c);
    if (!c) return CORO_ENOMEM;

    if (coro_stack_alloc(&c->stack, stk_size) != CORO_OK) {
        free(c);
        return CORO_ENOMEM;
    }
    c->fn = fn;
    c->arg = arg;
    c->attr = a;
    if (coro_ctx_init(&c->self, &c->stack, coro_entry_pending, c) != 0) {
        coro_stack_free(&c->stack);
        free(c);
        return CORO_EINVAL;
    }
    coro_at_store(&c->w, CORO_W_NEW);
    *out = c;
    return CORO_OK;
}

coro_status coro_destroy(coro_t* c) {
    if (!c) return CORO_EINVAL;
    /* Freeing the running coroutine would unmap its own stack. */
    if (coro_current() == c) return CORO_EBUSY;

    uint64_t cur = coro_at_load(&c->w);
    if (cur == CORO_W_RUNNING) return CORO_EBUSY;
    /* Single transition to DEAD, no retry loop: if another writer wins the
       compare-and-swap the state changed under us and we report the loss. */
    if (!coro_at_cas(&c->w, &cur, CORO_W_DEAD)) return CORO_EBUSY;

    coro_ctx_destroy(&c->self);
    coro_stack_free(&c->stack);
    free(c);
    return CORO_OK;
}

coro_state coro_get_state(const coro_t* c) {
    if (!c) return CORO_NEW;
    uint64_t w = coro_at_load(&c->w);
    if (w == CORO_W_NEW) return CORO_NEW;
    if (w == CORO_W_RUNNING) return CORO_RUNNING;
    if (w == CORO_W_DEAD) return CORO_DEAD;
    return CORO_SUSPENDED;
}
