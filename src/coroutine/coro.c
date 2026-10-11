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
 *   CORO_W_SWITCHING(2)   switching: the frame is being saved; not resumable
 *   CORO_W_DEAD     (max) finished; not resumable
 *   T       (3 .. max-1)  parked at a suspension point; T is the only token
 *                         that may resume this coroutine
 *
 * Tokens come from a monotone per-coroutine sequence, so a token is never
 * reused and a stale token can never be mistaken for the live one (no ABA).
 *
 * Settle-after-return: a resume publishes the next token only after
 * coro_ctx_swap has returned, i.e. once the coroutine's frame is fully saved
 * and its `finished` flag settled. Nothing can observe a half-stopped
 * coroutine as parked, and a token can never be published for a coroutine that
 * is still running.
 *
 * Every transition is a single compare-and-swap, never a retry loop: losing a
 * race is reported as a status code (CORO_EBUSY/CORO_ESTALE) and changes
 * nothing.
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
#define CORO_W_NEW        ((uint64_t)0)
#define CORO_W_RUNNING    ((uint64_t)1)
#define CORO_W_SWITCHING  ((uint64_t)2)
#define CORO_W_DEAD       ((uint64_t)~(uint64_t)0)
#define CORO_W_TOKEN_BASE ((uint64_t)3)

/* The coroutine object. Fields are private by design: the public contract is
   the entry points in coro.h, never this layout. */
struct coro {
    uint64_t    w;        /**< Single authority word: state + credential. */
    uint64_t    seq;      /**< Monotone token source; tokens are TOKEN_BASE + n. */
    int         finished; /**< Entry function returned; see the settle note above. */
    coro_fn     fn;       /**< Entry function. */
    void*       arg;      /**< Opaque argument forwarded to fn. */
    coro_stack  stack;    /**< Owned stack (guard page + usable bytes). */
    coro_ctx    self;     /**< Owned execution context. */
    coro_ctx*   resumer;  /**< Context that drove the current run: yield and
                               entry-return target. NULL while not running. */
    coro_attr_t attr;     /**< Attribute snapshot for diagnostics. */
};

/* Why `resumer` exists next to coro_ctx: the L0 adapter records "whoever
   transferred control to me" in its own private field, and a coroutine that
   yields back to its resumer overwrites the resumer's record. A parked
   coroutine therefore cannot use that record to find its way home. The drive
   path here stores the resumer it is about to wait in, and both the yield and
   the entry-return sentinel use that value, which nothing else rewrites. */

/* The coroutine this thread is currently running, or NULL while the thread's
   own code runs. It is what makes coro_yield and nested resume work: the
   library itself sets it around every drive, so no per-call plumbing is
   needed. User code must not depend on thread_local inside a coroutine
   (spec 5.5): this pointer is the library's own bookkeeping, not user state. */
static CORO_TLS coro_t* tls_current;

/* Context of the thread's own code. Used as the `from` side when a coroutine
   parked for the main flow is resumed; zero-initialized by TLS semantics, and
   coro_ctx_swap fills its frame pointer on the first switch. */
static CORO_TLS coro_ctx tls_driver;

static coro_t* coro_current(void) {
    return tls_current;
}

/* Entry frame: runs the user function on the coroutine's stack and returns to
   the resumer that drove this run when it completes. The `finished` flag is
   written before the switch and read by the resumer after it returns, so the
   handover is ordered by the switch itself. The switch below never returns:
   once the entry is done the coroutine is dead (W == DEAD, published by the
   resumer) and cannot be resumed again. */
static void coro_entry(void* arg) {
    coro_t* c = (coro_t*)arg;
    c->fn(c->arg);
    c->finished = 1;
    coro_ctx_swap(&c->self, c->resumer);
}

/* Map a failed claim to the status the caller sees. W was observed after the
   failed compare-and-swap, so it describes the state the coroutine is really
   in. */
static coro_status claim_failure(uint64_t w) {
    if (w == CORO_W_DEAD) return CORO_EDESTROYED;
    if (w == CORO_W_RUNNING || w == CORO_W_SWITCHING) return CORO_EBUSY;
    return CORO_ESTALE; /* parked at another token, or never started */
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
    if (coro_ctx_init(&c->self, &c->stack, coro_entry, c) != 0) {
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
    /* Freeing the running coroutine would unmap its own stack, and freeing one
       that is mid-switch would leave the resumer returning into freed memory. */
    if (coro_current() == c) return CORO_EBUSY;

    uint64_t cur = coro_at_load(&c->w);
    if (cur == CORO_W_RUNNING || cur == CORO_W_SWITCHING) return CORO_EBUSY;
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
    if (w == CORO_W_SWITCHING) return CORO_SUSPENDED;
    if (w == CORO_W_DEAD) return CORO_DEAD;
    return CORO_SUSPENDED;
}

/* Drive a coroutine after its claim succeeded: record the resumer, switch to
   it, then settle. `prev` is the coroutine that was running on this thread
   (NULL for the thread's own code); the driver context supplies `from` in
   that case. */
static coro_status coro_drive(coro_t* c, coro_token* next) {
    coro_t* prev = tls_current;
    coro_ctx* from = prev ? &prev->self : &tls_driver;
    c->resumer = from;
    tls_current = c;
    coro_ctx_swap(from, &c->self);
    tls_current = prev;

    /* Settle: the coroutine is fully saved now, so publishing is safe. */
    if (c->finished) {
        uint64_t exp = CORO_W_RUNNING;
        if (!coro_at_cas(&c->w, &exp, CORO_W_DEAD)) return CORO_EBUSY;
        *next = CORO_TOKEN_DEAD;
        return CORO_OK;
    } else {
        uint64_t t = CORO_W_TOKEN_BASE + c->seq;
        ++c->seq;
        uint64_t exp = CORO_W_SWITCHING; /* yield set this before switching */
        if (!coro_at_cas(&c->w, &exp, t)) return CORO_EBUSY;
        *next = t;
        return CORO_OK;
    }
}

coro_status coro_start(coro_t* c, coro_token* next) {
    if (!c || !next) return CORO_EINVAL;
    uint64_t exp = CORO_W_NEW;
    if (!coro_at_cas(&c->w, &exp, CORO_W_RUNNING)) {
        return claim_failure(coro_at_load(&c->w));
    }
    return coro_drive(c, next);
}

coro_status coro_resume(coro_t* c, coro_token tok, coro_token* next) {
    if (!c || !next) return CORO_EINVAL;
    /* Reserved values must never reach the claim: CORO_TOKEN_NONE is not a
       credential at all, the state tags (RUNNING/SWITCHING) are internal, and
       CORO_TOKEN_DEAD aliases the DEAD state word on purpose -- presenting it
       would otherwise claim a finished coroutine as if it were parked. */
    if (tok == CORO_TOKEN_NONE || tok < CORO_W_TOKEN_BASE) return CORO_EINVAL;
    if (tok == CORO_TOKEN_DEAD) return CORO_EDESTROYED;

    uint64_t exp = (uint64_t)tok;
    if (!coro_at_cas(&c->w, &exp, CORO_W_RUNNING)) {
        return claim_failure(coro_at_load(&c->w));
    }
    return coro_drive(c, next);
}

coro_status coro_yield(void) {
    coro_t* c = coro_current();
    if (!c) return CORO_ESTALE; /* called outside any coroutine */

    /* One claim-and-switch: take the permission token, or report that we hold
       no right to yield. No switch happens on the failure path. */
    uint64_t exp = CORO_W_RUNNING;
    if (!coro_at_cas(&c->w, &exp, CORO_W_SWITCHING)) return CORO_EBUSY;

    /* Yield to whoever drove this run. The resumer is recorded by the drive
       path and never rewritten by an intervening transfer, unlike the L0
       adapter's own "last transferrer" bookkeeping. */
    if (!c->resumer) return CORO_ESTALE; /* unreachable: running implies a resumer */
    coro_ctx* resumer = c->resumer;
    coro_ctx_swap(&c->self, resumer);

    /* A cancellation requested while we were parked surfaces here, so the
       entry function can unwind and complete the coroutine. */
    /* (cancel support arrives with the cancel/join task) */
    return CORO_OK;
}
