// corocc asio-host example -- 10k operation stress.
//
// 50 connections, 100 write/read round trips each, 4 io threads, one strand
// per connection: 10,000 host-loop operations total. Every byte is verified
// against the echo peer, so the count is only accepted when all operations
// actually completed with the right data.

#include <corocc/boost/asio_host.hpp>

#include "harness.h"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace asio = boost::asio;

typedef asio::io_context::executor_type executor_type;
typedef asio::strand<executor_type> strand_type;
typedef corocc::asio_host::yield_context<strand_type> yield_type;

int main() {
    const int kClients = 50;
    const int kRoundTrips = 100;  // 50 * 100 * 2 == 10,000 operations
    const int kIoThreads = 4;

    asio::io_context ioc;

    std::vector<strand_type> strands;
    std::vector<asio::ip::tcp::socket> clients;
    std::vector<std::shared_ptr<echo_peer> > peers;
    strands.reserve(kClients);
    clients.reserve(kClients);
    peers.reserve(kClients);

    for (int i = 0; i < kClients; ++i) {
        clients.emplace_back(ioc);
        asio::ip::tcp::socket peer(ioc);
        if (!make_loopback_pair(ioc, clients.back(), peer)) {
            std::fprintf(stderr, "loopback pair %d setup failed\n", i);
            return 1;
        }
        strands.emplace_back(ioc.get_executor());
        std::shared_ptr<echo_peer> e = std::make_shared<echo_peer>(std::move(peer));
        peers.push_back(e);
        e->start();
    }

    std::atomic<int> done(0);
    std::atomic<int> bad(0);
    std::atomic<long long> ops(0);
    std::atomic<long long> bytes(0);

    for (int i = 0; i < kClients; ++i) {
        CHECK(corocc::asio_host::spawn(strands[static_cast<std::size_t>(i)], [&, i](yield_type& y) {
            (void)y;
            int rc = 0;

            for (int r = 0; r < kRoundTrips && rc == 0; ++r) {
                const char out[4] = {
                    static_cast<char>(i),
                    static_cast<char>(r),
                    static_cast<char>(i ^ r),
                    static_cast<char>(0x5a)
                };
                char in[4] = {0, 0, 0, 0};

                boost::system::error_code ec;
                const std::size_t written = corocc::asio_host::async_write(
                    y, clients[static_cast<std::size_t>(i)], asio::buffer(out, 4), ec);
                if (ec || written != 4) {
                    rc = 1;
                    break;
                }
                const std::size_t got = corocc::asio_host::async_read(
                    y, clients[static_cast<std::size_t>(i)], asio::buffer(in, 4), ec);
                if (ec || got != 4) {
                    rc = 2;
                    break;
                }
                if (std::memcmp(in, out, 4) != 0) {
                    rc = 3;
                    break;
                }
                ops.fetch_add(2);
                bytes.fetch_add(static_cast<long long>(written + got));
            }

            if (rc != 0) {
                bad.fetch_add(1);
            }
            boost::system::error_code close_ec;
            clients[static_cast<std::size_t>(i)].close(close_ec);
            done.fetch_add(1);
        }) == CORO_OK);
    }

    std::vector<std::thread> threads;
    threads.reserve(kIoThreads);
    for (int t = 0; t < kIoThreads; ++t) {
        threads.push_back(std::thread([&ioc] { ioc.run(); }));
    }

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (done.load() < kClients &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK(done.load() == kClients);
    CHECK(bad.load() == 0);
    CHECK(ops.load() == 10000);
    CHECK(bytes.load() == 40000);

    ioc.stop();
    for (std::size_t t = 0; t < threads.size(); ++t) {
        threads[t].join();
    }

    std::printf("host_stress: %lld operations over %d connections on %d io threads\n",
                ops.load(), kClients, kIoThreads);
    return test_finish("host_stress");
}
