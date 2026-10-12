#include "../harness.h"
#include <corocc/coro.h>

/* Manual-mode cross-thread resume: a coroutine may be driven by any thread.
 *
 * The behaviours that make cross-thread driving trustworthy:
 *   - a foreign thread that presents the live credential drives the coroutine
 *     right there on its own thread, and learns the published credential;
 *   - the same credential presented by several threads at once executes the
 *     coroutine body exactly once, every other caller gets a rejection;
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

/* A gate that releases every caller of a phase together, so all the resumes
   of a phase race for the same credential. Waiting on it is a condition
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

/* Presents the live credential from this foreign thread and drives directly. */
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

/* Starts `count` foreign callers and releases them together; every caller
   drives the coroutine on its own thread, so the main thread only joins. */
static void run_wakers(unsigned count) {
    pthread_t t[WAKERS];
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

    for (unsigned i = 0; i < count; ++i) {
        pthread_join(t[i], NULL);
    }
}

static void run_tests(void) {
    coro_token next = CORO_TOKEN_NONE;

    CHECK(coro_create(&g_c, NULL, entry_count_rounds, NULL) == CORO_OK);
    CHECK(coro_start(g_c, &g_live) == CORO_OK);
    CHECK(g_rounds == 0u);

    /* ---------- a foreign thread drives and publishes ---------- */
    run_wakers(1);
    CHECK(g_wrc[0] == CORO_OK);
    CHECK(g_wout[0] != CORO_TOKEN_NONE);
    CHECK(g_wout[0] != CORO_TOKEN_DEAD);
    CHECK(g_rounds == 1u); /* the coroutine advanced exactly once */
    CHECK(coro_get_state(g_c) == CORO_SUSPENDED);
    g_live = g_wout[0];    /* the credential the foreign caller learned */

    /* ---------- the main thread keeps driving inline ---------- */
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
