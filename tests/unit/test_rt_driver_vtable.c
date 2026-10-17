#include "../harness.h"
#include "rt_driver_fake.h"

#include <corocc/coro.h>
#include <corocc/coro_rt.h>

/* B2: the driver vtable contract, exercised through a recording SIGNAL driver.
 *
 *   - now: the runtime adopts the driver clock (now_calls > 0) and a sleep
 *     measured on it completes on time;
 *   - wake: the runtime raises the doorbell on spawn / park / wake
 *     (wake_calls > 0);
 *   - timer_arm: receives the sleep's absolute deadline on the same scale as
 *     now, and firing the alarm resumes the sleep (arm_calls > 0, on time);
 *   - timer_cancel: a cancelled park's alarm never fires (fire_calls == 0);
 *   - the default path (no driver attached) is untouched: no callback runs.
 *
 * The vtable is a copied artifact: the suite registers a driver and attaches it
 * to a live runtime; the hot-plug suite (test_rt_driver_hotplug) covers the
 * attach/detach transitions.
 */

#define SLEEP_MS 60u

static volatile uint64_t g_done;
static volatile uint64_t g_sleep_ok;

static void sleeper(void* arg) {
    coro_rt_t* rt = (coro_rt_t*)arg;
    coro_status st = coro_rt_sleep(rt, coro_monotonic_ns() + SLEEP_MS * 1000000ull);
    if (st == CORO_OK) coro_at_store((volatile uint64_t*)&g_sleep_ok, 1);
    coro_at_store((volatile uint64_t*)&g_done, 1);
}

static void slow_sleeper(void* arg) {
    coro_rt_t* rt = (coro_rt_t*)arg;
    coro_status st = coro_rt_sleep(rt, coro_monotonic_ns() + 300000000ull); /* 300 ms */
    if (st == CORO_OK) coro_at_store((volatile uint64_t*)&g_sleep_ok, 1);
    coro_at_store((volatile uint64_t*)&g_done, 1);
}

/* ---- now / wake / timer_arm, and an on-time sleep ---- */

static void test_signal_vtable(void) {
    fake_driver f;
    coro_rt_driver_t* d = fake_start("vtab", &f, 0);
    CHECK(d != NULL);
    coro_rt_t* rt = coro_rt_create(2);
    CHECK(rt != NULL);

    CHECK(coro_rt_attached_driver(rt) == NULL); /* pointer: CHECK, not CHECK_EQ */
    CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_OK);
    CHECK(coro_rt_attached_driver(rt) == d);

    coro_at_store((volatile uint64_t*)&g_done, 0);
    coro_at_store((volatile uint64_t*)&g_sleep_ok, 0);
    uint64_t t0 = coro_monotonic_ns();
    CHECK_EQ(coro_rt_spawn(rt, NULL, sleeper, rt), CORO_OK);
    CHECK(fake_wait_flag(&g_done, 2000));
    uint64_t dt = coro_monotonic_ns() - t0;

    CHECK_EQ(coro_at_load(&g_sleep_ok), 1);
    CHECK(dt >= (SLEEP_MS - 15u) * 1000000ull);   /* did not return early */
    CHECK(dt <= 800ull * 1000000ull);             /* and did return */
    CHECK(coro_at_load(&f.now_calls) > 0);        /* rt_now dispatched to now */
    CHECK(coro_at_load(&f.wake_calls) > 0);       /* doorbell raised */
    CHECK(coro_at_load(&f.arm_calls) > 0);        /* sleep armed on the driver */

    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_detach_driver(rt), CORO_OK);
    CHECK(coro_rt_attached_driver(rt) == NULL); /* pointer: CHECK, not CHECK_EQ */
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_OK);
    fake_stop(&f);
}

/* ---- timer_arm received the sleep's deadline, same scale as now ---- */

static void test_timer_deadline(void) {
    fake_driver f;
    coro_rt_driver_t* d = fake_start("vtab-deadline", &f, 0);
    CHECK(d != NULL);
    coro_rt_t* rt = coro_rt_create(1);
    CHECK(rt != NULL);
    CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_OK);

    coro_at_store((volatile uint64_t*)&g_done, 0);
    uint64_t t0 = coro_monotonic_ns();
    CHECK_EQ(coro_rt_spawn(rt, NULL, sleeper, rt), CORO_OK);
    CHECK(fake_wait_flag(&g_done, 2000));

    uint64_t armed = coro_at_load(&f.last_deadline);
    /* The armed deadline is the caller's target (t0 + SLEEP_MS), within a wide
       window that allows for spawn/arm latency but proves the same scale. */
    CHECK(armed >= t0 + (SLEEP_MS - 20u) * 1000000ull);
    CHECK(armed <= t0 + (SLEEP_MS + 200u) * 1000000ull);

    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_OK);
    fake_stop(&f);
}

/* ---- a cancelled park's alarm never fires ---- */

static void test_cancel_no_fire(void) {
    fake_driver f;
    coro_rt_driver_t* d = fake_start("vtab-cancel", &f, 0);
    CHECK(d != NULL);
    coro_rt_t* rt = coro_rt_create(2);
    CHECK(rt != NULL);
    CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_OK);

    coro_at_store((volatile uint64_t*)&g_done, 0);
    coro_at_store((volatile uint64_t*)&g_sleep_ok, 0);
    coro_t* h = NULL;
    CHECK_EQ(coro_rt_spawn_h(rt, NULL, slow_sleeper, rt, &h), CORO_OK);
    CHECK(h != NULL);

    /* Wait until the sleep has been armed on the driver. */
    unsigned guard = 0;
    while (coro_at_load(&f.arm_calls) == 0 && guard < 2000) {
        fake_sleep_ms(1);
        ++guard;
    }
    CHECK(coro_at_load(&f.arm_calls) > 0);

    CHECK_EQ(coro_rt_cancel(rt, h), CORO_OK);
    /* Let the worker observe the cancel and retire the park, then wait past the
       original 300 ms deadline: the alarm must have been cancelled, so fn
       never ran. */
    fake_sleep_ms(400);
    CHECK_EQ(coro_at_load(&f.fire_calls), 0);
    CHECK_EQ(coro_at_load(&g_sleep_ok), 0); /* never completed the full sleep */

    for (unsigned i = 0; i < 2000 && coro_rt_collect(rt, h) != CORO_OK; ++i) {
        fake_sleep_ms(1);
    }
    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_OK);
    fake_stop(&f);
}

/* ---- the default path runs no driver callback ---- */

static void test_default_identity(void) {
    fake_driver f;
    coro_rt_driver_t* d = fake_start("vtab-idle", &f, 0);
    CHECK(d != NULL);
    coro_rt_t* rt = coro_rt_create(2);
    CHECK(rt != NULL);

    coro_at_store((volatile uint64_t*)&g_done, 0);
    CHECK_EQ(coro_rt_spawn(rt, NULL, sleeper, rt), CORO_OK);
    CHECK(fake_wait_flag(&g_done, 2000));
    CHECK_EQ(coro_at_load(&g_done), 1);
    /* No attach: none of the callbacks ran. */
    CHECK_EQ(coro_at_load(&f.now_calls), 0);
    CHECK_EQ(coro_at_load(&f.wake_calls), 0);
    CHECK_EQ(coro_at_load(&f.arm_calls), 0);

    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_OK);
    fake_stop(&f);
}

static void run_tests(void) {
    test_signal_vtable();
    test_timer_deadline();
    test_cancel_no_fire();
    test_default_identity();
}

TEST_MAIN()
