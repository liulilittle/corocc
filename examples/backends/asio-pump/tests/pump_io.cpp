// corocc asio-pump example -- real socket I/O driven by the pump.
//
// A coroutine starts an asynchronous write/read on a loopback socket and parks
// on a runtime semaphore; the completion handler posts that semaphore, which
// wakes the coroutine through the driver doorbell and the io loop pumps it
// again. The non-coroutine echo peer echoes what it reads (the shared harness).
#include <corocc/boost/asio_pump.hpp>

#include "harness.h"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <memory>

namespace asio = boost::asio;
using corocc::asio_pump::pump_runner;

namespace {

struct io_state {
    coro_rt_t* rt;
    asio::ip::tcp::socket* sock;
    coro_rt_sem_t sem;
    boost::system::error_code ec;
    std::size_t n;
    std::array<char, 12> out;
    std::array<char, 12> in;
    std::atomic<int> steps;
    std::atomic<int> rc;  // 0 = ok
};

void io_entry(void* arg) {
    io_state* s = static_cast<io_state*>(arg);

    s->ec.clear();
    s->n = 0;
    asio::async_write(*s->sock, asio::buffer(s->out.data(), s->out.size()),
                      [s](const boost::system::error_code& ec, std::size_t n) {
                          s->ec = ec;
                          s->n = n;
                          (void)coro_rt_sem_post(s->rt, &s->sem);
                      });
    (void)coro_rt_sem_wait(s->rt, &s->sem);
    if (s->ec || s->n != s->out.size()) {
        s->rc.store(1);
        return;
    }
    s->steps.fetch_add(1);

    s->ec.clear();
    s->n = 0;
    asio::async_read(*s->sock, asio::buffer(s->in.data(), s->in.size()),
                     [s](const boost::system::error_code& ec, std::size_t n) {
                         s->ec = ec;
                         s->n = n;
                         (void)coro_rt_sem_post(s->rt, &s->sem);
                     });
    (void)coro_rt_sem_wait(s->rt, &s->sem);
    if (s->ec || s->n != s->in.size()) {
        s->rc.store(2);
        return;
    }
    if (std::memcmp(s->in.data(), s->out.data(), s->out.size()) != 0) {
        s->rc.store(3);
        return;
    }
    s->steps.fetch_add(1);
    s->rc.store(0);

    boost::system::error_code close_ec;
    s->sock->close(close_ec);
}

}  // namespace

int main() {
    asio::io_context ioc;

    asio::ip::tcp::socket client(ioc);
    asio::ip::tcp::socket peer(ioc);
    if (!make_loopback_pair(ioc, client, peer)) {
        std::fprintf(stderr, "loopback pair setup failed\n");
        return 1;
    }
    std::shared_ptr<echo_peer> echo = std::make_shared<echo_peer>(std::move(peer));
    echo->start();

    pump_runner runner(ioc);
    CHECK(runner.ok());

    io_state s;
    s.rt = runner.rt();
    s.sock = &client;
    s.n = 0;
    s.steps.store(0);
    s.rc.store(-1);
    std::memcpy(s.out.data(), "corocc-pump!", s.out.size());
    s.in.fill('\0');
    coro_rt_sem_init(&s.sem, 0);

    CHECK(coro_rt_spawn(runner.rt(), NULL, io_entry, &s) == CORO_OK);

    ioc.run();

    CHECK(s.rc.load() == 0);
    CHECK(s.steps.load() == 2);
    CHECK(echo->received() > 0);
    CHECK(coro_rt_pending(runner.rt()) == 0);

    runner.shutdown();
    return test_finish("pump_io");
}
