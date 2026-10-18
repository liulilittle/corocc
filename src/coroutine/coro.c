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
#include "coroutine/coro_internal.h"
#include "coroutine/coro_runtime.h"

#include <stdlib.h>
#include <string.h>

#ifdef COROCC_PROBE_NOSPIN
/* Counter of every compare-and-swap the library performs, used by the
   zero-spin test to prove a control path makes exactly one attempt. */
unsigned long coro_probe_cas_count = 0;
#endif

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
    void*       sched_tag; /**< Opaque back-pointer the scheduler layer stores on
                               coroutines it owns; never read by this layer. */
    uint64_t    cancel_req; /**< Set once a cancellation was requested; read at
                                safe points, never part of W. */
    void*       local[CORO_LOCAL_SLOTS]; /**< Coroutine-local slots: live on the
                                             coroutine, so they move with it. */
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
   (spec 5.5): this pointer is the library's own bookkeeping, not user state.
   It lives in the per-thread runtime together with the thread's driver
   context and identity. */
static coro_t* coro_current(void) {
    return coro_runtime_current()->current;
}

static void coro_set_current(coro_t* c) {
    coro_runtime_current()->current = c;
}

/* Context of the thread's own code, used as the `from` side when this thread's
   flow resumes a coroutine. coro_ctx_swap fills its frame pointer on the first
   switch, so a zeroed context is a valid starting state. */
static coro_ctx* coro_driver_ctx(void) {
    return &coro_runtime_current()->driver;
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
    a.allocator = NULL;
    return a;
}

/* Classify a stack-allocation failure for coro_create's documented return set
   (coro.h:164): a size the stack layer refuses cannot be served with a guard
   page, which is CORO_EINVAL; every other failure is an allocation/mapping
   failure, CORO_ENOMEM. Both coro_stack_alloc and a custom allocator report
   this distinction, so it is preserved instead of flattened to CORO_ENOMEM. */
static coro_status stack_alloc_fail(coro_status st) {
    return (st == CORO_EINVAL) ? CORO_EINVAL : CORO_ENOMEM;
}

coro_status coro_create(coro_t** out, const coro_attr_t* attr, coro_fn fn,
                        void* arg) {
    if (!out || !fn) return CORO_EINVAL;

    coro_attr_t a = attr ? *attr : coro_attr_default();
    if (a.flags != CORO_F_NONE) return CORO_EINVAL;

    size_t stk_size = a.stack_size ? a.stack_size : CORO_DEFAULT_STACK_SIZE;

    coro_t* c;
    if (a.allocator) {
        c = (coro_t*)a.allocator->alloc_handle(sizeof *c);
        if (!c) return CORO_ENOMEM;
        /* The handle comes from the caller's allocator, which is not required
           to zero its memory. Everything the create path does not assign
           explicitly -- `cancel_req` (a nonzero leftover reads as "cancel
           requested", so the first yield would report CORO_ECANCELED),
           `sched_tag` and the local slots -- must start zeroed; the built-in
           path calloc's for the same reason. */
        memset(c, 0, sizeof *c);
        void* mem = NULL;
        coro_status sst = a.allocator->alloc_stack(&mem, stk_size);
        if (sst != CORO_OK) {
            a.allocator->free_handle(c);
            return stack_alloc_fail(sst);
        }
        c->stack.base = mem;
        c->stack.total = stk_size;
        c->stack.lo = (char*)mem;
        c->stack.usable = stk_size;
        c->stack.guard_bytes = 0;
    } else {
        c = (coro_t*)calloc(1, sizeof *c);
        if (!c) return CORO_ENOMEM;
        coro_status sst = coro_stack_alloc(&c->stack, stk_size);
        if (sst != CORO_OK) {
            free(c);
            return stack_alloc_fail(sst);
        }
    }
    c->fn = fn;
    c->arg = arg;
    c->attr = a;
    if (coro_ctx_init(&c->self, &c->stack, coro_entry, c) != 0) {
        if (a.allocator) {
            a.allocator->free_stack(c->stack.base);
            a.allocator->free_handle(c);
        } else {
            coro_stack_free(&c->stack);
            free(c);
        }
        return CORO_EINVAL;
    }
    coro_at_store(&c->w, CORO_W_NEW);
    *out = c;
    return CORO_OK;
}

/* Internal: build a coroutine handle around a stack the caller hands over
   (the runtime's reclaim pool). Mirrors coro_create's default-allocator
   path with the stack allocation replaced by the transfer. See
   coro_internal.h for the ownership contract. */
coro_status coro_create_on_stack(coro_t** out, const coro_attr_t* attr,
                                 coro_fn fn, void* arg, coro_stack* stk) {
    if (!out || !fn || !stk || !stk->base) return CORO_EINVAL;

    coro_attr_t a = attr ? *attr : coro_attr_default();
    if (a.flags != CORO_F_NONE) return CORO_EINVAL;
    if (a.allocator) return CORO_EINVAL; /* the pool never serves these */

    coro_t* c = (coro_t*)calloc(1, sizeof *c);
    if (!c) return CORO_ENOMEM;
    c->stack = *stk;
    c->fn = fn;
    c->arg = arg;
    c->attr = a;
    if (coro_ctx_init(&c->self, &c->stack, coro_entry, c) != 0) {
        free(c);
        return CORO_EINVAL; /* the stack stays the caller's */
    }
    coro_at_store(&c->w, CORO_W_NEW);
    *out = c;
    return CORO_OK;
}

/* Internal: the destroy path of the reclaim pool. Identical to coro_destroy
   up to the release; the stack then goes to *out instead of being freed. A
   handle created with a custom allocator is released through that allocator
   exactly as coro_destroy does, and *out stays zeroed. */
coro_status coro_destroy_to_stack(coro_t* c, coro_stack* out) {
    if (!c || !out) return CORO_EINVAL;
    memset(out, 0, sizeof *out);
    if (coro_current() == c) return CORO_EBUSY;

    uint64_t cur = coro_at_load(&c->w);
    if (cur == CORO_W_RUNNING || cur == CORO_W_SWITCHING) return CORO_EBUSY;
    if (!coro_at_cas(&c->w, &cur, CORO_W_DEAD)) return CORO_EBUSY;

    coro_ctx_destroy(&c->self);
    if (c->attr.allocator) {
        c->attr.allocator->free_stack(c->stack.base);
        c->attr.allocator->free_handle(c);
    } else {
        *out = c->stack;
        free(c);
    }
    return CORO_OK;
}

coro_status coro_destroy(coro_t* c) {
    if (!c) return CORO_EINVAL;
    if (coro_current() == c) return CORO_EBUSY;

    uint64_t cur = coro_at_load(&c->w);
    if (cur == CORO_W_RUNNING || cur == CORO_W_SWITCHING) return CORO_EBUSY;
    if (!coro_at_cas(&c->w, &cur, CORO_W_DEAD)) return CORO_EBUSY;

    coro_ctx_destroy(&c->self);
    if (c->attr.allocator) {
        c->attr.allocator->free_stack(c->stack.base);
        c->attr.allocator->free_handle(c);
    } else {
        coro_stack_free(&c->stack);
        free(c);
    }
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

coro_status coro_cancel(coro_t* c) {
    if (!c) return CORO_EINVAL;
    /* A request, not a transition: the coroutine observes it at its next safe
       point (the return from coro_yield). Nothing is preempted, no state is
       moved, so this never races with a drive and never fails on a dead or
       parked coroutine. */
    coro_at_store(&c->cancel_req, 1);
    return CORO_OK;
}

/* F2 (round-8): expose the outstanding cancellation request to the scheduler
   layer so a lock handoff can avoid evaporating the token on a cancel-bound
   waiter. Semantics, explicitly:
   - Internal only (coro_internal.h; not installed, not public ABI). Callers
     pass a live handle they own for the duration of the call; the probe does
     not dereference anything beyond cancel_req and never runs on a collected
     or destroyed handle. Reading a DEAD coroutine is safe while the handle is
     alive (the flag is just a leftover read as "cancel pending"); the scheduler
     only ever probes handles of nodes it currently tracks.
   - Racy by construction with take_cancellation (the coroutine layer's own
     consume): a true read may already be consumed -- a false positive for the
     handoff loop, which merely pops and serves one more waiter (harmless,
     bounded); a false negative cannot happen after the scheduler observed a
     parked waiter's state (the request is set before any resume can consume
     it, and resume implies the pre-switch exit that the state check catches).
   - Chain hygiene: a waiter that leaves its park (cancel branch, woken path)
     unlinks itself via rt_detach before it can retire, so a stale chain entry
     can never reference a freed node; the releaser's q_pop_wakeable unlinks
     every node it serves, so the two never race on the same link.
   Return: int, 1 when a request is outstanding, 0 otherwise. */
int coro_cancel_pending(const coro_t* c) {
    return c != NULL && coro_at_load((volatile uint64_t*)&c->cancel_req) != 0;
}

coro_status coro_join(coro_t* c) {
    if (!c) return CORO_EINVAL;
    /* Manual mode has no scheduler to park the caller in, and the zero-spin
       contract forbids polling, so joining reports whether the coroutine is
       already dead; the caller drives it to completion and joins afterwards.
       A blocking join ships with the runtime layer. */
    return coro_get_state(c) == CORO_DEAD ? CORO_OK : CORO_EAGAIN;
}

void* coro_local_get(const coro_t* c, size_t key) {
    if (!c || key >= CORO_LOCAL_SLOTS) return NULL;
    return c->local[key];
}

coro_status coro_local_set(coro_t* c, size_t key, void* val) {
    if (!c) return CORO_EINVAL;
    if (key >= CORO_LOCAL_SLOTS) return CORO_EINVAL;
    c->local[key] = val;
    return CORO_OK;
}

/* The owning layer's private slot; see coro_internal.h for why this door
   exists instead of exposing the struct. */
void coro_set_sched_tag(coro_t* c, void* tag) {
    if (c) c->sched_tag = tag;
}

void* coro_get_sched_tag(const coro_t* c) {
    return c ? c->sched_tag : NULL;
}

/* Drive a coroutine after its claim succeeded: record the resumer, switch to
   it, then settle. `prev` is the coroutine that was running on this thread
   (NULL for the thread's own code); the driver context supplies `from` in
   that case.
 *
 * Any thread may drive any coroutine: the claim (a single compare-and-swap on
   the state word) is what keeps exactly one execution flow inside the
   coroutine, and `from` is always the *calling* thread's own context, so a
   coroutine yields back to whichever thread last drove it. The owner field is
   gone by design -- migration between threads is a first-class feature, not a
   refusal. */
static coro_status coro_drive(coro_t* c, coro_token* next) {
    coro_t* prev = coro_current();
    coro_ctx* from = prev ? &prev->self : coro_driver_ctx();
    c->resumer = from;
    coro_set_current(c);
    coro_ctx_swap(from, &c->self);
    coro_set_current(prev);

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

/* Attempt one claim and drive, sharing the exact path the inline resume uses.
   Returns the status of the drive attempt. */
static coro_status coro_claim_and_drive(coro_t* c, coro_token tok,
                                        coro_token* next) {
    if (tok == CORO_TOKEN_NONE || tok < CORO_W_TOKEN_BASE) return CORO_EINVAL;
    if (tok == CORO_TOKEN_DEAD) return CORO_EDESTROYED;

    uint64_t exp = (uint64_t)tok;
    if (!coro_at_cas(&c->w, &exp, CORO_W_RUNNING)) {
        return claim_failure(coro_at_load(&c->w));
    }
    return coro_drive(c, next);
}

coro_status coro_resume(coro_t* c, coro_token tok, coro_token* next) {
    if (!c || !next) return CORO_EINVAL;
    /* Manual-mode resume: claim the credential and drive the coroutine right
       here, on this thread, no matter which thread drove it last. Exactly one
       driver wins the claim, so concurrent resumes from several threads are
       safe: losers report a status code and the coroutine is untouched. */
    return coro_claim_and_drive(c, tok, next);
}

size_t coro_pump(void) {
    size_t served = 0;
    for (;;) {
        coro_delivery* d = coro_runtime_pop(coro_runtime_current());
        if (!d) return served;
        d->published = CORO_TOKEN_NONE;
        d->status = coro_claim_and_drive(d->coro, d->tok, &d->published);
        d->done = 1;
        if (d->signal) coro_wait_raise(d->signal);
        ++served;
    }
}

size_t coro_pump_wait(void) {
    coro_runtime* r = coro_runtime_current();
    coro_runtime_wait_work(r);
    return coro_pump();
}

/* Observe a pending cancellation, consuming it. A cancellation is seen at a   safe point -- before parking and after being resumed -- so a request that
   arrives while the coroutine is running surfaces at the very next yield
   instead of one round trip later. */
static int take_cancellation(coro_t* c) {
    if (coro_at_load(&c->cancel_req)) {
        coro_at_store(&c->cancel_req, 0);
        return 1;
    }
    return 0;
}

coro_status coro_yield(void) {
    coro_t* c = coro_current();
    if (!c) return CORO_ESTALE; /* called outside any coroutine */

    /* Cancelled before we even park: the entry function must unwind now. */
    if (take_cancellation(c)) return CORO_ECANCELED;

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
    if (take_cancellation(c)) return CORO_ECANCELED;
    return CORO_OK;
}
