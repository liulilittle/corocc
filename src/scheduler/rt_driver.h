#ifndef COROCC_SCHEDULER_RT_DRIVER_H
#define COROCC_SCHEDULER_RT_DRIVER_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/scheduler/rt_driver.h -- internal shape of the runtime driver layer.
 *
 * The runtime talks to its host through a driver: a copied vtable plus an opaque
 * context. A runtime with rt->drv == NULL uses the built-in EDSM backend, and
 * every dispatch point in the hot path is a single predictable NULL test that
 * executes the original statement when no driver is attached. That is what keeps
 * the default path bit-for-bit identical to the pre-driver runtime.
 *
 * B1 defines the instance shape, the clock dispatch helper (rt_now) and the
 * compile-time switch COROCC_RT_DRIVER. The doorbell dispatch lives inline in
 * rt.c's signal_activity (it must fall through to the activity lock/broadcast,
 * which is private to that translation unit). The alarm re-arm and the
 * attach/detach state machine arrive with B2.
 *
 * Build switch: define COROCC_RT_DRIVER=0 to compile the dispatch branches out
 * entirely (embedded builds that never attach a driver); rt_now then reduces to
 * coro_monotonic_ns and signal_activity to its original body.
 *
 * This header is INTERNAL: it is not installed and is not part of the public
 * API or ABI.
 */
#include <corocc/coro_rt_driver.h>

#include "scheduler/rt.h"

#ifndef COROCC_RT_DRIVER
#define COROCC_RT_DRIVER 1
#endif

/**
 * struct coro_rt_driver -- one registered driver instance.
 *
 * The public header declares the tag; only this internal header completes it.
 * A driver is process-level state: it is not owned by any runtime, and the
 * registry (rt_driver.c) is the only code that creates, links and frees one.
 * ops is a private copy of the caller's vtable; ctx is the caller's pointer and
 * must outlive the instance's use. refs counts live attaches (B2); until then it
 * stays zero. next is the process registry link.
 */
struct coro_rt_driver {
    struct coro_rt_driver_ops ops;   /**< Private copy of the caller's vtable. */
    void*                    ctx;    /**< Caller context, forwarded to callbacks. */
    char*                    name;   /**< Owned copy of the name, or NULL. */
    unsigned                 refs;   /**< Live attaches (0 in B1). */
    struct coro_rt_driver*   next;   /**< Process registry link. */
};

/**
 * rt_now -- the runtime's monotone clock.
 *
 * A single NULL test: an attached driver with a now callback wins, otherwise
 * the built-in coro_monotonic_ns. With COROCC_RT_DRIVER=0 the test is compiled
 * out and the built-in clock is always used.
 */
static inline uint64_t rt_now(const coro_rt_t* rt) {
#if COROCC_RT_DRIVER
    if (rt->drv && rt->drv->ops.now) {
        return rt->drv->ops.now(rt->drv->ctx);
    }
#else
    (void)rt;
#endif
    return coro_monotonic_ns();
}

#endif /* COROCC_SCHEDULER_RT_DRIVER_H */
