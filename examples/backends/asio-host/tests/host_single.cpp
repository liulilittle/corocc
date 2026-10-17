// corocc asio-host example -- single io thread.
//
// Covers the manual-switch core of host-loop mode on one io_context thread:
//   spawn -> the entry runs on the io thread -> an operation suspends (the
//   thread returns to run()) -> the completion handler resumes the coroutine
//   inline -> ... -> the entry returns and the stack is reclaimed outside the
//   coroutine.
//
// Also covered: spawn on a plain executor (internally serialized by its own
// strand), a coroutine that never suspends, and a coroutine that throws (the
// exception is caught at the coroutine boundary; no std::terminate).
//
// The test is a plain executable: exit code 0 means every check passed.

#include <corocc/boost/asio_host.hpp>

#include "harness.h"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace asio = boost::asio;

typedef asio::io_context::executor_type executor_type;
typedef asio::strand<executor_type> strand_type;
typedef corocc::asio_host::yield_context<strand_type> yield_type;

namespace {

// A hot classic handler: keeps the io thread demonstrably busy between the
// coroutine's operations. If the io thread ever blocked inside a wait, the
// round trip would never finish and the ticker would starve.
struct ticker {
    asio::io_context* ioc;
    std::atomic<int>* ticks;
    std::atomic<bool>* stop;
    void operator()() const {
        if (stop->load()) {
            return;
        }
        ticks->fetch_add(1);
        asio::post(*ioc, *this);
    }
};

}  // namespace

int main() {
    asio::io_context ioc;
    strand_type strand(ioc.get_executor());

    asio::ip::tcp::socket client(ioc);
    asio::ip::tcp::socket peer(ioc);
    if (!make_loopback_pair(ioc, client, peer)) {
        std::fprintf(stderr, "loopback pair setup failed\n");
        return 1;
    }
    std::shared_ptr<echo_peer> echo = std::make_shared<echo_peer>(std::move(peer));
    echo->start();

    const std::thread::id io_thread = std::this_thread::get_id();

    std::atomic<int> ticks(0);
    std::atomic<bool> stop_ticker(false);
    asio::post(ioc, ticker{&ioc, &ticks, &stop_ticker});

    std::atomic<int> coroutines_done(0);
    std::atomic<int> round_trip_rc(-1);

    // Every coroutine body leaves through this guard; the last one stops the
    // ticker, which is what lets run() drain and return.
    struct finish {
        std::atomic<int>* done;
        std::atomic<bool>* stop;
        ~finish() {
            if (done->fetch_add(1) == 2) {
                stop->store(true);
            }
        }
    };

    // 1) A full round trip on a plain executor (spawn wraps it in a strand).
    CHECK(corocc::asio_host::spawn(ioc.get_executor(), [&](yield_type& y) {
        finish guard{&coroutines_done, &stop_ticker};
        CHECK(std::this_thread::get_id() == io_thread);
        CHECK(y.executor().running_in_this_thread());

        std::array<char, 12> out;
        std::memcpy(out.data(), "corocc-host!", 12);
        std::array<char, 12> in;
        in.fill('\0');

        const int ticks_before = ticks.load();
        boost::system::error_code ec;
        const std::size_t written =
            corocc::asio_host::async_write(y, client, asio::buffer(out), ec);
        if (ec || written != out.size()) {
            round_trip_rc.store(1);
            return;
        }
        const std::size_t got =
            corocc::asio_host::async_read(y, client, asio::buffer(in), ec);
        if (ec || got != in.size()) {
            round_trip_rc.store(2);
            return;
        }
        if (std::memcmp(in.data(), out.data(), out.size()) != 0) {
            round_trip_rc.store(3);
            return;
        }
        CHECK(y.executor().running_in_this_thread());
        CHECK(ticks.load() > ticks_before);  // the io thread kept running while this coroutine was parked

        // The *_some pair on the same connection; the echo peer returns what
        // it reads, so the read side loops until the message is whole.
        std::array<char, 5> msg = {{'h', 'o', 's', 't', '!'}};
        const std::size_t sent =
            corocc::asio_host::async_write_some(y, client, asio::buffer(msg), ec);
        if (ec || sent != msg.size()) {
            round_trip_rc.store(4);
            return;
        }
        std::array<char, 5> back;
        back.fill('\0');
        std::size_t acc = 0;
        while (acc < back.size()) {
            const std::size_t n = corocc::asio_host::async_read_some(
                y, client, asio::buffer(back.data() + acc, back.size() - acc), ec);
            if (ec || n == 0) {
                round_trip_rc.store(5);
                return;
            }
            acc += n;
        }
        if (std::memcmp(back.data(), msg.data(), msg.size()) != 0) {
            round_trip_rc.store(6);
            return;
        }

        boost::system::error_code close_ec;
        client.close(close_ec);  // the peer sees eof and stops echoing
        round_trip_rc.store(0);
    }) == CORO_OK);

    // 2) A coroutine that never suspends: start -> entry -> return -> reclaim.
    std::atomic<bool> ran_no_suspend(false);
    CHECK(corocc::asio_host::spawn(strand, [&](yield_type&) {
        finish guard{&coroutines_done, &stop_ticker};
        ran_no_suspend.store(true);
    }) == CORO_OK);

    // 3) A coroutine that throws: caught at the coroutine boundary, never
    //    reaching std::terminate, never disturbing the io_context.
    std::atomic<bool> thrown(false);
    CHECK(corocc::asio_host::spawn(strand, [&](yield_type&) {
        finish guard{&coroutines_done, &stop_ticker};
        thrown.store(true);
        throw std::runtime_error("asio-host example: deliberate test throw");
    }) == CORO_OK);

    ioc.run();  // returns once the last coroutine, echo handler and ticker are done

    CHECK(round_trip_rc.load() == 0);
    CHECK(ran_no_suspend.load());
    CHECK(thrown.load());

    return test_finish("host_single");
}
