#include "../harness.h"
#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_sched.h>      /* coro_monotonic_ns */
#include <corocc/coro_scope.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h> /* Sleep in the thread-side wait below */
#endif

/* L3 step 5: the group three-state family (verdict section 3.3).
 *
 *   - wait_all is the live==0 wait (already covered by the wait suite);
 *   - wait_first_error returns at the first non-OK result, cancels the
 *     remaining children, still waits out the drain (the structured
 *     invariant), and returns that first error -- it must NOT degrade to
 *     "report once everything finished", so the slow child's 500ms
 *     deadline is cancelled away and the call returns in tens of ms (the
 *     elapsed-time bound below is the timeliness anchor);
 *   - wait_first_done returns the first completed child's result while the
 *     scope stays alive: the caller must follow up with wait or cancel,
 *     and a destroy without that is a detectable EBUSY -- not a hang.
 */

static coro_rt_t* g_rt;

struct delay_code {
    unsigned    ms;
    coro_status code;
};

/* Sleeps dc->ms; returns the sleep's own status when the wait was cut
   (ECANCELED), otherwise the configured code. */
static coro_status reporter_entry(void* arg) {
    struct delay_code* dc = (struct delay_code*)arg;
    coro_status st = coro_rt_sleep(g_rt,
                        coro_monotonic_ns() + (uint64_t)dc->ms * 1000000ull);
    return st != CORO_OK ? st : dc->code;
}

static coro_status quick_entry(void* arg) {
    return (coro_status)(size_t)arg;
}

static void sleep_ms(unsigned ms) {
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

struct ferr_scene {
    coro_rt_scope_t*    s;
    coro_status         wait;          /* out */
    unsigned long long  elapsed_ns;    /* out */
};

static void first_error_waiter(void* arg) {
    struct ferr_scene* sc = (struct ferr_scene*)arg;
    uint64_t t0 = coro_monotonic_ns();
    sc->wait = coro_rt_group_wait_first_error(g_rt, sc->s);
    sc->elapsed_ns = (unsigned long long)(coro_monotonic_ns() - t0);
}

struct fdone_scene {
    coro_rt_scope_t*    s;
    coro_status         first;         /* out */
    unsigned long long  elapsed_ns;    /* out */
    int                 followup;      /* 1: drain afterwards */
    coro_status         after;         /* out: the follow-up wait */
};

static void first_done_waiter(void* arg) {
    struct fdone_scene* sc = (struct fdone_scene*)arg;
    uint64_t t0 = coro_monotonic_ns();
    (void)coro_rt_group_wait_first_done(g_rt, sc->s, &sc->first);
    sc->elapsed_ns = (unsigned long long)(coro_monotonic_ns() - t0);
    if (sc->followup) sc->after = coro_rt_scope_wait(g_rt, sc->s);
}

static void drain_waiter(void* arg) {
    coro_rt_scope_t* s = (coro_rt_scope_t*)arg;
    (void)coro_rt_scope_wait(g_rt, s);
}

static void run_tests(void) {
    coro_rt_t* rt = coro_rt_create(1);
    CHECK(rt != NULL);
    g_rt = rt;

    /* ---- wait_all: the live==0 alias -------------------------------- */
    {
        coro_rt_scope_t s;
        coro_status* cell = NULL;
        memset(&s, 0, sizeof s);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, quick_entry,
                                  (void*)(size_t)CORO_OK, NULL) == CORO_OK);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, quick_entry,
                                  (void*)(size_t)CORO_ESTALE, &cell) ==
              CORO_OK);
        /* a coroutine waiter, per the wait contract */
        CHECK(coro_rt_spawn(rt, NULL, drain_waiter, &s) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(*cell == CORO_ESTALE);
        CHECK(coro_rt_scope_status(&s) == CORO_ESTALE);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- first_error: prompt cancel, no all-done degradation -------- */
    {
        struct delay_code fast = { 10, CORO_ESTALE };  /* errors at ~10ms */
        struct delay_code slow = { 500, CORO_OK };     /* would sleep 500ms */
        coro_rt_scope_t s;
        struct ferr_scene sc;
        coro_status* cfast = NULL;
        coro_status* cslow = NULL;
        memset(&s, 0, sizeof s);
        memset(&sc, 0, sizeof sc);
        sc.s = &s;
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &fast,
                                  &cfast) == CORO_OK);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &slow,
                                  &cslow) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, first_error_waiter, &sc) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(sc.wait == CORO_ESTALE);          /* the first error */
        CHECK(sc.elapsed_ns < 300000000ull);    /* far below 500ms */
        CHECK(*cfast == CORO_ESTALE);
        CHECK(*cslow == CORO_ECANCELED);        /* the slow one was cut */
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- first_error with no error: behaves as wait_all ------------- */
    {
        coro_rt_scope_t s;
        struct ferr_scene sc;
        memset(&s, 0, sizeof s);
        memset(&sc, 0, sizeof sc);
        sc.s = &s;
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, quick_entry,
                                  (void*)(size_t)CORO_OK, NULL) == CORO_OK);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, quick_entry,
                                  (void*)(size_t)CORO_OK, NULL) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, first_error_waiter, &sc) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(sc.wait == CORO_OK);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- first_done: prompt, scope stays alive, no follow-up = EBUSY - */
    {
        struct delay_code fast = { 10, CORO_ESTALE };
        struct delay_code slow = { 500, CORO_OK };
        coro_rt_scope_t s;
        struct fdone_scene sc;
        memset(&s, 0, sizeof s);
        memset(&sc, 0, sizeof sc);
        sc.s = &s;
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &fast,
                                  NULL) == CORO_OK);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &slow,
                                  NULL) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, first_done_waiter, &sc) == CORO_OK);
        sleep_ms(120);   /* the first child reported; the second sleeps on */
        CHECK(sc.first == CORO_ESTALE);         /* the first completion */
        CHECK(sc.elapsed_ns < 300000000ull);
        /* No follow-up yet: the scope is still live -- destroy detects it
           instead of hanging or leaking silently (verdict 3.3). */
        CHECK(coro_rt_scope_destroy(&s) == CORO_EBUSY);
        /* the required follow-up: cancel the rest and drain */
        CHECK(coro_rt_scope_cancel(rt, &s) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, drain_waiter, &s) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(coro_rt_pending(rt) == 0);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- first_done with a follow-up drain: the normal pattern ------- */
    {
        struct delay_code fast = { 10, CORO_ESTALE };
        struct delay_code slow = { 500, CORO_OK };
        coro_rt_scope_t s;
        struct fdone_scene sc;
        coro_status* cfast = NULL;
        coro_status* cslow = NULL;
        memset(&s, 0, sizeof s);
        memset(&sc, 0, sizeof sc);
        sc.s = &s;
        sc.followup = 1;
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &fast,
                                  &cfast) == CORO_OK);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, reporter_entry, &slow,
                                  &cslow) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, first_done_waiter, &sc) == CORO_OK);
        sleep_ms(50);   /* the first child reported (10ms); the slow sleeps */
        CHECK(coro_rt_scope_cancel(rt, &s) == CORO_OK);   /* cut the slow */
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(sc.first == CORO_ESTALE);
        CHECK(sc.after == CORO_ESTALE);    /* the follow-up drain */
        CHECK(*cslow == CORO_ECANCELED);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    CHECK(coro_rt_pending(rt) == 0);
    CHECK(coro_rt_destroy(rt) == CORO_OK);
}

TEST_MAIN()
