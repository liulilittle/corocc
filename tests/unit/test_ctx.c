#include "../harness.h"
#include <corocc/coro_ctx.h>

static int g_ran = 0;

static void entry(void* arg) {
    (void)arg;
    g_ran = 1;
}

static void run_tests(void) {
    coro_stack stk;
    coro_ctx ctx, main_ctx;

    CHECK(coro_stack_alloc(&stk, 64 * 1024) == CORO_OK);
    CHECK(coro_ctx_init(&ctx, &stk, entry, NULL) == 0);
    /* enter coroutine; entry returns -> sentinel -> back to main_ctx */
    coro_ctx_swap(&main_ctx, &ctx);
    CHECK(g_ran == 1);
    coro_ctx_destroy(&ctx);
    coro_stack_free(&stk);
}

TEST_MAIN()
