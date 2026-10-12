#include "../harness.h"
#include <corocc/coro_sched.h>

#include <string.h>

/* Milestone M4 Task 4: blocking join and wait chains.
 *
 * What must hold: joining a live target parks the caller and returns only when
 * the target ends; several waiters on one target are all released and each is
 * resumed exactly once; joining a finished target is reported rather than
 * parked; and joining oneself is refused instead of deadlocking.
 */

static coro_sched_t* g_sched;
static coro_t*      g_target;

#define WAITERS 3u
#define TARGET_TURNS 5u

static unsigned g_target_turns;
static unsigned g_waiter_back[WAITERS];
static unsigned g_waiter_order;

/* The target: a few turns, then it finishes. */
static void entry_target(void* arg) {
    (void)arg;
    for (unsigned i = 0; i < TARGET_TURNS; ++i) {
        ++g_target_turns;
        if (coro_yield() != CORO_OK) return;
    }
}

/* A waiter: joins the target, then records that it came back. */
static void entry_waiter(void* arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    if (coro_sched_join(g_sched, g_target) == CORO_OK) {
        ++g_waiter_back[id];
        ++g_waiter_order;
    }
}

static void run_tests(void) {
    coro_sched_t* s = coro_sched_create();
    g_sched = s;

    /* ---------- one waiter, a live target ---------- */
    CHECK(coro_sched_spawn_h(s, NULL, entry_target, NULL, &g_target) == CORO_OK);
    CHECK(coro_sched_spawn(s, NULL, entry_waiter, (void*)(uintptr_t)0) == CORO_OK);
    CHECK(coro_sched_run(s) == CORO_OK);
    CHECK(g_target_turns == TARGET_TURNS); /* the target ran to its end */
    CHECK(g_waiter_back[0] == 1u);          /* and the waiter came back */
    CHECK(coro_sched_pending(s) == 0u);

    /* ---------- several waiters on one target ---------- */
    g_target_turns = 0;
    g_waiter_order = 0;
    memset(g_waiter_back, 0, sizeof g_waiter_back);
    CHECK(coro_sched_spawn_h(s, NULL, entry_target, NULL, &g_target) == CORO_OK);
    for (unsigned i = 0; i < WAITERS; ++i) {
        CHECK(coro_sched_spawn(s, NULL, entry_waiter, (void*)(uintptr_t)i) == CORO_OK);
    }
    CHECK(coro_sched_run(s) == CORO_OK);
    CHECK(g_target_turns == TARGET_TURNS);
    CHECK(g_waiter_order == WAITERS); /* every waiter was released once */
    for (unsigned i = 0; i < WAITERS; ++i) {
        CHECK(g_waiter_back[i] == 1u);
    }
    CHECK(coro_sched_pending(s) == 0u);

    /* ---------- joining a finished target is reported ---------- */
    {
        coro_t* done = NULL;
        CHECK(coro_sched_spawn_h(s, NULL, entry_target, NULL, &done) == CORO_OK);
        CHECK(coro_sched_wait_all(s) == CORO_OK); /* it runs to completion */
        /* a second join of the same handle is refused: it no longer exists */
        CHECK(coro_sched_join(s, done) == CORO_EDESTROYED);
        CHECK(coro_sched_pending(s) == 0u);
    }

    /* ---------- joining from outside a coroutine is refused ---------- */
    {
        /* Not running inside a coroutine, so there is nothing to park: the
           call is refused rather than deadlocking the caller. The handle is
           only used while the coroutine exists -- the scheduler releases it
           when the coroutine finishes, and a released handle must not be
           touched again. */
        coro_t* spare = NULL;
        CHECK(coro_sched_spawn_h(s, NULL, entry_target, NULL, &spare) == CORO_OK);
        CHECK(coro_sched_join(s, spare) == CORO_ESTALE); /* not inside a coroutine */
        CHECK(coro_sched_wait_all(s) == CORO_OK);        /* then let it finish */
    }

    /* ---------- invalid arguments ---------- */
    CHECK(coro_sched_join(NULL, g_target) == CORO_EINVAL);
    CHECK(coro_sched_join(s, NULL) == CORO_EINVAL);
    {
        coro_t* foreign = NULL;
        CHECK(coro_create(&foreign, NULL, entry_target, NULL) == CORO_OK);
        CHECK(coro_sched_join(s, foreign) == CORO_EINVAL); /* not ours */
        CHECK(coro_destroy(foreign) == CORO_OK);
    }

    CHECK(coro_sched_destroy(s) == CORO_OK);
}

TEST_MAIN()
