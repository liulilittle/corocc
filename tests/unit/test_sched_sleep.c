#include "../harness.h"
#include <corocc/coro_sched.h>

/* Milestone M4 Task 3: scheduled sleep and timer wakeups.
 *
 * What must hold: a sleeping coroutine is off the run queue, it is requeued by
 * the timer path (never switched to directly), nothing is lost however many
 * coroutines sleep at once, the run loop waits rather than polls, and a
 * cancellation cuts a sleep short instead of leaving a deadline behind.
 */

#define SLEEPERS 10u
#define BASE_MS  5u
#define STEP_MS  3u

static coro_sched_t* g_sched;
static unsigned      g_woke[SLEEPERS];
static unsigned      g_wake_count;
static unsigned      g_cancel_woken;
static uint64_t      g_slept_ns[SLEEPERS];

static void entry_sleeps_ms(void* arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    uint64_t ms = BASE_MS + id * STEP_MS;   /* 5 .. 32 ms */
    uint64_t start = coro_monotonic_ns();
    coro_status st = coro_sched_sleep(g_sched, start + ms * 1000000ull);
    g_slept_ns[id] = coro_monotonic_ns() - start;
    if (st == CORO_OK) {
        ++g_woke[id];
        ++g_wake_count;
    }
}

/* One long sleep: only a cancellation can end it early. */
static coro_t* g_target;
static void entry_sleep_long(void* arg) {
    (void)arg;
    coro_status st = coro_sched_sleep(g_sched,
                                      coro_monotonic_ns() + 5000000000ull);
    if (st == CORO_ECANCELED) ++g_cancel_woken;
}

#if !defined(_WIN32)
#include <pthread.h>
#include <unistd.h>
/* Cancels the target after a short pause, from another thread. */
static void* cancel_after_a_while(void* pv) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 20000000L; /* 20 ms: the loop is parked by now */
    nanosleep(&ts, NULL);
    CHECK(coro_cancel((coro_t*)pv) == CORO_OK);
    return NULL;
}
#endif

static void run_tests(void) {
    coro_sched_t* s = coro_sched_create();
    g_sched = s;
    g_target = NULL;

    /* ---------- nothing is lost, and the wall clock matches ---------- */
    for (unsigned i = 0; i < SLEEPERS; ++i) {
        CHECK(coro_sched_spawn(s, NULL, entry_sleeps_ms,
                              (void*)(uintptr_t)i) == CORO_OK);
    }
    CHECK(coro_sched_pending(s) == SLEEPERS);

    {
        uint64_t longest = (BASE_MS + (SLEEPERS - 1u) * STEP_MS) * 1000000ull;
        uint64_t t0 = coro_monotonic_ns();
        CHECK(coro_sched_run(s) == CORO_OK);
        uint64_t wall = coro_monotonic_ns() - t0;

        CHECK(g_wake_count == SLEEPERS);  /* every sleeper woke exactly once */
        CHECK(coro_sched_pending(s) == 0u);
        for (unsigned i = 0; i < SLEEPERS; ++i) {
            CHECK(g_woke[i] == 1u);
            /* the measured sleep is at least what was asked for */
            CHECK(g_slept_ns[i] >= (BASE_MS + i * STEP_MS) * 1000000ull);
        }
        /* the loop returns only after the last deadline, so it waited */
        CHECK(wall >= longest);
    }

    /* ---------- a cancellation cuts a long sleep short ---------- */
    {
        pthread_t canceller;
        coro_t* target = NULL;
        CHECK(coro_sched_spawn_h(s, NULL, entry_sleep_long, NULL, &target) == CORO_OK);
        CHECK(target != NULL);
        /* the canceller runs on another thread, the way a real user would,
           because the loop itself blocks until the deadline it is given */
        CHECK(pthread_create(&canceller, NULL, cancel_after_a_while, target) == 0);
        CHECK(coro_sched_run(s) == CORO_OK);
        pthread_join(canceller, NULL);
        CHECK(coro_sched_pending(s) == 0u);
        CHECK(g_cancel_woken == 1u);
    }

    /* cancelling something this scheduler does not own is refused */
    {
        coro_t* foreign = NULL;
        CHECK(coro_create(&foreign, NULL, entry_sleeps_ms, NULL) == CORO_OK);
        CHECK(coro_sched_cancel(s, foreign) == CORO_EINVAL);
        CHECK(coro_destroy(foreign) == CORO_OK);
    }

    CHECK(coro_sched_cancel(s, NULL) == CORO_EINVAL);
    CHECK(coro_sched_sleep(NULL, 0) == CORO_EINVAL);

    CHECK(coro_sched_destroy(s) == CORO_OK);
}

TEST_MAIN()
