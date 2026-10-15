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
 * coroutine currently running on it, plus its delivery queue.
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

#include <corocc/coro.h>
#include <corocc/coro_ctx.h>
#include "platform/coro_plat_locks.h"
#include "platform/threads.h"
#include "platform/wait.h"

/* coro_t is typedef'ed by the public header above (coro.h); repeating the
   typedef here would be a typedef redefinition, which strict C11-only
   toolchains (NDK clang -Wtypedef-redefinition) reject. */

/** coro_delivery -- one wakeup request handed to a coroutine's owner. */
typedef struct coro_delivery {
    struct coro_delivery* next; /**< PRIVATE: intrusive queue link. */
    coro_t*              coro; /**< PRIVATE: coroutine to wake. */
    coro_token           tok;  /**< PRIVATE: credential to present. */
    coro_token           published; /**< PRIVATE: credential the owner produced. */
    int                  done;      /**< PRIVATE: set once the owner handled it. */
    coro_status          status;    /**< PRIVATE: result of the owner's attempt. */
    struct coro_wait*    signal;    /**< PRIVATE: completion signal, may be NULL. */
} coro_delivery;

/** coro_runtime -- per-thread library state (fields are private). */
typedef struct coro_runtime {
    uint64_t        id;      /**< PRIVATE: monotone thread identity, unique for life. */
    coro_ctx        driver;  /**< PRIVATE: context of this thread's own code. */
    coro_t*         current; /**< PRIVATE: coroutine running here, NULL when none. */
    coro_delivery*  head;    /**< PRIVATE: first pending delivery, NULL when empty. */
    coro_delivery*  tail;    /**< PRIVATE: last pending delivery, NULL when empty. */
    coro_plat_mutex_t lock;  /**< PRIVATE: guards the queue, never held across a switch. */
    coro_wait       work;    /**< PRIVATE: raised whenever a delivery is queued. */
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
 * coro_runtime_enqueue -- append a delivery to this thread's queue.
 *
 * Called by any thread that wants to wake a coroutine this thread owns. The
 * node belongs to the caller: it must stay alive until the queue consumes it,
 * which the caller guarantees by waiting for completion or by keeping it on
 * its own stack while it blocks.
 *
 * Thread safety: safe to call concurrently with other enqueues to the same
 * runtime; the queue has a single consumer, this thread.
 *
 * @param r    Runtime to enqueue into.
 * @param node Delivery node to append; its coro and tok must already be set.
 */
void coro_runtime_enqueue(coro_runtime* r, coro_delivery* node);

/**
 * coro_runtime_pop -- take the oldest pending delivery, if any.
 *
 * Single-consumer only: the owning thread calls it. Returns NULL when the
 * queue is empty.
 *
 * @param r  Runtime to drain.
 * @return The oldest delivery, or NULL when there is nothing to do.
 */
coro_delivery* coro_runtime_pop(coro_runtime* r);

/** coro_runtime_register -- record this runtime under its identity. */
void coro_runtime_register(coro_runtime* r);

/** coro_runtime_unregister -- remove this runtime from the registry. */
void coro_runtime_unregister(coro_runtime* r);

/**
 * coro_runtime_wait_work -- sleep until a delivery is queued for this thread.
 *
 * The owner-side half of cross-thread wakeup: it sleeps instead of polling, and
 * returns as soon as work is available (or immediately if work is already
 * waiting). Reset-then-check ordering means a request that arrives at any point
 * is observed.
 *
 * @param r  Runtime to wait on.
 */
void coro_runtime_wait_work(coro_runtime* r);

/**
 * coro_runtime_signal_work -- wake a thread waiting in coro_runtime_wait_work*.
 *
 * Used when the queue itself did not change but the work state did -- a
 * cancelled sleep, for instance, whose deadline left the timer heap.
 */
void coro_runtime_signal_work(coro_runtime* r);

/**
 * coro_runtime_wait_work_until -- sleep until a delivery is queued or a
 * deadline passes.
 *
 * The scheduler's idle wait: it sleeps instead of polling, wakes as soon as
 * work arrives, and wakes at the deadline so a timer can fire on time. Same
 * reset-then-check ordering as coro_runtime_wait_work, so no wakeup is lost.
 *
 * @param r         Runtime to wait on.
 * @param deadline  Absolute monotone nanoseconds, as coro_monotonic_ns reports.
 */
void coro_runtime_wait_work_until(coro_runtime* r, uint64_t deadline);

/**
 * coro_runtime_by_id -- the runtime of a thread, by identity.
 *
 * Used by the delivery path to find the queue of the thread that owns a
 * coroutine. Returns NULL when that thread has no runtime (it never drove a
 * coroutine) or has already left, which the caller reports as a delivery
 * failure rather than a crash.
 *
 * @param id  Thread identity recorded in the coroutine's owner field.
 * @return The runtime, or NULL.
 */
coro_runtime* coro_runtime_by_id(uint64_t id);

/**
 * coro_runtime_pending -- number of deliveries still queued for this thread.
 *
 * @param r  Runtime to inspect.
 * @return How many wakeup requests the owner has not handled yet.
 */
size_t coro_runtime_pending(const coro_runtime* r);

/**
 * coro_runtime_drain -- remove every pending delivery and report them failed.
 *
 * Called when the owning thread leaves: a request nobody will serve must be
 * completed with CORO_EDESTROYED so the thread waiting on it is released and
 * nothing is left pointing at this runtime.
 *
 * @param r  Runtime to drain; may be NULL.
 */
void coro_runtime_drain(coro_runtime* r);

/**
 * coro_runtime_thread_exit -- thread-exit hook registered by the runtime.
 *
 * Frees the runtime of the exiting thread. Registered internally; declared
 * here so the hook installation stays in one place.
 */
void coro_runtime_thread_exit(void* arg);

#endif /* COROCC_CORO_RUNTIME_H */
