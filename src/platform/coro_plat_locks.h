#ifndef COROCC_PLATFORM_CORO_PLAT_LOCKS_H
#define COROCC_PLATFORM_CORO_PLAT_LOCKS_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/platform/coro_plat_locks.h -- internal lock/condition shim.
 *
 * The runtime layer needs one mutex type and one condition type it can
 * initialize uniformly on every platform: POSIX forwards straight to
 * pthreads, Windows maps to SRWLOCK / CONDITION_VARIABLE. Both Windows
 * types are legal in their zero-initialized state, so a host structure that
 * is calloc'ed (or statically allocated) needs no init step, while
 * coro_plat_cond_init exists for the documented explicit initialization of
 * condition variables. The shim is not a new synchronization design: it only
 * gives the existing pthread-shaped call sites one spelling to migrate to.
 *
 * The timed wait takes an absolute monotone-nanosecond deadline, the same
 * scale coro_monotonic_ns reports, so a caller that computes deadlines from
 * that clock never has to convert scales; the Windows side converts to the
 * relative milliseconds SleepConditionVariableSRW expects and the POSIX side
 * converts to the absolute timespec pthread_cond_timedwait expects (the
 * condition variable is created against CLOCK_MONOTONIC for that reason).
 *
 * This header is INTERNAL: it is not installed and is not part of the public
 * API or ABI. It compiles as C99.
 */
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>

typedef SRWLOCK            coro_plat_mutex_t; /**< zero-initialization is legal (MSDN). */
typedef CONDITION_VARIABLE coro_plat_cond_t;  /**< zero-initialization is legal (MSDN). */

#else

#include <pthread.h>

typedef pthread_mutex_t coro_plat_mutex_t; /**< PTHREAD_MUTEX_INITIALIZER applies. */
typedef pthread_cond_t  coro_plat_cond_t;

#endif

/** coro_plat_mutex_lock -- acquire; blocks until the lock is free. */
void coro_plat_mutex_lock(coro_plat_mutex_t* m);

/** coro_plat_mutex_unlock -- release a lock held by the caller. */
void coro_plat_mutex_unlock(coro_plat_mutex_t* m);

/**
 * coro_plat_cond_init -- explicitly initialize a condition variable.
 *
 * Windows: InitializeConditionVariable; POSIX: pthread_cond_init bound to
 * CLOCK_MONOTONIC, so coro_plat_cond_timedwait_ns deadlines use the same
 * scale as coro_monotonic_ns. Zero-initialized objects are already usable on
 * Windows; this call makes that a contract rather than an accident.
 *
 * @return 0 on success, non-zero on failure.
 */
int coro_plat_cond_init(coro_plat_cond_t* cv);

/**
 * coro_plat_cond_wait -- block until broadcast, without a deadline.
 *
 * The caller must hold the mutex; the wait releases it while blocked and
 * re-acquires it before returning.
 *
 * @return 0 when woken, non-zero on a wait error.
 */
int coro_plat_cond_wait(coro_plat_cond_t* cv, coro_plat_mutex_t* m);

/** coro_plat_cond_broadcast -- wake every waiter on the variable. */
int coro_plat_cond_broadcast(coro_plat_cond_t* cv);

/**
 * coro_plat_cond_timedwait_ns -- block until broadcast or an absolute deadline.
 *
 * @param cv           Condition variable.
 * @param m            Mutex held by the caller.
 * @param deadline_ns  Absolute monotone nanoseconds, as coro_monotonic_ns
 *                     reports. A deadline already in the past returns
 *                     immediately without sleeping.
 * @return 0 when woken by a broadcast, non-zero on timeout (or a wait error,
 *         which the caller treats like a timeout and re-checks its predicate).
 */
int coro_plat_cond_timedwait_ns(coro_plat_cond_t* cv, coro_plat_mutex_t* m,
                                uint64_t deadline_ns);

#endif /* COROCC_PLATFORM_CORO_PLAT_LOCKS_H */
