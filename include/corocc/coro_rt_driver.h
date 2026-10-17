/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * coro_rt_driver.h -- the runtime driver registry (milestone B, phase B1).
 *
 * A runtime contains two things a portable library cannot own by itself: a
 * doorbell to wake idle workers, a clock, and (optionally) timers. Those three
 * host needs are what the driver layer externalizes. A driver is a small vtable
 * of callbacks plus an opaque context; a process-level registry maps names to
 * driver instances, and a process default selects the driver that a runtime
 * created without an explicit attach will use. A NULL default (the initial
 * state) means the built-in EDSM backend: worker threads, the per-worker timer
 * heap and coro_monotonic_ns, exactly as before this layer existed.
 *
 * B1 lands the registry and the identity dispatch seams only: the callbacks
 * are copied and validated at registration, but no runtime attaches a driver
 * yet. Attach / detach (coro_rt_attach_driver and friends) and the loop pump
 * (coro_rt_pump) arrive in later phases and extend this same header.
 *
 * This header compiles as C99 and C++11 and includes no third-party header.
 */
#ifndef COROCC_CORO_RT_DRIVER_H
#define COROCC_CORO_RT_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#include "coro_status.h"
#include "coro_rt.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * enum coro_driver_flags -- capability bits for a driver.
 */
enum coro_driver_flags {
    CORO_DRIVER_FLAG_NONE = 0,
    /**
     * LOOP: the runtime no longer owns worker threads; the host loop drives
     * progress by calling coro_rt_pump. Without this bit the driver is SIGNAL:
     * worker threads stay, and the driver supplies only the doorbell, clock
     * and optional timers.
     */
    CORO_DRIVER_FLAG_LOOP = 1u << 0
};

/**
 * coro_driver_now_fn -- a monotone, non-decreasing nanosecond clock.
 *
 * May be called from any thread. When NULL the runtime uses its built-in
 * coro_monotonic_ns. The epoch is unspecified; only differences matter, and a
 * driver swapped in at run time must keep the same scale as the one it
 * replaces so that deadlines already stored in the timer heap stay valid.
 */
typedef uint64_t (*coro_driver_now_fn)(void* ctx);

/**
 * coro_driver_wake_fn -- the doorbell.
 *
 * The runtime raises it from any thread (a producing worker, a releaser, a
 * plain thread) whenever new runnable work appears or a sleeper may need to
 * rescan. It must be cheap, non-blocking, concurrency-safe and idempotent, and
 * it must NOT call back into the runtime synchronously (that would re-enter the
 * runtime's activity lock and self-deadlock). Mandatory: a driver without a
 * wake is rejected at registration.
 */
typedef void (*coro_driver_wake_fn)(void* ctx);

/**
 * coro_driver_timer_fn -- an alarm callback.
 *
 * Delivered exactly once by the driver on its own thread (unless the alarm was
 * cancelled first). Must not block the driver's loop thread.
 */
typedef void (*coro_driver_timer_fn)(void* arg);

/**
 * coro_driver_timer_arm_fn -- fire fn(arg) once at the absolute deadline.
 *
 * @param ctx       Driver context.
 * @param req       An opaque per-worker alarm token owned by the runtime.
 * @param deadline  Absolute nanoseconds on the same scale as the driver's now.
 * @param fn        Callback to deliver at the deadline.
 * @param arg       Opaque argument forwarded to fn.
 * @return 0 on success, non-zero when the alarm could not be armed.
 *
 * Optional (NULL means the driver has no timer capability). Non-NULL requires
 * timer_cancel to be non-NULL as well.
 */
typedef int (*coro_driver_timer_arm_fn)(void* ctx, void* req, uint64_t deadline,
                                        coro_driver_timer_fn fn, void* arg);

/**
 * coro_driver_timer_cancel_fn -- withdraw a not-yet-fired alarm.
 *
 * When this returns, fn will not run (or has already run). Returns 0 on success,
 * non-zero when the alarm had already fired or is unknown.
 */
typedef int (*coro_driver_timer_cancel_fn)(void* ctx, void* req);

/**
 * struct coro_rt_driver_ops -- the driver vtable.
 *
 * The runtime copies the whole structure at registration, so the caller may
 * release the memory it passed in. struct_size is the caller's idea of the
 * structure's size and is the version tag for forward compatibility: it must be
 * at least this build's sizeof(struct coro_rt_driver_ops); a larger value (a
 * caller built against a newer header) is accepted and its trailing fields are
 * ignored.
 */
struct coro_rt_driver_ops {
    uint32_t                    struct_size;   /**< = sizeof(struct), size/version tag. */
    uint32_t                    flags;         /**< enum coro_driver_flags. */
    coro_driver_now_fn          now;           /**< Optional; NULL = built-in clock. */
    coro_driver_wake_fn         wake;          /**< Mandatory. */
    coro_driver_timer_arm_fn    timer_arm;     /**< Optional alarm. */
    coro_driver_timer_cancel_fn timer_cancel;  /**< Required when timer_arm is set. */
};

/** coro_rt_driver_t -- an opaque, registered driver instance. */
typedef struct coro_rt_driver coro_rt_driver_t;

/**
 * coro_rt_driver_register -- create and register a driver instance.
 *
 * The ops block is copied into the instance; ctx is stored as-is and must stay
 * valid for as long as the instance may be used (through every attach). The
 * name may be NULL (an anonymous driver, not findable by name). Registration is
 * process-wide and thread-safe.
 *
 * Unlike the design's pointer-returning sketch, this entry point reports a
 * status so that a duplicate name is observable as CORO_ESTATE (ruling #2: a
 * second registration of an existing name is refused, not unified).
 *
 * @param name  Optional name; names are unique within the process.
 * @param ops   Driver vtable to copy. Must not be NULL.
 * @param ctx   Opaque context handed back to every callback.
 * @param out   Receives the new instance on success. Must not be NULL; set to
 *              NULL on every failure.
 * @return CORO_OK; CORO_EINVAL (NULL out/ops, NULL wake, struct_size smaller
 *         than this build's vtable, or a timer_arm/timer_cancel mismatch);
 *         CORO_ENOMEM; CORO_ESTATE when name is already registered.
 */
COROCC_API coro_status coro_rt_driver_register(
        const char* name, const struct coro_rt_driver_ops* ops, void* ctx,
        coro_rt_driver_t** out);

/**
 * coro_rt_driver_unregister -- release a driver instance.
 *
 * Refused with CORO_EBUSY while the instance is in use: it is currently the
 * process default (the driver that coro_rt_create reads and keeps a reference
 * to), or -- from B2 on -- attached to any runtime. On success the instance is
 * removed from the registry and freed.
 *
 * @param drv  Instance to release; NULL is accepted and ignored (idempotent).
 * @return CORO_OK on success (including NULL); CORO_EBUSY while in use;
 *         CORO_EINVAL when drv is a non-NULL pointer this registry does not own.
 */
COROCC_API coro_status coro_rt_driver_unregister(coro_rt_driver_t* drv);

/**
 * coro_rt_driver_find -- look up a registered driver by name.
 *
 * @param name  Name to look up; NULL returns NULL.
 * @return The instance, or NULL when no registered driver has that name. The
 *         runtime does not transfer ownership; the pointer is only valid until
 *         the driver is unregistered.
 */
COROCC_API coro_rt_driver_t* coro_rt_driver_find(const char* name);

/**
 * coro_rt_driver_set_default -- select the process-default driver.
 *
 * Runtimes created afterwards read this as their driver; unregistering the
 * default is refused while it is set.
 *
 * @param drv  A registered instance, or NULL to reset to the built-in EDSM
 *             path (the initial state). Must be currently registered.
 * @return CORO_OK on success; CORO_EINVAL when drv is non-NULL and not a
 *         registered instance.
 */
COROCC_API coro_status coro_rt_driver_set_default(coro_rt_driver_t* drv);

/**
 * coro_rt_driver_default -- the current process-default driver.
 *
 * @return The default instance, or NULL when the built-in EDSM path is in
 *         effect.
 */
COROCC_API coro_rt_driver_t* coro_rt_driver_default(void);

/**
 * coro_rt_attach_driver -- switch the driver a runtime uses.
 *
 * NULL is equivalent to coro_rt_detach_driver (restore the built-in EDSM
 * backend). The built-in and SIGNAL drivers may be swapped at any time, even
 * with coroutines parked: the runtime re-arms each worker's pending alarm
 * against the new driver, so no deadline is lost. LOOP is a different execution
 * model (no worker threads), so v1 refuses every cross-family switch with
 * CORO_EBUSY (a LOOP runtime must be created from a LOOP default); a same-family
 * re-attach is allowed.
 *
 * The caller must keep the driver registered and its ctx alive for the whole
 * attach.
 *
 * @return CORO_OK on success (including a same-driver no-op); CORO_EINVAL for a
 *         NULL runtime or a driver the registry does not own; CORO_EBUSY for a
 *         cross-family (LOOP vs. worker-mode) switch.
 */
COROCC_API coro_status coro_rt_attach_driver(coro_rt_t* rt,
                                             coro_rt_driver_t* drv);

/**
 * coro_rt_detach_driver -- restore the built-in EDSM backend.
 *
 * Equivalent to coro_rt_attach_driver(rt, NULL). Refused with CORO_EBUSY on a
 * LOOP runtime (its worker threads were never started, so v1 cannot rebuild
 * them).
 *
 * @return CORO_OK on success (also when no driver is attached); CORO_EINVAL for
 *         a NULL runtime; CORO_EBUSY on a LOOP runtime.
 */
COROCC_API coro_status coro_rt_detach_driver(coro_rt_t* rt);

/**
 * coro_rt_attached_driver -- the driver currently attached to a runtime.
 *
 * @return The attached driver, or NULL for the built-in EDSM backend (and for a
 *         NULL runtime).
 */
COROCC_API coro_rt_driver_t* coro_rt_attached_driver(const coro_rt_t* rt);

/**
 * coro_rt_pump -- run one bounded turn of a LOOP runtime.
 *
 * A runtime created from a LOOP default driver has no worker threads: the host
 * loop drives progress by calling coro_rt_pump. Each call drains expirations,
 * the inbox and the run queue of worker[0] up to @p budget runnable nodes
 * (budget 0 means "until nothing is runnable"), then returns; it never blocks.
 * When the loop has nothing to run, the host schedules another pump from the
 * driver's wake callback.
 *
 * @param rt      Runtime to pump.
 * @param budget  Maximum nodes to drive this turn; 0 drains everything runnable.
 * @return CORO_OK; CORO_EINVAL for a NULL runtime; CORO_EBUSY when the runtime
 *         is not a LOOP runtime, or when a pump is already running on it.
 */
COROCC_API coro_status coro_rt_pump(coro_rt_t* rt, size_t budget);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COROCC_CORO_RT_DRIVER_H */
