// corocc libevent example -- smoke: a LOOP runtime pumped by event_base_dispatch.
#include <corocc/event/event_pump.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

std::atomic<int> g_done(0);
int g_fail = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_fail; \
    } \
} while (0)

void entry(void* arg) {
    coro_rt_t* rt = static_cast<coro_rt_t*>(arg);
    for (int i = 0; i < 3; ++i) {
        (void)coro_rt_yield();
    }
    (void)coro_rt_sleep(
        rt, corocc::event_backend::pump_runner::now_ns() + 20000000ull);
    g_done.fetch_add(1);
}

}  // namespace

int main() {
    corocc::event_backend::pump_runner runner;
    CHECK(runner.ok());

    const std::uint64_t t0 = corocc::event_backend::pump_runner::now_ns();
    for (int i = 0; i < 4; ++i) {
        CHECK(coro_rt_spawn(runner.rt(), NULL, entry, runner.rt()) == CORO_OK);
    }

    runner.run();  // event_base_dispatch until the runtime drains

    const std::uint64_t dt =
        corocc::event_backend::pump_runner::now_ns() - t0;
    CHECK(g_done.load() == 4);
    CHECK(dt >= 15000000ull);
    CHECK(coro_rt_pending(runner.rt()) == 0);

    runner.shutdown();
    if (g_fail != 0) {
        std::fprintf(stderr, "event_smoke: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("event_smoke: ok\n");
    return 0;
}
