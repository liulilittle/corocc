#ifndef COROCC_CORO_RUNTIME_H
#define COROCC_CORO_RUNTIME_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/coroutine/coro_runtime.h -- per-thread coroutine runtime.
 *
 * One structure per thread holds everything the library needs to drive
 * coroutines on that thread: the context of the thread's own code (used as
 * the `from` side when the thread's flow resumes a coroutine) and the
 * coroutine currently running on it. The delivery queue joins it with
 * milestone M3.
 *
 * The runtime is created on first use and released by a thread-exit hook, so
 * it never outlives its thread. Its address is also the thread's owner key,
 * which is why the identity it hands out is numeric: an address into freed
 * thread storage would dangle the moment the thread exits.
 *
 * This header is INTERNAL: it is not installed and is not part of the public
 * API or ABI.
 */
#include <stdint.h>

#include <corocc/coro_ctx.h>
#include "platform/threads.h"

typedef struct coro coro_t;

/** coro_runtime -- per-thread library state (fields are private). */
typedef struct coro_runtime {
    uint64_t   id;      /**< PRIVATE: monotone thread identity, unique for life. */
    coro_ctx   driver;  /**< PRIVATE: context of this thread's own code. */
    coro_t*    current; /**< PRIVATE: coroutine running here, NULL when none. */
} coro_runtime;

/**
 * coro_runtime_current -- this thread's runtime, created on first use.
 *
 * @return The runtime of the calling thread; never NULL unless the process is
 *         out of memory, which aborts the coroutine layer's contract rather
 *         than being recoverable.
 */
coro_runtime* coro_runtime_current(void);

/**
 * coro_runtime_thread_exit -- thread-exit hook registered by the runtime.
 *
 * Frees the runtime of the exiting thread. Registered internally; declared
 * here so the hook installation stays in one place.
 */
void coro_runtime_thread_exit(void* arg);

#endif /* COROCC_CORO_RUNTIME_H */
