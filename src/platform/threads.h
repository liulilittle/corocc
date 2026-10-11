#ifndef COROCC_PLATFORM_THREADS_H
#define COROCC_PLATFORM_THREADS_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/platform/threads.h -- internal thread primitives.
 *
 * The coroutine object needs three things from the host: a way to start and
 * join threads (so the concurrency suites are real), a per-thread identity
 * that costs nothing to obtain (the owner rule of the coroutine layer keys on
 * it), and a hook that runs when a thread leaves (so per-thread bookkeeping
 * cannot outlive its thread).
 *
 * This header is INTERNAL: it is not installed and is not part of the public
 * API or ABI. It compiles as C99.
 */
#include <stddef.h>

#if defined(_WIN32)
typedef void* coro_thread_t;
#else
#include <pthread.h>
typedef pthread_t coro_thread_t;
#endif

typedef void (*coro_thread_fn)(void* arg);

/**
 * coro_thread_create -- start a thread that runs fn(arg).
 *
 * @param out     Receives the thread handle; must not be NULL.
 * @param fn      Entry point. Must not be NULL.
 * @param arg     Opaque argument forwarded to fn.
 * @return 0 on success, non-zero if the thread could not be started.
 */
int coro_thread_create(coro_thread_t* out, coro_thread_fn fn, void* arg);

/**
 * coro_thread_join -- wait for a thread started by coro_thread_create.
 *
 * @param t  Thread handle from coro_thread_create.
 * @return 0 on success, non-zero otherwise. Joining twice is not allowed.
 */
int coro_thread_join(coro_thread_t t);

/**
 * coro_thread_self_id -- identity of the calling thread.
 *
 * Returns a value that is unique among live threads, stable for the lifetime
 * of the calling thread, and obtained without an allocation or a system call
 * (the address of a thread-local variable serves). The identity is only used
 * for comparison: it is never dereferenced and never escapes the library.
 *
 * @return The identity of the calling thread.
 */
void* coro_thread_self_id(void);

/**
 * coro_thread_exit_hook -- register a callback that runs when this thread ends.
 *
 * Used by the coroutine layer to release per-thread bookkeeping (the runtime
 * that owns the thread's driver context and delivery queue). One hook slot is
 * provided; a second registration replaces the first and returns the previous
 * callback so it can be chained. Registering NULL clears the slot.
 *
 * @param fn   Callback, or NULL.
 * @return The previously registered callback, or NULL.
 */
coro_thread_fn coro_thread_exit_hook(coro_thread_fn fn);

#endif /* COROCC_PLATFORM_THREADS_H */
