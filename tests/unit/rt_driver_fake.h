#ifndef COROCC_TESTS_RT_DRIVER_FAKE_H
#define COROCC_TESTS_RT_DRIVER_FAKE_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * tests/unit/rt_driver_fake.h -- a recording driver for the B2 suites.
 *
 * A real timer-backed SIGNAL driver: now() forwards to coro_monotonic_ns and
 * counts calls, wake() counts doorbell raises, and a small manager thread fires
 * armed alarms at their deadline (each alarm keyed by the runtime's opaque req
 * token). It records arm/cancel/fire counts so the suites can assert the vtable
 * contract, and it never touches the runtime from a callback except through the
 * fn the runtime handed it. INTERNAL to the tests.
 */
#include "../palt_threads.h"
#include "arch/coro_atomic.h"

#include <corocc/coro_rt_driver.h>
#include <corocc/coro_sched.h>

#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#define FAKE_MAX_TIMERS 256

typedef struct fake_slot {
    void*                 req;
    uint64_t              deadline;
    coro_driver_timer_fn  fn;
    void*                 arg;
    int                   active;
} fake_slot;

typedef struct fake_driver {
    pt_mutex_t        lock;
    pt_cond_t         cv;
    pt_thread_t       mgr;
    int               mgr_started;
    int               stop;
    fake_slot         slots[FAKE_MAX_TIMERS];
    volatile uint64_t now_calls;
    volatile uint64_t wake_calls;
    volatile uint64_t arm_calls;
    volatile uint64_t cancel_calls;
    volatile uint64_t fire_calls;
    volatile uint64_t last_deadline;
} fake_driver;

static void fake_sleep_ms(unsigned ms) {
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static inline int fake_wait_flag(const volatile uint64_t* flag, unsigned ms) {
    for (unsigned i = 0; i < ms; ++i) {
        if (coro_at_load(flag) != 0) return 1;
        fake_sleep_ms(1);
    }
    return coro_at_load(flag) != 0;
}

static uint64_t fake_now(void* ctx) {
    fake_driver* f = (fake_driver*)ctx;
    (void)coro_at_fetch_add((volatile uint64_t*)&f->now_calls, 1);
    return coro_monotonic_ns();
}

static void fake_wake(void* ctx) {
    fake_driver* f = (fake_driver*)ctx;
    (void)coro_at_fetch_add((volatile uint64_t*)&f->wake_calls, 1);
}

static int fake_timer_arm(void* ctx, void* req, uint64_t deadline,
                          coro_driver_timer_fn fn, void* arg) {
    fake_driver* f = (fake_driver*)ctx;
    int slot = -1;
    pt_mutex_lock(&f->lock);
    (void)coro_at_fetch_add((volatile uint64_t*)&f->arm_calls, 1);
    coro_at_store((volatile uint64_t*)&f->last_deadline, deadline);
    for (int i = 0; i < FAKE_MAX_TIMERS; ++i) {
        if (f->slots[i].active && f->slots[i].req == req) { slot = i; break; }
        if (slot < 0 && !f->slots[i].active) slot = i;
    }
    if (slot < 0) { pt_mutex_unlock(&f->lock); return -1; }
    f->slots[slot].req = req;
    f->slots[slot].deadline = deadline;
    f->slots[slot].fn = fn;
    f->slots[slot].arg = arg;
    f->slots[slot].active = 1;
    pt_cond_broadcast(&f->cv);
    pt_mutex_unlock(&f->lock);
    return 0;
}

static int fake_timer_cancel(void* ctx, void* req) {
    fake_driver* f = (fake_driver*)ctx;
    int found = 0;
    pt_mutex_lock(&f->lock);
    (void)coro_at_fetch_add((volatile uint64_t*)&f->cancel_calls, 1);
    for (int i = 0; i < FAKE_MAX_TIMERS; ++i) {
        if (f->slots[i].active && f->slots[i].req == req) {
            f->slots[i].active = 0;
            found = 1;
        }
    }
    pt_mutex_unlock(&f->lock);
    return found ? 0 : 1;
}

static void* fake_mgr_main(void* p) {
    fake_driver* f = (fake_driver*)p;
    for (;;) {
        int slot = -1;
        uint64_t best = 0;
        pt_mutex_lock(&f->lock);
        if (f->stop) { pt_mutex_unlock(&f->lock); break; }
        for (int i = 0; i < FAKE_MAX_TIMERS; ++i) {
            if (f->slots[i].active &&
                (slot < 0 || f->slots[i].deadline < best)) {
                slot = i;
                best = f->slots[i].deadline;
            }
        }
        if (slot < 0) {
            pt_cond_wait(&f->cv, &f->lock);
            pt_mutex_unlock(&f->lock);
            continue;
        }
        if (best <= coro_monotonic_ns()) {
            coro_driver_timer_fn fn = f->slots[slot].fn;
            void* arg = f->slots[slot].arg;
            f->slots[slot].active = 0;
            pt_mutex_unlock(&f->lock);
            (void)coro_at_fetch_add((volatile uint64_t*)&f->fire_calls, 1);
            fn(arg);
            continue;
        }
        pt_mutex_unlock(&f->lock);
        fake_sleep_ms(1);
    }
    return NULL;
}

static void fake_ops_init(struct coro_rt_driver_ops* ops, uint32_t flags) {
    memset(ops, 0, sizeof *ops);
    ops->struct_size = (uint32_t)sizeof *ops;
    ops->flags = flags;
    ops->now = fake_now;
    ops->wake = fake_wake;
    ops->timer_arm = fake_timer_arm;
    ops->timer_cancel = fake_timer_cancel;
}

/* Start a fake driver instance (its manager thread) and its registry entry. */
static coro_rt_driver_t* fake_start(const char* name, fake_driver* f,
                                    uint32_t flags) {
    struct coro_rt_driver_ops ops;
    coro_rt_driver_t* d = NULL;
    memset(f, 0, sizeof *f);
    pt_mutex_init(&f->lock);
    pt_cond_init(&f->cv);
    if (pt_thread_create(&f->mgr, fake_mgr_main, f) != 0) return NULL;
    f->mgr_started = 1;
    fake_ops_init(&ops, flags);
    if (coro_rt_driver_register(name, &ops, f, &d) != CORO_OK) {
        pt_mutex_lock(&f->lock);
        f->stop = 1;
        pt_cond_broadcast(&f->cv);
        pt_mutex_unlock(&f->lock);
        pt_thread_join(f->mgr);
        f->mgr_started = 0;
        return NULL;
    }
    return d;
}

/* Stop the manager thread (call after the driver is unregistered). */
static void fake_stop(fake_driver* f) {
    if (!f->mgr_started) return;
    pt_mutex_lock(&f->lock);
    f->stop = 1;
    pt_cond_broadcast(&f->cv);
    pt_mutex_unlock(&f->lock);
    pt_thread_join(f->mgr);
    f->mgr_started = 0;
}

#endif /* COROCC_TESTS_RT_DRIVER_FAKE_H */
