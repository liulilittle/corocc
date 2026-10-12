/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * coro_sched.h -- optional single-thread scheduler for coroutines.
 *
 * This layer is optional: a coroutine created with coro_create can be driven
 * by hand with coro_start/coro_resume/coro_yield, exactly as documented in
 * coro.h, and nothing here changes that. What the scheduler adds is a run loop
 * that drives coroutines until they park, a queue so a coroutine can be handed
 * to the scheduler instead of to a caller, blocking joins, and timers.
 *
 * The run loop never spins. When no coroutine is runnable it sleeps until
 * either a wakeup is delivered to this thread or the nearest timer expires,
 * which is the same delivery wait the coroutine layer uses. Because of that, a
 * coroutine still belongs to the thread that runs the loop: cross-thread
 * wakeups are delivered to the loop's thread, never executed by the requester.
 *
 * This header is conservative: it compiles as C99 and C++11 and includes no
 * third-party header.
 */
#ifndef COROCC_CORO_SCHED_H
#define COROCC_CORO_SCHED_H

#include <stddef.h>
#include <stdint.h>

#include "coro.h"

#ifdef __cplusplus
extern "C" {
#endif

/** coro_sched_t -- a single-thread run loop (opaque handle). */
typedef struct coro_sched coro_sched_t;

/**
 * coro_sched_create -- create a run loop bound to the calling thread.
 *
 * @return The new scheduler, or NULL when it could not be allocated.
 */
COROCC_API coro_sched_t* coro_sched_create(void);

/**
 * coro_sched_destroy -- release a scheduler.
 *
 * Refuses (returns a non-zero status) while coroutines it owns are still live,
 * because destroying a scheduler under a running coroutine would free the very
 * context executing it. Drive every coroutine to completion first -- coro_sched_join
 * or coro_sched_wait_all -- and destroy afterwards.
 *
 * @param s  Scheduler to release; NULL is accepted and ignored.
 * @return CORO_OK on success; CORO_EBUSY when coroutines are still live.
 */
COROCC_API coro_status coro_sched_destroy(coro_sched_t* s);

/**
 * coro_sched_spawn -- create a coroutine and queue it on this scheduler.
 *
 * The coroutine does not run yet; it starts on the next run-loop turn, which
 * keeps spawn cheap enough to call in bulk. Passing NULL attributes selects
 * the defaults (a default-sized guarded stack, no name).
 *
 * @param s     Scheduler to queue on. Must not be NULL.
 * @param attr   Attributes, or NULL for the defaults.
 * @param fn     Entry point, called as fn(arg). Must not be NULL.
 * @param arg    Opaque argument forwarded to fn.
 * @return CORO_OK on success; CORO_EINVAL for a NULL scheduler or entry;
 *         CORO_ENOMEM when the coroutine, its stack or the queue node could
 *         not be allocated.
 */
COROCC_API coro_status coro_sched_spawn(coro_sched_t* s, const coro_attr_t* attr,
                                        coro_fn fn, void* arg);

/**
 * coro_sched_run -- drive coroutines until the scheduler has nothing to do.
 *
 * Runs until no coroutine is runnable and no timer is pending: a parked
 * coroutine resumed by a future wakeup is the scheduler's business again, not
 * a reason to stay inside the call. While no coroutine is runnable the loop
 * sleeps, so it never busy-waits.
 *
 * @param s  Scheduler to run. Must not be NULL.
 * @return CORO_OK on success; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_sched_run(coro_sched_t* s);

/**
 * coro_sched_pending -- how many coroutines this scheduler still owns.
 *
 * Counts runnable, sleeping and joined coroutines; zero means the scheduler is
 * idle and can be destroyed.
 *
 * @param s  Scheduler to inspect.
 * @return The live coroutine count, or 0 for NULL.
 */
COROCC_API size_t coro_sched_pending(const coro_sched_t* s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COROCC_CORO_SCHED_H */
