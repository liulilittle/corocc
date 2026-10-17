// corocc asio-host example -- awaitable bridge (OS-thread blocking form).
//
// await_read / await_read_some / await_write / await_write_some give a plain
// OS thread the same blocking shape the host-loop coroutine ops give a
// coroutine, by initiating the operation and waiting on a condition variable.
//
// Precondition under test: the calling thread must NOT be a thread that runs
// the stream's io_context. Here the io_context is driven by a separate thread
// and the main thread waits.

#include <corocc/boost/asio_host.hpp>

#include "harness.h"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <cstring>
#include <memory>
#include <thread>

namespace asio = boost::asio;

int main() {
    asio::io_context ioc;
    asio::executor_work_guard<asio::io_context::executor_type> guard =
        asio::make_work_guard(ioc.get_executor());

    asio::ip::tcp::socket client(ioc);
    asio::ip::tcp::socket peer(ioc);
    CHECK(make_loopback_pair(ioc, client, peer));

    std::shared_ptr<echo_peer> echo = std::make_shared<echo_peer>(std::move(peer));
    echo->start();

    std::thread io_thread([&ioc] { ioc.run(); });

    std::array<char, 16> out;
    std::memcpy(out.data(), "awaitable-bridge", 16);
    std::array<char, 16> in;
    in.fill('\0');

    boost::system::error_code ec;
    std::size_t n = corocc::asio_host::await_write(client, asio::buffer(out), ec);
    CHECK(!ec && n == out.size());

    n = corocc::asio_host::await_read(client, asio::buffer(in), ec);
    CHECK(!ec && n == in.size());
    CHECK(std::memcmp(in.data(), out.data(), in.size()) == 0);

    // eof path: a second pair whose peer is closed before any operation is
    // initiated. Closing happens before the read is armed, so no concurrent
    // use of the socket exists.
    {
        asio::ip::tcp::socket client2(ioc);
        asio::ip::tcp::socket peer2(ioc);
        CHECK(make_loopback_pair(ioc, client2, peer2));
        peer2.close();

        std::array<char, 8> buf;
        n = corocc::asio_host::await_read(client2, asio::buffer(buf), ec);
        CHECK(ec == asio::error::eof && n == 0);
    }

    // await_read_some / await_write_some on the first pair: the echo peer
    // returns exactly what it reads.
    {
        std::array<char, 3> msg = {{'e', 'n', 'd'}};
        n = corocc::asio_host::await_write_some(client, asio::buffer(msg), ec);
        CHECK(!ec && n == msg.size());

        std::array<char, 3> back;
        back.fill('\0');
        std::size_t acc = 0;
        while (acc < back.size()) {
            n = corocc::asio_host::await_read_some(
                client, asio::buffer(back.data() + acc, back.size() - acc), ec);
            if (ec || n == 0) {
                break;
            }
            acc += n;
        }
        CHECK(!ec && acc == back.size());
        CHECK(std::memcmp(back.data(), msg.data(), msg.size()) == 0);
    }

    guard.reset();
    ioc.stop();
    io_thread.join();

    return test_finish("host_awaitable");
}
