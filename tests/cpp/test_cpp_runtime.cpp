/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * tests/cpp/test_cpp_runtime.cpp -- the L2 runtime face: runtime RAII and
 * drain, detached spawn, typed task<R> (value / void / coro_status), the
 * thread-side target-directed wait, cancel, the detached-exception handler,
 * the metadata-OOM versus user-construction-exception split, move semantics
 * and multi-worker stress.
 */
#include "../harness.h"

#include <corocc/coro.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <new>
#include <string>
#include <thread>

/* ---- allocation failure injection (wrapper metadata OOM) ----
 *
 * The wrapper's own metadata (the shared state, the control block, the name
 * copy) is allocated with operator new. Replacing the global operator new in
 * this test lets one chosen allocation fail with std::bad_alloc, which the
 * wrapper must normalise to CORO_ENOMEM without touching the out handle.
 * The C layer allocates with calloc, so it is not on this path.
 */
namespace {
long g_new_countdown = -1; /* -1: never; 0: fail now; >0: pass N then fail */
}

/* GCC's inlining-dependent -Wmismatched-new-delete false positive: a box
 * allocated through the nothrow shim and later plain-deleted trips the check
 * at -O1/-O2/-O3 even though this shim's new/delete pair is a matched
 * malloc/free pair (the check is syntactic on the allocation function's
 * identity and cannot see the delegation). The silencing is scoped to the
 * shim itself. */
#if defined(__GNUC__) && !defined(_MSC_VER)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t n) {
    if (g_new_countdown == 0) {
        g_new_countdown = -1;
        throw std::bad_alloc();
    }
    if (g_new_countdown > 0) {
        --g_new_countdown;
    }
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void* p) noexcept {
    std::free(p);
}

/* The nothrow pair must be replaced too: with a sanitizer interposing its
 * own nothrow new, the default pairing would report an alloc-dealloc
 * mismatch (sanitizer operator new vs our free). */
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(n);
    } catch (...) {
        return NULL;
    }
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    ::operator delete(p);
}

void* operator new[](std::size_t n) {
    return ::operator new(n);
}

void operator delete[](void* p) noexcept {
    std::free(p);
}

/* The C++14 sized forms: without them -Werror=sized-deallocation fires in
 * forward-standard builds. */
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

#if defined(__GNUC__) && !defined(_MSC_VER)
#pragma GCC diagnostic pop
#endif

namespace {

struct my_problem {};

std::atomic<int> g_detached_runs(0);
std::atomic<long> g_sum(0);
std::atomic<int> g_handler_calls(0);
std::exception_ptr g_handler_eptr;
std::atomic<int> g_flag(0);

void capture_handler(std::exception_ptr e) {
    g_handler_calls.fetch_add(1);
    g_handler_eptr = e;
}

struct throwing_functor {
    throwing_functor() {}
    throwing_functor(const throwing_functor&) { throw 7; }
    throwing_functor(throwing_functor&& other) {
        (void)other;
        throw 7;
    }
    void operator()() const {}
};

} /* namespace */

static void test_create_and_move(void) {
    corocc::runtime rt(2);
    CHECK(rt.valid());
    CHECK(rt.native() != NULL);
    CHECK(rt.pending() == 0u);

    corocc::runtime moved(std::move(rt));
    CHECK(!rt.valid());
    CHECK(moved.valid());
    /* The moved-from handle answers the C NULL contract. */
    CHECK(rt.wait_all().code() == CORO_EINVAL);
    CHECK(rt.pending() == 0u);

    corocc::runtime assigned;
    assigned = std::move(moved);
    CHECK(!moved.valid());
    CHECK(assigned.valid());
    CHECK(assigned.destroy().ok());
    CHECK(!assigned.valid());
    CHECK(assigned.destroy().ok()); /* idempotent on the empty handle */
}

static void test_status_first_create(void) {
    corocc::runtime rt;
    CHECK(corocc::runtime::create(rt, 2).ok());
    CHECK(rt.valid());
    /* out already holds a live runtime: refused, out untouched. */
    CHECK(corocc::runtime::create(rt, 2).code() == CORO_ESTATE);
    CHECK(rt.valid());
    CHECK(rt.destroy().ok());
}

static void test_detached_spawn(void) {
    corocc::runtime rt(2);
    g_detached_runs.store(0);
    for (int i = 0; i < 8; ++i) {
        CHECK(rt.spawn([]() { g_detached_runs.fetch_add(1); }).ok());
    }
    CHECK(rt.wait_all().ok());
    CHECK(g_detached_runs.load() == 8);
    CHECK(rt.pending() == 0u);
}

static void test_detached_spawn_args(void) {
    corocc::runtime rt(2);
    g_sum.store(0);
    CHECK((rt.spawn([](int a, int b) { g_sum.fetch_add(a * b); }, 6, 7).ok()));
    CHECK(rt.wait_all().ok());
    CHECK(g_sum.load() == 42);

    /* std::ref expresses reference semantics explicitly. */
    int acc = 0;
    CHECK(rt.spawn([](int& x) { x += 5; }, std::ref(acc)).ok());
    CHECK(rt.wait_all().ok());
    CHECK(acc == 5);
}

static void test_task_value(void) {
    corocc::runtime rt(2);
    corocc::task<int> t;
    CHECK(!t.valid());
    CHECK(!t.done());
    CHECK(t.wait().code() == CORO_EINVAL);
    CHECK(t.cancel().code() == CORO_EINVAL);

    CHECK(rt.spawn_h(t, []() -> int { return 42; }).ok());
    CHECK(t.valid());
    CHECK(t.native() != NULL);

    /* Thread-side wait: target-directed collect retry; the target is
     * quick, so the retry rides its retirement out promptly. */
    CHECK(t.wait().ok());
    CHECK(t.done());
    CHECK(t.peek() != NULL);
    CHECK(*t.peek() == 42);
    CHECK(t.exception() == std::exception_ptr());
    CHECK(t.get() == 42);
    CHECK(t.wait().ok()); /* idempotent once collected */
    CHECK(rt.pending() == 0u);
}

static void test_task_args(void) {
    corocc::runtime rt(2);
    corocc::task<int> t;
    CHECK(rt.spawn_h(t, [](int a, int b) -> int { return a * b; }, 6, 7).ok());
    CHECK(t.wait().ok());
    CHECK(t.get() == 42);
}

static void test_task_void(void) {
    corocc::runtime rt(2);
    corocc::task<void> t;
    g_flag.store(0);
    CHECK(rt.spawn_h(t, []() { g_flag.store(1); }).ok());
    CHECK(t.wait().ok());
    CHECK(t.done());
    CHECK(g_flag.load() == 1);
    t.get(); /* rethrows on an exception; returns otherwise */
    CHECK(t.exception() == std::exception_ptr());
}

static void test_task_status(void) {
    corocc::runtime rt(2);
    corocc::task<coro_status> t;
    CHECK(rt.spawn_h(t, []() -> coro_status { return CORO_ESTALE; }).ok());
    CHECK(t.wait().ok());
    CHECK(t.get() == CORO_ESTALE);
    CHECK(t.peek() != NULL);
    CHECK(*t.peek() == CORO_ESTALE);
}

static void test_task_cancel_and_thread_wait(void) {
    corocc::runtime rt(2);
    corocc::task<int> t;
    CHECK(rt.spawn_h(t, [&rt]() -> int {
        corocc::status s = rt.sleep_for(std::chrono::seconds(30));
        return s.ok() ? 1 : -7;
    }).ok());
    CHECK(!t.done());
    CHECK(t.cancel().ok());
    CHECK(t.wait().ok()); /* the cancel wakes the sleep; the thread-side
                             wait rides the retirement window out */
    CHECK(t.done());
    CHECK(t.get() == -7);
}

static void test_task_exception(void) {
    corocc::runtime rt(2);
    corocc::task<int> t;
    CHECK(rt.spawn_h(t, []() -> int { throw my_problem(); }).ok());
    CHECK(t.wait().ok()); /* wait reports status, never throws */
    CHECK(t.exception() != std::exception_ptr());
    bool caught = false;
    try {
        (void)t.get();
    } catch (const my_problem&) {
        caught = true;
    }
    CHECK(caught);
}

/* The bounded thread-side wait reports EBUSY when the target outlives the
 * retry budget; the caller may retry later. A 30 s sleeper is such a target,
 * but waiting the whole budget would make the suite slow -- the design
 * accepts the wait() contract as verified by cpp_runtime's quick targets
 * plus this cancel path; the budget itself is compile-time fixed. */

static void test_task_move(void) {
    corocc::runtime rt(2);
    corocc::task<int> a;
    CHECK(rt.spawn_h(a, []() -> int { return 7; }).ok());
    corocc::task<int> b(std::move(a));
    CHECK(!a.valid());
    CHECK(b.valid());
    CHECK(b.wait().ok());
    CHECK(b.get() == 7);
}

static void test_task_dtor_cancels_and_drains(void) {
    corocc::runtime rt(2);
    {
        corocc::task<int> t;
        CHECK(rt.spawn_h(t, [&rt]() -> int {
            corocc::status s = rt.sleep_for(std::chrono::seconds(30));
            return s.ok() ? 1 : 0;
        }).ok());
        /* ~task(): cancel + drain to collection; must not leak or hang. */
    }
    CHECK(rt.pending() == 0u);
}

static void test_detached_handler(void) {
    corocc::runtime rt(2);
    g_handler_calls.store(0);
    g_handler_eptr = std::exception_ptr();
    corocc::detached_exception_handler old =
        corocc::set_detached_exception_handler(&capture_handler);
    CHECK(old != NULL);
    CHECK(rt.spawn([]() { throw my_problem(); }).ok());
    CHECK(rt.wait_all().ok());
    /* Restore before asserting: the handler ran on a worker thread and
     * wait_all's drain publishes it (live_notify's lock chain). */
    corocc::set_detached_exception_handler(old);
    CHECK(g_handler_calls.load() == 1);
    CHECK(g_handler_eptr != std::exception_ptr());
}

static void test_metadata_oom_normalised(void) {
    corocc::runtime rt(2);
    corocc::task<int> t;

    /* First allocation: the shared result state. */
    g_new_countdown = 0;
    corocc::status st = rt.spawn_h(t, []() -> int { return 1; });
    g_new_countdown = -1;
    CHECK(st.code() == CORO_ENOMEM);
    CHECK(!t.valid());
    CHECK(rt.pending() == 0u);

    /* Second allocation: the control block itself. */
    g_new_countdown = 1;
    st = rt.spawn_h(t, []() -> int { return 2; });
    g_new_countdown = -1;
    CHECK(st.code() == CORO_ENOMEM);
    CHECK(!t.valid());
    CHECK(rt.pending() == 0u);

    /* Detached spawn: the control block is the first allocation. */
    g_new_countdown = 0;
    st = rt.spawn([]() {});
    g_new_countdown = -1;
    CHECK(st.code() == CORO_ENOMEM);
    CHECK(rt.pending() == 0u);

    /* The runtime stays perfectly usable after the injected failures. */
    st = rt.spawn_h(t, []() -> int { return 3; });
    CHECK(st.ok());
    CHECK(t.wait().ok());
    CHECK(t.get() == 3);
}

static void test_user_ctor_exception_propagates(void) {
    corocc::runtime rt(2);
    bool caught = false;
    try {
        corocc::status st = rt.spawn(throwing_functor());
        (void)st;
    } catch (int v) {
        caught = (v == 7);
    }
    CHECK(caught);
    /* The refused spawn retained nothing. */
    CHECK(rt.pending() == 0u);
    CHECK(rt.wait_all().ok());
}

static void test_runtime_dtor_drains(void) {
    std::atomic<int> n(0);
    {
        corocc::runtime rt(4);
        for (int i = 0; i < 16; ++i) {
            CHECK(rt.spawn([&n]() { n.fetch_add(1); }).ok());
        }
        /* ~runtime(): wait_all + destroy; every child must have run. */
    }
    CHECK(n.load() == 16);
}

static void test_multi_worker(void) {
    corocc::runtime rt(4);
    g_sum.store(0);
    for (int i = 1; i <= 200; ++i) {
        CHECK(rt.spawn([i]() { g_sum.fetch_add(i); }).ok());
    }
    CHECK(rt.wait_all().ok());
    CHECK(g_sum.load() == 200L * 201L / 2L);
    CHECK(rt.pending() == 0u);
}

/* ---- wait() on the calling coroutine's own handle is the self-join and
 * reports EAGAIN, never the thread-side EBUSY retry exhaustion. */
static void test_wait_self_join_eagain(void) {
    corocc::runtime rt(2);
    std::atomic<int> self_code(-999);
    std::atomic<bool> go(false);
    corocc::task<int> t;
    CHECK(rt.spawn_h(t, [&]() -> int {
        for (long i = 0; i < 10000000L && !go.load(); ++i) {
            (void)corocc::yield();
        }
        self_code.store(static_cast<int>(t.wait().code()));
        return 7;
    }).ok());
    go.store(true);
    CHECK(t.wait().ok());
    rt.wait_all();
    CHECK(self_code.load() == CORO_EAGAIN);
    CHECK(t.done());
}

/* ---- A std::bad_alloc thrown while the wrapper moves/stores the user's
 * callable or arguments normalises to CORO_ENOMEM (hop1); other exceptions
 * and the call-site hop (hop0) keep propagating. */
struct ba_move_functor {
    int hops;
    int throw_at;
    int mode; /* 0 = bad_alloc, 1 = int */
    ba_move_functor(int throw_at_, int mode_)
        : hops(0), throw_at(throw_at_), mode(mode_) {}
    ba_move_functor(const ba_move_functor& o)
        : hops(o.hops), throw_at(o.throw_at), mode(o.mode) {}
    ba_move_functor(ba_move_functor&& o)
        : hops(o.hops + 1), throw_at(o.throw_at), mode(o.mode) {
        if (hops >= throw_at) {
            if (mode == 0) throw std::bad_alloc();
            throw 7;
        }
    }
    int operator()() const { return 3; }
};

std::atomic<int> g_ba_arg_hops(-1); /* the wrapper-internal hop that threw */

struct ba_count_arg {
    int hops;
    int throw_at;
    explicit ba_count_arg(int throw_at_) : hops(0), throw_at(throw_at_) {}
    ba_count_arg(const ba_count_arg& o)
        : hops(o.hops), throw_at(o.throw_at) {}
    ba_count_arg(ba_count_arg&& o)
        : hops(o.hops + 1), throw_at(o.throw_at) {
        if (hops >= throw_at) {
            g_ba_arg_hops.store(hops);
            throw std::bad_alloc();
        }
    }
};

static void test_bad_alloc_move_normalised(void) {
    corocc::runtime rt(2);

    /* Detached: hop1 bad_alloc -> ENOMEM. */
    {
        ba_move_functor m(1, 0);
        bool thrown = false;
        corocc::status st = CORO_OK;
        try {
            st = rt.spawn(m);
        } catch (const std::bad_alloc&) {
            thrown = true;
        }
        CHECK(!thrown);
        CHECK(st.code() == CORO_ENOMEM);
    }

    /* Typed: hop1 bad_alloc -> ENOMEM, out untouched. */
    {
        ba_move_functor m(1, 0);
        corocc::task<int> t;
        bool thrown = false;
        corocc::status st = CORO_OK;
        try {
            st = rt.spawn_h(t, m);
        } catch (const std::bad_alloc&) {
            thrown = true;
        }
        CHECK(!thrown);
        CHECK(st.code() == CORO_ENOMEM);
        CHECK(!t.valid());
    }

    /* Variadic arguments: both wrapper-internal argument hops (the by-value
     * parameter hop and the tuple-member hop) normalise to ENOMEM; the
     * counting mover pins the hop that actually threw (no elision assumed). */
    for (int throw_at = 1; throw_at <= 2; ++throw_at) {
        ba_count_arg a(throw_at);
        g_ba_arg_hops.store(-1);
        bool thrown = false;
        corocc::status st = CORO_OK;
        try {
            st = rt.spawn([](ba_count_arg) {}, a);
        } catch (const std::bad_alloc&) {
            thrown = true;
        }
        CHECK(!thrown);
        CHECK(st.code() == CORO_ENOMEM);
        CHECK(g_ba_arg_hops.load() >= throw_at);
    }

    /* Control: a non-bad_alloc storage move keeps propagating. */
    {
        ba_move_functor m(1, 1);
        bool caught = false;
        try {
            corocc::status st = rt.spawn(m);
            (void)st;
        } catch (int v) {
            caught = (v == 7);
        }
        CHECK(caught);
    }

    /* Control: a hop2 bad_alloc still normalises (box_new). */
    {
        ba_move_functor m(2, 0);
        corocc::status st = rt.spawn(m);
        CHECK(st.code() == CORO_ENOMEM);
    }

    CHECK(rt.pending() == 0u);
    CHECK(rt.wait_all().ok());
}

static void run_tests(void) {
    test_create_and_move();
    test_status_first_create();
    test_detached_spawn();
    test_detached_spawn_args();
    test_task_value();
    test_task_args();
    test_task_void();
    test_task_status();
    test_task_cancel_and_thread_wait();
    test_task_exception();
    test_task_move();
    test_task_dtor_cancels_and_drains();
    test_detached_handler();
    test_metadata_oom_normalised();
    test_user_ctor_exception_propagates();
    test_wait_self_join_eagain();
    test_bad_alloc_move_normalised();
    test_runtime_dtor_drains();
    test_multi_worker();
}
TEST_MAIN()
