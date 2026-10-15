#include "../harness.h"
#include "coroutine/coro_runtime.h"   /* internal: the current coroutine */
#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_sched.h>        /* coro_monotonic_ns */
#include <string.h>
#include <time.h>

/* L2 hardening set (R1-R4): the runtime behaviours the structured-concurrency
 * layer is built on, each pinned against a defect the dual audit red-proved
 * on the pre-fix runtime.
 *
 *   - a retained spawn_h handle is reclaimed by coro_rt_collect, which is
 *     pure reclamation: the live count was already decremented once, by the
 *     retire that ran when the coroutine finished (double decrement made
 *     coro_rt_pending underflow to (size_t)-1 and destroy stick at EBUSY);
 *   - coro_rt_join appends under the activity lock and detaches itself when
 *     the wait is cancelled, so a joiner cancelled before its park can never
 *     stay linked on a chain the retire path walks (heap-use-after-free);
 *   - a cancelled joiner keeps a usable node: its next plain yield requeues
 *     it, because the cancel branch retired the park marker (without that a
 *     plain yield after a cancelled join strands the coroutine);
 *
 * The cancel-of-a-sync-chain scene was moved out of this set by the W11 (c)
 * verdict (documented C-level limitation for the scope layer instead); the
 * exactly-once wake-request claim scene lands with R4.
 */

static void quick_entry(void* arg) { (void)arg; }

/* A short thread-side wait used where a coroutine must be observed in a
   state that only the passage of time can guarantee. */
static void sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ---- join: every waiter is released when the target dies ---- */

static coro_t*     g_join_target;
static coro_rt_t*  g_join_rt;
#define JOIN_WAITERS 4u
static unsigned    g_join_back[JOIN_WAITERS];

static void join_target_entry(void* arg) {
    coro_rt_t* rt = (coro_rt_t*)arg;
    (void)coro_rt_sleep(rt, coro_monotonic_ns() + 20000000ull); /* 20ms */
}

static void join_waiter_entry(void* arg) {
    unsigned id = (unsigned)(size_t)arg;
    if (coro_rt_join(g_join_rt, g_join_target) == CORO_OK) ++g_join_back[id];
}

/* ---- join: a joiner cancelled before its park detaches ---- */

static coro_t*     g_jc_target;
static coro_rt_t*  g_jc_rt;
static coro_status g_jc_st;

static void jc_target_entry(void* arg) {
    coro_rt_t* rt = (coro_rt_t*)arg;
    (void)coro_rt_sleep(rt, coro_monotonic_ns() + 50000000ull); /* dies later */
}

static void jc_joiner_entry(void* arg) {
    coro_rt_t* rt = (coro_rt_t*)arg;
    /* A pending cancellation on ourselves: the coro_yield inside the join
       consumes it and returns ECANCELED WITHOUT switching out, so the node
       is linked on the target's chain at that point. The join must detach
       it, or the target's retire walks this node after it is freed. */
    coro_cancel(coro_runtime_current()->current);
    g_jc_st = coro_rt_join(rt, g_jc_target);
}

/* ---- join: a dead retained target reports EDESTROYED ---- */

static coro_t*     g_jd_target;
static coro_rt_t*  g_jd_rt;
static coro_status g_jd_st;

static void jd_target_entry(void* arg) { (void)arg; }

static void jd_joiner_entry(void* arg) {
    coro_rt_t* rt = (coro_rt_t*)arg;
    g_jd_st = coro_rt_join(rt, g_jd_target);
}

/* ---- join: a cancelled joiner still requeues on a later plain yield ---- */

static coro_t*     g_jy_target;
static coro_rt_t*  g_jy_rt;
static coro_status g_jy_join_st;
static unsigned    g_jy_after_yield;

static void jy_target_entry(void* arg) {
    coro_rt_t* rt = (coro_rt_t*)arg;
    (void)coro_rt_sleep(rt, coro_monotonic_ns() + 50000000ull); /* dies later */
}

static void jy_joiner_entry(void* arg) {
    coro_rt_t* rt = (coro_rt_t*)arg;
    coro_cancel(coro_runtime_current()->current);
    g_jy_join_st = coro_rt_join(rt, g_jy_target);
    /* A cancelled join returns without the coroutine ever leaving its
       drive. A plain yield now must requeue it normally: the cancel branch
       retired the park marker, so the driver must not treat the node as
       parked-with-a-releaser. Pre-fix the driver dropped the node off the
       run queue here and the line below never ran (a silent lost wakeup). */
    if (coro_rt_yield() != CORO_OK) return;
    ++g_jy_after_yield;
}

/* ---- join: cancelled while parked, the target finished in the same
       instant: the satisfied condition wins over the cancellation ---- */

static coro_t*     g_j2_target;
static coro_rt_t*  g_j2_rt;
static coro_t*     g_j2_joiner;
static coro_status g_j2_st;

static void j2_target_entry(void* arg) {
    coro_rt_t* rt = (coro_rt_t*)arg;
    (void)coro_rt_sleep(rt, coro_monotonic_ns() + 20000000ull); /* ~20ms */
}

static void j2_joiner_entry(void* arg) {
    coro_rt_t* rt = (coro_rt_t*)arg;
    g_j2_st = coro_rt_join(rt, g_j2_target);   /* parks, no self-cancel */
}

static void j2_canceller_entry(void* arg) {
    coro_rt_t* rt = (coro_rt_t*)arg;
    (void)coro_rt_sleep(rt, coro_monotonic_ns() + 5000000ull);  /* ~5ms */
    /* Latched while the joiner is parked. A parked join-chain member is
       released by the target's death (waking a sync-chain park from a
       cancel is the held-back R1), so the request surfaces exactly when
       the target finishes and the join's cancel branch re-checks: the
       wait condition is satisfied, so CORO_OK is reported, not the
       cancellation. (If R1 ever lands, this expectation becomes
       CORO_ECANCELED -- the joiner would wake at 5ms with the target
       still alive.) */
    (void)coro_rt_cancel(rt, g_j2_joiner);
}

static void run_tests(void) {
    /* ---- retained handle accounting: retire decrements once ---- */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        coro_t* h = NULL;
        CHECK(coro_rt_spawn_h(rt, NULL, quick_entry, NULL, &h) == CORO_OK);
        /* The child dies on the worker; the retire is the single decrement. */
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(coro_rt_pending(rt) == 0);
        /* Collect is pure reclamation: the live count does not move. */
        CHECK(coro_rt_collect(rt, h) == CORO_OK);
        CHECK(coro_rt_pending(rt) == 0);
        /* The accounting is exact: a plain spawn counts one again and the
           runtime destroys cleanly. */
        CHECK(coro_rt_spawn(rt, NULL, quick_entry, NULL) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(coro_rt_pending(rt) == 0);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* ---- join: all waiters released, one resume each ---- */
    {
        g_join_rt = coro_rt_create(2);
        CHECK(g_join_rt != NULL);
        g_join_target = NULL;
        memset(g_join_back, 0, sizeof g_join_back);
        CHECK(coro_rt_spawn_h(g_join_rt, NULL, join_target_entry, g_join_rt,
                              &g_join_target) == CORO_OK);
        for (unsigned i = 0; i < JOIN_WAITERS; ++i) {
            CHECK(coro_rt_spawn(g_join_rt, NULL, join_waiter_entry,
                                (void*)(size_t)i) == CORO_OK);
        }
        CHECK(coro_rt_wait_all(g_join_rt) == CORO_OK);
        for (unsigned i = 0; i < JOIN_WAITERS; ++i) {
            CHECK(g_join_back[i] == 1u);
        }
        CHECK(coro_rt_pending(g_join_rt) == 0);
        CHECK(coro_rt_collect(g_join_rt, g_join_target) == CORO_OK);
        CHECK(coro_rt_pending(g_join_rt) == 0);
        CHECK(coro_rt_destroy(g_join_rt) == CORO_OK);
    }

    /* ---- join: cancelled before the park, no phantom on the chain ---- */
    {
        g_jc_rt = coro_rt_create(1);   /* single worker: strict ordering */
        CHECK(g_jc_rt != NULL);
        g_jc_target = NULL;
        g_jc_st = CORO_OK;
        CHECK(coro_rt_spawn_h(g_jc_rt, NULL, jc_target_entry, g_jc_rt,
                              &g_jc_target) == CORO_OK);
        CHECK(coro_rt_spawn(g_jc_rt, NULL, jc_joiner_entry, g_jc_rt) == CORO_OK);
        CHECK(coro_rt_wait_all(g_jc_rt) == CORO_OK);
        CHECK(g_jc_st == CORO_ECANCELED);
        CHECK(coro_rt_pending(g_jc_rt) == 0);
        CHECK(coro_rt_collect(g_jc_rt, g_jc_target) == CORO_OK);
        CHECK(coro_rt_destroy(g_jc_rt) == CORO_OK);
    }

    /* ---- join: a finished retained target reports EDESTROYED ---- */
    {
        g_jd_rt = coro_rt_create(1);
        CHECK(g_jd_rt != NULL);
        g_jd_target = NULL;
        g_jd_st = CORO_OK;
        CHECK(coro_rt_spawn_h(g_jd_rt, NULL, jd_target_entry, g_jd_rt,
                              &g_jd_target) == CORO_OK);
        sleep_ms(50);   /* the target finishes before the joiner is spawned */
        CHECK(coro_rt_spawn(g_jd_rt, NULL, jd_joiner_entry, g_jd_rt) == CORO_OK);
        CHECK(coro_rt_wait_all(g_jd_rt) == CORO_OK);
        CHECK(g_jd_st == CORO_EDESTROYED);
        CHECK(coro_rt_pending(g_jd_rt) == 0);
        CHECK(coro_rt_collect(g_jd_rt, g_jd_target) == CORO_OK);
        CHECK(coro_rt_destroy(g_jd_rt) == CORO_OK);
    }

    /* ---- a cancelled joiner requeues on its next plain yield ---- */
    {
        g_jy_rt = coro_rt_create(1);   /* single worker: strict ordering */
        CHECK(g_jy_rt != NULL);
        g_jy_target = NULL;
        g_jy_join_st = CORO_OK;
        g_jy_after_yield = 0;
        CHECK(coro_rt_spawn_h(g_jy_rt, NULL, jy_target_entry, g_jy_rt,
                              &g_jy_target) == CORO_OK);
        CHECK(coro_rt_spawn(g_jy_rt, NULL, jy_joiner_entry, g_jy_rt) == CORO_OK);
        CHECK(coro_rt_wait_all(g_jy_rt) == CORO_OK);
        CHECK(g_jy_join_st == CORO_ECANCELED);
        CHECK(g_jy_after_yield == 1u);
        CHECK(coro_rt_pending(g_jy_rt) == 0);
        CHECK(coro_rt_collect(g_jy_rt, g_jy_target) == CORO_OK);
        CHECK(coro_rt_destroy(g_jy_rt) == CORO_OK);
    }

    /* ---- a cancellation racing the target's death reports the outcome ---- */
    {
        g_j2_rt = coro_rt_create(1);   /* single worker: strict ordering */
        CHECK(g_j2_rt != NULL);
        g_j2_target = NULL;
        g_j2_joiner = NULL;
        g_j2_st = CORO_OK;
        CHECK(coro_rt_spawn_h(g_j2_rt, NULL, j2_target_entry, g_j2_rt,
                              &g_j2_target) == CORO_OK);
        CHECK(coro_rt_spawn_h(g_j2_rt, NULL, j2_joiner_entry, g_j2_rt,
                              &g_j2_joiner) == CORO_OK);
        CHECK(coro_rt_spawn(g_j2_rt, NULL, j2_canceller_entry, g_j2_rt) == CORO_OK);
        CHECK(coro_rt_wait_all(g_j2_rt) == CORO_OK);
        CHECK(g_j2_st == CORO_OK);   /* the target finished: condition met */
        CHECK(coro_rt_pending(g_j2_rt) == 0);
        CHECK(coro_rt_collect(g_j2_rt, g_j2_target) == CORO_OK);
        CHECK(coro_rt_collect(g_j2_rt, g_j2_joiner) == CORO_OK);
        CHECK(coro_rt_pending(g_j2_rt) == 0);
        CHECK(coro_rt_destroy(g_j2_rt) == CORO_OK);
    }

    /* ---- V4 contract: collect after destroy touches no runtime ---- */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        coro_t* h = NULL;
        CHECK(coro_rt_spawn_h(rt, NULL, quick_entry, NULL, &h) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(coro_rt_pending(rt) == 0);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
        /* The gate reads only the node flags (acquire), never the runtime,
           so collecting after destroy reclaims the handle -- with a
           lock-taking gate this path dereferenced the freed runtime. */
        CHECK(coro_rt_collect(rt, h) == CORO_OK);
    }
}

TEST_MAIN()
