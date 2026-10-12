/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/platform/wait.c -- internal one-shot completion signal (POSIX).
 */
#include "platform/wait.h"

#if defined(_WIN32)
#error "corocc: the Windows wait backend arrives with milestone M9"
#else

int coro_wait_init(coro_wait* w) {
    pthread_condattr_t attr;
    if (!w) return -1;
    w->done = 0;
    if (pthread_mutex_init(&w->lock, NULL) != 0) return -1;
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
    struct timespec ts;
    int result = -1;
    int rc;
    if (!w) return -1;
    ts.tv_sec = (time_t)(deadline / 1000000000ull);
    ts.tv_nsec = (long)(deadline % 1000000000ull);
    pthread_mutex_lock(&w->lock);
    while (!w->done) {
        rc = pthread_cond_timedwait(&w->cond, &w->lock, &ts);
        if (rc != 0) break; /* timeout, or an error the caller may inspect */
    }
    result = w->done ? 0 : 1;
    pthread_mutex_unlock(&w->lock);
    return result;
}

#endif /* !_WIN32 */
