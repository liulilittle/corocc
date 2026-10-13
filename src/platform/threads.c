/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/platform/threads.c -- internal thread primitives.
 *
 * POSIX implementation. The Windows counterpart ships with the M9 milestone,
 * which is also when the MSVC CI job exists; until then the Windows build
 * fails at the toolchain stage rather than here.
 *
 * The exit hook is installed once per thread, the first time a thread needs
 * it, and runs from a thread-specific destructor. It is deliberately tiny:
 * the coroutine layer only needs to release one structure per thread.
 */
#include "platform/threads.h"

#if defined(_WIN32)
#error "corocc: the Windows thread backend arrives with milestone M9"
#else

#include <pthread.h>
#include <stdlib.h>

static pthread_key_t  g_exit_key;
static pthread_once_t g_exit_once = PTHREAD_ONCE_INIT;
static int            g_exit_key_ready;

/* Runs at thread exit: invokes whatever the coroutine layer registered. */
static void threads_run_exit_hook(void* value) {
    coro_thread_fn fn = (coro_thread_fn)value;
    if (fn) fn(NULL);
}

static void threads_make_exit_key(void) {
    g_exit_key_ready = (pthread_key_create(&g_exit_key,
                                           threads_run_exit_hook) == 0);
}

/* Entry point handed to pthread: unpacks the library's own callback. */
struct threads_start_args {
    coro_thread_fn fn;
    void*          arg;
};

static void* threads_start(void* p) {
    struct threads_start_args* a = (struct threads_start_args*)p;
    coro_thread_fn fn = a->fn;
    void*          arg = a->arg;
    free(a); /* the starter owns the args heap block; release after unpack */
    fn(arg);
    return NULL;
}

int coro_thread_create(coro_thread_t* out, coro_thread_fn fn, void* arg) {
    struct threads_start_args* start;
    if (!out || !fn) return -1;
    /* The args cannot live on the caller's stack: pthread_create returns as
       soon as the thread is created, but the new thread reads its argument
       whenever it first runs. Allocate once, let the thread free it. */
    start = (struct threads_start_args*)malloc(sizeof *start);
    if (!start) return -1;
    start->fn = fn;
    start->arg = arg;
    if (pthread_create(out, NULL, threads_start, start) != 0) {
        free(start);
        return -1;
    }
    return 0;
}

int coro_thread_join(coro_thread_t t) {
    return pthread_join(t, NULL) == 0 ? 0 : -1;
}

/* The address of this thread's own TLS slot: unique per thread, free to take. */
static __thread char tls_identity;

void* coro_thread_self_id(void) {
    return (void*)&tls_identity;
}

/* The registered hook lives in thread-local storage so the destructor above
   can reach it; registration replaces the previous value. */
static __thread coro_thread_fn tls_exit_hook;

coro_thread_fn coro_thread_exit_hook(coro_thread_fn fn) {
    pthread_once(&g_exit_once, threads_make_exit_key);
    coro_thread_fn previous = tls_exit_hook;
    tls_exit_hook = fn;
    if (fn && g_exit_key_ready) {
        pthread_setspecific(g_exit_key, (void*)fn);
    }
    return previous;
}

#endif /* !_WIN32 */
