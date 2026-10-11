#include "../harness.h"
#include <corocc/coro_ctx.h>

/* A single round trip only proves that one suspension/resumption pair works.
   The public contract of coro_ctx_swap -- "from is updated in place with the
   resumption point so it can be resumed later" -- must hold for *repeated*
   round trips, because a context object is reusable. This test drives many
   round trips through the same pair of context objects and checks that:
     - the coroutine body is entered exactly once (no stale-frame re-entry),
     - the coroutine's own frame survives every resumption,
     - the main context's frame survives every resumption too. */

#define ROUNDS 100000u

static coro_ctx  g_main;
static coro_ctx  g_work;
static unsigned  g_rounds = 0;     /* last round number written by the coroutine */
static unsigned  g_entries = 0;    /* number of times the coroutine body ran */
static unsigned  g_main_iters = 0; /* main-side loop iterations */
static int       g_resumed_correctly = 1;

static void entry(void* arg) {
    (void)arg;
    unsigned seen = 0;
    int first = 1;
    ++g_entries;
    while (seen < ROUNDS) {
        /* Every pass after the first is a resume of this very frame: the value
           this frame wrote last round must still be observable, which only
           holds if the saved frame and the globals survived intact. */
        ++seen;
        if (!first && g_rounds != seen - 1) g_resumed_correctly = 0;
        first = 0;
        g_rounds = seen;
        coro_ctx_swap(&g_work, &g_main);
    }
}

static void run_tests(void) {
    coro_stack stk;

    CHECK(coro_stack_alloc(&stk, 64 * 1024) == CORO_OK);
    CHECK(coro_ctx_init(&g_work, &stk, entry, NULL) == 0);

    /* This loop body must run ROUNDS times inside one invocation: proof that
       the main context is resumed through the same frame each round. */
    while (g_rounds < ROUNDS) {
        ++g_main_iters;
        coro_ctx_swap(&g_main, &g_work);
    }

    CHECK(g_rounds == ROUNDS);
    CHECK(g_main_iters == ROUNDS);
    CHECK(g_resumed_correctly == 1);
    CHECK(g_entries == 1);

    coro_ctx_destroy(&g_work);
    coro_stack_free(&stk);
}

TEST_MAIN()
