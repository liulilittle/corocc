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
    CHECK(coro_rt_attached_driver(rt) == NULL); /* pointer: CHECK, not CHECK_EQ */
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

/* ---- LOOP attach on a live worker runtime now succeeds (B5 hot switch) ---- */

static void test_loop_attach_live(void) {
    coro_rt_t* rt = coro_rt_create(2);
    CHECK(rt != NULL);
    g_rt = rt;

    fake_driver f;
    coro_rt_driver_t* d = fake_start("loop-live", &f, CORO_DRIVER_FLAG_LOOP);
    CHECK(d != NULL);

    /* Idle runtime: the workers quiesce promptly and the switch completes. */
    CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_OK);
    CHECK(coro_rt_attached_driver(rt) == d);
    CHECK_EQ(coro_rt_detach_driver(rt), CORO_OK); /* release back to worker mode */
    CHECK(coro_rt_attached_driver(rt) == NULL);

    /* And while a coroutine is actively sleeping (a worker is driving). */
    coro_at_store((volatile uint64_t*)&g_completed, 0);
    CHECK_EQ(coro_rt_spawn(rt, NULL, ms_sleeper, (void*)(uintptr_t)40), CORO_OK);
    CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_OK); /* quiesce, then LOOP */
    CHECK(coro_rt_attached_driver(rt) == d);
    CHECK_EQ(coro_rt_detach_driver(rt), CORO_OK); /* release, sleep completes */

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

    CHECK(coro_rt_attached_driver(rt) == NULL); /* pointer: CHECK, not CHECK_EQ */
    CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_OK);
    CHECK(coro_rt_attached_driver(rt) == d);
    CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_OK); /* idempotent same driver */
    CHECK_EQ(coro_rt_detach_driver(rt), CORO_OK);
    CHECK(coro_rt_attached_driver(rt) == NULL); /* pointer: CHECK, not CHECK_EQ */
    CHECK_EQ(coro_rt_detach_driver(rt), CORO_OK); /* idempotent */
    CHECK_EQ(coro_rt_attach_driver(NULL, d), CORO_EINVAL);

    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_OK);
    fake_stop(&f);
}

/* ---- attach immediately after create, before any worker parks ---- */

static void test_attach_right_after_create(void) {
    fake_driver f;
    coro_rt_driver_t* d = fake_start("hotplug-fresh", &f, 0);
    CHECK(d != NULL);

    /* No spawn and no sleep: the workers have not parked anywhere. attach must
       succeed at once. A same-family switch re-arms alarms in place and leans
       on each worker's generation self-heal, so it never waits for a worker to
       reach a barrier -- that wall-clock wait is exactly what timed out on an
       emulated host and turned this attach into CORO_EBUSY. */
    for (int i = 0; i < 50; ++i) {
        coro_rt_t* rt = coro_rt_create(2);
        CHECK(rt != NULL);
        CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_OK);
        CHECK(coro_rt_attached_driver(rt) == d);
        CHECK_EQ(coro_rt_detach_driver(rt), CORO_OK);
        CHECK(coro_rt_attached_driver(rt) == NULL);
        coro_rt_wait_all(rt);
        CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    }

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

/* ---- B5 cross-class hot switches (LOOP <-> worker mode) ----
 *
 * The runtime is a live worker pool when a LOOP driver is attached: every
 * worker quiesces at the barrier and the host thread drives worker[0] through
 * coro_rt_pump. In-flight timed parks must survive both directions: entering
 * LOOP cancels outstanding alarms but leaves the timer heaps untouched, and the
 * pump's sweep of the parked peers keeps a multi-worker runtime on time;
 * leaving LOOP releases the workers, which re-arm their own alarms.
 */

/* ---- SIGNAL -> LOOP: in-flight timed sleeps finish under the host pump ---- */

static void test_crossclass_signal_to_loop(void) {
    coro_rt_t* rt = coro_rt_create(2);
    CHECK(rt != NULL);
    g_rt = rt;

    fake_driver f;
    coro_rt_driver_t* d =
        fake_start("xclass-into-loop", &f, CORO_DRIVER_FLAG_LOOP);
    CHECK(d != NULL);

    /* Several parks, distributed round-robin over both workers: the pump must
       sweep the peer worker's heap, not just worker[0]'s. */
    coro_at_store((volatile uint64_t*)&g_completed, 0);
    const uint64_t n = 10;
    uint64_t t0 = coro_monotonic_ns();
    for (uintptr_t i = 0; i < n; ++i) {
        CHECK_EQ(coro_rt_spawn(rt, NULL, ms_sleeper,
                               (void*)(uintptr_t)(30 + i * 4)),
                 CORO_OK);
    }
    fake_sleep_ms(4); /* let them reach their parks */

    CHECK_EQ(coro_rt_attach_driver(rt, d), CORO_OK); /* quiesce, then LOOP */
    CHECK(coro_rt_attached_driver(rt) == d);

    /* No worker threads drive now: the host pumps worker[0] by hand. */
    unsigned guard = 0;
    while (coro_at_load(&g_completed) < n && guard < 3000) {
        CHECK_EQ(coro_rt_pump(rt, 64), CORO_OK);
        fake_sleep_ms(1);
        ++guard;
    }
    uint64_t dt = coro_monotonic_ns() - t0;
    CHECK_EQ(coro_at_load(&g_completed), n);
    /* Every deadline (30..66 ms) fired on the pump, none before its time. */
    CHECK(dt >= 25ull * 1000000ull);
    CHECK(dt <= 2000ull * 1000000ull);

    CHECK_EQ(coro_rt_detach_driver(rt), CORO_OK); /* LOOP -> worker mode */
    CHECK(coro_rt_attached_driver(rt) == NULL);
    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_OK);
    fake_stop(&f);
}

/* ---- LOOP -> SIGNAL: a sleep parked under the pump finishes in thread mode ---- */

static void test_crossclass_loop_to_signal(void) {
    coro_rt_t* rt = coro_rt_create(2);
    CHECK(rt != NULL);
    g_rt = rt;

    fake_driver fl;
    coro_rt_driver_t* dl =
        fake_start("xclass-out-loop", &fl, CORO_DRIVER_FLAG_LOOP);
    CHECK(dl != NULL);
    fake_driver fs;
    coro_rt_driver_t* ds = fake_start("xclass-out-signal", &fs, 0);
    CHECK(ds != NULL);

    CHECK_EQ(coro_rt_attach_driver(rt, dl), CORO_OK); /* into LOOP */

    coro_at_store((volatile uint64_t*)&g_completed, 0);
    CHECK_EQ(coro_rt_spawn(rt, NULL, ms_sleeper, (void*)(uintptr_t)80), CORO_OK);
    /* Pump enough to move the sleeper onto worker[0]'s heap and park it. */
    for (int i = 0; i < 5 && coro_at_load(&g_completed) == 0; ++i) {
        CHECK_EQ(coro_rt_pump(rt, 64), CORO_OK);
        fake_sleep_ms(1);
    }
    CHECK_EQ(coro_at_load(&g_completed), 0); /* still in flight */

    CHECK_EQ(coro_rt_attach_driver(rt, ds), CORO_OK); /* back to thread mode */
    CHECK(coro_rt_attached_driver(rt) == ds);
    CHECK(wait_completed(1, 4000));
    CHECK_EQ(coro_at_load(&g_completed), 1);

    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(dl), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(ds), CORO_OK);
    fake_stop(&fl);
    fake_stop(&fs);
}

/* ---- bounce LOOP <-> worker mode under load ---- */

static void test_crossclass_bounce(void) {
    coro_rt_t* rt = coro_rt_create(2);
    CHECK(rt != NULL);
    g_rt = rt;

    fake_driver fl;
    coro_rt_driver_t* dl =
        fake_start("xclass-bounce-loop", &fl, CORO_DRIVER_FLAG_LOOP);
    CHECK(dl != NULL);

    coro_at_store((volatile uint64_t*)&g_completed, 0);
    const uint64_t per = 4;
    const int rounds = 8;
    uint64_t expected = 0;
    for (int r = 0; r < rounds; ++r) {
        /* Into LOOP: quiesce, spawn, drive every sleep to completion. */
        CHECK_EQ(coro_rt_attach_driver(rt, dl), CORO_OK);
        for (uintptr_t i = 0; i < per; ++i) {
            CHECK_EQ(coro_rt_spawn(rt, NULL, ms_sleeper,
                                   (void*)(uintptr_t)(2 + (r % 3) * 2)),
                     CORO_OK);
        }
        expected += per;
        unsigned guard = 0;
        while (coro_at_load(&g_completed) < expected && guard < 4000) {
            CHECK_EQ(coro_rt_pump(rt, 64), CORO_OK);
            fake_sleep_ms(1);
            ++guard;
        }
        CHECK_EQ(coro_at_load(&g_completed), expected);

        /* Back to worker mode: spawn into the pool, let the threads finish. */
        CHECK_EQ(coro_rt_detach_driver(rt), CORO_OK);
        for (uintptr_t i = 0; i < per; ++i) {
            CHECK_EQ(coro_rt_spawn(rt, NULL, ms_sleeper,
                                   (void*)(uintptr_t)(2 + (r % 3) * 2)),
                     CORO_OK);
        }
        expected += per;
        CHECK(wait_completed(expected, 4000));
    }
    CHECK_EQ(coro_at_load(&g_completed), expected);

    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(dl), CORO_OK);
    fake_stop(&fl);
}

/* ---- a pump owns worker[0]: moving in or out of LOOP is refused ---- */

static void busy_coro(void* arg) {
    (void)arg;
    coro_at_store((volatile uint64_t*)&g_completed, 1); /* re-use as "in" */
    while (coro_at_load(&g_done) == 0) { /* spin until released */ }
}

static void* pump_thread_fn(void* arg) {
    coro_rt_t* rt = (coro_rt_t*)arg;
    (void)coro_rt_pump(rt, 0);
    return NULL;
}

static void test_crossclass_pump_ebusy(void) {
    fake_driver fl;
    coro_rt_driver_t* dl =
        fake_start("xclass-ebusy-loop", &fl, CORO_DRIVER_FLAG_LOOP);
    CHECK(dl != NULL);
    CHECK_EQ(coro_rt_driver_set_default(dl), CORO_OK);

    coro_rt_t* rt = coro_rt_create(2); /* LOOP default: no worker threads */
    CHECK(rt != NULL);
    g_rt = rt;

    fake_driver fs;
    coro_rt_driver_t* ds = fake_start("xclass-ebusy-signal", &fs, 0);
    CHECK(ds != NULL);

    coro_at_store((volatile uint64_t*)&g_completed, 0);
    coro_at_store((volatile uint64_t*)&g_done, 0);
    CHECK_EQ(coro_rt_spawn(rt, NULL, busy_coro, NULL), CORO_OK);

    pt_thread_t th;
    CHECK_EQ(pt_thread_create(&th, pump_thread_fn, rt), 0);
    CHECK(fake_wait_flag(&g_completed, 2000)); /* the pump is inside rt_run_some */

    /* A running pump owns worker[0]; both directions are refused and the
       attached driver is unchanged. */
    CHECK_EQ(coro_rt_attach_driver(rt, ds), CORO_EBUSY);
    CHECK_EQ(coro_rt_detach_driver(rt), CORO_EBUSY);
    CHECK(coro_rt_attached_driver(rt) == dl);

    coro_at_store((volatile uint64_t*)&g_done, 1); /* let the pump finish */
    pt_thread_join(th);

    /* Pump idle again: the switch succeeds and starts the worker threads this
       LOOP-created runtime never had. */
    CHECK_EQ(coro_rt_attach_driver(rt, ds), CORO_OK);
    CHECK(coro_rt_attached_driver(rt) == ds);
    coro_rt_wait_all(rt);
    CHECK_EQ(coro_rt_destroy(rt), CORO_OK);

    CHECK_EQ(coro_rt_driver_set_default(NULL), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(dl), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(ds), CORO_OK);
    fake_stop(&fl);
    fake_stop(&fs);
}

static void run_tests(void) {
    test_attach_running();
    test_attach_then_detach();
    test_stress();
    test_cancel_after_attach();
    test_loop_attach_live();
    test_attached_query();
    test_attach_right_after_create();
    test_pump_loop();
    test_pump_signal_ebusy();
    test_crossclass_signal_to_loop();
    test_crossclass_loop_to_signal();
    test_crossclass_bounce();
    test_crossclass_pump_ebusy();
}

TEST_MAIN()
