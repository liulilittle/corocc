#include "../harness.h"
#include "../palt_threads.h"
#include <corocc/coro_rt_driver.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* B1: the process-level runtime-driver registry.
 *
 * The runtime driver layer (design v1, milestone B) decouples the runtime from
 * its host on three needs -- doorbell (wake), alarm (timer) and clock (now).
 * B1 lands only the registry and the default-identity dispatch seams; attach,
 * detach and pump arrive in B2. This suite pins the registry contract that the
 * later phases build on:
 *
 *   - register / find (named and anonymous), rejecting malformed ops;
 *   - duplicate names are refused with CORO_ESTATE (ruling #2; a status-
 *     returning register is what makes that observable);
 *   - concurrent register / find / unregister does not corrupt the list;
 *   - unregister is refused (CORO_EBUSY) while the driver is in use -- in B1 the
 *     reachable "in use" state is the process default, which is exactly what
 *     coro_rt_create reads and keeps a raw reference to;
 *   - unregister(NULL) is idempotent; set_default(NULL) resets to the built-in
 *     EDSM path (NULL), and default() reports it consistently.
 *
 * The vtable call contract and hot-plug semantics live in the B2 suites. */

static void     fake_wake(void* ctx) { (void)ctx; }
static int      fake_timer_arm(void* ctx, void* req, uint64_t deadline,
                               coro_driver_timer_fn fn, void* arg) {
    (void)ctx; (void)req; (void)deadline; (void)fn; (void)arg;
    return 0;
}
static int      fake_timer_cancel(void* ctx, void* req) {
    (void)ctx; (void)req;
    return 0;
}

/* A well-formed ops block carrying only the mandatory wake. */
static struct coro_rt_driver_ops good_ops(void) {
    struct coro_rt_driver_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.struct_size = (uint32_t)sizeof ops;
    ops.wake = fake_wake;
    return ops;
}

/* ---- 1. register / find / anonymous ---- */

static void test_register_find(void) {
    struct coro_rt_driver_ops ops = good_ops();
    coro_rt_driver_t* d1 = NULL;
    coro_rt_driver_t* anon = NULL;

    CHECK_EQ(coro_rt_driver_register("alpha", &ops, NULL, &d1), CORO_OK);
    CHECK(d1 != NULL);
    CHECK(coro_rt_driver_find("alpha") == d1);

    /* A second, distinct name is independent. */
    coro_rt_driver_t* d2 = NULL;
    CHECK_EQ(coro_rt_driver_register("beta", &ops, NULL, &d2), CORO_OK);
    CHECK(d2 != NULL);
    CHECK(coro_rt_driver_find("alpha") == d1);
    CHECK(coro_rt_driver_find("beta") == d2);

    /* Anonymous registration (name == NULL) succeeds but is not findable. */
    CHECK_EQ(coro_rt_driver_register(NULL, &ops, NULL, &anon), CORO_OK);
    CHECK(anon != NULL);
    CHECK(coro_rt_driver_find(NULL) == NULL);
    CHECK(coro_rt_driver_find("alpha") == d1); /* unaffected */

    /* A miss is NULL. */
    CHECK(coro_rt_driver_find("no-such-driver") == NULL);

    CHECK_EQ(coro_rt_driver_unregister(d1), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d2), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(anon), CORO_OK);
    CHECK(coro_rt_driver_find("alpha") == NULL);
}

/* ---- 2. malformed ops are refused ---- */

static void test_register_validation(void) {
    coro_rt_driver_t* out = (coro_rt_driver_t*)(void*)0x1; /* sentinel */

    /* NULL out pointer. */
    struct coro_rt_driver_ops ops = good_ops();
    CHECK_EQ(coro_rt_driver_register("v", &ops, NULL, NULL), CORO_EINVAL);

    /* NULL ops. */
    CHECK_EQ(coro_rt_driver_register("v", NULL, NULL, &out), CORO_EINVAL);
    CHECK(out == NULL);

    /* Missing mandatory wake. */
    ops = good_ops();
    ops.wake = NULL;
    CHECK_EQ(coro_rt_driver_register("v", &ops, NULL, &out), CORO_EINVAL);
    CHECK(out == NULL);

    /* struct_size smaller than the known prefix. */
    ops = good_ops();
    ops.struct_size = (uint32_t)offsetof(struct coro_rt_driver_ops, wake);
    CHECK_EQ(coro_rt_driver_register("v", &ops, NULL, &out), CORO_EINVAL);
    CHECK(out == NULL);

    /* struct_size larger than this build's struct is forward-compatible. */
    ops = good_ops();
    ops.struct_size = (uint32_t)sizeof ops + 64u;
    coro_rt_driver_t* fwd = NULL;
    CHECK_EQ(coro_rt_driver_register("v", &ops, NULL, &fwd), CORO_OK);
    CHECK(fwd != NULL);
    CHECK_EQ(coro_rt_driver_unregister(fwd), CORO_OK);
}

/* ---- 3. timer_arm without timer_cancel is refused ---- */

static void test_timer_pairing(void) {
    struct coro_rt_driver_ops ops = good_ops();
    coro_rt_driver_t* out = NULL;

    ops.timer_arm = fake_timer_arm;
    ops.timer_cancel = NULL;
    CHECK_EQ(coro_rt_driver_register("t-mismatch", &ops, NULL, &out),
             CORO_EINVAL);
    CHECK(out == NULL);

    ops.timer_cancel = fake_timer_cancel;
    CHECK_EQ(coro_rt_driver_register("t-pair", &ops, NULL, &out), CORO_OK);
    CHECK(out != NULL);
    CHECK_EQ(coro_rt_driver_unregister(out), CORO_OK);
}

/* ---- 4. duplicate names are refused with CORO_ESTATE (ruling #2) ---- */

static void test_duplicate_name(void) {
    struct coro_rt_driver_ops ops = good_ops();
    coro_rt_driver_t* first = NULL;
    coro_rt_driver_t* second = (coro_rt_driver_t*)(void*)0x1;

    CHECK_EQ(coro_rt_driver_register("dup", &ops, NULL, &first), CORO_OK);
    CHECK(first != NULL);
    CHECK_EQ(coro_rt_driver_register("dup", &ops, NULL, &second), CORO_ESTATE);
    CHECK(second == NULL);
    /* The first registration is untouched and still found by name. */
    CHECK(coro_rt_driver_find("dup") == first);

    CHECK_EQ(coro_rt_driver_unregister(first), CORO_OK);
    /* After the first is gone the name is free again. */
    CHECK_EQ(coro_rt_driver_register("dup", &ops, NULL, &second), CORO_OK);
    CHECK(second != NULL);
    CHECK_EQ(coro_rt_driver_unregister(second), CORO_OK);
}

/* ---- 5. concurrent register / find / unregister ---- */

#define CONC_THREADS 4
#define CONC_ROUNDS  200

typedef struct conc_result {
    int tid;
    int bad;
} conc_result;

static void* conc_worker(void* arg) {
    conc_result* r = (conc_result*)arg;
    struct coro_rt_driver_ops ops = good_ops();
    char name[48];

    for (int i = 0; i < CONC_ROUNDS; ++i) {
        coro_rt_driver_t* d = NULL;
        snprintf(name, sizeof name, "conc-%d-%d", r->tid, i);
        if (coro_rt_driver_register(name, &ops, NULL, &d) != CORO_OK ||
            d == NULL) {
            r->bad++;
            continue;
        }
        if (coro_rt_driver_find(name) != d) r->bad++;
        if (coro_rt_driver_unregister(d) != CORO_OK) r->bad++;
    }
    return NULL;
}

static void test_concurrent_registry(void) {
    pt_thread_t threads[CONC_THREADS];
    conc_result results[CONC_THREADS];
    int started = 0;

    for (int i = 0; i < CONC_THREADS; ++i) {
        results[i].tid = i;
        results[i].bad = 0;
        if (pt_thread_create(&threads[i], conc_worker, &results[i]) != 0) break;
        ++started;
    }
    for (int i = 0; i < started; ++i) pt_thread_join(threads[i]);
    CHECK_EQ(started, CONC_THREADS);
    for (int i = 0; i < CONC_THREADS; ++i) CHECK_EQ(results[i].bad, 0);

    /* Every worker unregistered its own entry: the registry is drained. */
    CHECK(coro_rt_driver_find("conc-0-0") == NULL);
    CHECK(coro_rt_driver_find("conc-3-199") == NULL);
}

/* ---- 6. unregister while in use; idempotent NULL ---- */

static void test_unregister_in_use(void) {
    struct coro_rt_driver_ops ops = good_ops();
    coro_rt_driver_t* d = NULL;

    CHECK_EQ(coro_rt_driver_register("busy", &ops, NULL, &d), CORO_OK);
    CHECK(d != NULL);

    /* In use as the process default: unregister must refuse. */
    CHECK_EQ(coro_rt_driver_set_default(d), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_EBUSY);
    CHECK(coro_rt_driver_find("busy") == d); /* still registered */

    /* Released from use: the same unregister now succeeds. */
    CHECK_EQ(coro_rt_driver_set_default(NULL), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_OK);
    CHECK(coro_rt_driver_find("busy") == NULL);

    /* NULL is accepted and ignored (idempotent). */
    CHECK_EQ(coro_rt_driver_unregister(NULL), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(NULL), CORO_OK);
}

/* ---- 7. set_default / default consistency ---- */

static void test_default(void) {
    struct coro_rt_driver_ops ops = good_ops();
    coro_rt_driver_t* d = NULL;

    /* The registry starts on the built-in EDSM path: no default driver. */
    CHECK(coro_rt_driver_default() == NULL);

    CHECK_EQ(coro_rt_driver_register("def", &ops, NULL, &d), CORO_OK);
    CHECK(d != NULL);
    CHECK_EQ(coro_rt_driver_set_default(d), CORO_OK);
    CHECK(coro_rt_driver_default() == d);

    /* Reset to the built-in path. */
    CHECK_EQ(coro_rt_driver_set_default(NULL), CORO_OK);
    CHECK(coro_rt_driver_default() == NULL);

    /* A pointer that is not a registered instance is refused (no deref). */
    CHECK_EQ(coro_rt_driver_set_default(d), CORO_OK); /* still registered */
    CHECK_EQ(coro_rt_driver_set_default(NULL), CORO_OK);
    CHECK_EQ(coro_rt_driver_unregister(d), CORO_OK);
    CHECK_EQ(coro_rt_driver_set_default(d), CORO_EINVAL);
    CHECK(coro_rt_driver_default() == NULL);
}

static void run_tests(void) {
    test_register_find();
    test_register_validation();
    test_timer_pairing();
    test_duplicate_name();
    test_concurrent_registry();
    test_unregister_in_use();
    test_default();
}

TEST_MAIN()
