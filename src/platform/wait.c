/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/platform/wait.c -- internal one-shot completion signal.
 *
 * Two implementations behind the same contract. The POSIX branch keeps the
 * pthread form the runtime always used (the condition variable is bound to
 * CLOCK_MONOTONIC so timed waits share the scheduler's deadline scale; on
 * macOS, which lacks pthread_condattr_setclock and runs condition variables
 * on the realtime clock, the timed wait converts the monotonic deadline to
 * realtime and re-arms after every ETIMEDOUT, keeping the same never-early
 * property). The Windows branch follows the Windows-backend shape: SRWLOCK
 * plus CONDITION_VARIABLE, SleepConditionVariableSRW for the waits, with the
 * absolute monotone deadline converted to relative milliseconds and
 * re-derived after every wakeup, so a spurious wakeup cannot fake a
 * completion. Windows timers are tick-slaved -- a timeout can fire a little
 * before or after the requested duration -- so the timed waits re-arm until
 * the deadline has truly passed: returning a timeout early would violate
 * the "not before the deadline" property the POSIX branch provides.
 */
#include "platform/wait.h"

#if defined(_WIN32)

#include "scheduler/timer.h" /* coro_monotonic_ns */

int coro_wait_init(coro_wait* w) {
    if (!w) return -1;
    w->done = 0;
    /* SRWLOCK/CONDITION_VARIABLE in zero-initialized storage are usable, but
       the explicit init calls make the state a contract rather than an
       accident (MSDN: InitializeConditionVariable may be called at any
       time). */
    InitializeSRWLock(&w->lock);
    InitializeConditionVariable(&w->cond);
    return 0;
}

int coro_wait_destroy(coro_wait* w) {
    if (!w) return -1;
    /* SRWLOCK and CONDITION_VARIABLE have no destroy step. */
    return 0;
}

int coro_wait_block(coro_wait* w) {
    int result = -1;
    if (!w) return -1;
    AcquireSRWLockExclusive(&w->lock);
    while (!w->done) {
        if (!SleepConditionVariableSRW(&w->cond, &w->lock, INFINITE, 0)) {
            goto out;
        }
    }
    result = 0;
out:
    ReleaseSRWLockExclusive(&w->lock);
    return result;
}

int coro_wait_raise(coro_wait* w) {
    if (!w) return -1;
    AcquireSRWLockExclusive(&w->lock);
    w->done = 1;
    WakeAllConditionVariable(&w->cond);
    ReleaseSRWLockExclusive(&w->lock);
    return 0;
}

int coro_wait_reset(coro_wait* w) {
    if (!w) return -1;
    AcquireSRWLockExclusive(&w->lock);
    w->done = 0;
    ReleaseSRWLockExclusive(&w->lock);
    return 0;
}

int coro_wait_block_until(coro_wait* w, uint64_t deadline) {
    int result = -1;
    if (!w) return -1;
    AcquireSRWLockExclusive(&w->lock);
    while (!w->done) {
        uint64_t now = coro_monotonic_ns();
        uint64_t ms;
        if (now >= deadline) break; /* the deadline passed: report a timeout */
        /* Round up to the next millisecond so a positive remainder never
           becomes an immediate (0 ms) wait; clamp to INFINITE-1 so a very
           distant deadline cannot turn into an infinite wait (INFINITE as a
           relative timeout never expires). */
        ms = (deadline - now + 999999ull) / 1000000ull;
        if (ms > 0xFFFFFFFEull) ms = 0xFFFFFFFEull; /* INFINITE - 1 */
        if (ms == 0) ms = 1;
        if (!SleepConditionVariableSRW(&w->cond, &w->lock, (DWORD)ms, 0)) {
            /* Windows timer granularity means a timeout can fire a little
               before the requested duration elapsed (and a little after).
               Keep the deadline a true lower bound, the way the POSIX branch
               behaves, by re-arming: the loop re-checks `done` on every
               return, and only reports a timeout once the deadline passed. */
        }
    }
    result = w->done ? 0 : 1;
    ReleaseSRWLockExclusive(&w->lock);
    return result;
}

#else /* !_WIN32 */

#if defined(__APPLE__)
#include <errno.h>      /* ETIMEDOUT */
#include <sys/time.h>   /* gettimeofday: realtime base for the cond wait */
#include "scheduler/timer.h" /* coro_monotonic_ns */
#endif

int coro_wait_init(coro_wait* w) {
    if (!w) return -1;
    w->done = 0;
    if (pthread_mutex_init(&w->lock, NULL) != 0) return -1;
#if defined(__APPLE__)
    /* macOS: no pthread_condattr_setclock; the condition variable stays on
       the realtime clock and the timed wait below converts its deadline. */
    if (pthread_cond_init(&w->cond, NULL) != 0) {
        pthread_mutex_destroy(&w->lock);
        return -1;
    }
    return 0;
#else
    pthread_condattr_t attr;
    /* Bind the condition variable to the monotone clock so timed waits use the
       same scale as the scheduler's timer deadlines. */
    if (pthread_condattr_init(&attr) != 0) {
        pthread_mutex_destroy(&w->lock);
        return -1;
    }
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
        pthread_condattr_destroy(&attr);
        pthread_mutex_destroy(&w->lock);
        return -1;
    }
    if (pthread_cond_init(&w->cond, &attr) != 0) {
        pthread_condattr_destroy(&attr);
        pthread_mutex_destroy(&w->lock);
        return -1;
    }
    pthread_condattr_destroy(&attr);
    return 0;
#endif
}

int coro_wait_destroy(coro_wait* w) {
    if (!w) return -1;
    pthread_cond_destroy(&w->cond);
    pthread_mutex_destroy(&w->lock);
    return 0;
}

int coro_wait_block(coro_wait* w) {
    int result = -1;
    if (!w) return -1;
    pthread_mutex_lock(&w->lock);
    while (!w->done) {
        if (pthread_cond_wait(&w->cond, &w->lock) != 0) goto out;
    }
    result = 0;
out:
    pthread_mutex_unlock(&w->lock);
    return result;
}

int coro_wait_raise(coro_wait* w) {
    if (!w) return -1;
    pthread_mutex_lock(&w->lock);
    w->done = 1;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
    return 0;
}

int coro_wait_reset(coro_wait* w) {
    if (!w) return -1;
    pthread_mutex_lock(&w->lock);
    w->done = 0;
    pthread_mutex_unlock(&w->lock);
    return 0;
}

int coro_wait_block_until(coro_wait* w, uint64_t deadline) {
    int result = -1;
    if (!w) return -1;
    pthread_mutex_lock(&w->lock);
    while (!w->done) {
#if defined(__APPLE__)
        /* realtime-clock conversion, re-armed after every ETIMEDOUT: only the
           monotonic deadline below decides when the timeout is reported, so a
           wall-clock step cannot end the wait early (see file header). */
        uint64_t now = coro_monotonic_ns();
        uint64_t rem;
        struct timeval tv;
        struct timespec ts;
        int rc;
        if (now >= deadline) break;
        rem = deadline - now;
        if (gettimeofday(&tv, NULL) != 0) break;
        ts.tv_sec  = tv.tv_sec + (time_t)(rem / 1000000000ull);
        ts.tv_nsec = (long)tv.tv_usec * 1000L + (long)(rem % 1000000000ull);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        rc = pthread_cond_timedwait(&w->cond, &w->lock, &ts);
        if (rc == 0) continue;
        if (rc != ETIMEDOUT) break;
        /* ETIMEDOUT: loop re-checks the monotonic deadline. */
#else
        struct timespec ts;
        int rc;
        ts.tv_sec = (time_t)(deadline / 1000000000ull);
        ts.tv_nsec = (long)(deadline % 1000000000ull);
        rc = pthread_cond_timedwait(&w->cond, &w->lock, &ts);
        if (rc != 0) break; /* timeout, or an error the caller may inspect */
#endif
    }
    result = w->done ? 0 : 1;
    pthread_mutex_unlock(&w->lock);
    return result;
}

#endif /* !_WIN32 */
