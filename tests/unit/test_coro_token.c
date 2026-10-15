#include "../harness.h"
#include <corocc/coro.h>

/* The credential protocol. Every suspension point
   publishes a unique monotone token; only the holder of the live token can
   resume, a stale token is rejected, and exactly once. */

#define ROUNDS 100u

/* ---- single coroutine driven through many suspensions ---- */
static coro_t*  g_worker;
static unsigned g_rounds_seen;
static int      g_resume_was_real = 1;

static void entry_yield_each_round(void* arg) {
    (void)arg;
    unsigned seen = 0;
    int first = 1;
    while (seen < ROUNDS) {
        ++seen;
        /* A resume must drop us exactly where the last round stopped: the
           counter we wrote before yielding has to be visible again. */
        if (!first && g_rounds_seen != seen - 1) g_resume_was_real = 0;
        first = 0;
        g_rounds_seen = seen;
        CHECK(coro_yield() == CORO_OK);
    }
}

/* ---- coroutine whose entry returns immediately ---- */
static int g_ran = 0;
static void entry_return(void* arg) {
    (void)arg;
    g_ran = 1;
}

/* ---- nesting: coroutine A drives coroutine B ---- */
static coro_t*  g_inner;
static coro_t*  g_outer;
static unsigned g_inner_rounds;
static unsigned g_outer_rounds;

static void entry_inner(void* arg) {
    (void)arg;
    while (g_inner_rounds < 3u) {
        ++g_inner_rounds;
        CHECK(coro_yield() == CORO_OK); /* returns into the outer coroutine */
    }
}

static void entry_outer(void* arg) {
    (void)arg;
    coro_token tok;
    CHECK(coro_start(g_inner, &tok) == CORO_OK); /* start B from inside A */
    while (g_outer_rounds < 3u) {
        ++g_outer_rounds;
        coro_token next;
        CHECK(coro_resume(g_inner, tok, &next) == CORO_OK);
        if (next == CORO_TOKEN_DEAD) break; /* inner coroutine finished */
        tok = next;
        CHECK(coro_yield() == CORO_OK); /* back to the main flow */
    }
}

static void run_tests(void) {
    /* ---------- credentials for repeated suspensions ---------- */
    CHECK(coro_create(&g_worker, NULL, entry_yield_each_round, NULL) == CORO_OK);
    CHECK(coro_get_state(g_worker) == CORO_NEW);

    coro_token tok = CORO_TOKEN_NONE, next = CORO_TOKEN_NONE;
    CHECK(coro_start(g_worker, &tok) == CORO_OK);
    CHECK(tok != CORO_TOKEN_NONE);
    CHECK(tok != CORO_TOKEN_DEAD);
    CHECK(coro_get_state(g_worker) == CORO_SUSPENDED);

    CHECK(coro_resume(g_worker, tok, &next) == CORO_OK);
    CHECK(next > tok);                    /* monotone, never reused */
    CHECK(coro_get_state(g_worker) == CORO_SUSPENDED);
    CHECK(g_rounds_seen == 2u);
    CHECK(g_resume_was_real == 1);

    /* the token was consumed: replaying it must be rejected */
    CHECK(coro_resume(g_worker, tok, &next) == CORO_ESTALE);
    CHECK(coro_resume(g_worker, tok + 12345u, &next) == CORO_ESTALE);
    /* argument validation leaves the state alone */
    CHECK(coro_resume(g_worker, CORO_TOKEN_NONE, &next) == CORO_EINVAL);
    CHECK(coro_resume(g_worker, next, NULL) == CORO_EINVAL);
    CHECK(coro_resume(NULL, next, &next) == CORO_EINVAL);
    CHECK(coro_get_state(g_worker) == CORO_SUSPENDED);

    /* drive the remaining rounds to completion */
    while (next != CORO_TOKEN_DEAD) {
        coro_token cur = next;
        CHECK(coro_resume(g_worker, cur, &next) == CORO_OK);
    }
    CHECK(g_rounds_seen == ROUNDS);
    CHECK(g_resume_was_real == 1);
    CHECK(coro_get_state(g_worker) == CORO_DEAD);

    /* dead coroutines refuse every credential */
    CHECK(coro_resume(g_worker, next, &tok) == CORO_EDESTROYED);
    CHECK(coro_start(g_worker, &tok) == CORO_EDESTROYED);
    CHECK(coro_destroy(g_worker) == CORO_OK);

    /* ---------- entry that returns ends the coroutine ---------- */
    CHECK(coro_create(&g_worker, NULL, entry_return, NULL) == CORO_OK);
    tok = CORO_TOKEN_NONE;
    CHECK(coro_start(g_worker, &tok) == CORO_OK);
    CHECK(tok == CORO_TOKEN_DEAD);
    CHECK(g_ran == 1);
    CHECK(coro_get_state(g_worker) == CORO_DEAD);
    CHECK(coro_destroy(g_worker) == CORO_OK);

    /* ---------- credentials for a coroutine that never started ---------- */
    CHECK(coro_create(&g_worker, NULL, entry_yield_each_round, NULL) == CORO_OK);
    CHECK(coro_resume(g_worker, 42u, &next) == CORO_ESTALE);
    CHECK(coro_get_state(g_worker) == CORO_NEW);
    CHECK(coro_destroy(g_worker) == CORO_OK);

    /* ---------- yield outside any coroutine ---------- */
    CHECK(coro_yield() == CORO_ESTALE);

    /* ---------- nested coroutines ---------- */
    CHECK(coro_create(&g_inner, NULL, entry_inner, NULL) == CORO_OK);
    CHECK(coro_create(&g_outer, NULL, entry_outer, NULL) == CORO_OK);

    coro_token outer_tok = CORO_TOKEN_NONE, outer_next = CORO_TOKEN_NONE;
    CHECK(coro_start(g_outer, &outer_tok) == CORO_OK);
    while (outer_next != CORO_TOKEN_DEAD) {
        coro_token cur = outer_tok;
        CHECK(coro_resume(g_outer, cur, &outer_next) == CORO_OK);
        outer_tok = outer_next;
    }
    CHECK(g_inner_rounds == 3u);
    CHECK(g_outer_rounds == 3u);
    CHECK(coro_get_state(g_inner) == CORO_DEAD);
    CHECK(coro_get_state(g_outer) == CORO_DEAD);
    CHECK(coro_destroy(g_inner) == CORO_OK);
    CHECK(coro_destroy(g_outer) == CORO_OK);
}

TEST_MAIN()
