/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * tests/perf/bench_coro.c -- coroutine lifecycle and switch cost benchmark.
 *
 * Measures, for one coroutine driven by the thread that created it:
 *   - ns per resume/yield round trip (two context switches plus the claim and
 *     publish compare-and-swaps of the credential protocol),
 *   - ns per create+drive-to-completion+destroy cycle,
 * both with CLOCK_MONOTONIC.
 *
 * Registered as a non-test executable: it reports figures, it asserts none.
 */
#include <corocc/coro.h>

#include <stdio.h>
#include <time.h>

#define ROUNDS      2000000u
#define LIFECYCLES  200000u

static coro_t* g_c;
static double  g_ns;

static void entry_yield_per_round(void* arg) {
    (void)arg;
    for (;;) {
        if (coro_yield() != CORO_OK) return;
    }
}

static void entry_finish_immediately(void* arg) {
    (void)arg;
}

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

int main(void) {
    double t0, t1;

    /* ---- resume/yield round trip ---- */
    if (coro_create(&g_c, NULL, entry_yield_per_round, NULL) != CORO_OK) {
        return 1;
    }
    coro_token tok = CORO_TOKEN_NONE, next = CORO_TOKEN_NONE;
    if (coro_start(g_c, &tok) != CORO_OK) return 1;

    t0 = now_ns();
    for (unsigned i = 0; i < ROUNDS; ++i) {
        if (coro_resume(g_c, tok, &next) != CORO_OK) return 1;
        tok = next;
    }
    t1 = now_ns();
    g_ns = (t1 - t0) / (double)ROUNDS;
    printf("rounds=%u ns/round_trip=%.2f\n", ROUNDS, g_ns);

    /* stop the coroutine, then release it */
    coro_cancel(g_c);
    if (coro_resume(g_c, tok, &next) != CORO_OK) return 1;
    if (next != CORO_TOKEN_DEAD) return 1;
    if (coro_destroy(g_c) != CORO_OK) return 1;

    /* ---- create + drive to completion + destroy ---- */
    t0 = now_ns();
    for (unsigned i = 0; i < LIFECYCLES; ++i) {
        coro_t* c = NULL;
        coro_token t = CORO_TOKEN_NONE;
        if (coro_create(&c, NULL, entry_finish_immediately, NULL) != CORO_OK) {
            return 1;
        }
        if (coro_start(c, &t) != CORO_OK) return 1;
        if (t != CORO_TOKEN_DEAD) return 1;
        if (coro_destroy(c) != CORO_OK) return 1;
    }
    t1 = now_ns();
    printf("cycles=%u ns/lifecycle=%.2f\n", LIFECYCLES,
           (t1 - t0) / (double)LIFECYCLES);
    return 0;
}
