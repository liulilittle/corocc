// corocc asio-host example -- abandoned-coroutine lifecycle guard.
//
// The detached design has one lifecycle contract: drain run() (or end the
// outstanding I/O) before destroying the io_context. This test exercises the
// guard that keeps a violated contract leak-free: coroutines that are still
// pending (their start never ran) or parked (their I/O never completed) are
// reclaimed during execution_context shutdown by the integration's service.
// Run under AddressSanitizer/LeakSanitizer (as the ASan ctest build does),
// "the guard worked" is asserted by the run being leak-free; without a
// sanitizer the test asserts the stronger property that nothing is touched
// after the io_context is gone (a fresh io_context still works).
//
// Note the last phase: the guard must not disturb normal operation.

#include <corocc/boost/asio_host.hpp>

#include "harness.h"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace asio = boost::asio;

typedef asio::io_context::executor_type executor_type;
typedef asio::strand<executor_type> strand_type;
typedef corocc::asio_host::yield_context<strand_type> yield_type;

int main() {
    const int kCoroutines = 50;

    // a) Pending starts: the io_context is destroyed without ever running, so
    //    50 start handlers (and their coroutine stacks) are still owed.
    {
        asio::io_context ioc;
        strand_type strand(ioc.get_executor());
        for (int i = 0; i < kCoroutines; ++i) {
            CHECK(corocc::asio_host::spawn(strand, [](yield_type&) {}) == CORO_OK);
        }
    }

    // b) Parked coroutines: each is started and suspends in a read on a
    //    socket whose peer never writes, then the io_context is stopped (the
    //    contract's "end the I/O" step is deliberately skipped) and destroyed
    //    while all 50 are still parked.
    {
        asio::io_context ioc;
        std::vector<strand_type> strands;
        std::vector<asio::ip::tcp::socket> clients;
        std::vector<asio::ip::tcp::socket> peers;
        strands.reserve(kCoroutines);
        clients.reserve(kCoroutines);
        peers.reserve(kCoroutines);

        for (int i = 0; i < kCoroutines; ++i) {
            clients.emplace_back(ioc);
            asio::ip::tcp::socket peer(ioc);
            if (!make_loopback_pair(ioc, clients.back(), peer)) {
                std::fprintf(stderr, "loopback pair %d setup failed\n", i);
                return 1;
            }
            peers.push_back(std::move(peer));
            strands.push_back(strand_type(ioc.get_executor()));
        }

        std::atomic<int> parked(0);
        for (int i = 0; i < kCoroutines; ++i) {
            asio::ip::tcp::socket* sock = &clients[static_cast<std::size_t>(i)];
            CHECK(corocc::asio_host::spawn(strands[static_cast<std::size_t>(i)],
                [&, sock](yield_type& y) {
                    parked.fetch_add(1);
                    char buf[4];
                    boost::system::error_code ec;
                    (void)corocc::asio_host::async_read_some(
                        y, *sock, asio::buffer(buf, 4), ec);
                }) == CORO_OK);
        }

        std::vector<std::thread> threads;
        threads.push_back(std::thread([&ioc] { ioc.run(); }));
        threads.push_back(std::thread([&ioc] { ioc.run(); }));

        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (parked.load() < kCoroutines &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(parked.load() == kCoroutines);

        ioc.stop();
        for (std::size_t t = 0; t < threads.size(); ++t) {
            threads[t].join();
        }
    }

    // c) The guard must leave the process fully usable: a fresh io_context
    //    runs a normal round trip afterwards.
    {
        asio::io_context ioc;
        strand_type strand(ioc.get_executor());
        asio::ip::tcp::socket client(ioc);
        asio::ip::tcp::socket peer(ioc);
        CHECK(make_loopback_pair(ioc, client, peer));
        std::shared_ptr<echo_peer> echo = std::make_shared<echo_peer>(std::move(peer));
        echo->start();

        std::atomic<int> rc(-1);
        CHECK(corocc::asio_host::spawn(strand, [&](yield_type& y) {
            char out[4] = {'g', 'u', 'a', 'r'};
            char in[4] = {0, 0, 0, 0};
            boost::system::error_code ec;
            const std::size_t w = corocc::asio_host::async_write(
                y, client, asio::buffer(out, 4), ec);
            if (ec || w != 4) {
                rc.store(1);
                return;
            }
            const std::size_t n = corocc::asio_host::async_read(
                y, client, asio::buffer(in, 4), ec);
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
    }

    return test_finish("host_abandon");
}
