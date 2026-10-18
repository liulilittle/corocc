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

#include "arch/coro_atomic.h"
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
 * must outlive the instance's use. refs counts live holds by runtimes: one per
 * runtime that adopted it as its create-time default and one per live attach.
 * coro_rt_driver_unregister refuses while it is non-zero, so an instance in use
 * can never be freed. next is the process registry link.
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
    coro_rt_driver_t* d =
        (coro_rt_driver_t*)coro_atp_load((const volatile uintptr_t*)&rt->drv);
    if (d && d->ops.now) {
        return d->ops.now(d->ctx);
    }
#else
    (void)rt;
#endif
    return coro_monotonic_ns();
}

/* ---- runtime services attach/detach/pump rely on (defined in rt.c) ---- */

/**
 * rt_signal_activity -- raise the runtime's activity signal.
 *
 * With a driver attached the doorbell runs first (outside the activity lock,
 * per the wake contract); the built-in broadcast always follows, so worker
 * threads sleeping on the activity condition are woken in either family.
 */
void rt_signal_activity(coro_rt_t* rt);

/**
 * rt_alarm_wake -- the runtime's alarm callback.
 *
 * A driver invokes it on its own thread when an armed alarm fires; @p arg is
 * the runtime. It raises the activity signal so the worker whose deadline just
 * passed re-collects its timer heap. It never blocks the caller.
 */
void rt_alarm_wake(void* arg);

/* ---- registry service (defined in rt_driver.c) ---- */

/** rt_driver_registered -- whether drv is an instance the registry owns. */
int rt_driver_registered(const coro_rt_driver_t* drv);

/**
 * rt_driver_pin / rt_driver_unpin -- hold and release a registry instance.
 *
 * rt_driver_pin tests registry ownership and increments the hold count in one
 * registry-lock critical section, so the caller may safely use drv afterwards
 * (unregister is refused until the matching rt_driver_unpin). Returns 1 when
 * drv is a registered instance (and was pinned), 0 otherwise.
 */
int rt_driver_pin(coro_rt_driver_t* drv);
void rt_driver_unpin(coro_rt_driver_t* drv);

/**
 * rt_driver_default_pin -- pin and return the process default.
 *
 * Like rt_driver_default, but the default is pinned in the same critical
 * section it is read, so a concurrent set_default(NULL)+unregister cannot free
 * it before the caller stores it. Returns NULL when the built-in path is in
 * effect. The caller releases the pin with rt_driver_unpin.
 */
coro_rt_driver_t* rt_driver_default_pin(void);

#endif /* COROCC_SCHEDULER_RT_DRIVER_H */
