// corocc -- asio-pump integration example (stand-alone project).
//
// This header bridges Boost.Asio to the corocc runtime's driver layer in the
// LOOP family: the runtime owns no worker threads and the io_context loop
// drives progress by calling coro_rt_pump from within the driver's doorbell.
//
//   * now        -- std::chrono::steady_clock, in nanoseconds (the same scale
//                   the runtime compares deadlines on);
//   * wake       -- a coalesced boost::asio::post of the pump task (cheap,
//                   idempotent, safe from any thread);
//   * timer_arm  -- a single-shot boost::asio::steady_timer;
//   * timer_cancel.
//
// A LOOP runtime can only be created from a LOOP process-default driver (the
// runtime's create() reads coro_rt_driver_default()), so pump_runner registers
// its driver, sets it as the default and then creates the runtime.
//
// The loop re-arms itself for parked coroutines: because the runtime does not
// arm the driver timer in LOOP mode, pump_runner schedules a short follow-up
// pump through timer_arm while any coroutine is still live, which is how a
// coro_rt_sleep deadline is observed without a worker thread.
#ifndef COROCC_EXAMPLE_ASIO_PUMP_HPP
#define COROCC_EXAMPLE_ASIO_PUMP_HPP

#include <corocc/coro_rt.h>
#include <corocc/coro_rt_driver.h>

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace corocc {
namespace asio_pump {

// The follow-up pump interval: the runtime does not expose its nearest deadline
// in LOOP mode, so the loop re-checks at this cadence while work is live. Kept
// small so a sleep's observed completion stays close to its deadline.
const std::uint64_t kFollowUpNs = 500000ull;  // 0.5 ms

// pump_runner -- an asio-hosted LOOP runtime.
//
// Lifecycle:
//     asio::io_context ioc;
//     corocc::asio_pump::pump_runner runner(ioc);
//     coro_rt_spawn(runner.rt(), NULL, entry, arg);
//     ioc.run();            // drains through the pump until nothing is live
//     runner.shutdown();    // destroy runtime + unregister the driver
class pump_runner {
public:
    explicit pump_runner(boost::asio::io_context& ioc)
        : ioc_(ioc),
          rt_(NULL),
          drv_(NULL),
          nudge_(ioc),
          scheduled_(false),
          stopped_(false),
          timer_req_(NULL),
          timer_fn_(NULL),
          timer_arg_(NULL) {
        ops_.struct_size = static_cast<std::uint32_t>(sizeof ops_);
        ops_.flags = CORO_DRIVER_FLAG_LOOP;
        ops_.now = &pump_runner::now_fn;
        ops_.wake = &pump_runner::wake_fn;
        ops_.timer_arm = &pump_runner::timer_arm_fn;
        ops_.timer_cancel = &pump_runner::timer_cancel_fn;

        if (coro_rt_driver_register("asio-pump", &ops_, this, &drv_) != CORO_OK) {
            drv_ = NULL;
            return;
        }
        if (coro_rt_driver_set_default(drv_) != CORO_OK) {
            coro_rt_driver_unregister(drv_);
            drv_ = NULL;
            return;
        }
        rt_ = coro_rt_create(2);  // LOOP default: one pump context, no threads
    }

    ~pump_runner() { shutdown(); }

    pump_runner(const pump_runner&) = delete;
    pump_runner& operator=(const pump_runner&) = delete;

    bool ok() const { return rt_ != NULL && drv_ != NULL; }
    coro_rt_t* rt() const { return rt_; }

    // The driver clock, in nanoseconds: coroutine code must compute absolute
    // coro_rt_sleep deadlines from exactly this source (rt_now dispatches here).
    static std::uint64_t now_ns() {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    // Stop scheduling further pumps; io_context::run() then drains and returns.
    void stop() { stopped_.store(true); }

    // Tear down: destroy the runtime and release the driver. Call after run()
    // has returned (or after the runtime has drained).
    void shutdown() {
        stop();
        if (rt_ != NULL) {
            if (coro_rt_pending(rt_) == 0) {
                (void)coro_rt_destroy(rt_);
                rt_ = NULL;
            }
        }
        if (drv_ != NULL) {
            (void)coro_rt_driver_set_default(NULL);
            (void)coro_rt_driver_unregister(drv_);
            drv_ = NULL;
        }
    }

private:
    static std::uint64_t now_fn(void* ctx) {
        (void)ctx;
        return now_ns();
    }

    static void wake_fn(void* ctx) {
        static_cast<pump_runner*>(ctx)->schedule();
    }

    static int timer_arm_fn(void* ctx, void* req, std::uint64_t deadline,
                            coro_driver_timer_fn fn, void* arg) {
        pump_runner* r = static_cast<pump_runner*>(ctx);
        r->timer_req_ = req;
        r->timer_fn_ = fn;
        r->timer_arg_ = arg;
        r->nudge_.expires_at(std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(deadline)));
        r->nudge_.async_wait([r, req](const boost::system::error_code& ec) {
            if (!ec && r->timer_fn_ != NULL && r->timer_req_ == req) {
                coro_driver_timer_fn f = r->timer_fn_;
                void* a = r->timer_arg_;
                r->timer_fn_ = NULL;
                f(a);
            }
        });
        return 0;
    }

    static int timer_cancel_fn(void* ctx, void* req) {
        pump_runner* r = static_cast<pump_runner*>(ctx);
        if (r->timer_req_ == req && r->timer_fn_ != NULL) {
            r->timer_fn_ = NULL;
            boost::system::error_code ec;
            r->nudge_.cancel(ec);
            return 0;
        }
        return 1;
    }

    static void nudge_fired(void* arg) {
        static_cast<pump_runner*>(arg)->schedule();
    }

    void schedule() {
        if (stopped_.load()) return;
        bool expected = false;
        if (scheduled_.compare_exchange_strong(expected, true)) {
            boost::asio::post(ioc_, [this]() {
                scheduled_.store(false);
                do_pump();
            });
        }
    }

    void do_pump() {
        if (stopped_.load() || rt_ == NULL) return;
        (void)coro_rt_pump(rt_, 0);  // budget 0: drain everything runnable
        if (!stopped_.load() && coro_rt_pending(rt_) > 0) {
            // Re-check soon: a parked coroutine's deadline is observed by the
            // next pump. Uses the driver's own timer op, so the arm/cancel path
            // is exercised exactly as the runtime would use it.
            (void)timer_arm_fn(this, &nudge_key_, now_fn(this) + kFollowUpNs,
                               &pump_runner::nudge_fired, this);
        }
    }

    boost::asio::io_context& ioc_;
    coro_rt_t* rt_;
    coro_rt_driver_t* drv_;
    struct coro_rt_driver_ops ops_;
    boost::asio::steady_timer nudge_;
    std::atomic<bool> scheduled_;
    std::atomic<bool> stopped_;
    void* timer_req_;
    coro_driver_timer_fn timer_fn_;
    void* timer_arg_;
    int nudge_key_;  // opaque token for the follow-up timer
};

}  // namespace asio_pump
}  // namespace corocc

#endif  // COROCC_EXAMPLE_ASIO_PUMP_HPP
