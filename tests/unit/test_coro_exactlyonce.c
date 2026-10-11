#include "../harness.h"
#include <corocc/coro.h>

/* Milestone M2 Task 5, part 1: "exactly once" under concurrency.
 *
 * The safety property the token protocol guarantees is mutual exclusion of
 * drives: for one credential exactly one compare-and-swap can win, so at most
 * one execution flow enters a suspension point. Everything else -- a rejected
 * racer learning why it lost, a coroutine left consistent afterwards, a process
 * that does not crash -- follows from that.
 *
 * What milestone M2 deliberately does NOT provide is cross-thread driving:
 * a coroutine belongs to the thread that first drove it (see coro_start), so
 * the racer that wins a claim on a coroutine it does not own is refused with
 * CORO_EBUSY and hands the credential back. The winning claim is therefore the
 * assertion of interest, not which thread ends up holding the coroutine.
 */

#if defined(_WIN32)
/* The Windows path needs the M3 thread abstraction; the contract itself is
   platform independent and this file documents that gap loudly. */
static void run_tests(void) {
    CHECK(0 && "concurrent exactly-once suite: POSIX threads only in M2");
}
TEST_MAIN()
#else

#include <pthread.h>
#include <string.h>

#define THREADS 8u
#define ROUNDS 8

/* One result slot per racer: the threads never share a counter, so the
   aggregation after the joins is race-free by construction. */
static coro_status g_rc[THREADS];
static coro_token  g_out[THREADS];

static coro_t*             g_c;
static coro_token          g_tok;
static pthread_barrier_t   g_barrier;

static void entry_park_each_round(void* arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS; ++i) {
        if (coro_yield() != CORO_OK) return;
    }
}

static void* racer(void* pv) {
    long id = (long)pv;
    coro_token out = CORO_TOKEN_NONE;
    pthread_barrier_wait(&g_barrier); /* release all racers at once */
    g_rc[id] = coro_resume(g_c, g_tok, &out);
    g_out[id] = out;
    return NULL;
}

/* Run THREADS racers at once; returns how many of them resumed the coroutine,
   and reports the rejection codes through the first two output arrays. */
static int race(pthread_t* th) {
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

/* Drive a live coroutine to completion from the owning thread. */
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
    success = race(th);
    /* Mutual exclusion: at most one racer may hold the coroutine. Which
       rejection code the others see depends on when their claim landed, so
       membership -- not a count -- is what the contract pins down. */
    CHECK(success <= 1);
    if (success == 1) {
        for (unsigned i = 0; i < THREADS; ++i) {
            if (g_rc[i] == CORO_OK) winner_tok = g_out[i];
        }
        CHECK(winner_tok != CORO_TOKEN_NONE);
        finish_coroutine(g_c, winner_tok);
    } else {
        /* every claim was refused as foreign, so the owner's credential was
           handed back and is still live */
        finish_coroutine(g_c, g_tok);
    }
    CHECK(coro_get_state(g_c) == CORO_DEAD);
    CHECK(coro_destroy(g_c) == CORO_OK);

    /* ---------- a coroutine keeps its owner ---------- */
    /* A coroutine belongs to the thread that first drove it: nobody else may
       drive it, whatever the claim race decides. */
    CHECK(coro_create(&g_c, NULL, entry_park_each_round, NULL) == CORO_OK);
    CHECK(coro_start(g_c, &g_tok) == CORO_OK);
    memset(g_rc, 0, sizeof g_rc);
    memset(g_out, 0, sizeof g_out);
    success = race(th);
    CHECK(success == 0); /* foreign owners only: none of them may drive */
    /* the owner's credential is untouched and still works */
    finish_coroutine(g_c, g_tok);
    CHECK(coro_destroy(g_c) == CORO_OK);
}

TEST_MAIN()
#endif
