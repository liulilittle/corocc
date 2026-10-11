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
 * as a coro_status. See docs/superpowers/specs/2026-10-11-corocc-design.md
 * section 3.3 for the contract.
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
 * coro_attr_default -- the default attributes.
 *
 * @return An attribute structure with stack_size 0 (the default size), name
 *         NULL and flags CORO_F_NONE. The returned value is suitable to pass
 *         to coro_create and may be modified field by field afterwards.
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COROCC_CORO_H */
