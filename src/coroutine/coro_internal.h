#ifndef COROCC_CORO_INTERNAL_H
#define COROCC_CORO_INTERNAL_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/coroutine/coro_internal.h -- internals the higher layers are allowed to
 * touch.
 *
 * The coroutine object is opaque on purpose: coro.h never exposes its layout,
 * so the layering stays honest and the ABI can change without touching
 * consumers. The scheduler layer, however, must attach its own bookkeeping to a
 * coroutine it owns, so it gets a single opaque slot through this door instead
 * of the struct.
 *
 * This header is INTERNAL: it is not installed and is not part of the public
 * API or ABI.
 */
#include <corocc/coro.h>
#include <corocc/coro_stack.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * coro_set_sched_tag -- store the owning layer's private pointer on a handle.
 *
 * The slot starts NULL, is written once by whoever takes ownership, and is
 * cleared when the handle is destroyed. It is never interpreted by the
 * coroutine layer.
 */
COROCC_API void coro_set_sched_tag(coro_t* c, void* tag);

/**
 * coro_get_sched_tag -- read the slot written by coro_set_sched_tag.
 *
 * @return The stored pointer, or NULL when nothing owns this handle.
 */
COROCC_API void* coro_get_sched_tag(const coro_t* c);

/**
 * coro_create_on_stack -- create a coroutine around a caller-owned stack.
 *
 * The runtime's stack-reclaim pool hands back a stack retired earlier; this
 * entry builds the handle and context around it without allocating. The
 * ownership contract: on CORO_OK the coroutine owns the stack (destroy
 * releases it like any default-allocator stack); on error the caller keeps
 * the stack. Only valid when the attribute carries no custom allocator (a
 * custom allocator owns its own memory and the pool never sees it).
 */
coro_status coro_create_on_stack(coro_t** out, const coro_attr_t* attr,
                                 coro_fn fn, void* arg, coro_stack* stk);

/**
 * coro_destroy_to_stack -- destroy a coroutine, handing its stack back.
 *
 * The default-allocator counterpart of coro_destroy for the runtime's
 * reclaim pool: instead of releasing the stack it is returned in *out for
 * the caller to park or free. A handle created with a custom allocator is
 * released exactly like coro_destroy and leaves *out zeroed. Same state
 * gate as coro_destroy; *out is always zeroed first.
 */
coro_status coro_destroy_to_stack(coro_t* c, coro_stack* out);

#ifdef __cplusplus
}
#endif

#endif /* COROCC_CORO_INTERNAL_H */
