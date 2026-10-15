/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * coro_ctx.h -- execution context: a stack plus a saved register frame.
 *
 * A coro_ctx binds a coroutine entry function and its argument to a
 * coro_stack. Switching between two contexts saves the callee-saved
 * registers of one and restores those of the other; it performs no system
 * call and holds no lock. This is the lowest layer of the library and the
 * foundation every higher layer builds on.
 *
 * This header is conservative: it compiles as both C99 and C++11 and
 * includes no Boost header.
 */
#ifndef COROCC_CORO_CTX_H
#define COROCC_CORO_CTX_H

#include "coro_stack.h"
#include "coro_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * coro_ctx -- a complete execution context (fields are private).
 *
 * Holds the machine state required to resume a coroutine: its stack and a
 * saved register frame. The type is complete so callers may embed it by
 * value (on the stack or in a pool) with no heap allocation. The fields are
 * an implementation detail and PRIVATE: never read or write them directly.
 * The layout is part of the documented ABI.
 */
typedef struct coro_ctx {
    void*             fctx;   /* PRIVATE: saved register frame (backend-internal). */
    const coro_stack* stack;  /* PRIVATE: borrowed stack range (ASan hooks etc.). */
    void            (*entry)(void*); /* PRIVATE: coroutine entry function. */
    void*             arg;    /* PRIVATE: opaque argument forwarded to entry. */
    struct coro_ctx*  back;   /* PRIVATE: resumer context (sentinel target). */
    void*             asan_fake; /* PRIVATE: ASan fake-stack save handle handed
                                    over at a switch into this context; only
                                    meaningful when the implementation is built
                                    with COROCC_ASAN, otherwise unused. */
} coro_ctx;

/**
 * coro_ctx_init -- build an entry frame on a stack.
 *
 * Prepares the context so that the first swap into it invokes @p entry with
 * @p arg. The context borrows @p stk; the stack must outlive the context
 * and remain unchanged for the context's lifetime. On success the context
 * object is zeroed and then filled; on failure it is left untouched.
 *
 * @param cx     Context storage to initialize. Must not be NULL.
 * @param stk    Live stack (from coro_stack_alloc) to run on. Must not be
 *               NULL. Whether the stack is currently allocated is a caller
 *               precondition, not validated by the call.
 * @param entry  Coroutine entry point, called as entry(arg). Must not be
 *               NULL. Returning from entry is allowed: control returns to
 *               the context that last swapped in (never terminates the
 *               process).
 * @param arg    Opaque argument forwarded verbatim to entry.
 * @return 0 on success; CORO_EINVAL (a non-zero status code) if any argument
 *         is NULL. Failure is reported as a non-zero status, so `if (rc)`
 *         is the valid failure test.
 *
 * Precondition: cx is not already live (call coro_ctx_destroy first).
 */
COROCC_API int coro_ctx_init(coro_ctx* cx, const coro_stack* stk,
                             void (*entry)(void*), void* arg);

/**
 * coro_ctx_destroy -- discard a context's machine state.
 *
 * Releases the context's saved frame and zeroes the object. Idempotent:
 * destroying a NULL or already-destroyed context is a no-op. It does not
 * free the associated stack.
 *
 * @param cx  Context to destroy; NULL is accepted and ignored.
 * @return Nothing.
 *
 * Precondition: cx is not the currently running context and will not be
 * swapped into again.
 */
COROCC_API void coro_ctx_destroy(coro_ctx* cx);

/**
 * coro_ctx_swap -- transfer control from one context to another.
 *
 * Saves the callee-saved state of @p from, restores that of @p to, and
 * resumes execution in @p to. Must be declared and compiled as nothrow:
 * it must not propagate C++ exceptions across the boundary.
 *
 * @param from  Context to suspend. Updated in place with the resumption
 *              point so it can be resumed later. Must not be NULL.
 * @param to    Context to resume. Must not be NULL.
 * @return Nothing. The call "returns" in @p from the next time some context
 *         swaps back into it.
 *
 * Precondition: both contexts are live and distinct. No lock may be held
 * across the call.
 */
COROCC_API void coro_ctx_swap(coro_ctx* from, coro_ctx* to);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COROCC_CORO_CTX_H */
