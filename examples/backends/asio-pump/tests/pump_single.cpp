// corocc asio-pump example -- single io thread, LOOP runtime.
//
// The runtime owns no worker threads; an io_context loop drives it by pumping.
// This test spawns several coroutines that yield and sleep, and asserts they
// all finish with the sleep observed close to its deadline.
#include <corocc/boost/asio_pump.hpp>

#include "harness.h"

#include <atomic>
#include <cstdint>

namespace asio = boost::asio;
using corocc::asio_pump::pump_runner;

namespace {

const int kCoros = 4;
const std::uint64_t kSleepNs = 20000000ull;  // 20 ms

std::atomic<int> g_done(0);
std::atomic<int> g_yields(0);

void worker_entry(void* arg) {
    coro_rt_t* rt = static_cast<coro_rt_t*>(arg);
    for (int i = 0; i < 3; ++i) {
        if (coro_rt_yield() == CORO_OK) g_yields.fetch_add(1);
    }
    (void)coro_rt_sleep(rt, pump_runner::now_ns() + kSleepNs);
    g_done.fetch_add(1);
}

}  // namespace

int main() {
    asio::io_context ioc;
    pump_runner runner(ioc);
    CHECK(runner.ok());
    CHECK(coro_rt_attached_driver(runner.rt()) != NULL);

    const std::uint64_t t0 = pump_runner::now_ns();
    for (int i = 0; i < kCoros; ++i) {
        CHECK(coro_rt_spawn(runner.rt(), NULL, worker_entry, runner.rt()) ==
              CORO_OK);
    }

    ioc.run();  // drains through the pump until nothing is live

    const std::uint64_t dt = pump_runner::now_ns() - t0;
    CHECK(g_done.load() == kCoros);
    CHECK(g_yields.load() == kCoros * 3);
    CHECK(dt >= kSleepNs - 5000000ull);  // did not return early
    CHECK(dt <= 1000000000ull);          // and did return
    CHECK(coro_rt_pending(runner.rt()) == 0);

    runner.shutdown();
    return test_finish("pump_single");
}
