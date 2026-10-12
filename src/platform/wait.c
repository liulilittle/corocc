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
    if (!w) return -1;
    w->done = 0;
    if (pthread_mutex_init(&w->lock, NULL) != 0) return -1;
    if (pthread_cond_init(&w->cond, NULL) != 0) {
        pthread_mutex_destroy(&w->lock);
        return -1;
    }
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

#endif /* !_WIN32 */
