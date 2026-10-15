/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * tests/cpp/test_cpp_scope.cpp -- the L3 structured-concurrency face:
 * scope state transitions, typed cells, the group three-state family with
 * its timeliness pins, cancel semantics (idempotent, and ECANCELED
 * disambiguation against the exception channel), wait_for deadline
 * priority, the wait-own-cancel immunity, concurrent spawn, and the
 * cell-out-of-scope lifetime.
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

std::atomic<int> g_runs(0);

/* Namespace scope: the lambdas use them without a capture (MSVC requires a
 * local const captured, clang flags the unnecessary capture). */
const int kSpawnThreads = 4;
const int kSpawnPer = 25;

} /* namespace */

static void test_scope_state_machine(void) {
    corocc::runtime rt(2);
    corocc::scope s(rt);
    CHECK(s.native() != NULL);
    CHECK(!s.cancel_requested());

    /* An empty scope drains immediately with the OK aggregate. */
    CHECK(s.wait().ok());
    CHECK(s.status().ok());

    /* After a drain: spawn is refused without registering, cancel reports
     * ESTATE, wait stays a read of the frozen aggregate. */
    CHECK(s.spawn([]() {}).code() == CORO_ESTATE);
    CHECK(s.cancel().code() == CORO_ESTATE);
    CHECK(s.wait().ok());
    CHECK(s.destroy().ok());
    CHECK(s.destroy().ok()); /* idempotent */

    /* After destroy: ESTALE everywhere. */
    CHECK(s.spawn([]() {}).code() == CORO_ESTALE);
    CHECK(s.wait().code() == CORO_ESTALE);
    CHECK(s.cancel().code() == CORO_ESTALE);
}

static void test_typed_cells(void) {
    corocc::runtime rt(2);
    corocc::scope s(rt);
    corocc::cell<int> a;
    corocc::cell<int> b;
    CHECK(!a.valid());
    CHECK(!a.ready());
    CHECK(a.peek() == NULL);

    CHECK(s.spawn(a, [&rt]() -> int {
        (void)rt.sleep_for(std::chrono::milliseconds(30));
        return 40 + 2;
    }).ok());
    CHECK(a.valid());        /* bound on success */
    CHECK(!a.ready());       /* the slow child has not finished yet */
    CHECK(s.spawn(b, [](int x) -> int { return x * 2; }, 21).ok());

    CHECK(s.wait().ok());
    CHECK(a.ready() && b.ready());
    CHECK(a.status().ok());
    CHECK(a.exception() == std::exception_ptr());
    CHECK(*a.peek() == 42);
    CHECK(a.get() == 42);
    int v = 0;
    CHECK(b.try_get(v) && v == 42);

    CHECK(s.destroy().ok());
    /* The cells outlive the scope: reads go to the wrapper snapshot. */
    CHECK(a.ready());
    CHECK(*b.peek() == 42);
    CHECK(a.status().ok());
}

static void test_aggregate_first_non_ok(void) {
    corocc::runtime rt(2);
    corocc::scope s(rt);
    g_runs.store(0);
    CHECK(s.spawn([]() { g_runs.fetch_add(1); }).ok());
    CHECK(s.spawn([]() -> coro_status { return CORO_OK; }).ok());
    CHECK(s.spawn([]() -> coro_status { return CORO_ESTALE; }).ok());
    const corocc::status agg = s.wait();
    CHECK(agg.code() == CORO_ESTALE);
    CHECK(g_runs.load() == 1);
    CHECK(s.first_exception() == std::exception_ptr());
    CHECK(s.destroy().ok());
}

static void test_first_error_timeliness_pin(void) {
    /* Pin 1 (A7/P1-1): a void child that throws must drive
     * wait_first_error's early exit and cancel the long sleeper. With the
     * cell_out=NULL or code=OK regression this degrades to the full 2 s
     * sleep, so the elapsed bound is the anchor. */
    corocc::runtime rt(2);
    corocc::scope s(rt);
    CHECK(s.spawn([]() { throw my_problem(); }).ok());
    CHECK(s.spawn([&rt]() {
        (void)rt.sleep_for(std::chrono::seconds(2));
    }).ok());

    const long long t0 = static_cast<long long>(coro_monotonic_ns());
    const corocc::status agg = s.wait_first_error();
    const long long elapsed = static_cast<long long>(coro_monotonic_ns()) - t0;

    CHECK(agg.code() == CORO_ECANCELED); /* the abnormal-end marker */
    CHECK(s.first_exception() != std::exception_ptr());
    CHECK(elapsed < 1000000000LL);       /* early exit, not the 2 s drain */

    bool caught = false;
    try {
        std::rethrow_exception(s.first_exception());
    } catch (const my_problem&) {
        caught = true;
    }
    CHECK(caught);
    CHECK(s.destroy().ok());
}

static void test_typed_cell_exception(void) {
    corocc::runtime rt(2);
    corocc::scope s(rt);
    corocc::cell<int> bad;
    CHECK(s.spawn(bad, []() -> int { throw my_problem(); }).ok());
    const corocc::status agg = s.wait();
    CHECK(agg.code() == CORO_ECANCELED);
    CHECK(bad.ready());
    CHECK(bad.status().code() == CORO_ECANCELED);
    CHECK(bad.exception() != std::exception_ptr());
    bool caught = false;
    try {
        (void)bad.get();
    } catch (const my_problem&) {
        caught = true;
    }
    CHECK(caught);
    CHECK(s.first_exception() != std::exception_ptr());
    CHECK(s.destroy().ok());
}

static void test_cancel_vs_exception_disambiguation(void) {
    corocc::runtime rt(2);
    corocc::scope s(rt);
    /* A child that observes the cancel and propagates it: the aggregate is
     * ECANCELED with NO exception -- meaning (1) in the three-way split. */
    CHECK(s.spawn([&rt]() -> coro_status {
        return rt.sleep_for(std::chrono::seconds(30)).code() == CORO_ECANCELED
                   ? CORO_ECANCELED
                   : CORO_OK;
    }).ok());
    CHECK(s.cancel().ok()); /* idempotent */
    CHECK(s.cancel().ok());
    CHECK(s.cancel_requested());
    const corocc::status agg = s.wait();
    CHECK(agg.code() == CORO_ECANCELED);
    CHECK(s.first_exception() == std::exception_ptr());
    CHECK(s.destroy().ok());
}

static void test_wait_for_timeout_priority(void) {
    corocc::runtime rt(2);
    corocc::scope s(rt);
    /* A void child that ignores the cancellation: the only ECANCELED is the
     * waiter's own deadline (meaning (3)), and the exception channel is
     * empty -- the documented disambiguation. */
    CHECK(s.spawn([&rt]() {
        (void)rt.sleep_for(std::chrono::seconds(30));
    }).ok());
    const corocc::status st = s.wait_for(std::chrono::milliseconds(30));
    CHECK(st.code() == CORO_ECANCELED);
    CHECK(s.first_exception() == std::exception_ptr());
    /* The aggregate settled by the frozen drain is OK (the child completed
     * normally after its cancel). */
    CHECK(s.status().ok());
    CHECK(s.destroy().ok());
}

static void test_wait_first_done_followup(void) {
    corocc::runtime rt(2);
    corocc::scope s(rt);
    CHECK(s.spawn([]() -> coro_status { return CORO_ESTALE; }).ok());
    CHECK(s.spawn([&rt]() {
        (void)rt.sleep_for(std::chrono::seconds(30));
    }).ok());
    corocc::status first(CORO_OK);
    CHECK(s.wait_first_done(first).ok());
    CHECK(first.code() == CORO_ESTALE);
    /* The scope stays alive: destroy without the follow-up is EBUSY. */
    CHECK(s.destroy().code() == CORO_EBUSY);
    CHECK(s.cancel().ok());      /* the mandatory follow-up (cancel form) */
    const corocc::status agg = s.wait();
    CHECK(agg.code() == CORO_ESTALE);
    CHECK(s.destroy().ok());
}

static void test_wait_immune_to_own_cancel(void) {
    corocc::runtime rt(2);
    corocc::scope s(rt);
    CHECK(s.spawn([&rt]() {
        (void)rt.sleep_for(std::chrono::milliseconds(80));
    }).ok());
    std::atomic<int> finished(0);
    corocc::task<void> waiter;
    CHECK(rt.spawn_h(waiter, [&s, &finished]() {
        (void)s.wait();          /* parks on the scope event chain */
        finished.store(1);
    }).ok());

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(waiter.cancel().ok()); /* the waiter's own cancellation */
    CHECK(waiter.wait().ok());   /* immune: it still waited for live==0 */
    CHECK(finished.load() == 1);
    CHECK(s.status().ok());
    CHECK(s.destroy().ok());
}

static void test_concurrent_spawn(void) {
    corocc::runtime rt(4);
    corocc::scope s(rt);
    std::atomic<int> total(0);
    std::atomic<int> spawned(0);

    std::thread threads[kSpawnThreads];
    for (int t = 0; t < kSpawnThreads; ++t) {
        threads[t] = std::thread([&s, &total, &spawned]() {
            for (int i = 0; i < kSpawnPer; ++i) {
                const corocc::status st =
                    s.spawn([&total]() { total.fetch_add(1); });
                if (st.ok()) spawned.fetch_add(1);
            }
        });
    }
    for (int t = 0; t < kSpawnThreads; ++t) threads[t].join();

    const corocc::status agg = s.wait();
    CHECK(agg.ok());
    CHECK(spawned.load() == kSpawnThreads * kSpawnPer);
    CHECK(total.load() == kSpawnThreads * kSpawnPer);
    CHECK(s.destroy().ok());
}

static void test_metadata_oom(void) {
    corocc::runtime rt(2);
    corocc::scope s(rt);
    corocc::cell<int> out;

    /* First allocation: the shared child state. */
    g_new_countdown = 0;
    corocc::status st = s.spawn(out, []() -> int { return 1; });
    g_new_countdown = -1;
    CHECK(st.code() == CORO_ENOMEM);
    CHECK(!out.valid());

    /* Second allocation: the control block. */
    g_new_countdown = 1;
    st = s.spawn(out, []() -> int { return 2; });
    g_new_countdown = -1;
    CHECK(st.code() == CORO_ENOMEM);
    CHECK(!out.valid());

    /* Force a children-table growth: with the two per-spawn metadata
     * allocations passing and the next allocation failing, the spawn that
     * needs to grow the vector must report ENOMEM without reaching C. The
     * growth point depends on the standard library's growth policy (2x on
     * libstdc++, 1.5x on MSVC), so the loop walks until it is hit. */
    std::atomic<int> ran(0);
    int accepted = 0;
    corocc::status grow_st(CORO_OK);
    for (int i = 0; i < 64; ++i) {
        g_new_countdown = 2;
        grow_st = s.spawn([&ran]() { ran.fetch_add(1); });
        g_new_countdown = -1;
        if (grow_st.code() == CORO_ENOMEM) break;
        CHECK(grow_st.ok());
        ++accepted;
    }
    CHECK(grow_st.code() == CORO_ENOMEM);
    {
        g_new_countdown = 2;
        st = s.spawn(out, []() -> int { return 3; });
        g_new_countdown = -1;
        /* The typed variant hits the same growth gate. The table may have
         * just grown (capacity spare), in which case this spawn succeeds and
         * nothing was injected: accept both shapes, never a half state. */
        CHECK(st.ok() || st.code() == CORO_ENOMEM);
        if (st.failed()) {
            CHECK(!out.valid());
        }
    }
    CHECK(s.wait().ok());
    CHECK(ran.load() == accepted); /* the refused child never registered */
    CHECK(s.destroy().ok());

    /* The scope and the runtime stay usable. */
    corocc::scope s2(rt);
    CHECK(s2.spawn(out, []() -> int { return 4; }).ok());
    CHECK(s2.wait().ok());
    CHECK(out.get() == 4);
    CHECK(s2.destroy().ok());
}

static void test_chrono_conversion_boundaries(void) {
    /* The ceil/clamp/saturation rules (design v1.1 #6): negative and zero
     * clamp to 0 (the C check-once deadline), sub-unit requests ceil to the
     * next unit, huge requests saturate instead of wrapping. */
    using corocc::detail::duration_to_ms_ceil;
    using corocc::detail::duration_to_ns_ceil;

    CHECK(duration_to_ms_ceil(std::chrono::nanoseconds(-5)) == 0u);
    CHECK(duration_to_ms_ceil(std::chrono::nanoseconds(0)) == 0u);
    CHECK(duration_to_ms_ceil(std::chrono::nanoseconds(999999)) == 1u);
    CHECK(duration_to_ms_ceil(std::chrono::microseconds(1000)) == 1u);
    CHECK(duration_to_ms_ceil(std::chrono::milliseconds(5)) == 5u);
    CHECK(duration_to_ms_ceil(std::chrono::hours(1000)) == 3600000000ULL);
    /* std::chrono::hours' rep is implementation-defined (int on MSVC): use
     * an explicit long-long 3600-ratio period for the saturation probes. */
    typedef std::chrono::duration<long long, std::ratio<3600> > big_hours;
    CHECK(duration_to_ms_ceil(big_hours(1LL << 42)) == 0x7fffffffffffffffULL);
    CHECK(duration_to_ns_ceil(std::chrono::nanoseconds(1)) == 1u);
    CHECK(duration_to_ns_ceil(std::chrono::nanoseconds(-1)) == 0u);
    CHECK(duration_to_ns_ceil(std::chrono::duration<double>(0.999e-9)) == 1u);
    CHECK(duration_to_ns_ceil(big_hours(1LL << 40)) == 0x7fffffffffffffffULL);

    /* Functional form: an empty scope drains before any deadline; an
     * expired time point becomes a zero deadline; a clamped negative
     * duration drives the timeout path (the deadline fires first). */
    corocc::runtime rt(2);
    corocc::scope s(rt);
    CHECK(s.wait_for(std::chrono::nanoseconds(0)).ok());
    corocc::scope s2(rt);
    CHECK(s2
              .wait_until(corocc::monotonic_clock::now() -
                          std::chrono::seconds(1))
              .ok());
    corocc::scope s3(rt);
    CHECK(s3.spawn([&rt]() -> coro_status {
        return rt.sleep_for(std::chrono::seconds(30)).ok() ? CORO_OK
                                                           : CORO_ECANCELED;
    }).ok());
    const corocc::status to = s3.wait_for(std::chrono::nanoseconds(-1000));
    CHECK(to.code() == CORO_ECANCELED); /* clamp -> 0 -> deadline first */
    CHECK(s3.destroy().ok());
}

/* ---- F-2 pin (audit ruling ii, scope typed face): a std::bad_alloc
 * thrown while the wrapper stores the user's callable normalises to
 * CORO_ENOMEM and the target cell stays untouched. */
struct sc_ba_move_functor {
    int hops;
    sc_ba_move_functor() : hops(0) {}
    sc_ba_move_functor(const sc_ba_move_functor& o) : hops(o.hops) {}
    sc_ba_move_functor(sc_ba_move_functor&& o) : hops(o.hops + 1) {
        if (hops >= 1) throw std::bad_alloc();
    }
    int operator()() const { return 3; }
};

static void test_typed_bad_alloc_move_normalised(void) {
    corocc::runtime rt(2);
    corocc::scope s(rt);
    corocc::cell<int> c;
    sc_ba_move_functor m;
    bool thrown = false;
    corocc::status st = CORO_OK;
    try {
        st = s.spawn(c, m);
    } catch (const std::bad_alloc&) {
        thrown = true;
    }
    CHECK(!thrown);
    CHECK(st.code() == CORO_ENOMEM);
    CHECK(!c.valid());
    CHECK(rt.pending() == 0u);
    CHECK(s.destroy().ok());
}

static void run_tests(void) {
    test_scope_state_machine();
    test_typed_cells();
    test_aggregate_first_non_ok();
    test_first_error_timeliness_pin();
    test_typed_cell_exception();
    test_cancel_vs_exception_disambiguation();
    test_wait_for_timeout_priority();
    test_wait_first_done_followup();
    test_wait_immune_to_own_cancel();
    test_concurrent_spawn();
    test_metadata_oom();
    test_chrono_conversion_boundaries();
    test_typed_bad_alloc_move_normalised();
}
TEST_MAIN()
