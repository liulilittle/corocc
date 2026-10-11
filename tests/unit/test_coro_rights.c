#include "../harness.h"
#include <corocc/coro.h>

/* Milestone M2 Task 5, part 2: failure without side effects, and the rights
   that a coroutine does not have.
 *
 * A rejected operation must change nothing -- not the state word, not the
 * credential, not the stack -- so the caller can retry or route elsewhere.
 * A coroutine also cannot resume itself: the token it would need is the one
 * its own run has already consumed.
 */

static coro_t*  g_c;
static unsigned g_rounds;

static void entry_one_round_then_park(void* arg) {
    (void)arg;
    ++g_rounds;
    if (coro_yield() == CORO_ECANCELED) return;
    ++g_rounds;
    if (coro_yield() == CORO_ECANCELED) return;
    ++g_rounds;
}

/* Resuming itself must be refused: the live credential belongs to the caller
   that is currently running this coroutine. The handle is read from the global
   that coro_create has already filled in by the time the entry runs. */
static void entry_self_resume(void* arg) {
    (void)arg;
    coro_token out = CORO_TOKEN_NONE;
    ++g_rounds;
    CHECK(g_c != NULL);
    CHECK(coro_resume(g_c, CORO_TOKEN_DEAD, &out) == CORO_EDESTROYED);
    CHECK(coro_resume(g_c, 99u, &out) == CORO_EBUSY);
    CHECK(coro_resume(g_c, (coro_token)0, &out) == CORO_EINVAL);
    (void)coro_yield();
    ++g_rounds;
}

static void run_tests(void) {
    coro_token tok = CORO_TOKEN_NONE, next = CORO_TOKEN_NONE;

    /* ---------- rejected operations leave everything untouched ---------- */
    CHECK(coro_create(&g_c, NULL, entry_one_round_then_park, NULL) == CORO_OK);
    CHECK(coro_start(g_c, &tok) == CORO_OK);
    CHECK(g_rounds == 1u);
    CHECK(coro_get_state(g_c) == CORO_SUSPENDED);

    /* a pile of illegal resumes: none of them may advance the coroutine */
    for (int i = 0; i < 8; ++i) {
        coro_token bogus = CORO_TOKEN_NONE;
        CHECK(coro_resume(g_c, tok + 1u + (coro_token)i, &bogus) == CORO_ESTALE);
        CHECK(coro_resume(g_c, CORO_TOKEN_NONE, &bogus) == CORO_EINVAL);
        CHECK(coro_resume(g_c, CORO_TOKEN_DEAD, &bogus) == CORO_EDESTROYED);
        CHECK(coro_resume(g_c, 1u, &bogus) == CORO_EINVAL);
        CHECK(coro_resume(g_c, 2u, &bogus) == CORO_EINVAL);
        CHECK(coro_get_state(g_c) == CORO_SUSPENDED);
        CHECK(g_rounds == 1u);
    }

    /* the credential still works after all that abuse */
    CHECK(coro_resume(g_c, tok, &next) == CORO_OK);
    CHECK(g_rounds == 2u);
    CHECK(coro_get_state(g_c) == CORO_SUSPENDED);
    tok = next;
    CHECK(coro_resume(g_c, tok, &next) == CORO_OK);
    CHECK(g_rounds == 3u);
    CHECK(next == CORO_TOKEN_DEAD);
    CHECK(coro_get_state(g_c) == CORO_DEAD);
    CHECK(coro_destroy(g_c) == CORO_OK);

    /* ---------- a coroutine cannot resume itself ---------- */
    g_rounds = 0;
    CHECK(coro_create(&g_c, NULL, entry_self_resume, NULL) == CORO_OK);
    CHECK(coro_start(g_c, &tok) == CORO_OK);
    CHECK(g_rounds == 1u);
    CHECK(coro_resume(g_c, tok, &next) == CORO_OK);
    CHECK(g_rounds == 2u);
    CHECK(next == CORO_TOKEN_DEAD);
    CHECK(coro_get_state(g_c) == CORO_DEAD);
    CHECK(coro_destroy(g_c) == CORO_OK);
}

TEST_MAIN()
