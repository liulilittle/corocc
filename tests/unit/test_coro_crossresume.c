#include "../harness.h"
#include <corocc/coro.h>

/* Milestone M3 Task 3: cross-thread resume delivers to the owner.
 *
 * The behaviours that make the delivery path trustworthy:
 *   - a request from a foreign thread is served by the owner, and the foreign
 *     caller learns the credential the owner published;
 *   - the same credential presented by two threads at once executes the
 *     coroutine body exactly once;
 *   - a stale credential is refused and nothing runs;
 *   - a coroutine that already died reports that instead of waking anybody.
 */

#if defined(_WIN32)
static void run_tests(void) {
    CHECK(0 && "cross-thread suites: POSIX threads only in M3");
}
TEST_MAIN()
#else

#include <pthread.h>
#include <string.h>

#define WAKERS 4u

/* A gate the owner opens once every caller of a phase is running, so all the
   requests of a phase are posted together. Waiting on it is a condition
   variable, never a spin. */
static pthread_mutex_t g_gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_gate_cond = PTHREAD_COND_INITIALIZER;
static int             g_gate_open;

static coro_t*     g_c;
static coro_token  g_live;              /**< credential the coroutine published */
static unsigned    g_rounds;
static coro_status g_wrc[WAKERS];       /**< what each foreign caller received */
static coro_token  g_wout[WAKERS];

static void entry_count_rounds(void* arg) {
    (void)arg;
    while (coro_yield() == CORO_OK) {
        ++g_rounds;
    }
}

/* Asks for one wakeup, the way any foreign thread does. */
static void* waker(void* pv) {
    unsigned slot = (unsigned)(uintptr_t)pv;
    pthread_mutex_lock(&g_gate_lock);
    while (!g_gate_open) {
        pthread_cond_wait(&g_gate_cond, &g_gate_lock);
    }
    pthread_mutex_unlock(&g_gate_lock);
    g_wrc[slot] = coro_resume(g_c, g_live, &g_wout[slot]);
    return NULL;
}

/* Starts `count` foreign callers, releases them together, and serves their
   requests from the owning thread. */
static void run_wakers(unsigned count) {
    pthread_t t[WAKERS];
    int served = 0;
    CHECK(count >= 1u && count <= WAKERS);

    pthread_mutex_lock(&g_gate_lock);
    g_gate_open = 0;
    pthread_mutex_unlock(&g_gate_lock);
    memset(g_wrc, 0, sizeof g_wrc);
    memset(g_wout, 0, sizeof g_wout);
    for (unsigned i = 0; i < count; ++i) {
        CHECK(pthread_create(&t[i], NULL, waker, (void*)(uintptr_t)i) == 0);
    }
    pthread_mutex_lock(&g_gate_lock);
    g_gate_open = 1;
    pthread_cond_broadcast(&g_gate_cond);
    pthread_mutex_unlock(&g_gate_lock);

    while (served < (int)count) {
        int n = (int)coro_pump();
        if (n > 0) served += n; else served += (int)coro_pump_wait();
    }
    for (unsigned i = 0; i < count; ++i) {
        pthread_join(t[i], NULL);
    }
}

static void run_tests(void) {
    coro_token next = CORO_TOKEN_NONE;

    CHECK(coro_create(&g_c, NULL, entry_count_rounds, NULL) == CORO_OK);
    CHECK(coro_start(g_c, &g_live) == CORO_OK);
    CHECK(g_rounds == 0u);

    /* ---------- a foreign wakeup is served by the owner ---------- */
    run_wakers(1);
    CHECK(g_wrc[0] == CORO_OK);
    CHECK(g_wout[0] != CORO_TOKEN_NONE);
    CHECK(g_wout[0] != CORO_TOKEN_DEAD);
    CHECK(g_rounds == 1u); /* the coroutine advanced exactly once */
    CHECK(coro_get_state(g_c) == CORO_SUSPENDED);
    g_live = g_wout[0];    /* the credential the foreign caller learned */

    /* ---------- the owner keeps driving inline ---------- */
    CHECK(coro_resume(g_c, g_live, &next) == CORO_OK);
    CHECK(g_rounds == 2u);
    g_live = next;

    /* ---------- a stale credential is refused, nothing runs ---------- */
    CHECK(coro_resume(g_c, g_live + 1000u, &next) == CORO_ESTALE);
    CHECK(coro_resume(g_c, CORO_TOKEN_DEAD, &next) == CORO_EDESTROYED);
    CHECK(g_rounds == 2u);

    /* ---------- the same credential twice: one execution ---------- */
    run_wakers(2);
    {
        int ok = 0;
        for (unsigned i = 0; i < 2u; ++i) {
            if (g_wrc[i] == CORO_OK) {
                ++ok;
                CHECK(g_wout[i] != CORO_TOKEN_NONE);
                CHECK(g_wout[i] != CORO_TOKEN_DEAD);
            } else {
                CHECK(g_wrc[i] == CORO_EBUSY || g_wrc[i] == CORO_ESTALE);
            }
        }
        CHECK(ok <= 1);
        for (unsigned i = 0; i < 2u; ++i) {
            if (g_wrc[i] == CORO_OK) g_live = g_wout[i];
        }
    }
    CHECK(g_rounds == 3u); /* the body ran exactly once more */

    /* ---------- a dead coroutine is reported as dead ---------- */
    coro_cancel(g_c);
    CHECK(coro_resume(g_c, g_live, &next) == CORO_OK);
    CHECK(next == CORO_TOKEN_DEAD);
    CHECK(coro_get_state(g_c) == CORO_DEAD);
    CHECK(coro_resume(g_c, g_live, &next) == CORO_EDESTROYED);
    CHECK(coro_destroy(g_c) == CORO_OK);
}

TEST_MAIN()
#endif
