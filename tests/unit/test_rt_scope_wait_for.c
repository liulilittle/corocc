#include "../harness.h"
#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_sched.h>      /* coro_monotonic_ns */
#include <corocc/coro_scope.h>
#include <string.h>
#include <time.h>

/* L3 step 6: wait_for and its watchdog (verdict section 3.2).
 *
 *   - the watchdog is a retained spawn with an explicit small stack; it
 *     never enters the scope's child table and never touches the scope's
 *     live count (an empty scope with a 500ms timeout must return at
 *     once);
 *   - on the deadline it claims the scope's timeout exactly once (a CAS on
 *     timer_fired under the activity lock) and cancels the children, so
 *     the wait drains for real and reports ECANCELED;
 *   - when everything finishes first, the waiter cancels the watchdog and
 *     reclaims it (cancel -> join -> collect), leaving nothing behind.
 *
 * The wait_for callers are coroutines, per the wait contract.
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

struct wf_scene {
    coro_rt_scope_t*    s;
    uint64_t            timeout_ms;
    coro_status         wait;          /* out */
    unsigned long long  elapsed_ns;    /* out */
};

static void wf_waiter(void* arg) {
    struct wf_scene* sc = (struct wf_scene*)arg;
    uint64_t t0 = coro_monotonic_ns();
    sc->wait = coro_rt_scope_wait_for(g_rt, sc->s, sc->timeout_ms);
    sc->elapsed_ns = (unsigned long long)(coro_monotonic_ns() - t0);
}

static void run_tests(void) {
    coro_rt_t* rt = coro_rt_create(1);
    CHECK(rt != NULL);
    g_rt = rt;

    /* ---- early exit: the watchdog is reclaimed, the aggregate wins --- */
    {
        coro_rt_scope_t s;
        struct wf_scene sc;
        coro_status* cell = NULL;
        memset(&s, 0, sizeof s);
        memset(&sc, 0, sizeof sc);
        sc.s = &s;
        sc.timeout_ms = 500;
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, quick_entry,
                                  (void*)(size_t)CORO_OK, NULL) == CORO_OK);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, quick_entry,
                                  (void*)(size_t)CORO_ESTALE, &cell) ==
              CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, wf_waiter, &sc) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(sc.wait == CORO_ESTALE);          /* the aggregate, not a timeout */
        CHECK(sc.elapsed_ns < 300000000ull);    /* did not wait out 500ms */
        CHECK(*cell == CORO_ESTALE);
        CHECK(coro_rt_pending(rt) == 0);        /* watchdog reclaimed */
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- timeout: the watchdog cancels, the wait drains, ECANCELED ---- */
    {
        struct delay_code slow = { 5000, CORO_OK };
        coro_rt_scope_t s;
        struct wf_scene sc;
        coro_status* cell = NULL;
        memset(&s, 0, sizeof s);
        memset(&sc, 0, sizeof sc);
        sc.s = &s;
        sc.timeout_ms = 100;
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &slow,
                                  &cell) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, wf_waiter, &sc) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(sc.wait == CORO_ECANCELED);
        CHECK(sc.elapsed_ns < 1000000000ull);   /* far below the 5s sleep */
        CHECK(*cell == CORO_ECANCELED);         /* the child was cut */
        CHECK(coro_rt_pending(rt) == 0);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- ms == 0: straight to the timeout path ----------------------- */
    {
        struct delay_code slow = { 5000, CORO_OK };
        coro_rt_scope_t s;
        struct wf_scene sc;
        memset(&s, 0, sizeof s);
        memset(&sc, 0, sizeof sc);
        sc.s = &s;
        sc.timeout_ms = 0;
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &slow,
                                  NULL) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, wf_waiter, &sc) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(sc.wait == CORO_ECANCELED);
        CHECK(sc.elapsed_ns < 1000000000ull);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- the watchdog is not a child: an empty scope returns at once -- */
    {
        coro_rt_scope_t s;
        struct wf_scene sc;
        memset(&s, 0, sizeof s);
        memset(&sc, 0, sizeof sc);
        sc.s = &s;
        sc.timeout_ms = 500;
        CHECK(coro_rt_spawn(rt, NULL, wf_waiter, &sc) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(sc.wait == CORO_OK);
        CHECK(sc.elapsed_ns < 100000000ull);  /* not the 500ms timeout */
        CHECK(coro_rt_pending(rt) == 0);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- priority: the deadline fired before the drain -> ECANCELED --- */
    {
        struct delay_code fast = { 10, CORO_ESTALE };
        struct delay_code slow = { 5000, CORO_OK };
        coro_rt_scope_t s;
        struct wf_scene sc;
        coro_status* cslow = NULL;
        memset(&s, 0, sizeof s);
        memset(&sc, 0, sizeof sc);
        sc.s = &s;
        sc.timeout_ms = 200;
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &fast,
                                  NULL) == CORO_OK);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &slow,
                                  &cslow) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, wf_waiter, &sc) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        /* the fast error landed at 10ms, but the wait only ends on live==0
           -- and the deadline fired (and cancelled the rest) first, so the
           timeout owns the result */
        CHECK(sc.wait == CORO_ECANCELED);
        CHECK(*cslow == CORO_ECANCELED);
        CHECK(sc.elapsed_ns < 1000000000ull);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- wait_for on an already drained scope: immediate aggregate ---- */
    {
        coro_rt_scope_t s;
        struct wf_scene sc;
        memset(&s, 0, sizeof s);
        memset(&sc, 0, sizeof sc);
        sc.s = &s;
        sc.timeout_ms = 500;
        CHECK(coro_rt_scope_cancel(rt, &s) == CORO_OK);   /* empty, canceling */
        CHECK(coro_rt_spawn(rt, NULL, wf_waiter, &sc) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(sc.wait == CORO_OK);
        CHECK(sc.elapsed_ns < 100000000ull);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    CHECK(coro_rt_pending(rt) == 0);
    CHECK(coro_rt_destroy(rt) == CORO_OK);
}

TEST_MAIN()
