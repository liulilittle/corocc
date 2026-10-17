// corocc asio-pump example -- cancellation of a timed park in LOOP mode.
//
// A coroutine sleeps far into the future; a steady_timer fires a cancel, which
// requeues the parked node and the pump observes it at the park's safe point,
// so the sleep returns CORO_ECANCELED instead of its deadline.
#include <corocc/boost/asio_pump.hpp>

#include "harness.h"

#include <boost/system/error_code.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace asio = boost::asio;
using corocc::asio_pump::pump_runner;

namespace {

std::atomic<int> g_rc(-1);

void long_sleeper(void* arg) {
    coro_rt_t* rt = static_cast<coro_rt_t*>(arg);
    coro_status st =
        coro_rt_sleep(rt, pump_runner::now_ns() + 5000000000ull);  // 5 s
    g_rc.store(st == CORO_ECANCELED ? 0 : 1);
}

}  // namespace

int main() {
    asio::io_context ioc;
    pump_runner runner(ioc);
    CHECK(runner.ok());
    coro_rt_t* rt = runner.rt();

    coro_t* h = NULL;
    CHECK(coro_rt_spawn_h(rt, NULL, long_sleeper, rt, &h) == CORO_OK);
    CHECK(h != NULL);

    asio::steady_timer cancel_timer(ioc);
    cancel_timer.expires_after(std::chrono::milliseconds(50));
    cancel_timer.async_wait([rt, h](const boost::system::error_code& ec) {
        if (!ec) {
            (void)coro_rt_cancel(rt, h);
        }
    });

    ioc.run();

    CHECK(g_rc.load() == 0);
    CHECK(coro_rt_pending(rt) == 0);

    coro_status cr = coro_rt_collect(rt, h);
    CHECK(cr == CORO_OK);

    runner.shutdown();
    return test_finish("pump_cancel");
}
