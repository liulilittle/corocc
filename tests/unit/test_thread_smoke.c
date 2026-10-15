#include "../harness.h"
#include <corocc/coro.h>

/* The per-thread runtime, observed through the public API.
 *
 * The coroutine layer keeps one runtime per thread (its driver context, the
 * coroutine running on it and its identity). The observable consequence is
 * that a coroutine can be created, started, finished and destroyed on any
 * thread: each worker does that with a coroutine of its own while the main
 * thread drives its own, so every runtime is created, used and released on the
 * thread that created it. Cross-thread wakeup is a separate concern, covered by
 * test_coro_crossresume.c.
 */

#include "../palt_threads.h"

#define WORKERS 4u

static int g_worker_ok = 1;
static pt_barrier_t g_barrier;

static void entry_park_each_round(void* arg) {
    (void)arg;
    while (coro_yield() == CORO_OK) {
    }
}

/* Each worker runs a complete coroutine lifecycle on its own thread. */
static void* worker(void* pv) {
    (void)pv;
    pt_barrier_wait(&g_barrier);

    coro_t* own = NULL;
    coro_token t = CORO_TOKEN_NONE, nx = CORO_TOKEN_NONE;
    if (coro_create(&own, NULL, entry_park_each_round, NULL) != CORO_OK ||
        coro_start(own, &t) != CORO_OK || t == CORO_TOKEN_DEAD) {
        g_worker_ok = 0;
        return NULL;
    }
    coro_cancel(own);
    if (coro_resume(own, t, &nx) != CORO_OK || nx != CORO_TOKEN_DEAD) {
        g_worker_ok = 0;
    }
    if (coro_get_state(own) != CORO_DEAD) g_worker_ok = 0;
    if (coro_destroy(own) != CORO_OK) g_worker_ok = 0;
    return NULL;
}

static void run_tests(void) {
    pt_thread_t th[WORKERS];
    coro_t* c = NULL;
    coro_token tok = CORO_TOKEN_NONE, next = CORO_TOKEN_NONE;

    /* the main thread drives its own coroutine while the workers run theirs */
    CHECK(coro_create(&c, NULL, entry_park_each_round, NULL) == CORO_OK);
    CHECK(coro_start(c, &tok) == CORO_OK);

    CHECK(pt_barrier_init(&g_barrier, WORKERS) == 0);
    for (unsigned i = 0; i < WORKERS; ++i) {
        CHECK(pt_thread_create(&th[i], worker, NULL) == 0);
    }
    /* the main flow keeps driving its own coroutine while the workers work */
    coro_cancel(c);
    CHECK(coro_resume(c, tok, &next) == CORO_OK);
    CHECK(next == CORO_TOKEN_DEAD);

    for (unsigned i = 0; i < WORKERS; ++i) {
        pt_thread_join(th[i]);
    }
    CHECK(g_worker_ok == 1);

    CHECK(coro_get_state(c) == CORO_DEAD);
    CHECK(coro_destroy(c) == CORO_OK);
}

TEST_MAIN()
