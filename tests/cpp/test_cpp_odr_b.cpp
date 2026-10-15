/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * tests/cpp/test_cpp_odr_b.cpp -- the ODR partner TU: the C headers come
 * FIRST here (the reverse of test_cpp_odr.cpp's include order), then
 * coro.hpp. It exports the addresses and compile-time facts the main TU
 * compares against, so a duplicated inline static, a divergent class
 * definition (e.g. a mixed COROCC_CPP_NO_EXCEPTIONS) or a different C++
 * standard would break the link or the comparisons.
 */
#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_sched.h>
#include <corocc/coro_scope.h>

#include <corocc/coro.hpp>

#include <cstddef>
#include <atomic>
#include <type_traits>
#include <utility>

std::atomic<corocc::detached_exception_handler>* odr_partner_handler_addr() {
    return &corocc::detail::detached_handler_slot();
}

std::size_t odr_partner_sizeof_status() {
    return sizeof(corocc::status);
}

std::size_t odr_partner_sizeof_attrs() {
    return sizeof(corocc::attrs);
}

int odr_partner_noexcept_pins() {
    const int wait_noexcept =
        noexcept(std::declval<corocc::task<int>&>().wait()) ? 1 : 0;
    const int join_noexcept =
        noexcept(std::declval<corocc::task<int>&>().join()) ? 1 : 0;
    const int runtime_noexcept =
        noexcept(std::declval<corocc::runtime&>().wait_all()) ? 1 : 0;
    const int scope_wait_noexcept =
        noexcept(std::declval<corocc::scope&>().wait()) ? 1 : 0;
    const int status_noexcept = noexcept(std::declval<corocc::status&>().ok()) ? 1 : 0;
    const int move_only =
        std::is_copy_constructible<corocc::task<int> >::value ? 0 : 1;
    const int value_types =
        std::is_copy_constructible<corocc::attrs>::value ? 1 : 0;
    /* Bitwise: every operand is a compile-time constant, and clang warns
     * about && on constants. */
    return (wait_noexcept & join_noexcept & runtime_noexcept &
            scope_wait_noexcept & status_noexcept & move_only & value_types)
               ? 1
               : 0;
}

corocc::status odr_partner_make_status() {
    return corocc::status(CORO_ESTALE);
}
