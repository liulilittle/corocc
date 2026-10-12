#include "../harness.h"
#include <corocc/coro.h>

/* Concurrency suites, designed to run under ThreadSanitizer.
 *
 * Double-resume rejection (spec 12): several threads present the same
 * credential at once. Exactly one execution flow may enter the suspension
 * point, every other attempt is rejected with a status code, and no data race
 * is reported while they race. With manual-mode resume every caller drives on
 * its own thread, so the race is a genuine CAS contest.
 */

#if defined(_WIN32)
static void run_tests(void) {
    CHECK(0 && "concurrency suites: POSIX threads only in M3");
}
TEST_MAIN()
#else

#include <pthread.h>
#include <string.h>

#define RACERS 8u
#define ROUNDS 500u

static coro_t*     g_c;
static coro_token  g_live;
static unsigned    g_rounds;
static coro_status g_rc[RACERS];
static coro_token  g_out[RACERS];
static pthread_mutex_t g_gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_gate_cond = PTHREAD_COND_INITIALIZER;
static int             g_gate_open;

static void entry_count_rounds(void* arg) {
    (void)arg;
    while (coro_yield() == CORO_OK) {
        ++g_rounds;
    }
}

static void* racer(void* pv) {
    unsigned slot = (unsigned)(uintptr_t)pv;
    pthread_mutex_lock(&g_gate_lock);
    while (!g_gate_open) pthread_cond_wait(&g_gate_cond, &g_gate_lock);
    pthread_mutex_unlock(&g_gate_lock);
    g_rc[slot] = coro_resume(g_c, g_live, &g_out[slot]);
    return NULL;
}

/* Open the gate to `count` racers all holding the same credential, join them. */
static void run_racers(unsigned count) {
    pthread_t t[RACERS];
    pthread_mutex_lock(&g_gate_lock);
    g_gate_open = 0;
    pthread_mutex_unlock(&g_gate_lock);
    memset(g_rc, 0, sizeof g_rc);
    memset(g_out, 0, sizeof g_out);
    for (unsigned i = 0; i < count; ++i) {
        CHECK(pthread_create(&t[i], NULL, racer, (void*)(uintptr_t)i) == 0);
    }
    pthread_mutex_lock(&g_gate_lock);
    g_gate_open = 1;
    pthread_cond_broadcast(&g_gate_cond);
    pthread_mutex_unlock(&g_gate_lock);
    for (unsigned i = 0; i < count; ++i) pthread_join(t[i], NULL);
}

static void run_tests(void) {
    coro_token next = CORO_TOKEN_NONE;

    /* ---------- double resume rejection ---------- */
    CHECK(coro_create(&g_c, NULL, entry_count_rounds, NULL) == CORO_OK);
    CHECK(coro_start(g_c, &g_live) == CORO_OK);
    CHECK(g_rounds == 0u);

    run_racers(RACERS);
    {
        int ok = 0;
        for (unsigned i = 0; i < RACERS; ++i) {
            if (g_rc[i] == CORO_OK) {
                ++ok;
                g_live = g_out[i];
            } else {
                CHECK(g_rc[i] == CORO_EBUSY || g_rc[i] == CORO_ESTALE);
            }
        }
        CHECK(ok <= 1);        /* mutual exclusion of drives */
        CHECK(g_rounds <= 1u); /* the body ran at most once */
    }
    /* the coroutine is still consistent and drivable by the main thread */
    CHECK(coro_cancel(g_c) == CORO_OK);
    CHECK(coro_resume(g_c, g_live, &next) == CORO_OK);
    CHECK(next == CORO_TOKEN_DEAD);
    CHECK(coro_destroy(g_c) == CORO_OK);

    /* ---------- repeated races, fixed count ---------- */
    CHECK(coro_create(&g_c, NULL, entry_count_rounds, NULL) == CORO_OK);
    CHECK(coro_start(g_c, &g_live) == CORO_OK);
    g_rounds = 0;
    for (unsigned r = 0; r < ROUNDS; ++r) {
        run_racers(RACERS);
        int ok = 0;
        for (unsigned i = 0; i < RACERS; ++i) {
            if (g_rc[i] == CORO_OK) {
                ++ok;
                g_live = g_out[i];
            } else {
                CHECK(g_rc[i] == CORO_EBUSY || g_rc[i] == CORO_ESTALE);
            }
        }
        CHECK(ok <= 1);
    }
    /* every won race advanced the body exactly once */
    CHECK(g_rounds == ROUNDS);
    coro_cancel(g_c);
    CHECK(coro_resume(g_c, g_live, &next) == CORO_OK);
    CHECK(next == CORO_TOKEN_DEAD);
    CHECK(coro_destroy(g_c) == CORO_OK);
}

TEST_MAIN()
#endif
