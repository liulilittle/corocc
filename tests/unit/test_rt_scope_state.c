#include "../harness.h"
#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_sched.h>      /* coro_monotonic_ns */
#include <corocc/coro_scope.h>
#include <string.h>
#include <time.h>

/* L3 step 1: the scope state machine (verdict §3.8) and the per-API return
 * codes that guard it (verdict §3.9). Every illegal transition is pinned:
 *
 *   - CANCELING/DRAINED 后 spawn  -> CORO_ESTATE (state unchanged);
 *   - DESTROYED 后任何操作        -> CORO_ESTALE (destroy itself stays
 *                                    idempotent: CORO_OK, the one exception
 *                                    the verdict states twice);
 *   - live > 0 时 destroy         -> CORO_EBUSY, scope stays usable;
 *   - 零初始化                    -> OPEN (a zeroed scope accepts a spawn,
 *                                    the D11 sentinel detection is retired);
 *   - 二次 destroy                -> CORO_OK (idempotent).
 *
 * The scope is created with no children for most scenes; the one scene that
 * needs a live child (the EBUSY proof) spawns a sleeper and then drains it
 * through cancel + wait so the run leaves no retained handle behind.
 */

static coro_status ok_entry(void* arg) {
    (void)arg;
    return CORO_OK;
}

static coro_status ebusy_entry(void* arg) {
    (void)arg;
    return CORO_EBUSY;
}

static coro_status sleeper_entry(void* arg) {
    coro_rt_t* rt = (coro_rt_t*)arg;
    (void)coro_rt_sleep(rt, coro_monotonic_ns() + 100000000ull); /* 100ms */
    return CORO_OK;
}

/* A short thread-side wait, as the L2 lifecycle suite uses it. */
static void sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}

static void run_tests(void) {
    coro_rt_t* rt = coro_rt_create(1);
    CHECK(rt != NULL);

    /* ---- NULL contract: every entry point refuses NULL ---------------- */
    {
        coro_rt_scope_t s;
        memset(&s, 0, sizeof s);
        CHECK(coro_rt_scope_init(NULL) == CORO_EINVAL);
        CHECK(coro_rt_scope_destroy(NULL) == CORO_EINVAL);
        CHECK(coro_rt_scope_status(NULL) == CORO_EINVAL);
        CHECK(coro_rt_scope_cancel_requested(NULL) == 0);
        CHECK(coro_rt_scope_spawn(rt, NULL, NULL, ok_entry, NULL, NULL) ==
              CORO_EINVAL);
        CHECK(coro_rt_scope_spawn(NULL, &s, NULL, ok_entry, NULL, NULL) ==
              CORO_EINVAL);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, NULL, NULL, NULL) ==
              CORO_EINVAL);
        CHECK(coro_rt_scope_cancel(rt, NULL) == CORO_EINVAL);
        CHECK(coro_rt_scope_cancel(NULL, &s) == CORO_EINVAL);
        CHECK(coro_rt_scope_wait(rt, NULL) == CORO_EINVAL);
        CHECK(coro_rt_scope_wait(NULL, &s) == CORO_EINVAL);
        CHECK(coro_rt_scope_wait_for(rt, NULL, 0) == CORO_EINVAL);
        CHECK(coro_rt_scope_wait_for(NULL, &s, 0) == CORO_EINVAL);
        CHECK(coro_rt_group_wait_all(rt, NULL) == CORO_EINVAL);
        CHECK(coro_rt_group_wait_all(NULL, &s) == CORO_EINVAL);
        CHECK(coro_rt_group_wait_first_error(rt, NULL) == CORO_EINVAL);
        CHECK(coro_rt_group_wait_first_error(NULL, &s) == CORO_EINVAL);
        coro_status first = CORO_OK;
        CHECK(coro_rt_group_wait_first_done(rt, NULL, &first) == CORO_EINVAL);
        CHECK(coro_rt_group_wait_first_done(NULL, &s, &first) == CORO_EINVAL);
        CHECK(coro_rt_group_wait_first_done(rt, &s, NULL) == CORO_EINVAL);
        /* the scope above was never touched: it must still read OPEN */
        CHECK(coro_rt_scope_status(&s) == CORO_OK);
        CHECK(coro_rt_scope_cancel_requested(&s) == 0);
    }

    /* ---- zero-initialized is OPEN (D11: no sentinel detection) -------- */
    {
        coro_rt_scope_t s;
        memset(&s, 0, sizeof s);
        CHECK(coro_rt_scope_status(&s) == CORO_OK);
        CHECK(coro_rt_scope_cancel_requested(&s) == 0);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, ok_entry, NULL, NULL) ==
              CORO_OK);
        sleep_ms(50);   /* the child finishes on its own */
        CHECK(coro_rt_scope_wait(rt, &s) == CORO_OK);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- CANCELING: spawn refused, cancel idempotent ------------------ */
    {
        coro_rt_scope_t s;
        memset(&s, 0, sizeof s);
        CHECK(coro_rt_scope_cancel(rt, &s) == CORO_OK);   /* OPEN -> CANCELING */
        CHECK(coro_rt_scope_cancel_requested(&s) == 1);
        CHECK(coro_rt_scope_cancel(rt, &s) == CORO_OK);   /* idempotent */
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, ok_entry, NULL, NULL) ==
              CORO_ESTATE);
        /* no child was created: nothing is live, destroy is allowed */
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- DRAINED (entered by wait on an empty scope): spawn/cancel
     *      refused with ESTATE ------------------------------------------ */
    {
        coro_rt_scope_t s;
        memset(&s, 0, sizeof s);
        CHECK(coro_rt_scope_wait(rt, &s) == CORO_OK);     /* live==0: DRAINED */
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, ok_entry, NULL, NULL) ==
              CORO_ESTATE);
        CHECK(coro_rt_scope_cancel(rt, &s) == CORO_ESTATE);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);      /* DRAINED -> DESTROYED */
    }

    /* ---- DESTROYED: every operation but destroy reports ESTALE -------- */
    {
        coro_rt_scope_t s;
        coro_status first = CORO_OK;
        memset(&s, 0, sizeof s);
        CHECK(coro_rt_scope_wait(rt, &s) == CORO_OK);     /* DRAINED */
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);      /* DESTROYED */
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);      /* idempotent OK */
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, ok_entry, NULL, NULL) ==
              CORO_ESTALE);
        CHECK(coro_rt_scope_cancel(rt, &s) == CORO_ESTALE);
        CHECK(coro_rt_scope_wait(rt, &s) == CORO_ESTALE);
        CHECK(coro_rt_scope_wait_for(rt, &s, 0) == CORO_ESTALE);
        CHECK(coro_rt_group_wait_all(rt, &s) == CORO_ESTALE);
        CHECK(coro_rt_group_wait_first_error(rt, &s) == CORO_ESTALE);
        CHECK(coro_rt_group_wait_first_done(rt, &s, &first) == CORO_ESTALE);
        CHECK(coro_rt_scope_status(&s) == CORO_ESTALE);
        CHECK(coro_rt_scope_cancel_requested(&s) == 0);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);      /* still idempotent */
    }

    /* ---- live > 0: destroy is EBUSY and the scope stays usable --------- */
    {
        coro_rt_scope_t s;
        memset(&s, 0, sizeof s);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, sleeper_entry, rt, NULL) ==
              CORO_OK);
        sleep_ms(10);   /* the child is parked on its deadline */
        CHECK(coro_rt_scope_destroy(&s) == CORO_EBUSY);
        /* refused, unchanged: the scope still cancels and drains */
        CHECK(coro_rt_scope_cancel(rt, &s) == CORO_OK);
        sleep_ms(50);   /* the cancelled sleeper finished and posted */
        CHECK(coro_rt_scope_wait(rt, &s) == CORO_OK);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- cancel does not write a result cell (D5) ---------------------- */
    {
        coro_rt_scope_t s;
        coro_status* cell = NULL;
        memset(&s, 0, sizeof s);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, ok_entry, NULL, &cell) ==
              CORO_OK);
        CHECK(cell != NULL);
        sleep_ms(50);   /* the child finished; the trampoline wrote OK */
        /* A cancel that lands after the fact must not touch the cell: the
           trampoline is the only writer, and a second writer would clobber
           the child's own result (D5). */
        CHECK(coro_rt_scope_cancel(rt, &s) == CORO_OK);
        CHECK(coro_rt_scope_wait(rt, &s) == CORO_OK);
        CHECK(*cell == CORO_OK);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- a child may legitimately report CORO_EBUSY (D13) -------------- */
    {
        /* No EBUSY sentinel: a child's own CORO_EBUSY result is its result,
           not the mark of an unwritten cell (the retired sentinel scheme
           could not tell the two apart and downgraded it to ECANCELED). */
        coro_rt_scope_t s;
        coro_status* cell = NULL;
        memset(&s, 0, sizeof s);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, ebusy_entry, NULL, &cell) ==
              CORO_OK);
        sleep_ms(50);
        CHECK(coro_rt_scope_wait(rt, &s) == CORO_EBUSY);
        CHECK(*cell == CORO_EBUSY);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    CHECK(coro_rt_pending(rt) == 0);
    CHECK(coro_rt_destroy(rt) == CORO_OK);
}

TEST_MAIN()
