#ifndef COROCC_ARCH_TSAN_H
#define COROCC_ARCH_TSAN_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/arch/coro_tsan.h -- ThreadSanitizer fiber-switch annotations.
 *
 * The vendored Boost.Context assembly moves the machine stack (and the saved
 * register frame) with no signal to ThreadSanitizer, so without help TSan
 * attributes every access made on a coroutine stack to whatever OS thread
 * happens to run the coroutine at that instant. A coroutine created on one
 * thread and driven on another then looks like two threads sharing one stack,
 * which TSan reports as data races on memory that is in fact single-threaded.
 *
 * TSan models this pattern with "fibers": each execution flow that switches
 * stacks gets its own fiber handle (__tsan_create_fiber), and the switch is
 * announced with __tsan_switch_to_fiber so TSan follows the flow instead of
 * guessing from the OS thread. The calling thread's own (non-fiber) flow is
 * itself a fiber, obtained with __tsan_get_current_fiber; the driver context
 * in coro.c keeps no fiber of its own and resolves to it.
 *
 * Everything here compiles only when the translation unit is really
 * instrumented. GCC defines __SANITIZE_THREAD__ under -fsanitize=thread;
 * Clang defines only the __has_feature(thread_sanitizer) predicate, so both
 * spellings are checked into COROCC_TSAN (shared with the public coro_ctx.h,
 * which cannot include this internal header). A plain build sees the no-op
 * inlines below, so there is no call, no symbol and no ABI/API change (the
 * coro_ctx fiber field is guarded by the same macro).
 *
 * The __tsan_* entry points are declared here rather than pulled from
 * <sanitizer/tsan_interface.h>, keeping the headers free of any sanitizer
 * dependency (the ASan hooks next door do the same); the ABI of these four
 * calls is fixed by the compiler-rt TSan runtime both GCC and Clang link.
 *
 * This header is INTERNAL: it is not installed and is not part of the public
 * API or ABI.
 */
#include <stddef.h>

#ifndef COROCC_TSAN
#  if defined(__SANITIZE_THREAD__)
#    define COROCC_TSAN 1
#  elif defined(__has_feature)
#    if __has_feature(thread_sanitizer)
#      define COROCC_TSAN 1
#    endif
#  endif
#endif

#if defined(COROCC_TSAN)

extern void* __tsan_create_fiber(unsigned flags);
extern void* __tsan_get_current_fiber(void);
extern void  __tsan_destroy_fiber(void* fiber);
extern void  __tsan_switch_to_fiber(void* fiber, unsigned flags);

/* A fresh fiber for a newly built coroutine context. Propagating the return
   as-is: a NULL handle (the runtime never returns one in practice) simply
   degrades the switch annotation to a no-op rather than dereferencing it. */
static inline void* coro_tsan_create_fiber(void) {
    return __tsan_create_fiber(0);
}

static inline void coro_tsan_destroy_fiber(void* fiber) {
    if (fiber) __tsan_destroy_fiber(fiber);
}

/* The fiber of the calling flow right now: the thread's own fiber while the
   thread's code runs, or the coroutine's fiber once inside one. Captured at
   the only sound moment (see ctx.c) and cached per thread. */
static inline void* coro_tsan_current_fiber(void) {
    return __tsan_get_current_fiber();
}

static inline void coro_tsan_switch_to_fiber(void* fiber) {
    if (fiber) __tsan_switch_to_fiber(fiber, 0);
}

#else /* !COROCC_TSAN: zero-cost, zero-ABI no-ops */

static inline void* coro_tsan_create_fiber(void)          { return NULL; }
static inline void  coro_tsan_destroy_fiber(void* fiber)  { (void)fiber; }
static inline void* coro_tsan_current_fiber(void)         { return NULL; }
static inline void  coro_tsan_switch_to_fiber(void* fiber){ (void)fiber; }

#endif /* COROCC_TSAN */

#endif /* COROCC_ARCH_TSAN_H */
