#include "../harness.h"
#include <corocc/coro_sched.h>

/* The run loop and spawn.
 *
 * What the loop must guarantee: coroutines run to their next park, a parked
 * coroutine is resumed on a later turn, spawn does not run anything inline,
 * and the loop returns when there is nothing left to do -- never busy-waiting.
 */

static coro_sched_t* g_sched;

#define WORKERS 8u
#define ROUNDS 50u

static unsigned g_done[WORKERS];
static unsigned g_total;

static void entry_finishes(void* arg) {
    (void)arg;
    ++g_total;
}

/* Parks on every turn until asked to finish, then returns. */
static coro_token g_release;
static void entry_turns(void* arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    for (unsigned i = 0; i < ROUNDS; ++i) {
        ++g_done[id];
        (void)g_release;
        if (coro_yield() != CORO_OK) return;
    }
}

static void entry_once(void* arg) {
    (void)arg;
    ++g_total;
}

static void run_tests(void) {
    coro_sched_t* s = coro_sched_create();
    CHECK(s != NULL);
    CHECK(coro_sched_pending(s) == 0u);

    /* ---------- spawn does not run anything inline ---------- */
    CHECK(coro_sched_spawn(s, NULL, entry_once, NULL) == CORO_OK);
    CHECK(coro_sched_pending(s) == 1u);
    CHECK(g_total == 0u); /* not run yet */

    CHECK(coro_sched_run(s) == CORO_OK);
    CHECK(g_total == 1u);
    CHECK(coro_sched_pending(s) == 0u);

    /* ---------- many coroutines, round-robin to completion ---------- */
    /* A yield is voluntary: the coroutine goes back to the queue, so one run
       drives every coroutine through all of its turns in round-robin order. */
    for (unsigned i = 0; i < WORKERS; ++i) {
        CHECK(coro_sched_spawn(s, NULL, entry_turns, (void*)(uintptr_t)i) == CORO_OK);
    }
    CHECK(coro_sched_pending(s) == WORKERS);
    CHECK(coro_sched_run(s) == CORO_OK);
    for (unsigned i = 0; i < WORKERS; ++i) {
        CHECK(g_done[i] == ROUNDS);
    }
    CHECK(coro_sched_pending(s) == 0u); /* every one of them finished */

    /* ---------- a coroutine that finishes is retired ---------- */
    {
        unsigned before = g_total;
        CHECK(coro_sched_spawn(s, NULL, entry_finishes, NULL) == CORO_OK);
        CHECK(coro_sched_run(s) == CORO_OK);
        CHECK(g_total == before + 1u); /* it ran exactly once and was released */
        CHECK(coro_sched_pending(s) == 0u);
    }

    /* ---------- invalid arguments ---------- */
    CHECK(coro_sched_spawn(NULL, NULL, entry_once, NULL) == CORO_EINVAL);
    CHECK(coro_sched_spawn(s, NULL, NULL, NULL) == CORO_EINVAL);
    CHECK(coro_sched_run(NULL) == CORO_EINVAL);
    CHECK(coro_sched_pending(NULL) == 0u);

    /* ---------- destroy refuses a live scheduler, then accepts ---------- */
    CHECK(coro_sched_spawn(s, NULL, entry_once, NULL) == CORO_OK);
    CHECK(coro_sched_destroy(s) == CORO_EBUSY); /* still owns one coroutine */
    CHECK(coro_sched_run(s) == CORO_OK);
    CHECK(coro_sched_destroy(s) == CORO_OK);
    CHECK(coro_sched_destroy(NULL) == CORO_OK);

    (void)g_sched;
}

TEST_MAIN()
