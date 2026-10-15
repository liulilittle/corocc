#include "../harness.h"
#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_sched.h>      /* coro_monotonic_ns */
#include <corocc/coro_scope.h>
#include <string.h>
#include <time.h>

/* The non-coroutine (thread-blocking) entry.
 *
 * Every wait family call here is issued FROM THE MAIN THREAD: it must
 * block that thread on the runtime's settled condition until the scope
 * predicate holds -- never report CORO_EAGAIN, never return early, and
 * give the same aggregate/priority results as the coroutine entry. The
 * thread-side wait_for waits on its own deadline (no watchdog coroutine is
 * needed) and claims the same once-only timeout.
 */

static coro_rt_t* g_rt;

struct delay_code {
    unsigned    ms;
    coro_status code;
};

static coro_status reporter_entry(void* arg) {
    struct delay_code* dc = (struct delay_code*)arg;
    coro_status st = coro_rt_sleep(g_rt,
                        coro_monotonic_ns() + (uint64_t)dc->ms * 1000000ull);
    return st != CORO_OK ? st : dc->code;
}

static coro_status quick_entry(void* arg) {
    return (coro_status)(size_t)arg;
}

static unsigned long long elapsed_since(uint64_t t0) {
    return (unsigned long long)(coro_monotonic_ns() - t0);
}

static void run_tests(void) {
    coro_rt_t* rt = coro_rt_create(1);
    CHECK(rt != NULL);
    g_rt = rt;

    /* ---- group_wait_all from the thread blocks until the drain ------- */
    {
        struct delay_code slow = { 50, CORO_OK };
        coro_rt_scope_t s;
        coro_status* cell = NULL;
        memset(&s, 0, sizeof s);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &slow,
                                  NULL) == CORO_OK);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, quick_entry,
                                  (void*)(size_t)CORO_ESTALE, &cell) ==
              CORO_OK);
        uint64_t t0 = coro_monotonic_ns();
        CHECK(coro_rt_group_wait_all(rt, &s) == CORO_ESTALE);
        CHECK(elapsed_since(t0) >= 40000000ull);   /* blocked, not early */
        CHECK(*cell == CORO_ESTALE);
        CHECK(coro_rt_pending(rt) == 0);           /* children collected */
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- first_error from the thread: prompt cancel ------------------ */
    {
        struct delay_code fast = { 10, CORO_ESTALE };
        struct delay_code slow = { 5000, CORO_OK };
        coro_rt_scope_t s;
        coro_status* cfast = NULL;
        coro_status* cslow = NULL;
        memset(&s, 0, sizeof s);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &fast,
                                  &cfast) == CORO_OK);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &slow,
                                  &cslow) == CORO_OK);
        uint64_t t0 = coro_monotonic_ns();
        CHECK(coro_rt_group_wait_first_error(rt, &s) == CORO_ESTALE);
        CHECK(elapsed_since(t0) < 1000000000ull);  /* far below 5s */
        CHECK(*cfast == CORO_ESTALE);
        CHECK(*cslow == CORO_ECANCELED);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- first_done from the thread: alive scope, then drain --------- */
    {
        struct delay_code fast = { 10, CORO_ESTALE };
        struct delay_code slow = { 5000, CORO_OK };
        coro_rt_scope_t s;
        coro_status first = CORO_OK;
        coro_status* cfast = NULL;
        coro_status* cslow = NULL;
        memset(&s, 0, sizeof s);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &fast,
                                  &cfast) == CORO_OK);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &slow,
                                  &cslow) == CORO_OK);
        uint64_t t0 = coro_monotonic_ns();
        CHECK(coro_rt_group_wait_first_done(rt, &s, &first) == CORO_OK);
        CHECK(first == CORO_ESTALE);
        CHECK(elapsed_since(t0) < 1000000000ull);
        CHECK(coro_rt_scope_destroy(&s) == CORO_EBUSY);   /* still alive */
        CHECK(coro_rt_scope_cancel(rt, &s) == CORO_OK);
        CHECK(coro_rt_scope_wait(rt, &s) == CORO_ESTALE); /* thread drain */
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- wait_for from the thread: timeout path ---------------------- */
    {
        struct delay_code slow = { 5000, CORO_OK };
        coro_rt_scope_t s;
        coro_status* cell = NULL;
        memset(&s, 0, sizeof s);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &slow,
                                  &cell) == CORO_OK);
        uint64_t t0 = coro_monotonic_ns();
        CHECK(coro_rt_scope_wait_for(rt, &s, 100) == CORO_ECANCELED);
        CHECK(elapsed_since(t0) < 1000000000ull);
        CHECK(*cell == CORO_ECANCELED);            /* the child was cut */
        CHECK(coro_rt_pending(rt) == 0);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- wait_for from the thread: early exit ------------------------ */
    {
        coro_rt_scope_t s;
        coro_status* cell = NULL;
        memset(&s, 0, sizeof s);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, quick_entry,
                                  (void*)(size_t)CORO_ESTALE, &cell) ==
              CORO_OK);
        uint64_t t0 = coro_monotonic_ns();
        CHECK(coro_rt_scope_wait_for(rt, &s, 500) == CORO_ESTALE);
        CHECK(elapsed_since(t0) < 300000000ull);   /* did not wait 500ms */
        CHECK(*cell == CORO_ESTALE);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    CHECK(coro_rt_pending(rt) == 0);
    CHECK(coro_rt_destroy(rt) == CORO_OK);
}

TEST_MAIN()
