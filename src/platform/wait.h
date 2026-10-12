#ifndef COROCC_PLATFORM_WAIT_H
#define COROCC_PLATFORM_WAIT_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/platform/wait.h -- internal one-shot completion signal.
 *
 * A caller that hands a wakeup request to another thread needs to wait until
 * that thread has handled it. The wait is a condition variable, never a spin:
 * the waiting thread sleeps and is woken by the handler, and it re-checks the
 * completion flag so spurious wakeups cannot pass for completion.
 *
 * This header is INTERNAL: it is not installed and is not part of the public
 * API or ABI.
 */
#if defined(_WIN32)
/* Windows arrives with milestone M9 together with its CI. */
#error "corocc: the Windows wait backend arrives with milestone M9"
#else

#include <pthread.h>
#include <stdint.h>

typedef struct coro_wait {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             done;
} coro_wait;

/** coro_wait_init -- prepare a reusable completion signal. */
int coro_wait_init(coro_wait* w);

/** coro_wait_destroy -- release a completion signal. */
int coro_wait_destroy(coro_wait* w);

/**
 * coro_wait_block -- sleep until the signal is raised.
 *
 * @return 0 when the signal was observed, non-zero on a wait error.
 */
int coro_wait_block(coro_wait* w);

/**
 * coro_wait_block_until -- sleep until the signal is raised or a deadline.
 *
 * The deadline is an absolute monotone timestamp, as coro_monotonic_ns
 * reports, so the two agree on the scale. The condition variable is created
 * against the monotone clock for exactly this reason.
 *
 * @param w         Signal to wait on.
 * @param deadline  Absolute monotone nanoseconds.
 * @return 0 when the signal was observed, non-zero on a timeout or a wait
 *         error (the caller distinguishes them only if it needs to).
 */
int coro_wait_block_until(coro_wait* w, uint64_t deadline);

/** coro_wait_raise -- mark the signal done and wake every waiter. */
int coro_wait_raise(coro_wait* w);

/**
 * coro_wait_reset -- clear the signal so the same object can be awaited again.
 *
 * Reset before re-checking the condition it protects: a signal raised between
 * the reset and the following block is observed, and one raised before the
 * reset is covered by re-reading the condition, so no wakeup is lost.
 */
int coro_wait_reset(coro_wait* w);

#endif /* !_WIN32 */
#endif /* COROCC_PLATFORM_WAIT_H */
