#include "../harness.h"
#include "rt_driver_fake.h"

#include <corocc/coro.h>
#include <corocc/coro_rt.h>

/* B2: hot-plugging a SIGNAL driver on a live runtime.
 *
 *   - attach while coroutines are parked, then detach mid-run: every timed park
 *     still completes, no wakeup is lost;
 *   - attach and detach racing a stream of sleeps does not hang;
 *   - cancellation of a timed park still takes effect under an attached driver;
 *   - attached_driver() tracks the transitions;
 *   - a LOOP-class attach on a worker-mode runtime is refused (CORO_EBUSY) --
 *     v1 only permits the built-in/SIGNAL families to hot-swap (ruling #4);
 *   - a runtime created from a LOOP default runs worker[0] by hand through
 *     coro_rt_pump (single loop thread, ruling #5), and pump on a non-LOOP
 *     runtime is refused.
 */

static volatile uint64_t g_completed;
static volatile uint64_t g_cancelled;
static volatile uint64_t g_done;
static coro_rt_t*        g_rt;

static void ms_sleeper(void* arg) {
    unsigned ms = (unsigned)(uintptr_t)arg;
    coro_status st =
        coro_rt_sleep(g_rt, coro_monotonic_ns() + (uint64_t)ms * 1000000ull);
    if (st == CORO_OK) {
        (void)coro_at_fetch_add((volatile uint64_t*)&g_completed, 1);
    } else {
        (void)coro_at_fetch_add((volatile uint64_t*)&g_cancelled, 1);
    }
}

static int wait_completed(uint64_t n, unsigned ms) {
    for (unsigned i = 0; i < ms; ++i) {
        if (coro_at_load(&g_completed) >= n) return 1;
        fake_sleep_ms(1);
    }
    return coro_at_load(&g_completed) >= n;
}

/* ---- attach on a live, parked runtime, then detach mid-run ---- */

static void test_attach_running(void) {
    coro_rt_t* rt = coro_rt_create(2);
    CHECK(rt != NULL);
    g_rt = rt;

    /* Parks created while the built-in backend is active. */
    coro_at_store((volatile uint64_t*)&g_completed, 0);
    for (uintptr_t i = 0; i < 8; ++i) {
        CHECK_EQ(coro_rt_spawn(rt, NULL, ms_sleeper, (void*)(uintptr_t)(20 + i * 5)),
                 CORO_OK);
    }
    fake_sleep_ms(4); /* let them reach their parks */

    fake_driver f;
    coro_rt_driver_t* d = fake_start("hotplug-running", &f, 0);
    CHECK(d != NULL);
    CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_OK); /* running attach */
    CHECK(coro_rt_attached_driver(rt) == d);

    CHECK(wait_completed(8, 4000));
    CHECK_EQ(coro_at_load(&g_completed), 8);

    CHECK_EQ(coro_rt_detach_driver(rt), CORO_OK);
    CHECK_EQ(coro_rt_attached_driver(rt), NULL);
    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_OK);
    fake_stop(&f);
}

/* ---- attach before the work, detach while it is in flight ---- */

static void test_attach_then_detach(void) {
    coro_rt_t* rt = coro_rt_create(2);
    CHECK(rt != NULL);
    g_rt = rt;

    fake_driver f;
    coro_rt_driver_t* d = fake_start("hotplug-toggle", &f, 0);
    CHECK(d != NULL);
    CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_OK);

    coro_at_store((volatile uint64_t*)&g_completed, 0);
    for (uintptr_t i = 0; i < 6; ++i) {
        CHECK_EQ(coro_rt_spawn(rt, NULL, ms_sleeper, (void*)(uintptr_t)(40 + i * 5)),
                 CORO_OK);
    }
    fake_sleep_ms(4);
    CHECK_EQ(coro_rt_detach_driver(rt), CORO_OK); /* back to built-in, in flight */

    CHECK(wait_completed(6, 4000));
    CHECK_EQ(coro_at_load(&g_completed), 6);

    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_OK);
    fake_stop(&f);
}

/* ---- attach/detach racing a stream of sleeps ---- */

static void test_stress(void) {
    coro_rt_t* rt = coro_rt_create(3);
    CHECK(rt != NULL);
    g_rt = rt;

    fake_driver f;
    coro_rt_driver_t* d = fake_start("hotplug-stress", &f, 0);
    CHECK(d != NULL);

    coro_at_store((volatile uint64_t*)&g_completed, 0);
    const uint64_t per_round = 6;
    const int rounds = 20;
    for (int r = 0; r < rounds; ++r) {
        CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_OK);
        for (uintptr_t i = 0; i < per_round; ++i) {
            CHECK_EQ(coro_rt_spawn(rt, NULL, ms_sleeper,
                                   (void*)(uintptr_t)(2 + (r % 3) * 2)),
                     CORO_OK);
        }
        fake_sleep_ms(1);
        CHECK_EQ(coro_rt_detach_driver(rt), CORO_OK);
        CHECK(wait_completed((uint64_t)(r + 1) * per_round, 4000));
    }
    CHECK_EQ(coro_at_load(&g_completed), (uint64_t)rounds * per_round);
    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_OK);
    fake_stop(&f);
}

/* ---- cancel of a timed park under an attached driver ---- */

static void test_cancel_after_attach(void) {
    coro_rt_t* rt = coro_rt_create(2);
    CHECK(rt != NULL);
    g_rt = rt;

    fake_driver f;
    coro_rt_driver_t* d = fake_start("hotplug-cancel", &f, 0);
    CHECK(d != NULL);
    CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_OK);

    coro_at_store((volatile uint64_t*)&g_completed, 0);
    coro_t* h = NULL;
    CHECK_EQ(coro_rt_spawn_h(rt, NULL, ms_sleeper, (void*)(uintptr_t)5000, &h),
             CORO_OK);
    CHECK(h != NULL);

    unsigned guard = 0;
    while (coro_at_load(&f.arm_calls) == 0 && guard < 2000) {
        fake_sleep_ms(1);
        ++guard;
    }
    CHECK(coro_at_load(&f.arm_calls) > 0);
    CHECK_EQ(coro_rt_cancel(rt, h), CORO_OK);
    fake_sleep_ms(50);
    CHECK_EQ(coro_at_load(&g_completed), 0); /* the 5 s sleep did not finish */

    for (unsigned i = 0; i < 2000 && coro_rt_collect(rt, h) != CORO_OK; ++i) {
        fake_sleep_ms(1);
    }
    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_OK);
    fake_stop(&f);
}

/* ---- LOOP attach on a worker-mode runtime is refused (ruling #4) ---- */

static void test_loop_attach_ebusy(void) {
    coro_rt_t* rt = coro_rt_create(2);
    CHECK(rt != NULL);
    g_rt = rt;

    fake_driver f;
    coro_rt_driver_t* d = fake_start("loop-refused", &f, CORO_DRIVER_FLAG_LOOP);
    CHECK(d != NULL);

    /* Idle runtime: still refused, because its worker threads cannot be
       repurposed in v1. */
    CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_EBUSY);
    CHECK_EQ(coro_rt_attached_driver(rt), NULL);

    /* And while a coroutine is actively sleeping (a worker is driving). */
    coro_at_store((volatile uint64_t*)&g_completed, 0);
    CHECK_EQ(coro_rt_spawn(rt, NULL, ms_sleeper, (void*)(uintptr_t)60), CORO_OK);
    CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_EBUSY);

    CHECK(wait_completed(1, 4000));
    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_OK);
    fake_stop(&f);
}

/* ---- getter and idempotent transitions ---- */

static void test_attached_query(void) {
    coro_rt_t* rt = coro_rt_create(1);
    CHECK(rt != NULL);

    fake_driver f;
    coro_rt_driver_t* d = fake_start("hotplug-query", &f, 0);
    CHECK(d != NULL);

    CHECK_EQ(coro_rt_attached_driver(rt), NULL);
    CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_OK);
    CHECK(coro_rt_attached_driver(rt) == d);
    CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_OK); /* idempotent same driver */
    CHECK_EQ(coro_rt_detach_driver(rt), CORO_OK);
    CHECK_EQ(coro_rt_attached_driver(rt), NULL);
    CHECK_EQ(coro_rt_detach_driver(rt), CORO_OK); /* idempotent */
    CHECK_EQ(coro_rt_attach_driver(NULL, d), CORO_EINVAL);

    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_OK);
    fake_stop(&f);
}

/* ---- a LOOP runtime is pumped by hand through coro_rt_pump ---- */

static void yielder(void* arg) {
    coro_rt_t* rt = (coro_rt_t*)arg;
    for (int i = 0; i < 5; ++i) (void)coro_rt_yield();
    (void)coro_rt_sleep(rt, coro_monotonic_ns() + 20000000ull); /* 20 ms */
    coro_at_store((volatile uint64_t*)&g_done, 1);
}

static void test_pump_loop(void) {
    fake_driver f;
    coro_rt_driver_t* d = fake_start("pump-loop", &f, CORO_DRIVER_FLAG_LOOP);
    CHECK(d != NULL);
    CHECK_EQ(coro_rt_driver_set_default(d), CORO_OK);

    coro_rt_t* rt = coro_rt_create(4); /* LOOP default: no worker threads */
    CHECK(rt != NULL);
    CHECK(coro_rt_attached_driver(rt) == d);

    coro_at_store((volatile uint64_t*)&g_done, 0);
    CHECK_EQ(coro_rt_spawn(rt, NULL, yielder, rt), CORO_OK);

    unsigned guard = 0;
    while (coro_at_load(&g_done) == 0 && guard < 3000) {
        CHECK_EQ(coro_rt_pump(rt, 64), CORO_OK);
        fake_sleep_ms(1);
        ++guard;
    }
    CHECK_EQ(coro_at_load(&g_done), 1);

    coro_rt_wait_all(rt); /* live == 0: returns immediately */
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    CHECK_EQ(coro_rt_driver_set_default(NULL), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_OK);
    fake_stop(&f);
}

/* ---- pump on a worker-mode runtime is refused ---- */

static void test_pump_signal_ebusy(void) {
    coro_rt_t* rt = coro_rt_create(2);
    CHECK(rt != NULL);
    CHECK_EQ(coro_rt_pump(rt, 1), CORO_EBUSY);
    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
}

static void run_tests(void) {
    test_attach_running();
    test_attach_then_detach();
    test_stress();
    test_cancel_after_attach();
    test_loop_attach_ebusy();
    test_attached_query();
    test_pump_loop();
    test_pump_signal_ebusy();
}

TEST_MAIN()
