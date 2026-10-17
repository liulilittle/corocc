// corocc asio-host example -- coexistence / ODR pin, TU A.
//
// This translation unit includes the host-loop header FIRST and the runtime
// header SECOND; host_coexist_b.cpp does the opposite. Linking both into one
// binary proves the two integrations coexist (same library, disjoint
// namespaces, no duplicate symbols), and the shared template instantiation
// pinned below proves the header's vague-linkage entities collapse to one
// address across translation units (an ODR violation would either fail to
// link or produce two different addresses).
//
// The binary also exercises both sides at link level: the runtime handle is
// created/destroyed (coro_rt_* symbols) and one host-loop round trip runs.

#include <corocc/boost/asio_host.hpp>
#include <corocc/boost/asio.hpp>

#include "harness.h"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include <cstring>

namespace asio = boost::asio;

typedef asio::io_context::executor_type executor_type;
typedef asio::strand<executor_type> strand_type;
typedef corocc::asio_host::yield_context<strand_type> yield_type;

typedef std::size_t (*wait_fn)(corocc::asio_host::detail::coro_state<strand_type>*,
                               corocc::asio_host::detail::op_latch&,
                               boost::system::error_code&);

wait_fn op_wait_addr_a() {
    return &corocc::asio_host::detail::op_wait<strand_type>;
}

int sizeof_yield_a() {
    return static_cast<int>(sizeof(yield_type));
}

// from TU B
wait_fn op_wait_addr_b();
int sizeof_yield_b();

int main() {
    // The ODR pin: one instantiation, one address, both TUs.
    CHECK(op_wait_addr_a() == op_wait_addr_b());
    CHECK(sizeof_yield_a() == sizeof_yield_b());

    // The runtime side of the same binary still works (link-level proof).
    coro_rt_t* rt = coro_rt_create(1);
    CHECK(rt != 0);
    if (rt) {
        coro_rt_destroy(rt);
    }

    // A host-loop round trip in the same binary.
    asio::io_context ioc;
    strand_type strand(ioc.get_executor());
    asio::ip::tcp::socket client(ioc);
    asio::ip::tcp::socket peer(ioc);
    CHECK(make_loopback_pair(ioc, client, peer));

    std::shared_ptr<echo_peer> echo = std::make_shared<echo_peer>(std::move(peer));
    echo->start();

    std::atomic<int> rc(-1);
    CHECK(corocc::asio_host::spawn(strand, [&](yield_type& y) {
        char out[4] = {'o', 'd', 'r', '!'};
        char in[4] = {0, 0, 0, 0};
        boost::system::error_code ec;
        const std::size_t w = corocc::asio_host::async_write(y, client, asio::buffer(out, 4), ec);
        if (ec || w != 4) {
            rc.store(1);
            return;
        }
        const std::size_t n = corocc::asio_host::async_read(y, client, asio::buffer(in, 4), ec);
        if (ec || n != 4 || std::memcmp(in, out, 4) != 0) {
            rc.store(2);
            return;
        }
        boost::system::error_code cec;
        client.close(cec);
        rc.store(0);
    }) == CORO_OK);

    ioc.run();
    CHECK(rc.load() == 0);

    return test_finish("host_coexist");
}
