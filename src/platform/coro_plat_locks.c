/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/platform/coro_plat_locks.c -- internal lock/condition shim.
 *
 * Thin forwards only; see coro_plat_locks.h for the contract. The Windows
 * side follows the shape verified for the Windows backend: SRWLOCK plus
 * CONDITION_VARIABLE, relative-millisecond timeout conversion clamped to
 * [1, INFINITE-1], and a past deadline reported as an immediate timeout.
 * Windows timers are tick-slaved -- SleepConditionVariableSRW may fire a
 * little before or after the requested duration -- so the timed wait re-arms
 * until the absolute deadline has truly passed; the POSIX branch reports the
 * timeout no earlier than its deadline, and the Windows branch keeps that
 * property.
 *
 * macOS has no pthread_condattr_setclock and its condition variables run on
 * the realtime clock: the Apple branch converts the monotonic deadline to an
 * absolute realtime timespec and re-arms after every ETIMEDOUT, so a
 * wall-clock step cannot make the wait report a timeout before the monotonic
 * deadline (the same never-early property).
 */
#include "platform/coro_plat_locks.h"

#include "scheduler/timer.h" /* coro_monotonic_ns */

#if defined(__APPLE__) && !defined(_WIN32)
#include <errno.h>      /* ETIMEDOUT */
#include <sys/time.h>   /* gettimeofday: realtime base for the cond wait */
#endif

#if defined(_WIN32)

void coro_plat_mutex_lock(coro_plat_mutex_t* m) {
    AcquireSRWLockExclusive(m);
}

void coro_plat_mutex_unlock(coro_plat_mutex_t* m) {
    ReleaseSRWLockExclusive(m);
}

int coro_plat_cond_init(coro_plat_cond_t* cv) {
    if (!cv) return -1;
    InitializeConditionVariable(cv);
    return 0;
}

int coro_plat_cond_wait(coro_plat_cond_t* cv, coro_plat_mutex_t* m) {
    if (!cv || !m) return -1;
    return SleepConditionVariableSRW(cv, m, INFINITE, 0) ? 0 : -1;
}

int coro_plat_cond_broadcast(coro_plat_cond_t* cv) {
    if (!cv) return -1;
    WakeAllConditionVariable(cv);
    return 0;
}

int coro_plat_cond_timedwait_ns(coro_plat_cond_t* cv, coro_plat_mutex_t* m,
                                uint64_t deadline_ns) {
    if (!cv || !m) return 1;
    for (;;) {
        uint64_t now = coro_monotonic_ns();
        uint64_t ms;
        if (now >= deadline_ns) return 1; /* past deadline: timeout now */
        /* Round up to the next millisecond so a positive remainder never
           turns into a zero-millisecond (immediate) wait, then clamp: a
           relative wait of INFINITE would never expire, so the maximum is
           INFINITE-1. */
        ms = (deadline_ns - now + 999999ull) / 1000000ull;
        if (ms > 0xFFFFFFFEull) ms = 0xFFFFFFFEull; /* INFINITE - 1 */
        if (ms == 0) ms = 1;
        if (SleepConditionVariableSRW(cv, m, (DWORD)ms, 0)) return 0;
        /* Windows timers are tick-slaved: a timeout may fire a little before
           the requested duration elapsed. Re-arm until the deadline actually
           passed, so the timeout keeps the POSIX contract of never being
           reported before the absolute deadline. */
    }
}

#else /* !_WIN32 */

void coro_plat_mutex_lock(coro_plat_mutex_t* m) {
    pthread_mutex_lock(m);
}

void coro_plat_mutex_unlock(coro_plat_mutex_t* m) {
    pthread_mutex_unlock(m);
}

int coro_plat_cond_init(coro_plat_cond_t* cv) {
#if defined(__APPLE__)
    /* macOS: pthread_condattr_setclock does not exist and the condition
       variable stays on the default realtime clock; the timed wait below
       converts its monotonic deadline accordingly. */
    if (!cv) return -1;
    return pthread_cond_init(cv, NULL) == 0 ? 0 : -1;
#else
    pthread_condattr_t attr;
    if (!cv) return -1;
    if (pthread_condattr_init(&attr) != 0) return -1;
    /* Bind the condition variable to the monotone clock so timed waits use
       the same scale as the scheduler's timer deadlines. */
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
        pthread_condattr_destroy(&attr);
        return -1;
    }
    if (pthread_cond_init(cv, &attr) != 0) {
        pthread_condattr_destroy(&attr);
        return -1;
    }
    pthread_condattr_destroy(&attr);
    return 0;
#endif
}

int coro_plat_cond_wait(coro_plat_cond_t* cv, coro_plat_mutex_t* m) {
    if (!cv || !m) return -1;
    return pthread_cond_wait(cv, m) == 0 ? 0 : -1;
}

int coro_plat_cond_broadcast(coro_plat_cond_t* cv) {
    if (!cv) return -1;
    return pthread_cond_broadcast(cv) == 0 ? 0 : -1;
}

int coro_plat_cond_timedwait_ns(coro_plat_cond_t* cv, coro_plat_mutex_t* m,
                                uint64_t deadline_ns) {
    if (!cv || !m) return 1;
#if defined(__APPLE__)
    /* realtime-clock conversion: monotonic deadline -> absolute realtime
       timespec, re-armed after every ETIMEDOUT. A wall-clock step during the
       wait can only make an individual arming expire early; the loop
       re-checks the monotonic deadline before reporting, so the timeout is
       never reported before deadline_ns -- the property the Linux branch gets
       from a CLOCK_MONOTONIC condition variable. */
    for (;;) {
        uint64_t now = coro_monotonic_ns();
        uint64_t rem;
        struct timeval tv;
        struct timespec ts;
        int rc;
        if (now >= deadline_ns) return 1;
        rem = deadline_ns - now;
        if (gettimeofday(&tv, NULL) != 0) return 1;
        ts.tv_sec  = tv.tv_sec + (time_t)(rem / 1000000000ull);
        ts.tv_nsec = (long)tv.tv_usec * 1000L + (long)(rem % 1000000000ull);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        rc = pthread_cond_timedwait(cv, m, &ts);
        if (rc == 0) return 0;
        if (rc != ETIMEDOUT) return 1;
    }
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(deadline_ns / 1000000000ull);
    ts.tv_nsec = (long)(deadline_ns % 1000000000ull);
    return pthread_cond_timedwait(cv, m, &ts) == 0 ? 0 : 1;
#endif
}

#endif /* !_WIN32 */
