// corocc asio-host example -- coexistence / ODR pin, TU B.
//
// Opposite include order of host_coexist.cpp: the runtime header first, the
// host-loop header second.

#include <corocc/boost/asio.hpp>
#include <corocc/boost/asio_host.hpp>

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

namespace asio = boost::asio;

typedef asio::io_context::executor_type executor_type;
typedef asio::strand<executor_type> strand_type;
typedef corocc::asio_host::yield_context<strand_type> yield_type;

typedef std::size_t (*wait_fn)(corocc::asio_host::detail::coro_state<strand_type>*,
                               corocc::asio_host::detail::op_latch&,
                               boost::system::error_code&);

wait_fn op_wait_addr_b() {
    return &corocc::asio_host::detail::op_wait<strand_type>;
}

int sizeof_yield_b() {
    return static_cast<int>(sizeof(yield_type));
}
