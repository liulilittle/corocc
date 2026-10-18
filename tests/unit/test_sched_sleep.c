#include "../harness.h"
#include <corocc/coro_sched.h>

/* Scheduled sleep and timer wakeups.
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

/* A coroutine that sleeps once and then plain-yields several times. The park
   marker of the sleep must be retired when the deadline path requeues it, so
   each plain yield keeps round-robin requeueing the node instead of being
   dropped off the run queue. */
static unsigned g_yield_steps;
static void entry_sleep_then_yield(void* arg) {
    (void)arg;
    coro_status st = coro_sched_sleep(g_sched, coro_monotonic_ns() + 1000000ull);
    if (st != CORO_OK) return;
    for (unsigned i = 0; i < 5u; ++i) {
        if (coro_yield() != CORO_OK) return;
        ++g_yield_steps;
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

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
/* Cancels the target after a short pause, from another thread (Win32). */
static unsigned __stdcall cancel_after_a_while(void* pv) {
    Sleep(20); /* 20 ms: the loop is parked by now */
    CHECK(coro_cancel((coro_t*)pv) == CORO_OK);
    return 0;
}
#else
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

/* A cancellation filed while the target is RUNNING (it never parks) must not
   outlive the target: retire_node frees the node, and a wake-list entry left
   behind would make the next drain touch freed memory (heap-use-after-free).
   The body spins until released, so it never observes the cancel and never
   yields. */
static coro_t*       g_busy_target;
static volatile long g_busy_in_body;
static volatile long g_busy_go;

static void entry_busy_until_go(void* arg) {
    (void)arg;
    g_busy_in_body = 1;
    while (!g_busy_go) { /* spin: never yields, never observes the cancel */ }
}

#if defined(_WIN32)
static unsigned __stdcall cancel_running_then_release(void* pv) {
    (void)pv;
    while (!g_busy_in_body) { /* wait until the body is running */ }
    CHECK(coro_sched_cancel(g_sched, g_busy_target) == CORO_OK);
    g_busy_go = 1;
    return 0;
}
#else
static void* cancel_running_then_release(void* pv) {
    (void)pv;
    while (!g_busy_in_body) { /* wait until the body is running */ }
    CHECK(coro_sched_cancel(g_sched, g_busy_target) == CORO_OK);
    g_busy_go = 1;
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

    /* ---------- a sleep wake-up does not strand later plain yields -------
       Regression for a stale park marker: the sleep's wakeup (deadline
       expiry) must retire the marker so the plain yield after it is requeued
       by the round-robin loop; otherwise the coroutine is dropped with the
       scheduler still counting it as live. */
    {
        g_yield_steps = 0;
        CHECK(coro_sched_spawn(s, NULL, entry_sleep_then_yield, NULL) == CORO_OK);
        CHECK(coro_sched_pending(s) == 1u);
        CHECK(coro_sched_run(s) == CORO_OK);
        CHECK(coro_sched_pending(s) == 0u);
        CHECK(g_yield_steps == 5u);
    }

    /* ---------- a cancel of a running node does not outlive it ----------
       The wake request filed for a node that is running (never parked) must
       be dropped when the node retires; otherwise the next drain reads freed
       memory. Deterministic under ASan. */
    {
        g_busy_target = NULL;
        g_busy_in_body = 0;
        g_busy_go = 0;
        CHECK(coro_sched_spawn_h(s, NULL, entry_busy_until_go, NULL,
                                 &g_busy_target) == CORO_OK);
        CHECK(g_busy_target != NULL);
#if defined(_WIN32)
        HANDLE busy_canceller = (HANDLE)_beginthreadex(
            NULL, 0, cancel_running_then_release, NULL, 0, NULL);
        CHECK(busy_canceller != NULL);
        CHECK(coro_sched_run(s) == CORO_OK);
        WaitForSingleObject(busy_canceller, INFINITE);
        CloseHandle(busy_canceller);
#else
        pthread_t busy_canceller;
        CHECK(pthread_create(&busy_canceller, NULL,
                             cancel_running_then_release, NULL) == 0);
        CHECK(coro_sched_run(s) == CORO_OK);
        pthread_join(busy_canceller, NULL);
#endif
        CHECK(coro_sched_pending(s) == 0u);
    }

    /* ---------- a cancellation cuts a long sleep short ---------- */
    {
#if defined(_WIN32)
        HANDLE canceller;
        coro_t* target = NULL;
        CHECK(coro_sched_spawn_h(s, NULL, entry_sleep_long, NULL, &target) == CORO_OK);
        CHECK(target != NULL);
        /* the canceller runs on another thread, the way a real user would,
           because the loop itself blocks until the deadline it is given */
        canceller = (HANDLE)_beginthreadex(NULL, 0, cancel_after_a_while,
                                          target, 0, NULL);
        CHECK(canceller != NULL);
        CHECK(coro_sched_run(s) == CORO_OK);
        WaitForSingleObject(canceller, INFINITE);
        CloseHandle(canceller);
        CHECK(coro_sched_pending(s) == 0u);
        CHECK(g_cancel_woken == 1u);
#else
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
#endif
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
