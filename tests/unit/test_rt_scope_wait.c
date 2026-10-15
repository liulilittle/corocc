#include "../harness.h"
#include "coroutine/coro_runtime.h"   /* internal: the current coroutine */
#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_sched.h>      /* coro_monotonic_ns */
#include <corocc/coro_scope.h>
#include <string.h>
#include <time.h>

/* The wait predicate.
 *
 *   - the predicate is live==0, checked under the runtime's activity lock,
 *     parked on the scope's event chain -- one chain, one parking point;
 *   - the child-exit event wakes every parked waiter, so several waiters
 *     on one scope all observe the drain, each resumed exactly once per
 *     park (the 1:1 balance; a double wake would relink the node in the
 *     worker's inbox and hang or crash);
 *   - the wait survives its waiter's own cancellation: a request consumed
 *     at the park's safe point detaches the waiter and the loop continues
 *     (the documented caller-level contract);
 *   - children registered WHILE a waiter is parked still count: the wait
 *     ends only when the scope's live count really reached zero.
 *
 * The waiters are coroutines; the main thread drives the runtime to
 * quiescence with coro_rt_wait_all before asserting.
 */

static coro_rt_t* g_rt;

struct scene {
    coro_rt_scope_t* s;
    coro_status      wait;    /* out: what this waiter's wait returned */
    unsigned         runs;    /* out: how often the entry ran */
};

#define WAITERS 4u

static void waiter_entry(void* arg) {
    struct scene* sc = (struct scene*)arg;
    ++sc->runs;
    sc->wait = coro_rt_scope_wait(g_rt, sc->s);
}

/* Sleeps ms (carried in arg) and returns the code after it. */
struct delay_code {
    unsigned    ms;
    coro_status code;
};

static coro_status delayed_code_entry(void* arg) {
    struct delay_code* dc = (struct delay_code*)arg;
    (void)coro_rt_sleep(g_rt,
                        coro_monotonic_ns() + (uint64_t)dc->ms * 1000000ull);
    return dc->code;
}

/* The spawn-during-wait scene: registers a second child on the same scope
   while waiters are already parked. The late child's code lives at file
   scope: the spawner's frame dies before the queued child runs. */
static coro_rt_scope_t* g_late_scope;
static coro_status*     g_late_cell;
static struct delay_code g_late_dc = { 0, CORO_ESTALE };

static coro_status late_spawner_entry(void* arg) {
    (void)arg;
    (void)coro_rt_sleep(g_rt, coro_monotonic_ns() + 10000000ull); /* 10ms */
    /* a later arrival: the waiters' predicate must cover it */
    (void)coro_rt_scope_spawn(g_rt, g_late_scope, NULL, delayed_code_entry,
                              &g_late_dc, &g_late_cell);
    return CORO_OK;
}

/* The immune-waiter scene: cancels itself, then waits anyway. */
static void immune_waiter_entry(void* arg) {
    struct scene* sc = (struct scene*)arg;
    coro_cancel(coro_runtime_current()->current);
    ++sc->runs;
    sc->wait = coro_rt_scope_wait(g_rt, sc->s);
}

static void run_tests(void) {
    coro_rt_t* rt = coro_rt_create(1);
    CHECK(rt != NULL);
    g_rt = rt;

    /* ---- several waiters on one scope: all released, one resume each -- */
    {
        struct delay_code dc = { 50, CORO_EBUSY };
        coro_rt_scope_t s;
        struct scene sc[WAITERS];
        coro_status* cell = NULL;
        memset(&s, 0, sizeof s);
        memset(sc, 0, sizeof sc);
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, delayed_code_entry, &dc,
                                  &cell) == CORO_OK);
        for (unsigned i = 0; i < WAITERS; ++i) {
            sc[i].s = &s;
            CHECK(coro_rt_spawn(rt, NULL, waiter_entry, &sc[i]) == CORO_OK);
        }
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        for (unsigned i = 0; i < WAITERS; ++i) {
            CHECK(sc[i].runs == 1u);            /* exactly one resume */
            CHECK(sc[i].wait == CORO_EBUSY);    /* all saw the aggregate */
        }
        CHECK(*cell == CORO_EBUSY);
        CHECK(coro_rt_scope_status(&s) == CORO_EBUSY);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- a waiter cancelled while parked keeps waiting (immunity) ----- */
    {
        struct delay_code dc = { 50, CORO_EBUSY };
        coro_rt_scope_t s;
        struct scene sc;
        coro_status* cell = NULL;
        memset(&s, 0, sizeof s);
        memset(&sc, 0, sizeof sc);
        sc.s = &s;
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, delayed_code_entry, &dc,
                                  &cell) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, immune_waiter_entry, &sc) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(sc.runs == 1u);
        /* The self-cancellation was consumed at the park's safe point and
           ignored: the wait ended on the predicate, with the aggregate --
           not with an interrupted ECANCELED that would strand the child. */
        CHECK(sc.wait == CORO_EBUSY);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    /* ---- children registered during the wait are covered -------------- */
    {
        coro_rt_scope_t s;
        struct scene sc;
        memset(&s, 0, sizeof s);
        memset(&sc, 0, sizeof sc);
        sc.s = &s;
        g_late_scope = &s;
        g_late_cell = NULL;
        CHECK(coro_rt_scope_spawn(rt, &s, NULL, late_spawner_entry, NULL,
                                  NULL) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, waiter_entry, &sc) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(sc.wait == CORO_ESTALE);   /* the late arrival's error is in */
        CHECK(g_late_cell != NULL && *g_late_cell == CORO_ESTALE);
        CHECK(coro_rt_pending(rt) == 0);
        CHECK(coro_rt_scope_destroy(&s) == CORO_OK);
    }

    CHECK(coro_rt_pending(rt) == 0);
    CHECK(coro_rt_destroy(rt) == CORO_OK);
}

TEST_MAIN()
