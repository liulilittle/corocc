/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * coro.h -- coroutine object (L1).
 *
 * A coro_t is a scheduling entity bound to one execution context (stack plus
 * saved frame). It can be driven manually -- create, start, resume, yield,
 * destroy -- with no scheduler and no runtime present, which is the smallest
 * useful unit of the library; the M:N scheduler and structured-concurrency
 * layers are built on top of exactly these entry points.
 *
 * Control transfer is credential-based. Each suspension point publishes a
 * unique, monotone token, and only the holder of that token may resume the
 * coroutine. A stale or duplicated token is rejected instead of corrupting the
 * coroutine's stack, so "resume" is a claim-and-switch operation that cannot
 * double-enter a suspension point and never aborts: every failure is reported
 * as a coro_status.
 *
 * This header is conservative: it compiles as C99 and C++11, includes no
 * third-party header, and never exposes a Boost type.
 */
#ifndef COROCC_CORO_H
#define COROCC_CORO_H

#include <stddef.h>
#include <stdint.h>
#include "coro_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * CORO_DEFAULT_STACK_SIZE -- usable stack size used when an attribute
 * requests the default (stack_size == 0). One guard page is mapped below the
 * usable region, so the reserved footprint is this value plus one page.
 */
#define CORO_DEFAULT_STACK_SIZE ((size_t)(256u * 1024u))

/**
 * CORO_LOCAL_SLOTS -- number of coroutine-local storage slots.
 *
 * Local values live on the coroutine, not on the thread, so they survive
 * thread migration and are independent of thread_local. The slot count is
 * fixed: it keeps the handle a single allocation and keeps migration free of
 * any hidden heap traffic.
 */
#define CORO_LOCAL_SLOTS ((size_t)32u)

/** Attribute flag bits accepted by coro_attr_t.flags. */
#define CORO_F_NONE ((unsigned)0u) /**< No flags. The only value accepted today. */

/**
 * coro_token -- credential for one suspension point.
 *
 * Tokens are non-zero, monotone and unique per coroutine: every suspension
 * point publishes the next value of a monotone sequence. Presenting a token is
 * what authorizes a resume; an old or wrong token is rejected (CORO_ESTALE).
 */
typedef uint64_t coro_token;

/** Returned when the coroutine has no suspension to report: it finished. */
#define CORO_TOKEN_NONE ((coro_token)0)

/** Sentinel token meaning "the coroutine ran to completion and is dead". */
#define CORO_TOKEN_DEAD ((coro_token)~(coro_token)0)

/**
 * coro_fn -- coroutine entry point.
 *
 * Runs on the coroutine's own stack. Returning from it ends the coroutine:
 * control goes back to the context that last resumed it, and the coroutine
 * becomes CORO_DEAD. Returning from the entry is NOT a process exit.
 */
typedef void (*coro_fn)(void* arg);

/**
 * coro_state -- observable coroutine state.
 *
 * The state a manually driven coroutine can observe is NEW, RUNNING,
 * SUSPENDED or DEAD. CORO_READY is recorded for the documented state machine
 * and is produced by the scheduler layer: a coroutine sitting in a run queue
 * reads CORO_READY, one that a caller is about to drive by hand reads NEW or
 * SUSPENDED.
 */
typedef enum {
    CORO_NEW = 0,   /**< Created, never started. */
    CORO_READY,     /**< Runnable and queued by the scheduler layer. */
    CORO_RUNNING,   /**< Currently executing on some thread. */
    CORO_SUSPENDED, /**< Parked at a suspension point holding a token. */
    CORO_DEAD       /**< Entry returned or cancellation finished the coroutine. */
} coro_state;

typedef struct coro_allocator coro_allocator_t;

/**
 * coro_attr_t -- coroutine creation attributes.
 *
 * Pass a pointer to a filled-in structure, or NULL for the defaults. The
 * strings and sizes are copied or borrowed as documented per field; the
 * structure itself may be discarded after coro_create returns.
 */
typedef struct coro_attr {
    size_t      stack_size; /**< Desired usable stack bytes; 0 selects CORO_DEFAULT_STACK_SIZE. */
    const char *name;       /**< Optional diagnostic name. Borrowed: the caller keeps it alive. */
    unsigned    flags;      /**< CORO_F_*; anything but CORO_F_NONE is rejected. */
    const coro_allocator_t *allocator; /**< Custom stack/handle allocator, or NULL for defaults. */
} coro_attr_t;

/**
 * coro_t -- coroutine handle (opaque).
 *
 * A live coroutine object owns one stack and one execution context. Handles
 * are created with coro_create and released with coro_destroy. The layout is
 * internal on purpose: the public contract is the set of entry points, never
 * the fields.
 */
typedef struct coro coro_t;

/**
 * coro_allocator_t -- custom memory allocator for coroutine stacks and handles.
 *
 * When attr->allocator is non-NULL, coro_create uses alloc_stack to obtain
 * the stack memory and alloc_handle to allocate the coro_t handle itself;
 * coro_destroy calls free_stack and free_handle to release them. When NULL,
 * the library uses its built-in mmap-based stack and calloc/free.
 *
 * The allocator must remain valid for the lifetime of every coroutine created
 * with it. All callbacks must be thread-safe.
 */
/* The typedef lives at the top of this header (next to coro_t); defining the
   struct again with a trailing typedef here would redefine the name, which
   strict C11-only toolchains (NDK clang -Wtypedef-redefinition) reject. */
struct coro_allocator {
    coro_status (*alloc_stack)(void** mem, size_t size);
    void        (*free_stack)(void* mem);
    void*       (*alloc_handle)(size_t size);
    void        (*free_handle)(void* p);
};

/**
 * coro_attr_default -- the default attributes.
 *
 * @return An attribute structure with stack_size 0 (the default size), name
 *         NULL, flags CORO_F_NONE, and allocator NULL. The returned value is
 *         suitable to pass to coro_create and may be modified field by field
 *         afterwards.
 */
COROCC_API coro_attr_t coro_attr_default(void);

/**
 * coro_create -- create a coroutine that will run @p fn on its own stack.
 *
 * Allocates the handle, maps a guard-page-protected stack and builds the
 * entry frame. The coroutine is idle afterwards: it runs nothing until the
 * first start/resume, so no user code executes during this call.
 *
 * @param out  Receives the new handle. Must not be NULL; on failure it is
 *             left untouched.
 * @param attr  Attributes, or NULL for coro_attr_default(). A structure with
 *             unknown flags is rejected.
 * @param fn   Entry point, called as fn(arg). Must not be NULL.
 * @param arg  Opaque argument forwarded verbatim to fn.
 * @return CORO_OK on success; CORO_EINVAL if out or fn is NULL, attr has
 *         unknown flags, or the requested stack size cannot be served with a
 *         guard page; CORO_ENOMEM if the handle or the stack mapping fails.
 *
 * On failure *out is untouched and no resource is retained.
 */
COROCC_API coro_status coro_create(coro_t** out, const coro_attr_t* attr,
                                   coro_fn fn, void* arg);

/**
 * coro_destroy -- release a coroutine handle and everything it owns.
 *
 * Marks the coroutine dead, releases its stack and frees the handle. Must be
 * called exactly once per coro_create, like free() on a pointer: destroying a
 * handle twice, or after a failed create, is undefined.
 *
 * Destroying a coroutine that is currently running -- including the running
 * coroutine destroying itself, which would free the stack it is executing on
 * -- is refused with CORO_EBUSY and changes nothing. A coroutine that another
 * thread is driving has the same protection. Destroying a NEW, SUSPENDED or
 * completed coroutine is always allowed.
 *
 * @param c  Handle to release, or NULL (accepted, returns CORO_EINVAL).
 * @return CORO_OK on success; CORO_EBUSY if the coroutine is running or its
 *         state changed concurrently (safe to re-read and retry);
 *         CORO_EINVAL if c is NULL.
 */
COROCC_API coro_status coro_destroy(coro_t* c);

/**
 * coro_get_state -- report the coroutine's observable state.
 *
 * Reads the coroutine's single state word. NULL returns CORO_NEW, the zero
 * value of the enumeration: a NULL handle is a caller bug that this query
 * refuses to turn into a crash.
 *
 * @param c  Handle to query.
 * @return One of CORO_NEW, CORO_READY, CORO_RUNNING, CORO_SUSPENDED,
 *         CORO_DEAD.
 */
COROCC_API coro_state coro_get_state(const coro_t* c);

/**
 * coro_start -- start a coroutine that has never run.
 *
 * Claims the coroutine (NEW -> RUNNING with a single compare-and-swap),
 * transfers control to its entry function and waits for it to either park or
 * finish. Equivalent to resuming the implicit "never started" credential.
 *
 * @param c     Handle to start.
 * @param next  Receives the credential for the suspension point the coroutine
 *              parked at, or CORO_TOKEN_DEAD if the entry function returned.
 *              Only written on success.
 * @return CORO_OK on success;
 *         CORO_EINVAL if c or next is NULL;
 *         CORO_EBUSY if the coroutine is running or mid-switch on some thread,
 *         or if it belongs to another thread (see below);
 *         CORO_ESTALE if it was already started and parked (resume it with the
 *         token you were given instead of starting it again);
 *         CORO_EDESTROYED if it is already dead.
 *
 * Thread driving: control is not bound to the creating thread. Each
 * suspension point publishes a fresh credential, and whichever thread holds
 * it may start or resume the coroutine; the claim is a compare-and-swap, so
 * concurrent drives fail safely with CORO_EBUSY and exactly one execution
 * flow enters the coroutine.
 */
COROCC_API coro_status coro_start(coro_t* c, coro_token* next);

/**
 * coro_resume -- resume a parked coroutine by presenting its credential.
 *
 * The only way to wake a coroutine: present the token that its previous
 * suspension point published. A successful call claims the coroutine
 * (token -> RUNNING), switches to it, and only publishes the next credential
 * after the switch has fully saved the coroutine's frame, so no observer can
 * see a half-stopped coroutine as parked.
 *
 * Exactly one holder of a token can claim it: after a successful resume the
 * token is consumed. Presenting the same token twice, or an older one, fails
 * with CORO_ESTALE and leaves the coroutine untouched.
 *
 * @param c     Handle to resume.
 * @param tok   Credential obtained from the previous coro_start/coro_resume.
 * @param next  Receives the credential of the next suspension point, or
 *              CORO_TOKEN_DEAD if the entry function returned. Only written on
 *              success.
 * @return CORO_OK on success;
 *         CORO_EINVAL if c or next is NULL, or tok is not a credential
 *         (CORO_TOKEN_NONE or a reserved internal value);
 *         CORO_EDESTROYED if tok is the CORO_TOKEN_DEAD sentinel the coroutine
 *         already published, i.e. the coroutine has finished;
 *         CORO_ESTALE if tok is not the live credential (stale or never
 *         issued);
 *         CORO_EBUSY if the coroutine is running or mid-switch.
 */
COROCC_API coro_status coro_resume(coro_t* c, coro_token tok,
                                   coro_token* next);

/**
 * coro_yield -- park the running coroutine and return control to its resumer.
 *
 * Must be called from inside a coroutine. It gives up the coroutine's
 * permission token, switches back to the context that resumed it, and returns
 * when that context (or another holder of the credential it will publish)
 * resumes the coroutine again.
 *
 * Yield is cooperative: a coroutine that never yields can never be cancelled,
 * preempted or starve another thread of the processor.
 *
 * @return CORO_OK when the coroutine was resumed;
 *         CORO_ECANCELED when it was resumed and a cancellation had been
 *         requested in the meantime (the entry function should return);
 *         CORO_EBUSY if the coroutine lost its permission token to a
 *         concurrent actor, in which case no switch happened;
 *         CORO_ESTALE if called outside any coroutine (nothing to yield).
 */
COROCC_API coro_status coro_yield(void);

/**
 * coro_pump -- run every wakeup request waiting in this thread's queue.
 *
 * Wakeup requests can be queued for this thread instead of driving a
 * coroutine in place; coro_pump executes them at a point of the caller's
 * choosing (a blocking wait, a scheduler tick, or simply between operations).
 * Claiming each request through the same compare-and-swap the inline resume
 * uses is what keeps exactly one execution flow entering any suspension point.
 *
 * Each queued request is claimed once, with the same compare-and-swap the
 * inline resume uses, so a request that loses the claim is dropped as a
 * rejected credential rather than driving the coroutine twice. A coroutine
 * whose entry finishes during a pumped drive publishes CORO_TOKEN_DEAD.
 *
 * @return How many requests were served. 0 when the queue was empty.
 */
COROCC_API size_t coro_pump(void);

/**
 * coro_pump_wait -- sleep until a wakeup arrives, then run what is queued.
 *
 * The owning thread's blocking wait: it sleeps on a condition variable (never
 * polls) and returns to its caller after draining every request that was
 * waiting, exactly like coro_pump. Use it when a thread has nothing to do but
 * serve coroutines; use coro_pump when the thread has its own work and only
 * wants to sweep the queue.
 *
 * A caller that must serve a known number of requests should count what this
 * returns as well, because one pumped batch can serve many requests at once.
 *
 * Must be called from the owning thread.
 *
 * @return How many requests were served by the drain that followed the wait.
 */
COROCC_API size_t coro_pump_wait(void);

/**
 * coro_cancel -- request that a coroutine stop at its next safe point.
 *
 * Cooperative: the request is recorded and observed by the coroutine when the
 * next coro_yield returns, which then reports CORO_ECANCELED to the entry
 * function so it can unwind. Nothing is preempted mid-execution and no state
 * transition is forced, so a parked, new or already dead coroutine accepts the
 * request without complaint; a coroutine that never yields never observes it.
 *
 * @param c  Handle to cancel, or NULL.
 * @return CORO_OK on success; CORO_EINVAL if c is NULL.
 */
COROCC_API coro_status coro_cancel(coro_t* c);

/**
 * coro_join -- wait until a coroutine has finished.
 *
 * The coroutine must already be dead: without a runtime layer there is nothing
 * to park the caller in, and this contract never spins waiting for one. Drive
 * the coroutine to completion (its coro_start/coro_resume returns
 * CORO_TOKEN_DEAD) and then join. A blocking join ships with the scheduler.
 *
 * @param c  Handle to wait for, or NULL.
 * @return CORO_OK if the coroutine is dead; CORO_EAGAIN if it is still live;
 *         CORO_EINVAL if c is NULL.
 */
COROCC_API coro_status coro_join(coro_t* c);

/**
 * coro_local_get -- read a coroutine-local slot.
 *
 * Local values live on the coroutine, not on the thread, so they survive
 * thread migration and are invisible to thread_local. A slot that was never
 * written reads NULL.
 *
 * @param c    Handle to read from.
 * @param key  Slot index, must be < CORO_LOCAL_SLOTS.
 * @return The stored value, or NULL for an unset slot or an invalid request.
 */
COROCC_API void* coro_local_get(const coro_t* c, size_t key);

/**
 * coro_local_set -- write a coroutine-local slot.
 *
 * May be called by anyone holding the handle, before, during or between runs;
 * it never touches the coroutine's state word and never blocks it.
 *
 * @param c    Handle to write to.
 * @param key  Slot index, must be < CORO_LOCAL_SLOTS.
 * @param val  Value to store; NULL clears the slot.
 * @return CORO_OK on success; CORO_EINVAL if c is NULL or key is out of range.
 */
COROCC_API coro_status coro_local_set(coro_t* c, size_t key, void* val);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COROCC_CORO_H */
