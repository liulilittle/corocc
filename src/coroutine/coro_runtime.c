/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/coroutine/coro_runtime.c -- per-thread coroutine runtime.
 *
 * One runtime per thread, created on first use and released by a thread-exit
 * hook. The identity stored in it is a monotone number rather than the
 * runtime's own address: an address into freed thread storage would dangle the
 * moment the thread exits, while a number never does.
 */
#include "coroutine/coro_runtime.h"

#include "arch/coro_atomic.h"
#include "arch/coro_tls.h"

#include <stdlib.h>

/* Threads are numbered in the order they first touch the library. The counter
   is monotone and never reused, so an identity stays unique for the life of
   the process -- which is what the owner rule compares. */
static uint64_t g_next_id;

/* The runtime of the calling thread, NULL until first use. */
static CORO_TLS coro_runtime* tls_runtime;

coro_runtime* coro_runtime_current(void) {
    coro_runtime* r = tls_runtime;
    if (r) return r;

    r = (coro_runtime*)calloc(1, sizeof *r);
    if (!r) {
        /* No runtime means no coroutine driving, and the drive path has no
           status code that describes it. Failing hard here is the documented
           contract for an unrecoverable condition. */
        abort();
    }
    r->id = coro_at_fetch_add(&g_next_id, 1) + 1;
    tls_runtime = r;
    coro_thread_exit_hook(coro_runtime_thread_exit);
    return r;
}

void coro_runtime_thread_exit(void* arg) {
    (void)arg;
    coro_runtime* r = tls_runtime;
    tls_runtime = NULL;
    free(r);
}
