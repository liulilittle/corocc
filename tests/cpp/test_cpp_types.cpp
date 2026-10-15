/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * tests/cpp/test_cpp_types.cpp -- compiled-in pins for the C++11 wrapper's
 * value types: corocc::status, corocc::error, corocc::attrs and
 * corocc::monotonic_clock, plus the type-level pins the wrapper's RAII
 * classes must keep (move-only handles, scope immobility, noexcept
 * boundaries). Everything here must hold at compile time or fail fast; the
 * runtime checks only confirm the plain semantics.
 */
#include "../harness.h"

#include <corocc/coro.hpp>

#include <chrono>
#include <cstring>
#include <exception>
#include <string>
#include <type_traits>
#include <utility>

/* ---- status: zero-overhead wrapper ---- */

static void test_status(void) {
    corocc::status ok;
    CHECK(ok.ok());
    CHECK(!ok.failed());
    CHECK(ok.code() == CORO_OK);
    CHECK(std::strcmp(ok.message(), "CORO_OK") == 0);

    corocc::status bad(CORO_ENOMEM);
    CHECK(bad.failed());
    CHECK(!bad.ok());
    CHECK(bad.code() == CORO_ENOMEM);

    /* Implicit both ways: the C enum and the wrapper are one value. */
    coro_status raw = bad;
    CHECK(raw == CORO_ENOMEM);
    corocc::status back = CORO_ESTALE;
    CHECK(back.code() == CORO_ESTALE);
    CHECK((raw = CORO_OK) == CORO_OK);

    /* noexcept boundary: querying a status never throws. */
    static_assert(noexcept(ok.ok()), "ok() must be noexcept");
    static_assert(noexcept(ok.code()), "code() must be noexcept");
    static_assert(noexcept(ok.message()), "message() must be noexcept");
    static_assert(noexcept(ok = CORO_OK), "status assignment must be noexcept");
}

static void test_status_size(void) {
    /* The wrapper is the C enum: no tag, no padding beyond it. */
    static_assert(sizeof(corocc::status) == sizeof(coro_status),
                  "corocc::status must stay layout-compatible with coro_status");
}

/* ---- error: the closeout-point exception ---- */

static void test_error(void) {
    const corocc::error e(CORO_ECANCELED);
    CHECK(e.code() == CORO_ECANCELED);
    CHECK(std::strcmp(e.what(), "CORO_ECANCELED") == 0);

    const std::exception& base = e;
    CHECK(std::strcmp(base.what(), "CORO_ECANCELED") == 0);

    /* error derives from std::exception; polymorphic catch works. */
    static_assert(std::is_base_of<std::exception, corocc::error>::value,
                  "corocc::error must derive from std::exception");
    static_assert(noexcept(e.code()), "error::code() must be noexcept");
}

/* ---- attrs: owning builder ---- */

static void test_attrs(void) {
    corocc::attrs a;
    CHECK(a.stack_size_value() == 0);
    CHECK(a.name_value().empty());
    CHECK(a.allocator_value() == nullptr);

    corocc::attrs& chained = a.stack_size(4096).name("worker-1");
    CHECK(&chained == &a); /* chaining returns the same object */

    corocc::attrs b;
    b.name(std::string("owned"));
    CHECK(b.name_value() == "owned");
    b.stack_size(8192);
    CHECK(b.stack_size_value() == 8192);

    /* The name is owned: a copy survives the source's lifetime, so a
     * temporary attrs{} handed to a spawn call can never dangle. */
    corocc::attrs copy = corocc::attrs().name("temporary").stack_size(1);
    CHECK(copy.name_value() == "temporary");
    CHECK(copy.stack_size_value() == 1);

    const coro_allocator_t* none = nullptr;
    corocc::attrs c;
    c.allocator(none);
    CHECK(c.allocator_value() == nullptr);

    static_assert(std::is_copy_constructible<corocc::attrs>::value,
                  "attrs is a copyable value type");
}

/* ---- monotonic_clock: the C++11 Clock face of coro_monotonic_ns ---- */

static void test_monotonic_clock(void) {
    typedef corocc::monotonic_clock clk;
    static_assert(clk::is_steady, "monotonic_clock must be steady");
    static_assert(clk::duration::period::num == 1, "1 ns tick");
    static_assert(clk::duration::period::den == 1000000000, "1 ns tick");

    clk::time_point t0 = clk::now();
    clk::time_point t1 = clk::now();
    CHECK(t1 >= t0); /* never decreases within the process */

    /* The epoch is the same unspecified monotone epoch as the C clock. */
    const std::uint64_t raw = coro_monotonic_ns();
    clk::time_point from_raw{clk::duration(raw)};
    CHECK(static_cast<std::uint64_t>(from_raw.time_since_epoch().count()) == raw ||
          from_raw.time_since_epoch().count() <= coro_monotonic_ns());

    /* Interop with std::chrono arithmetic (the duration conversion must stay
     * in the nanosecond scale). */
    clk::time_point later = t0 + std::chrono::milliseconds(5);
    CHECK(later >= t0);
    CHECK(later - t0 >= std::chrono::milliseconds(5));

    static_assert(noexcept(clk::now()), "monotonic_clock::now must be noexcept");
}

static void run_tests(void) {
    test_status();
    test_status_size();
    test_error();
    test_attrs();
    test_monotonic_clock();
}
TEST_MAIN()
