// corocc asio-host example -- migration stress (the clang >= -O1 regression).
//
// A mock AsyncStream whose async_read_some/async_write_some deliver their
// completion by POSTING it to the coroutine's strand from inside the
// coroutine's own call. This is the configuration that used to corrupt the
// heap: with clang >= -O1 and several io threads, asio machinery inlined into
// the migrating coroutine's frames left thread-local state (the per-thread
// scheduler allocation cache) cached across the suspension, and after the
// coroutine moved to another thread that stale state raced with the original
// thread's own use of it. The call fence in the header keeps every asio call
// in a frame that cannot span a suspension, so this stress run must pass on
// every compiler and optimization level.
//
// 4 io threads, 8 coroutines, 250 operations each = 2000 operations per run;
// every delivery asserts the coroutine is parked (CORO_SUSPENDED) at the
// instant the completion handler is entered, and migrations are counted (a
// multi-thread run of this size migrates hundreds of times).

#include <corocc/boost/asio_host.hpp>

#include "harness.h"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace asio = boost::asio;

typedef asio::io_context::executor_type executor_type;
typedef asio::strand<executor_type> strand_type;
typedef corocc::asio_host::yield_context<strand_type> yield_type;

namespace {

/* A real, uncacheable thread identity: pthread_self (and hence
 * std::this_thread::get_id) is declared const and may be reused across the
 * suspension by the optimizer, which would hide every migration. */
unsigned long real_thread_id() {
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentThreadId());
#else
    return static_cast<unsigned long>(::syscall(SYS_gettid));
#endif
}

std::atomic<int> g_violations(0);  // handler entered while the coroutine runs
std::atomic<int> g_deliveries(0);
std::atomic<int> g_bad_results(0);
std::atomic<int> g_migrations(0);

struct posted_stream {
    typedef strand_type executor_type;

    strand_type strand;
    coro_t*     handle;

    posted_stream(const strand_type& s, coro_t* h) : strand(s), handle(h) {}

    executor_type get_executor() const { return strand; }

    template <class MutableBufferSequence, class ReadHandler>
    void async_read_some(const MutableBufferSequence& bufs, ReadHandler h) {
        deliver(h, asio::buffer_size(bufs));
    }

    template <class ConstBufferSequence, class WriteHandler>
    void async_write_some(const ConstBufferSequence& bufs, WriteHandler h) {
        deliver(h, asio::buffer_size(bufs));
    }

private:
    template <class Handler>
    void deliver(Handler h, std::size_t n) {
        // Posted from inside the coroutine's own call: the reproducer shape.
        asio::post(strand, asio::bind_executor(strand,
            [this, h, n]() mutable {
                if (coro_get_state(handle) != CORO_SUSPENDED) {
                    g_violations.fetch_add(1);
                }
                g_deliveries.fetch_add(1);
                h(boost::system::error_code(), n);
            }));
    }
};

void run_body(yield_type& y, int rounds) {
    posted_stream s(y.executor(), y.native());
    char buf[8];
    for (int r = 0; r < rounds; ++r) {
        const unsigned long before = real_thread_id();
        boost::system::error_code ec;
        const std::size_t n = corocc::asio_host::async_read_some(
            y, s, asio::buffer(buf, 8), ec);
        if (ec || n != 8) {
            g_bad_results.fetch_add(1);
        }
        if (real_thread_id() != before) {
            g_migrations.fetch_add(1);
        }
    }
}

}  // namespace

int main() {
    const int kCoroutines = 8;
    const int kRounds = 250;  // 2000 operations per run
    const int kThreads = 4;

    asio::io_context ioc;
    std::vector<strand_type> strands;
    for (int i = 0; i < kCoroutines; ++i) {
        strands.push_back(strand_type(ioc.get_executor()));
    }

    std::atomic<int> done(0);
    for (int i = 0; i < kCoroutines; ++i) {
        CHECK(corocc::asio_host::spawn(strands[static_cast<std::size_t>(i)],
            [&](yield_type& y) {
                run_body(y, kRounds);
                done.fetch_add(1);
            }) == CORO_OK);
    }

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.push_back(std::thread([&ioc] { ioc.run(); }));
    }
    for (std::size_t t = 0; t < threads.size(); ++t) {
        threads[t].join();
    }

    CHECK(done.load() == kCoroutines);
    CHECK(g_violations.load() == 0);
    CHECK(g_bad_results.load() == 0);
    CHECK(g_deliveries.load() == kCoroutines * kRounds);
    CHECK(g_migrations.load() > 0);

    std::printf("host_migration: %d ops, %d migrations\n",
                g_deliveries.load(), g_migrations.load());
    return test_finish("host_migration");
}
