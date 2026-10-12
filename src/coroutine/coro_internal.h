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

#ifdef __cplusplus
}
#endif

#endif /* COROCC_CORO_INTERNAL_H */
