#include "../harness.h"
#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_sched.h>

/* Milestone: M:N work-stealing runtime.
 *
 * Behaviours being pinned:
 *   - coro_rt_create(1) is a single-worker runtime that makes progress without
 *     deadlock (single-core contract): many coroutines yield to each other and
 *     all finish;
 *   - coro_rt_create(4) distributes the same workload across workers and still
 *     completes it exactly;
 *   - coro_rt_sleep parks a coroutine until the deadline and it resumes once;
 *   - coro_rt_wait_all drains every spawn; coro_rt_pending drops to zero.
 */

#define COROS 500u
#define ITERS 20u

static unsigned g_runs[COROS];

static void entry_count(void* pv) {
    unsigned id = (unsigned)(size_t)pv;
    for (unsigned i = 0; i < ITERS; ++i) {
        if (coro_yield() != CORO_OK) return;
        ++g_runs[id];
    }
}

/* A coroutine that sleeps for ~1ms then reports. */
static void entry_sleep(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    uint64_t now = coro_monotonic_ns();
    coro_status st = coro_rt_sleep(rt, now + 1000000ull);
    if (st != CORO_OK) {
        /* cannot CHECK in a coroutine portably; signal via a global */
        extern unsigned g_sleep_failed;
        ++g_sleep_failed;
    }
}

unsigned g_sleep_failed;

static void run_spawn_storm(coro_rt_t* rt) {
    for (unsigned i = 0; i < COROS; ++i) g_runs[i] = 0;
    for (unsigned i = 0; i < COROS; ++i) {
        CHECK(coro_rt_spawn(rt, NULL, entry_count, (void*)(size_t)i) == CORO_OK);
    }
    /* Workers run asynchronously; some may already have finished. */
    CHECK(coro_rt_pending(rt) <= COROS);
    CHECK(coro_rt_wait_all(rt) == CORO_OK);
    CHECK(coro_rt_pending(rt) == 0);
    for (unsigned i = 0; i < COROS; ++i) {
        CHECK(g_runs[i] == ITERS);
    }
}

static void run_tests(void) {
    /* Single-core contract: one worker, many coroutines, no deadlock. */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        run_spawn_storm(rt);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* Multi-worker: the same storm stolen by four workers, still exact. */
    {
        coro_rt_t* rt = coro_rt_create(4);
        CHECK(rt != NULL);
        run_spawn_storm(rt);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* Sleep parks and resumes once. */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        g_sleep_failed = 0;
        for (unsigned i = 0; i < 10u; ++i) {
            CHECK(coro_rt_spawn(rt, NULL, entry_sleep, rt) == CORO_OK);
        }
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(g_sleep_failed == 0);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }
}

TEST_MAIN()
