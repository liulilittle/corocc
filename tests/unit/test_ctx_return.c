#include "../harness.h"
#include <corocc/coro_ctx.h>

static int after = 0;

static void entry(void* a) {
    (void)a;
    /* returns immediately; must reach the sentinel, not Boost's _exit */
}

static void run_tests(void) {
    coro_stack stk;
    coro_ctx c, main_ctx;

    coro_stack_alloc(&stk, 64 * 1024);
    coro_ctx_init(&c, &stk, entry, NULL);
    /* If the implementation hits Boost _exit, the process dies here and this
       test can never pass. */
    coro_ctx_swap(&main_ctx, &c);
    after = 1;
    CHECK(after == 1);
    coro_stack_free(&stk);
}

TEST_MAIN()
