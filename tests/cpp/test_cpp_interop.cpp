/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * tests/cpp/test_cpp_interop.cpp -- the C/C++ coexistence face: a C child
 * spawned straight onto a wrapper scope (the C cell still aggregates, the
 * wrapper reports it), a wrapper coroutine driven entirely through the C
 * API via native(), a C join parked from inside a wrapper task while the
 * wrapper still collects, and the cross-language note: a C consumer of a
 * scope with C++ children sees CORO_ECANCELED for an abnormal child.
 */
#include "../harness.h"

#include <corocc/coro.hpp>

#include <atomic>
#include <chrono>
#include <exception>

namespace {

struct my_problem {};

coro_status c_child_estale(void* arg) {
    (void)arg;
    return CORO_ESTALE;
}

coro_status c_child_ok(void* arg) {
    (void)arg;
    return CORO_OK;
}

} /* namespace */

static void test_c_child_on_cpp_scope(void) {
    corocc::runtime rt(2);
    corocc::scope s(rt);

    coro_status* alias = NULL;
    coro_status* alias2 = NULL;
    CHECK(coro_rt_scope_spawn(rt.native(), s.native(), NULL, c_child_estale,
                              NULL, &alias) == CORO_OK);
    CHECK(coro_rt_scope_spawn(rt.native(), s.native(), NULL, c_child_ok, NULL,
                              &alias2) == CORO_OK);
    CHECK(alias != NULL && alias2 != NULL);

    const corocc::status agg = s.wait();
    CHECK(agg.code() == CORO_ESTALE); /* the C cell aggregated */
    CHECK(alias != NULL && *alias == CORO_ESTALE); /* C cell still readable */
    CHECK(s.first_exception() == std::exception_ptr()); /* no wrapper child */
    CHECK(s.destroy().ok());
}

static void test_cpp_coroutine_driven_by_c(void) {
    int steps = 0;
    corocc::coroutine c;
    CHECK(corocc::coroutine::create(c, [&steps]() -> coro_status {
        ++steps;
        for (int i = 0; i < 2; ++i) {
            if (coro_yield() != CORO_OK) return CORO_ECANCELED;
            ++steps;
        }
        return CORO_OK;
    }).ok());

    /* The whole drive runs through the C API on native(): the wrapper only
     * observes via state()/done()/exception(). */
    coro_token tok = CORO_TOKEN_NONE;
    CHECK(coro_start(c.native(), &tok) == CORO_OK);
    CHECK(steps == 1);
    CHECK(c.state() == CORO_SUSPENDED);
    CHECK(coro_resume(c.native(), tok, &tok) == CORO_OK);
    CHECK(steps == 2);
    CHECK(coro_resume(c.native(), tok, &tok) == CORO_OK);
    CHECK(steps == 3);
    CHECK(tok == CORO_TOKEN_DEAD);
    CHECK(c.done());
    CHECK(c.join().ok());
    CHECK(c.cancel().ok()); /* a request on a dead coroutine is fine */
    CHECK(c.destroy().ok());
}

static void test_cpp_task_joined_by_c(void) {
    corocc::runtime rt(2);
    corocc::task<int> t;
    CHECK(rt.spawn_h(t, [&rt]() -> int {
        (void)rt.sleep_for(std::chrono::milliseconds(30));
        return 7;
    }).ok());

    int seen = -1;
    corocc::status join_st(CORO_EAGAIN);
    corocc::task<void> waiter;
    CHECK(rt.spawn_h(waiter, [&rt, &t, &seen, &join_st]() {
        /* The C join parks this wrapper coroutine on the target's chain. */
        join_st = corocc::status(coro_rt_join(rt.native(), t.native()));
        if (join_st.ok()) {
            seen = t.get(); /* reads the wrapper snapshot */
        }
    }).ok());
    CHECK(waiter.wait().ok());
    CHECK(join_st.ok());
    CHECK(seen == 7);
    /* The C join does not collect: the wrapper wait() closes the handle. */
    CHECK(t.wait().ok());
    CHECK(rt.pending() == 0u);
}

static void test_c_consumer_sees_abnormal_marker(void) {
    /* The cross-language note (A8 item 3): a C consumer waiting a scope that
     * holds C++ children sees CORO_ECANCELED for an uncaptured exception --
     * and that is what drives the C-level early exit. */
    corocc::runtime rt(2);
    corocc::scope s(rt);
    CHECK(s.spawn([]() { throw my_problem(); }).ok());
    CHECK(s.spawn([&rt]() {
        (void)rt.sleep_for(std::chrono::seconds(2));
    }).ok());

    const long long t0 = static_cast<long long>(coro_monotonic_ns());
    const coro_status agg =
        coro_rt_group_wait_first_error(rt.native(), s.native());
    const long long elapsed = static_cast<long long>(coro_monotonic_ns()) - t0;
    CHECK(agg == CORO_ECANCELED);
    CHECK(elapsed < 1000000000LL); /* the early exit, not the 2 s drain */
    /* The C++ consumer recovers the truth from the exception channel. */
    CHECK(s.first_exception() != std::exception_ptr());
    CHECK(s.destroy().ok());
}

static void test_native_borrow_window(void) {
    corocc::runtime rt(2);
    corocc::task<int> t;
    CHECK(rt.spawn_h(t, []() -> int { return 3; }).ok());
    CHECK(t.native() != NULL);
    /* The borrowed handle is valid until the wrapper closes it out. */
    CHECK(rt.pending() >= 1u || t.done());
    CHECK(t.wait().ok());
    CHECK(t.native() == NULL); /* collected: the borrow window closed */
    CHECK(rt.pending() == 0u);
    CHECK(rt.destroy().ok());

    /* The native runtime handle stays the C object. */
    corocc::runtime rt2(2);
    CHECK(rt2.native() != NULL);
    CHECK(coro_rt_pending(rt2.native()) == 0u);
    CHECK(rt2.destroy().ok());
    CHECK(rt2.native() == NULL);
}

static void run_tests(void) {
    test_c_child_on_cpp_scope();
    test_cpp_coroutine_driven_by_c();
    test_cpp_task_joined_by_c();
    test_c_consumer_sees_abnormal_marker();
    test_native_borrow_window();
}
TEST_MAIN()
