// corocc asio-pump example -- sleep/yield stress with zero loss.
#include <corocc/boost/asio_pump.hpp>

#include "harness.h"

#include <atomic>
#include <cstdint>

namespace asio = boost::asio;
using corocc::asio_pump::pump_runner;

namespace {

const int kCoros = 24;
const int kRounds = 40;

std::atomic<int> g_rounds(0);

void stress_entry(void* arg) {
    coro_rt_t* rt = static_cast<coro_rt_t*>(arg);
    for (int i = 0; i < kRounds; ++i) {
        (void)coro_rt_yield();
        (void)coro_rt_sleep(
            rt, pump_runner::now_ns() +
                    static_cast<std::uint64_t>(i % 3) * 1000000ull);
        g_rounds.fetch_add(1);
    }
}

}  // namespace

int main() {
    asio::io_context ioc;
    pump_runner runner(ioc);
    CHECK(runner.ok());

    for (int i = 0; i < kCoros; ++i) {
        CHECK(coro_rt_spawn(runner.rt(), NULL, stress_entry, runner.rt()) ==
              CORO_OK);
    }

    ioc.run();

    CHECK(g_rounds.load() == kCoros * kRounds);
    CHECK(coro_rt_pending(runner.rt()) == 0);

    runner.shutdown();
    return test_finish("pump_stress");
}
