#include "../harness.h"
#include <corocc/coro_stack.h>

static void run_tests(void) {
    coro_stack stk;

    CHECK(coro_stack_alloc(&stk, 64 * 1024) == CORO_OK);
    CHECK(coro_stack_size(&stk) >= 64 * 1024);
    CHECK(coro_stack_avail(&stk) <= coro_stack_size(&stk));
    coro_stack_free(&stk);

    CHECK(coro_stack_alloc(NULL, 4096) == CORO_EINVAL);
    CHECK(coro_stack_alloc(&stk, 1) != CORO_OK); /* too small */
}

TEST_MAIN()
