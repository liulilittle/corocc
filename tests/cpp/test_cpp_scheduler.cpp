/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * tests/cpp/test_cpp_scheduler.cpp -- the single-thread scheduler face:
 * create/run/wait_all/pending, spawn and sched_task<R> (the handle is
 * retired by the scheduler, so the wrapper's done flag gates every touch),
 * the thread-affinity rules, sleeping with a deadline, and the drain dtor.
 */
#include "../harness.h"

#include <corocc/coro.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <thread>

namespace {

struct my_problem {};

std::atomic<int> g_runs(0);

} /* namespace */

static void test_empty_scheduler(void) {
    corocc::scheduler s;
    CHECK(!s.valid());
    CHECK(s.native() == NULL);
    CHECK(s.pending() == 0u);
    CHECK(s.run().code() == CORO_EINVAL);
    CHECK(s.wait_all().code() == CORO_EINVAL);
    CHECK(s.destroy().ok()); /* C NULL destroy is a no-op */
}

static void test_create_and_destroy(void) {
    corocc::scheduler s;
    CHECK(corocc::scheduler::create(s).ok());
    CHECK(s.valid());
    /* out already holds a live scheduler: refused, out untouched. */
    CHECK(corocc::scheduler::create(s).code() == CORO_ESTATE);
    CHECK(s.valid());
    CHECK(s.destroy().ok());
    CHECK(!s.valid());
    CHECK(s.destroy().ok()); /* idempotent */
}

static void test_spawn_and_run(void) {
    corocc::scheduler s;
    CHECK(corocc::scheduler::create(s).ok());
    g_runs.store(0);
    for (int i = 0; i < 4; ++i) {
        CHECK(s.spawn([]() { g_runs.fetch_add(1); }).ok());
    }
    CHECK(s.run().ok());
    CHECK(g_runs.load() == 4);
    CHECK(s.pending() == 0u);
}

static void test_spawn_args_and_move(void) {
    corocc::scheduler s;
    CHECK(corocc::scheduler::create(s).ok());
    int acc = 0;
    CHECK(s.spawn([](int a, int& out) { out = a * 2; }, 21, std::ref(acc)).ok());
    CHECK(s.wait_all().ok());
    CHECK(acc == 42);

    corocc::scheduler moved(std::move(s));
    CHECK(!s.valid());
    CHECK(moved.valid());
    CHECK(moved.destroy().ok());
}

static void test_sched_task_value(void) {
    corocc::scheduler s;
    CHECK(corocc::scheduler::create(s).ok());
    corocc::sched_task<int> t;
    CHECK(!t.valid());
    CHECK(t.wait().code() == CORO_EINVAL);
    CHECK(t.cancel().code() == CORO_EINVAL);

    CHECK(s.spawn_h(t, []() -> int { return 5; }).ok());
    CHECK(t.valid());
    CHECK(s.run().ok());
    /* The scheduler retired the node; the wrapper's done flag is set, so
     * wait() answers without touching the (already freed) C handle. */
    CHECK(t.done());
    CHECK(t.wait().ok());
    CHECK(t.get() == 5);
    CHECK(t.peek() != NULL);
    CHECK(*t.peek() == 5);
    CHECK(t.cancel().ok()); /* idempotent on a finished task: no C touch */
    CHECK(s.destroy().ok());
}

static void test_sched_task_join_from_coroutine(void) {
    corocc::scheduler s;
    CHECK(corocc::scheduler::create(s).ok());
    corocc::sched_task<int> t;
    CHECK(s.spawn_h(t, []() -> int { return 9; }).ok());

    corocc::status inside_st(CORO_EAGAIN);
    int inside_value = 0;
    CHECK(s.spawn([&t, &inside_st, &inside_value]() {
        inside_st = t.wait();          /* parks on the target's chain */
        if (inside_st.ok()) {
            inside_value = t.get();    /* reads the wrapper snapshot */
        }
    }).ok());
    CHECK(s.run().ok());
    CHECK(inside_st.ok());
    CHECK(inside_value == 9);
}

static void test_sched_task_exception(void) {
    corocc::scheduler s;
    CHECK(corocc::scheduler::create(s).ok());
    corocc::sched_task<int> t;
    CHECK(s.spawn_h(t, []() -> int { throw my_problem(); }).ok());
    CHECK(s.run().ok());
    CHECK(t.done());
    CHECK(t.exception() != std::exception_ptr());
    bool caught = false;
    try {
        (void)t.get();
    } catch (const my_problem&) {
        caught = true;
    }
    CHECK(caught);
}

static void test_sched_task_void_and_status(void) {
    corocc::scheduler s;
    CHECK(corocc::scheduler::create(s).ok());
    corocc::sched_task<void> tv;
    g_runs.store(0);
    CHECK(s.spawn_h(tv, []() { g_runs.store(1); }).ok());
    corocc::sched_task<coro_status> ts;
    CHECK(s.spawn_h(ts, []() -> coro_status { return CORO_EBUSY; }).ok());
    CHECK(s.run().ok());
    CHECK(tv.done());
    CHECK(tv.wait().ok());
    tv.get();
    CHECK(g_runs.load() == 1);
    CHECK(ts.wait().ok());
    CHECK(ts.get() == CORO_EBUSY);
}

static void test_sched_task_cancel(void) {
    corocc::scheduler s;
    CHECK(corocc::scheduler::create(s).ok());
    corocc::sched_task<int> t;
    CHECK(s.spawn_h(t, [&s]() -> int {
        corocc::status st = s.sleep_for(std::chrono::seconds(30));
        return st.ok() ? 1 : -3;
    }).ok());
    CHECK(t.cancel().ok());           /* bound thread, not done: C cancel */
    CHECK(s.run().ok());              /* the cancel wakes the sleep */
    CHECK(t.done());
    CHECK(t.get() == -3);
}

static void test_sched_task_cancel_wrong_thread(void) {
    corocc::scheduler s;
    CHECK(corocc::scheduler::create(s).ok());
    corocc::sched_task<int> t;
    CHECK(s.spawn_h(t, []() -> int { return 1; }).ok());

    corocc::status from_other(CORO_OK);
    std::thread other([&t, &from_other]() {
        from_other = t.cancel();      /* must refuse without touching C */
    });
    other.join();
    CHECK(from_other.code() == CORO_ESTALE);

    CHECK(s.run().ok());
    CHECK(t.wait().ok());
    CHECK(t.get() == 1);
}

static void test_sched_task_dtor_cancels(void) {
    corocc::scheduler s;
    CHECK(corocc::scheduler::create(s).ok());
    {
        corocc::sched_task<int> t;
        CHECK(s.spawn_h(t, [&s]() -> int {
            corocc::status st = s.sleep_for(std::chrono::seconds(30));
            return st.ok() ? 1 : 0;
        }).ok());
        /* ~sched_task(): bound thread + unfinished -> one cancel request. */
    }
    CHECK(s.run().ok());              /* the cancelled sleeper finishes */
    CHECK(s.pending() == 0u);
    CHECK(s.destroy().ok());
}

static void test_sleep_deadline_accuracy(void) {
    corocc::scheduler s;
    CHECK(corocc::scheduler::create(s).ok());
    corocc::sched_task<long long> t;
    CHECK(s.spawn_h(t, [&s]() -> long long {
        const long long t0 = static_cast<long long>(coro_monotonic_ns());
        corocc::status st = s.sleep_for(std::chrono::milliseconds(20));
        const long long t1 = static_cast<long long>(coro_monotonic_ns());
        if (!st.ok()) return -1;
        return t1 - t0;
    }).ok());
    CHECK(s.run().ok());
    CHECK(t.wait().ok());
    const long long elapsed = t.get();
    /* The deadline must not fire early; the tolerance window is generous. */
    CHECK(elapsed >= 15000000LL);
    CHECK(elapsed < 2000000000LL);
}

static void test_sleep_zero_and_negative_are_immediate(void) {
    /* The negative/zero clamp: sleep_for(0) and sleep_for(-x) park for one
     * loop turn, not at all (the deadline is "now"). */
    corocc::scheduler s;
    CHECK(corocc::scheduler::create(s).ok());
    corocc::sched_task<long long> t;
    CHECK(s.spawn_h(t, [&s]() -> long long {
        const long long t0 = static_cast<long long>(coro_monotonic_ns());
        const corocc::status a = s.sleep_for(std::chrono::nanoseconds(0));
        const long long t1 = static_cast<long long>(coro_monotonic_ns());
        const corocc::status b = s.sleep_for(std::chrono::nanoseconds(-1000));
        const long long t2 = static_cast<long long>(coro_monotonic_ns());
        if (!a.ok() || !b.ok()) return -1;
        return (t1 - t0) + (t2 - t1);
    }).ok());
    CHECK(s.run().ok());
    CHECK(t.wait().ok());
    const long long elapsed = t.get();
    CHECK(elapsed >= 0);
    CHECK(elapsed < 100000000LL); /* well under 100 ms */
}

static void test_scheduler_dtor_drains(void) {
    std::atomic<int> n(0);
    {
        corocc::scheduler s;
        CHECK(corocc::scheduler::create(s).ok());
        for (int i = 0; i < 8; ++i) {
            CHECK(s.spawn([&n]() { n.fetch_add(1); }).ok());
        }
        /* ~scheduler(): wait_all + destroy. */
    }
    CHECK(n.load() == 8);
}

static void run_tests(void) {
    test_empty_scheduler();
    test_create_and_destroy();
    test_spawn_and_run();
    test_spawn_args_and_move();
    test_sched_task_value();
    test_sched_task_join_from_coroutine();
    test_sched_task_exception();
    test_sched_task_void_and_status();
    test_sched_task_cancel();
    test_sched_task_cancel_wrong_thread();
    test_sched_task_dtor_cancels();
    test_sleep_deadline_accuracy();
    test_sleep_zero_and_negative_are_immediate();
    test_scheduler_dtor_drains();
}
TEST_MAIN()
