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
 * coro_sched_spawn_h -- create a coroutine, queue it, and report its handle.
 *
 * Same as coro_sched_spawn, plus the coroutine handle so the caller can query
 * its state, cancel it or join it. The handle belongs to the scheduler: it must
 * not be destroyed by hand, and it becomes invalid the moment the coroutine
 * finishes and the scheduler releases it. The handle also becomes invalid when
 * the run loop's next drive loses the claim to the coroutine (the coroutine
 * was cancelled or driven elsewhere): the loop then destroys the coroutine
 * exactly as on completion, without it having finished -- so a scheduler-owned
 * coroutine must never be driven by hand.
 *
 * @param s      Scheduler to queue on. Must not be NULL.
 * @param attr   Attributes, or NULL for the defaults.
 * @param fn     Entry point, called as fn(arg). Must not be NULL.
 * @param arg    Opaque argument forwarded to fn.
 * @param out    Receives the handle; must not be NULL.
 * @return CORO_OK on success; CORO_EINVAL for a NULL scheduler, entry or out;
 *         CORO_ENOMEM when the coroutine, its stack or the queue node could not
 *         be allocated.
 */
COROCC_API coro_status coro_sched_spawn_h(coro_sched_t* s, const coro_attr_t* attr,
                                          coro_fn fn, void* arg, coro_t** out);

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

/**
 * coro_monotonic_ns -- current time from a monotone source.
 *
 * The clock the scheduler and its timers use. Only differences are meaningful.
 *
 * @return Nanoseconds from an unspecified epoch; never decreases within a
 *         process.
 */
COROCC_API uint64_t coro_monotonic_ns(void);

/**
 * coro_sched_join -- wait until a coroutine on this scheduler has finished.
 *
 * The blocking counterpart of coro_join: rather than reporting whether the
 * coroutine is dead, it parks the caller on that coroutine's wait chain and
 * returns when the coroutine ends. Because the caller is parked, the run loop
 * keeps making progress -- that is the whole point of a scheduler.
 *
 * If several coroutines wait on one target they are all released, and each is
 * resumed exactly once (the wait chain is unlinked by whoever runs first).
 *
 * @param s  Scheduler the target runs on. Must not be NULL.
 * @param c  Coroutine to wait for. Must be owned by this scheduler.
 * @return CORO_OK when the coroutine is dead;
 *         CORO_EINVAL for a NULL argument or a coroutine this scheduler does
 *         not own;
 *         CORO_EDESTROYED when the coroutine already finished before the call.
 */
COROCC_API coro_status coro_sched_join(coro_sched_t* s, coro_t* c);

/**
 * coro_sched_wait_all -- run the loop until every coroutine it owns is dead.
 *
 * The convenience form of joining everything: it drives the scheduler until
 * nothing is left, so a caller can spawn work and collect it without naming
 * each handle. Equivalent to running coro_sched_run to completion.
 *
 * @param s  Scheduler to drain. Must not be NULL.
 * @return CORO_OK on success; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_sched_wait_all(coro_sched_t* s);

/**
 * coro_sched_cancel -- ask a coroutine on this scheduler to stop.
 *
 * The coroutine observes the request at its next safe point, exactly as
 * coro_cancel documents. If it is sleeping, its deadline is removed and it is
 * requeued so that safe point is reached now instead of at the deadline.
 *
 * @param s  Scheduler that owns the coroutine.
 * @param c  Coroutine to cancel.
 * @return CORO_OK on success; CORO_EINVAL for a NULL argument or a coroutine
 *         this scheduler does not own.
 */
COROCC_API coro_status coro_sched_cancel(coro_sched_t* s, coro_t* c);

/**
 * coro_sched_sleep -- park the running coroutine until a deadline passes.
 *
 * Must be called from inside a coroutine on this scheduler. The deadline is an
 * absolute monotone timestamp, as coro_monotonic_ns reports; pass now + delta.
 * The coroutine is off the run queue until its deadline, and it is requeued by
 * the timer path -- never switched to directly -- so the run loop stays the only
 * thread that drives it.
 *
 * Cancellation is observed the same way as anywhere else: if a cancellation is
 * requested before the deadline, the deadline is removed and the coroutine is
 * woken early, and the call returns CORO_ECANCELED.
 *
 * @param s         Scheduler the coroutine runs on.
 * @param deadline  Absolute monotone nanoseconds.
 * @return CORO_OK when the deadline passed;
 *         CORO_ECANCELED when the sleep was cut short by a cancellation;
 *         CORO_EINVAL for a NULL scheduler, or CORO_ESTALE when called outside
 *         any coroutine.
 */
COROCC_API coro_status coro_sched_sleep(coro_sched_t* s, uint64_t deadline);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COROCC_CORO_SCHED_H */
