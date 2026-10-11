/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * coro_stack.h -- opaque guarded stack type and its lifetime/query API.
 *
 * A coro_stack owns a contiguous region of memory with a hardware guard
 * page at its low address end. Because stacks grow downward on every
 * supported ABI, an overflow writes past the usable region into the guard
 * page and faults instead of silently corrupting neighbouring memory.
 *
 * The type is opaque: callers must treat it as storage only and must not
 * inspect its fields. The full definition lives in the private header
 * src/coroutine/internal.h and is shared only with the implementation.
 *
 * This header includes no Boost (or other third-party) header and compiles
 * as both C99 and C++11.
 */
#ifndef COROCC_CORO_STACK_H
#define COROCC_CORO_STACK_H

#include <stddef.h>
#include "coro_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * coro_stack -- an opaque, guard-page-protected coroutine stack.
 *
 * Callers allocate a coro_stack on the stack or in static storage, pass
 * its address to coro_stack_alloc, and query it with coro_stack_size /
 * coro_stack_avail. The struct layout is private; do not access fields.
 *
 * Lifetime: after a successful coro_stack_alloc the object must be released
 * exactly once with coro_stack_free. Freeing an already-freed or
 * zero-initialized object is a safe no-op.
 */
typedef struct coro_stack coro_stack;

/**
 * coro_stack_alloc -- allocate and protect a stack region.
 *
 * Rounds `size` up to a whole number of system pages and maps that many
 * usable bytes plus one guard page. A `size` of 0 selects the default
 * usable size.
 *
 * @param stk   Stack object to initialize. Must not be NULL.
 * @param size  Desired usable size in bytes; 0 selects the default.
 * @return CORO_OK on success;
 *         CORO_EINVAL if `stk` is NULL or `size` is too small to be served
 *         with a guard page;
 *         CORO_ENOMEM if the page size cannot be determined or the mapping
 *         or guard protection fails.
 *
 * Preconditions: `stk` points to writable storage. On failure `stk` is
 * left zero-initialized and must not be passed to coro_stack_free.
 * Thread-safety: distinct coro_stack objects may be allocated concurrently;
 * a single object must not be used from two threads simultaneously.
 */
COROCC_API coro_status coro_stack_alloc(coro_stack* stk, size_t size);

/**
 * coro_stack_free -- release a stack region allocated by coro_stack_alloc.
 *
 * Unmaps the whole region (usable bytes plus guard page) and zero-initializes
 * the object so it may be reused or safely freed again.
 *
 * @param stk  Stack object to release; NULL is accepted and ignored.
 * @return None.
 *
 * Preconditions: `stk` was either never allocated, or was allocated and not
 * subsequently freed. Idempotent: freeing a zero-initialized object is a
 * no-op. The stack must not be in use by any context when freed.
 */
COROCC_API void coro_stack_free(coro_stack* stk);

/**
 * coro_stack_size -- report the usable size of a stack in bytes.
 *
 * @param stk  Stack object to query; NULL is accepted.
 * @return The usable byte count (excluding the guard page), or 0 if `stk`
 *         is NULL or not allocated.
 */
COROCC_API size_t coro_stack_size(const coro_stack* stk);

/**
 * coro_stack_avail -- report the currently available stack bytes.
 *
 * Reports the "water level" remaining below the current usage watermark.
 * This is an advisory metric and may return the full usable size if the
 * implementation does not track a fine-grained watermark.
 *
 * @param stk  Stack object to query; NULL is accepted.
 * @return Available bytes (<= coro_stack_size(stk)), or 0 if `stk` is NULL
 *         or not allocated.
 */
COROCC_API size_t coro_stack_avail(const coro_stack* stk);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COROCC_CORO_STACK_H */
