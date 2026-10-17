// corocc -- libevent integration example (stand-alone project).
//
// Bridges a libevent event_base to the corocc runtime's driver layer in the
// LOOP family: no worker threads, and event_base_dispatch drives progress by
// calling coro_rt_pump from the driver's doorbell.
//
//   * now        -- clock_gettime(CLOCK_MONOTONIC), nanoseconds;
//   * wake       -- event_active on a persistent user event (thread-safe once
//                   evthread_use_pthreads() has run);
//   * timer_arm  -- one single-shot evtimer;
//   * timer_cancel.
//
// A LOOP runtime is created from a LOOP process-default driver, so pump_runner
// registers its driver, sets it as the default and then creates the runtime;
// it re-arms a short follow-up timer while any coroutine stays live (the
// runtime does not arm the driver timer in LOOP mode) so coro_rt_sleep
// deadlines are observed.
#ifndef COROCC_EXAMPLE_EVENT_PUMP_HPP
#define COROCC_EXAMPLE_EVENT_PUMP_HPP

#include <corocc/coro_rt.h>
#include <corocc/coro_rt_driver.h>

#include <event2/event.h>
#include <event2/thread.h>

#include <cstdint>
#include <ctime>

namespace corocc {
namespace event_backend {

const std::uint64_t kFollowUpNs = 500000ull;  // 0.5 ms

inline void ensure_threads() {
    static bool done = false;
    if (!done) {
        (void)evthread_use_pthreads();
        done = true;
    }
}

class pump_runner {
public:
    pump_runner()
        : rt_(NULL),
          drv_(NULL),
          base_(NULL),
          wake_ev_(NULL),
          timer_ev_(NULL),
          timer_fn_(NULL),
          timer_arg_(NULL) {
        ensure_threads();
        base_ = event_base_new();
        if (base_ == NULL) return;

        ops_.struct_size = static_cast<std::uint32_t>(sizeof ops_);
        ops_.flags = CORO_DRIVER_FLAG_LOOP;
        ops_.now = &pump_runner::now_fn;
        ops_.wake = &pump_runner::wake_fn;
        ops_.timer_arm = &pump_runner::timer_arm_fn;
        ops_.timer_cancel = &pump_runner::timer_cancel_fn;

        if (coro_rt_driver_register("libevent-pump", &ops_, this, &drv_) !=
            CORO_OK) {
            drv_ = NULL;
            return;
        }
        if (coro_rt_driver_set_default(drv_) != CORO_OK) {
            coro_rt_driver_unregister(drv_);
            drv_ = NULL;
            return;
        }

        wake_ev_ = event_new(base_, -1, EV_PERSIST, &pump_runner::wake_cb, this);
        timer_ev_ = event_new(base_, -1, 0, &pump_runner::timer_cb, this);
        if (wake_ev_ == NULL || timer_ev_ == NULL) return;
        (void)event_add(wake_ev_, NULL);

        rt_ = coro_rt_create(2);  // LOOP default: one pump context, no threads
    }

    ~pump_runner() { shutdown(); }

    pump_runner(const pump_runner&) = delete;
    pump_runner& operator=(const pump_runner&) = delete;

    bool ok() const { return rt_ != NULL && drv_ != NULL; }
    coro_rt_t* rt() const { return rt_; }

    static std::uint64_t now_ns() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
               static_cast<std::uint64_t>(ts.tv_nsec);
    }

    void stop() { stopped_ = true; }

    // Run the event loop until the runtime drains (do_pump breaks it).
    void run() {
        if (base_ != NULL) (void)event_base_dispatch(base_);
    }

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
        if (wake_ev_ != NULL) {
            event_free(wake_ev_);
            wake_ev_ = NULL;
        }
        if (timer_ev_ != NULL) {
            event_free(timer_ev_);
            timer_ev_ = NULL;
        }
        if (base_ != NULL) {
            event_base_free(base_);
            base_ = NULL;
        }
    }

private:
    static std::uint64_t now_fn(void* ctx) {
        (void)ctx;
        return now_ns();
    }

    static void wake_fn(void* ctx) {
        pump_runner* r = static_cast<pump_runner*>(ctx);
        if (!r->stopped_ && r->wake_ev_ != NULL) {
            event_active(r->wake_ev_, 0, 0);
        }
    }

    static int timer_arm_fn(void* ctx, void* req, std::uint64_t deadline,
                            coro_driver_timer_fn fn, void* arg) {
        pump_runner* r = static_cast<pump_runner*>(ctx);
        r->timer_req_ = req;
        r->timer_fn_ = fn;
        r->timer_arg_ = arg;
        std::uint64_t now = now_ns();
        std::uint64_t d = deadline > now ? deadline - now : 0ull;
        struct timeval tv;
        tv.tv_sec = static_cast<long>(d / 1000000000ull);
        tv.tv_usec = static_cast<long>((d % 1000000000ull) / 1000ull);
        (void)evtimer_add(r->timer_ev_, &tv);
        return 0;
    }

    static int timer_cancel_fn(void* ctx, void* req) {
        pump_runner* r = static_cast<pump_runner*>(ctx);
        if (r->timer_req_ == req && r->timer_fn_ != NULL) {
            r->timer_fn_ = NULL;
            (void)evtimer_del(r->timer_ev_);
            return 0;
        }
        return 1;
    }

    static void wake_cb(evutil_socket_t fd, short what, void* arg) {
        (void)fd;
        (void)what;
        static_cast<pump_runner*>(arg)->do_pump();
    }

    static void timer_cb(evutil_socket_t fd, short what, void* arg) {
        (void)fd;
        (void)what;
        pump_runner* r = static_cast<pump_runner*>(arg);
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
            (void)timer_arm_fn(this, &nudge_key_, now_ns() + kFollowUpNs,
                               &pump_runner::nudge_cb, this);
        } else if (!stopped_ && base_ != NULL) {
            event_base_loopbreak(base_);  // drained: end dispatch
        }
    }

    coro_rt_t* rt_;
    coro_rt_driver_t* drv_;
    struct event_base* base_;
    struct coro_rt_driver_ops ops_;
    struct event* wake_ev_;
    struct event* timer_ev_;
    void* timer_req_ = NULL;
    coro_driver_timer_fn timer_fn_;
    void* timer_arg_;
    int nudge_key_ = 0;
    bool stopped_ = false;
};

}  // namespace event_backend
}  // namespace corocc

#endif  // COROCC_EXAMPLE_EVENT_PUMP_HPP
