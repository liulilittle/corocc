// corocc -- a zero-dependency C/C++ coroutine library.
//
// tests/coexist/test_boost_coexist.cpp
//
// Proves that corocc and the real Boost.Context can be linked into ONE
// binary and drive their own contexts side by side with no symbol clash.
// corocc renames Boost's make_fcontext/jump_fcontext to
// corocc_asm_make/corocc_asm_jump precisely so this coexistence works.
#include <boost/context/fiber.hpp>
#include <corocc/coro_ctx.h>

#include <cstdio>
#include <utility>

static int boost_ran = 0, coro_ran = 0;

int main() {
    namespace ctx = boost::context;

    // 1) Drive a real Boost.Context fiber.
    ctx::fiber f([](ctx::fiber&& f) {
        boost_ran = 1;
        return std::move(f);
    });
    f = std::move(f).resume();

    // 2) Drive a corocc context on our own stack.
    coro_stack stk;
    coro_ctx c, m;
    coro_stack_alloc(&stk, 64 * 1024);
    coro_ctx_init(&c, &stk, [](void*) { coro_ran = 1; }, nullptr);
    coro_ctx_swap(&m, &c);

    std::printf("boost=%d coro=%d\n", boost_ran, coro_ran);
    coro_stack_free(&stk);
    return (boost_ran && coro_ran) ? 0 : 1;
}
