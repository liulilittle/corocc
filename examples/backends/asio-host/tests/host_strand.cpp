// corocc asio-host example -- multi-threaded io_context, one strand per
// connection, 32 concurrent coroutines.
//
// Every coroutine records the io thread it runs on at each resume point and
// asserts that it is inside its own connection's strand (that is the strand
// inheritance rule: the completion handlers inherit the strand that spawn was
// given, so a resume can never race the coroutine's own frame).
//
// One coroutine also spawns a nested coroutine onto the same strand while
// holding it, proving that spawn-from-inside-a-coroutine cannot deadlock.

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
    const int kClients = 32;
    const int kRoundTrips = 10;
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
    std::atomic<int> nested_ran(0);

    for (int i = 0; i < kClients; ++i) {
        CHECK(corocc::asio_host::spawn(strands[static_cast<std::size_t>(i)], [&, i](yield_type& y) {
            int rc = 0;
            // The entry runs inside the strand's handler on the spawning
            // thread, so this query is reliable here. It is deliberately NOT
            // repeated after the operations: once a coroutine has migrated
            // between io threads, asio's thread_local bookkeeping
            // (running_in_this_thread) is not meaningful inside it -- the
            // library documents that user code must not depend on
            // thread_local inside a coroutine. The serialization the mode
            // relies on does not depend on thread_local: it is the strand's
            // mutex/queue state, and the host_basic/host_single checks cover
            // the context assertions.
            CHECK(y.executor().running_in_this_thread());

            for (int r = 0; r < kRoundTrips && rc == 0; ++r) {
                char out[4];
                out[0] = static_cast<char>(i);
                out[1] = static_cast<char>(r);
                out[2] = 'A';
                out[3] = 'B';
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

                if (i == 0 && r == 0) {
                    // Nested spawn onto the same strand while the coroutine
                    // holds it; it must run later, not deadlock.
                    CHECK(corocc::asio_host::spawn(strands[0], [&](yield_type&) {
                        nested_ran.store(1);
                    }) == CORO_OK);
                }
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
        std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while ((done.load() < kClients || nested_ran.load() == 0) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK(done.load() == kClients);
    CHECK(nested_ran.load() == 1);
    CHECK(bad.load() == 0);
    CHECK(ops.load() == static_cast<long long>(kClients) * kRoundTrips * 2);

    ioc.stop();
    for (std::size_t t = 0; t < threads.size(); ++t) {
        threads[t].join();
    }

    return test_finish("host_strand");
}
