/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * tests/cpp/test_cpp_odr.cpp -- the two-TU ODR pin (A7 pin 2 / P2-2). This
 * TU includes coro.hpp before the C headers; its partner
 * test_cpp_odr_b.cpp includes the C headers first and coro.hpp second. The
 * two translation units are linked into one binary, so the header's single
 * shared state (the detached-handler slot) must have ONE address, the type
 * pins must agree, and the handler set from either side must be visible to
 * the other. A mixed C++ standard or a duplicated inline static would fail
 * here or at link time.
 */
#include "../harness.h"

#include <corocc/coro.hpp>

#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_sched.h>
#include <corocc/coro_scope.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <type_traits>

/* The partner TU's exports (test_cpp_odr_b.cpp). */
std::atomic<corocc::detached_exception_handler>* odr_partner_handler_addr();
std::size_t odr_partner_sizeof_status();
std::size_t odr_partner_sizeof_attrs();
int odr_partner_noexcept_pins();
corocc::status odr_partner_make_status();

namespace {

std::atomic<int> g_odr_handler_calls(0);

void odr_probe_handler(std::exception_ptr) {
    g_odr_handler_calls.fetch_add(1);
}

} /* namespace */

static void test_handler_singleton_across_tus(void) {
    /* One instance per binary: both TUs must resolve to the same slot. */
    CHECK(&corocc::detail::detached_handler_slot() == odr_partner_handler_addr());

    /* Setting through this TU is visible through the partner and vice
     * versa; the slot is the same object. */
    corocc::detached_exception_handler old =
        corocc::set_detached_exception_handler(&odr_probe_handler);
    CHECK(odr_partner_handler_addr()->load() == &odr_probe_handler);
    corocc::set_detached_exception_handler(old);
    CHECK(&corocc::detail::detached_handler_slot() == odr_partner_handler_addr());
}

static void test_type_pins_agree_across_tus(void) {
    CHECK(odr_partner_sizeof_status() == sizeof(corocc::status));
    CHECK(odr_partner_sizeof_attrs() == sizeof(corocc::attrs));
    CHECK(odr_partner_noexcept_pins() == 1);
    CHECK(odr_partner_make_status().code() == CORO_ESTALE);

    static_assert(sizeof(corocc::status) == sizeof(coro_status),
                  "status must stay layout-compatible (this TU)");
    static_assert(noexcept(std::declval<corocc::task<int>&>().wait()),
                  "task::wait must be noexcept (this TU)");
    static_assert(noexcept(std::declval<corocc::runtime&>().wait_all()),
                  "runtime::wait_all must be noexcept (this TU)");
    static_assert(!std::is_copy_constructible<corocc::task<int> >::value,
                  "task must stay move-only (this TU)");
}

static void test_scope_illegal_move_is_refused(void) {
    /* scope is immovable (its C record points back at the object). */
    static_assert(!std::is_move_constructible<corocc::scope>::value,
                  "scope must not be movable");
    static_assert(!std::is_copy_constructible<corocc::scope>::value,
                  "scope must not be copyable");
    static_assert(std::is_move_constructible<corocc::coroutine>::value,
                  "coroutine must be movable");
}

static void run_tests(void) {
    test_handler_singleton_across_tus();
    test_type_pins_agree_across_tus();
    test_scope_illegal_move_is_refused();
}
TEST_MAIN()
