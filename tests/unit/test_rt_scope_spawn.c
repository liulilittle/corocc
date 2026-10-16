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

/* The cell contract.
 *
 *   - the trampoline is the only writer of a child's result cell: the
 *     value read after a wait is exactly what the entry returned;
 *   - cell_out=NULL is tolerated and means "no result": the child is not
 *     aggregated (an explicit opt-out);
 *   - the aggregate is the first non-OK result in COMPLETION order, and it
 *     covers children that finished before the wait even started;
 *   - a cancelled child reports its own final value (conventionally
 *     ECANCELED) -- the scope never pre-writes a result (a second writer
 *     would race the trampoline) and there is no EBUSY sentinel in the
 *     cell (the child's own status is its sole content).
 *
 * The writer discipline itself is guarded by negative injections
 * (cancel-as-second-writer, sentinel regression) pinned in the state suite;
 * this file pins the positive contract they guard.
 *
 * The wait family's waiter must be a coroutine, so every
 * scene parks a waiter coroutine on the scope and the main thread drives
 * the runtime to quiescence with coro_rt_wait_all before asserting.
 */

static coro_rt_t* g_rt;   /* the one runtime these scenes use */

struct scene {
    coro_rt_scope_t* s;       /* the scope under test */
    coro_status      wait;    /* out: what the waiter's wait returned */
};

static void waiter_entry(void* arg) {
    struct scene* sc = (struct scene*)arg;
    sc->wait = coro_rt_scope_wait(g_rt, sc->s);
}

/* Register the scope's waiter and run the runtime until it finished. */
static void run_scene(struct scene* sc) {
    CHECK(coro_rt_spawn(g_rt, NULL, waiter_entry, sc) == CORO_OK);
    CHECK(coro_rt_wait_all(g_rt) == CORO_OK);
}

/* Returns the coro_status carried in arg. */
static coro_status code_entry(void* arg) {
    return (coro_status)(size_t)arg;
}

/* Completion-order handoff for the aggregation scene below: the
   first-registered child parks on this semaphore until the second child has
   finished. */
static coro_rt_sem_t g_order_gate;

/* The completion-order scene's second child: finishes on its own timer (a
   real timer-driven park stays in the scene), then releases the gate and
   reports ESTALE. */
static coro_status fast_err_entry(void* arg) {
    (void)arg;
    (void)coro_rt_sleep(g_rt, coro_monotonic_ns() + 10000000ull); /* 10ms */
    (void)coro_rt_sem_post(g_rt, &g_order_gate);
    return CORO_ESTALE;
}

/* The first-registered child: parks on the gate and reports EBUSY only
   after the second child finished, so completion order is
   fast-then-slow by construction. */
static coro_status slow_err_entry(void* arg) {
    (void)arg;
    (void)coro_rt_sem_wait(g_rt, &g_order_gate);
    return CORO_EBUSY;
}

/* Sleeps 100ms and propagates the sleep's status: OK at the deadline,
   ECANCELED when a cancel cut it. */
static coro_status cancel_aware_entry(void* arg) {
    (void)arg;
    return coro_rt_sleep(g_rt, coro_monotonic_ns() + 100000000ull);
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

static void run_tests(void) {
    coro_rt_t* rt = coro_rt_create(1);
    CHECK(rt != NULL);
    g_rt = rt;

    /* ---- the cell carries exactly the entry's return value ----------- */
    {
        static const coro_status codes[] = {
            CORO_OK, CORO_EBUSY, CORO_ECANCELED, CORO_ESTALE,
            CORO_EINVAL, CORO_ENOMEM, CORO_ESTATE
        };
        for (size_t i = 0; i < sizeof codes / sizeof codes[0]; ++i) {
            coro_rt_scope_t s;
            struct scene sc;
            coro_status* cell = NULL;
            memset(&s, 0, sizeof s);
            sc.s = &s;
            sc.wait = CORO_OK;
            CHECK(coro_rt_scope_spawn(rt, &s, NULL, code_entry,
                                      (void*)(size_t)codes[i],
                                      &cell) == CORO_OK);
            CHECK(cell != NULL);
            run_scene(&sc);
            CHECK(sc.wait == codes[i]);
            CHECK(*cell == codes[i]);   /* still readable after the wait */
            CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
        }
    }

    /* ---- cell_out=NULL: no result, not aggregated ---------------------- */
    {
        coro_rt_scope_t s;
        struct scene sc;
        memset(&s, 0, sizeof s);
        sc.s = &s;
        sc.wait = CORO_OK;
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, code_entry,
                                  (void*)(size_t)CORO_EBUSY, NULL) ==
              CORO_OK);
        run_scene(&sc);
        CHECK(sc.wait == CORO_OK);   /* the opt-out child is not aggregated */
        CHECK(coro_rt_scope_status(&s) == CORO_OK);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- aggregation is the first non-OK in completion order ---------- */
    {
        /* The first-registered child (slow) reports EBUSY only after the
           second (fast) has finished and released the gate, so fast's
           ESTALE is the first error in COMPLETION order while the aggregate
           must NOT follow registration order (EBUSY was registered first).
           The completion order is enforced by the semaphore handoff, not by
           a 10ms-vs-60ms timer gap: when a scheduling stall lets two timers
           expire in the same wake cycle, the runtime resumes the expired
           nodes in deque order and makes no completion-order promise for
           simultaneously expired timers (reproduced by freezing the process
           across the deadlines: the aggregate then legitimately reflects
           whichever expiry is driven first, which reddened the macOS CI
           sites that had pinned the old timer-gap assumption). */
        coro_rt_scope_t s;
        struct scene sc;
        coro_status* cslow = NULL;
        coro_status* cfast = NULL;
        memset(&s, 0, sizeof s);
        coro_rt_sem_init(&g_order_gate, 0);
        sc.s = &s;
        sc.wait = CORO_OK;
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, slow_err_entry, NULL,
                                  &cslow) == CORO_OK);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, fast_err_entry, NULL,
                                  &cfast) == CORO_OK);
        run_scene(&sc);
        CHECK_EQ(sc.wait, CORO_ESTALE);
        CHECK_EQ(*cslow, CORO_EBUSY);
        CHECK_EQ(*cfast, CORO_ESTALE);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- a child that finished before the wait still aggregates ------ */
    {
        coro_rt_scope_t s;
        struct scene sc;
        coro_status* cell = NULL;
        memset(&s, 0, sizeof s);
        sc.s = &s;
        sc.wait = CORO_OK;
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, code_entry,
                                  (void*)(size_t)CORO_ESTALE, &cell) ==
              CORO_OK);
        sleep_ms(50);   /* it finished and posted before the waiter parked */
        run_scene(&sc);
        CHECK(sc.wait == CORO_ESTALE);   /* covered although it predates the wait */
        CHECK(*cell == CORO_ESTALE);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- two children keep independent cells --------------------------- */
    {
        coro_rt_scope_t s;
        struct scene sc;
        coro_status* c1 = NULL;
        coro_status* c2 = NULL;
        memset(&s, 0, sizeof s);
        sc.s = &s;
        sc.wait = CORO_OK;
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, code_entry,
                                  (void*)(size_t)CORO_EBUSY, &c1) ==
              CORO_OK);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, code_entry,
                                  (void*)(size_t)CORO_OK, &c2) == CORO_OK);
        CHECK(c1 != NULL && c2 != NULL && c1 != c2);
        run_scene(&sc);
        CHECK(sc.wait == CORO_EBUSY);
        CHECK(*c1 == CORO_EBUSY);
        CHECK(*c2 == CORO_OK);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- a cancelled child reports its own final value ---------------- */
    {
        /* The scope cancel (issued from the main thread while the waiter is
           parked) cuts the child's sleep; the entry propagates the
           ECANCELED, and that value -- written by the trampoline alone --
           is what the wait aggregates. The scope wrote nothing. */
        coro_rt_scope_t s;
        struct scene sc;
        coro_status* cell = NULL;
        memset(&s, 0, sizeof s);
        sc.s = &s;
        sc.wait = CORO_OK;
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, cancel_aware_entry, NULL,
                                  &cell) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, waiter_entry, &sc) == CORO_OK);
        sleep_ms(10);   /* the waiter is parked, the child on its deadline */
        CHECK(coro_rt_scope_cancel(rt, &s) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(sc.wait == CORO_ECANCELED);
        CHECK(cell != NULL && *cell == CORO_ECANCELED);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- a refused spawn leaves the caller's cell untouched ----------- */
    {
        coro_rt_scope_t s;
        coro_status* cell = (coro_status*)1;   /* poison */
        memset(&s, 0, sizeof s);
        CHECK(coro_rt_scope_cancel(rt, &s) == CORO_OK);   /* CANCELING */
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, code_entry,
                                  (void*)(size_t)CORO_OK, &cell) ==
              CORO_ESTATE);
        CHECK(cell == (coro_status*)1);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    CHECK(coro_rt_pending(rt) == 0);
    CHECK(coro_rt_destroy(rt) == CORO_OK);
}

TEST_MAIN()
