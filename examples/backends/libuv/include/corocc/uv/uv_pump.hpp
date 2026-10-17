// corocc -- libuv integration example (stand-alone project).
//
// Bridges libuv's loop to the corocc runtime's driver layer in the LOOP family:
// no worker threads, and uv_run drives progress by calling coro_rt_pump from
// the driver's doorbell.
//
//   * now        -- uv_hrtime() (monotonic nanoseconds);
//   * wake       -- uv_async_send (a coalescing, thread-safe doorbell);
//   * timer_arm  -- one libuv timer, single-shot;
//   * timer_cancel.
//
// A LOOP runtime is created from a LOOP process-default driver, so pump_runner
// registers its driver, sets it as the default and then creates the runtime.
// Because the runtime does not arm the driver timer in LOOP mode, pump_runner
// re-arms a short follow-up timer while any coroutine stays live, which is how
// a coro_rt_sleep deadline is observed.
#ifndef COROCC_EXAMPLE_UV_PUMP_HPP
#define COROCC_EXAMPLE_UV_PUMP_HPP

#include <corocc/coro_rt.h>
#include <corocc/coro_rt_driver.h>

#include <uv.h>

#include <cstdint>

namespace corocc {
namespace uv_backend {

const std::uint64_t kFollowUpNs = 500000ull;  // 0.5 ms

class pump_runner {
public:
    pump_runner()
        : rt_(NULL), drv_(NULL), timer_fn_(NULL), timer_arg_(NULL) {
        if (uv_loop_init(&loop_) != 0) return;
        loop_ok_ = true;

        ops_.struct_size = static_cast<std::uint32_t>(sizeof ops_);
        ops_.flags = CORO_DRIVER_FLAG_LOOP;
        ops_.now = &pump_runner::now_fn;
        ops_.wake = &pump_runner::wake_fn;
        ops_.timer_arm = &pump_runner::timer_arm_fn;
        ops_.timer_cancel = &pump_runner::timer_cancel_fn;

        if (coro_rt_driver_register("libuv-pump", &ops_, this, &drv_) !=
            CORO_OK) {
            drv_ = NULL;
            return;
        }
        if (coro_rt_driver_set_default(drv_) != CORO_OK) {
            coro_rt_driver_unregister(drv_);
            drv_ = NULL;
            return;
        }
        rt_ = coro_rt_create(2);  // LOOP default: one pump context, no threads
        if (rt_ == NULL) return;

        async_.data = this;
        timer_.data = this;
        (void)uv_async_init(&loop_, &async_, &pump_runner::async_cb);
        (void)uv_timer_init(&loop_, &timer_);
    }

    ~pump_runner() { shutdown(); }

    pump_runner(const pump_runner&) = delete;
    pump_runner& operator=(const pump_runner&) = delete;

    bool ok() const { return rt_ != NULL && drv_ != NULL; }
    coro_rt_t* rt() const { return rt_; }

    static std::uint64_t now_ns() { return static_cast<std::uint64_t>(uv_hrtime()); }

    void stop() { stopped_ = true; }

    // Run the libuv loop until the runtime has drained (do_pump breaks it).
    void run() { uv_run(&loop_, UV_RUN_DEFAULT); }

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
        if (loop_ok_) {
            if (async_.loop != NULL) uv_close((uv_handle_t*)&async_, NULL);
            if (timer_.loop != NULL) uv_close((uv_handle_t*)&timer_, NULL);
            uv_run(&loop_, UV_RUN_DEFAULT);  // reap the close callbacks
            (void)uv_loop_close(&loop_);
            loop_ok_ = false;
        }
    }

private:
    static std::uint64_t now_fn(void* ctx) {
        (void)ctx;
        return now_ns();
    }

    static void wake_fn(void* ctx) {
        pump_runner* r = static_cast<pump_runner*>(ctx);
        if (!r->stopped_) uv_async_send(&r->async_);
    }

    static int timer_arm_fn(void* ctx, void* req, std::uint64_t deadline,
                            coro_driver_timer_fn fn, void* arg) {
        pump_runner* r = static_cast<pump_runner*>(ctx);
        r->timer_req_ = req;
        r->timer_fn_ = fn;
        r->timer_arg_ = arg;
        std::uint64_t now = now_ns();
        std::uint64_t timeout =
            deadline > now ? (deadline - now + 999999ull) / 1000000ull : 0ull;
        (void)uv_timer_start(&r->timer_, &pump_runner::timer_cb, timeout, 0);
        return 0;
    }

    static int timer_cancel_fn(void* ctx, void* req) {
        pump_runner* r = static_cast<pump_runner*>(ctx);
        if (r->timer_req_ == req && r->timer_fn_ != NULL) {
            r->timer_fn_ = NULL;
            (void)uv_timer_stop(&r->timer_);
            return 0;
        }
        return 1;
    }

    static void async_cb(uv_async_t* handle) {
        static_cast<pump_runner*>(handle->data)->do_pump();
    }

    static void timer_cb(uv_timer_t* handle) {
        pump_runner* r = static_cast<pump_runner*>(handle->data);
        coro_driver_timer_fn f = r->timer_fn_;
        void* a = r->timer_arg_;
        r->timer_fn_ = NULL;
        if (f != NULL) f(a);
    }

    static void nudge_cb(void* arg) {
        static_cast<pump_runner*>(arg)->do_pump();
    }

    void do_pump() {
        if (stopped_ || rt_ == NULL) return;
        (void)coro_rt_pump(rt_, 0);  // budget 0: drain everything runnable
        if (!stopped_ && coro_rt_pending(rt_) > 0) {
            // Re-check soon through the driver's own timer op.
            (void)timer_arm_fn(this, &nudge_key_, now_ns() + kFollowUpNs,
                               &pump_runner::nudge_cb, this);
        } else if (!stopped_) {
            uv_stop(&loop_);  // drained: let uv_run return
        }
    }

    uv_loop_t loop_;
    bool loop_ok_ = false;
    coro_rt_t* rt_;
    coro_rt_driver_t* drv_;
    struct coro_rt_driver_ops ops_;
    uv_async_t async_;
    uv_timer_t timer_;
    void* timer_req_ = NULL;
    coro_driver_timer_fn timer_fn_;
    void* timer_arg_;
    int nudge_key_ = 0;
    bool stopped_ = false;
};

}  // namespace uv_backend
}  // namespace corocc

#endif  // COROCC_EXAMPLE_UV_PUMP_HPP
