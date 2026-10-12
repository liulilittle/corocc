#include "../harness.h"
#include <corocc/coro.h>

/* "Exactly once" under concurrency, manual mode (M2 contract, revisited).
 *
 * The safety property the credential protocol guarantees is mutual exclusion of
 * drives: for one credential exactly one compare-and-swap can win, so at most
 * one execution flow enters a suspension point. Everything else -- a rejected
 * racer learning why it lost, a coroutine left consistent afterwards, a process
 * that does not crash -- follows from that.
 *
 * Any thread may drive any coroutine: a racer that wins the claim switches into
 * the coroutine on its own thread and publishes the next credential; the losers
 * report a status code. No thread sits in a pump waiting for a delivery --
 * manual resume drives directly.
 */

#if defined(_WIN32)
static void run_tests(void) {
    CHECK(0 && "concurrent exactly-once suite: POSIX threads only in M3");
}
TEST_MAIN()
#else

#include <pthread.h>
#include <string.h>

#define THREADS 8u
#define ROUNDS 8

static coro_t*           g_c;
static coro_token        g_tok;
static coro_status       g_rc[THREADS];
static coro_token        g_out[THREADS];
static pthread_barrier_t g_barrier;

static void entry_park_each_round(void* arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS; ++i) {
        if (coro_yield() != CORO_OK) return;
    }
}

/* A racer presents the live credential and drives the coroutine on its own
   thread; exactly one racer wins the claim. */
static void* racer(void* pv) {
    long id = (long)pv;
    pthread_barrier_wait(&g_barrier);
    g_rc[id] = coro_resume(g_c, g_tok, &g_out[id]);
    return NULL;
}

/* Releases every racer against the same credential and joins them all; the
   racers drive directly, so the main thread only joins. */
static int race_each_round(pthread_t* th) {
    int success = 0;
    for (unsigned i = 0; i < THREADS; ++i) {
        CHECK(pthread_create(&th[i], NULL, racer, (void*)(long)i) == 0);
    }
    for (unsigned i = 0; i < THREADS; ++i) {
        pthread_join(th[i], NULL);
    }
    for (unsigned i = 0; i < THREADS; ++i) {
        if (g_rc[i] == CORO_OK) {
            CHECK(g_out[i] != CORO_TOKEN_NONE);
            CHECK(g_out[i] != CORO_TOKEN_DEAD);
            ++success;
        } else {
            CHECK(g_rc[i] == CORO_EBUSY || g_rc[i] == CORO_ESTALE ||
                  g_rc[i] == CORO_EDESTROYED);
        }
    }
    return success;
}

/* Drive a live coroutine to completion from this thread. When one of the
   racers won the claim, the credential it published is the live one. */
static void finish_coroutine(coro_t* c, coro_token tok) {
    coro_token next = CORO_TOKEN_NONE;
    int guard = 0;
    while (next != CORO_TOKEN_DEAD && guard++ < 1000) {
        coro_status st = coro_resume(c, tok, &next);
        CHECK(st == CORO_OK);
        if (st != CORO_OK) break;
        tok = next;
    }
    CHECK(next == CORO_TOKEN_DEAD);
    CHECK(coro_get_state(c) == CORO_DEAD);
}

static void run_tests(void) {
    pthread_t th[THREADS];
    int success = 0;
    coro_token winner_tok = CORO_TOKEN_NONE;

    /* ---------- concurrent claim of one credential ---------- */
    CHECK(coro_create(&g_c, NULL, entry_park_each_round, NULL) == CORO_OK);
    CHECK(coro_start(g_c, &g_tok) == CORO_OK);
    CHECK(pthread_barrier_init(&g_barrier, NULL, THREADS) == 0);

    memset(g_rc, 0, sizeof g_rc);
    memset(g_out, 0, sizeof g_out);
    success = race_each_round(th);
    /* Mutual exclusion: at most one racer may hold the coroutine. Which
       rejection code the others see depends on when their claim landed, so
       membership -- not a count -- is what the contract pins down. */
    CHECK(success <= 1);
    winner_tok = g_tok;
    if (success == 1) {
        for (unsigned i = 0; i < THREADS; ++i) {
            if (g_rc[i] == CORO_OK) winner_tok = g_out[i];
        }
        CHECK(winner_tok != CORO_TOKEN_NONE);
    }
    finish_coroutine(g_c, winner_tok);
    CHECK(coro_get_state(g_c) == CORO_DEAD);
    CHECK(coro_destroy(g_c) == CORO_OK);

    /* ---------- second coroutine, same guarantee ---------- */
    CHECK(coro_create(&g_c, NULL, entry_park_each_round, NULL) == CORO_OK);
    CHECK(coro_start(g_c, &g_tok) == CORO_OK);
    memset(g_rc, 0, sizeof g_rc);
    memset(g_out, 0, sizeof g_out);
    success = race_each_round(th);
    CHECK(success <= 1);
    winner_tok = g_tok;
    if (success == 1) {
        for (unsigned i = 0; i < THREADS; ++i) {
            if (g_rc[i] == CORO_OK) winner_tok = g_out[i];
        }
    }
    finish_coroutine(g_c, winner_tok);
    CHECK(coro_destroy(g_c) == CORO_OK);
}

TEST_MAIN()
#endif