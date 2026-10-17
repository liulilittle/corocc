// corocc asio-pump example -- a built-in (worker-thread) runtime and a LOOP
// runtime coexist in one process without interfering.
#include <corocc/boost/asio_pump.hpp>
#include <corocc/coro_sched.h>

#include "harness.h"

#include <atomic>
#include <cstdint>

namespace asio = boost::asio;
using corocc::asio_pump::pump_runner;

namespace {

std::atomic<int> g_ed(0);
std::atomic<int> g_loop(0);

void ed_entry(void* arg) {
    coro_rt_t* rt = static_cast<coro_rt_t*>(arg);
    (void)coro_rt_sleep(rt, coro_monotonic_ns() + 15000000ull);
    g_ed.fetch_add(1);
}

void loop_entry(void* arg) {
    coro_rt_t* rt = static_cast<coro_rt_t*>(arg);
    (void)coro_rt_sleep(rt, pump_runner::now_ns() + 15000000ull);
    g_loop.fetch_add(1);
}

}  // namespace

int main() {
    // 1) The built-in runtime is created while the process default is NULL.
    coro_rt_t* ed = coro_rt_create(2);
    CHECK(ed != NULL);
    CHECK(coro_rt_attached_driver(ed) == NULL);  // built-in EDSM
    for (int i = 0; i < 4; ++i) {
        CHECK(coro_rt_spawn(ed, NULL, ed_entry, ed) == CORO_OK);
    }

    // 2) A LOOP runtime is created from the asio-pump default driver.
    asio::io_context ioc;
    pump_runner runner(ioc);
    CHECK(runner.ok());
    CHECK(coro_rt_attached_driver(runner.rt()) != NULL);
    for (int i = 0; i < 4; ++i) {
        CHECK(coro_rt_spawn(runner.rt(), NULL, loop_entry, runner.rt()) ==
              CORO_OK);
    }

    ioc.run();             // drains the LOOP runtime through the pump
    CHECK(g_loop.load() == 4);
    coro_rt_wait_all(ed);  // drains the EDSM runtime through its workers
    CHECK(g_ed.load() == 4);

    CHECK(coro_rt_destroy(ed) == CORO_OK);
    runner.shutdown();
    return test_finish("pump_mixed");
}
