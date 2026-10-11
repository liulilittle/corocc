#include "../harness.h"
#include <corocc/coro.h>

/* Milestone M3 Task 1: the per-thread runtime, observed through the public
 * API.
 *
 * The coroutine layer keeps one runtime per thread (its driver context, the
 * coroutine running on it and its identity). The observable consequences are:
 * a coroutine can be created, started and destroyed on any thread, and only
 * the thread that first drove a coroutine may keep driving it. Both are
 * checked here; the identity itself is internal state.
 */

#if defined(_WIN32)
static void run_tests(void) {
    CHECK(0 && "multithreaded suites: POSIX threads only in M3");
}
TEST_MAIN()
#else

#include <pthread.h>

#define WORKERS 4u

static coro_t*   g_c;
static coro_token g_tok;
static unsigned  g_body_runs;
static int       g_worker_ok = 1;
static pthread_barrier_t g_barrier;

static void entry_park_each_round(void* arg) {
    (void)arg;
    ++g_body_runs;
    while (coro_yield() == CORO_OK) {
        ++g_body_runs;
    }
}

/* Own coroutine of a worker thread: must not touch the shared counter. */
static void entry_park_quietly(void* arg) {
    (void)arg;
    while (coro_yield() == CORO_OK) {
    }
}

/* Each worker tries to drive a coroutine it does not own, then proves it can
   create, start and destroy a coroutine of its own on this thread. */
static void* worker(void* pv) {
    (void)pv;
    coro_token out = CORO_TOKEN_NONE;
    pthread_barrier_wait(&g_barrier);

    /* foreign drive: refused while the owner rule holds (delivery lands in
       Task 3 of this milestone) and, either way, must not run the body */
    coro_status s = coro_resume(g_c, g_tok, &out);
    if (s != CORO_EBUSY) g_worker_ok = 0;

    coro_t* own = NULL;
    coro_token t = CORO_TOKEN_NONE;
    if (coro_create(&own, NULL, entry_park_quietly, NULL) != CORO_OK ||
        coro_start(own, &t) != CORO_OK || t == CORO_TOKEN_DEAD) {
        g_worker_ok = 0;
    }
    if (own) {
        coro_cancel(own);
        coro_token nx = CORO_TOKEN_NONE;
        if (coro_resume(own, t, &nx) != CORO_OK || nx != CORO_TOKEN_DEAD) {
            g_worker_ok = 0;
        }
        if (coro_destroy(own) != CORO_OK) g_worker_ok = 0;
    }
    return NULL;
}

static void run_tests(void) {
    pthread_t th[WORKERS];
    coro_token next = CORO_TOKEN_NONE;

    /* the owner thread creates and starts the coroutine */
    CHECK(coro_create(&g_c, NULL, entry_park_each_round, NULL) == CORO_OK);
    CHECK(coro_start(g_c, &g_tok) == CORO_OK);
    CHECK(g_body_runs == 1u);

    CHECK(pthread_barrier_init(&g_barrier, NULL, WORKERS) == 0);
    for (unsigned i = 0; i < WORKERS; ++i) {
        CHECK(pthread_create(&th[i], NULL, worker, NULL) == 0);
    }
    for (unsigned i = 0; i < WORKERS; ++i) {
        pthread_join(th[i], NULL);
    }
    CHECK(g_worker_ok == 1);

    /* no foreign drive got through: the body ran exactly once and the
       credential is still live for the owner */
    CHECK(g_body_runs == 1u);
    CHECK(coro_get_state(g_c) == CORO_SUSPENDED);
    CHECK(coro_resume(g_c, g_tok, &next) == CORO_OK);
    CHECK(g_body_runs == 2u);

    /* the owner finishes the coroutine, and a new one can be driven on the
       same thread afterwards: the identity is stable for the thread's life */
    coro_cancel(g_c);
    CHECK(coro_resume(g_c, next, &next) == CORO_OK);
    CHECK(next == CORO_TOKEN_DEAD);
    CHECK(coro_get_state(g_c) == CORO_DEAD);
    CHECK(coro_destroy(g_c) == CORO_OK);
}

TEST_MAIN()
#endif
