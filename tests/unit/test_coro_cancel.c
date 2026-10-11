#include "../harness.h"
#include <corocc/coro.h>

/* Milestone M2 Tasks 3-4: cancellation observed at a safe point, join without
   a runtime layer, and coroutine-local slots that survive suspension. */

static coro_t*  g_c;
static unsigned g_rounds;
static int      g_cancel_seen;

/* Enters, parks once, then unwinds once a cancellation has been observed. */
static void entry_cancellable(void* arg) {
    (void)arg;
    ++g_rounds;
    while (coro_yield() == CORO_OK) {
        ++g_rounds;
    }
    g_cancel_seen = 1;
}

static void entry_stores_local(void* arg) {
    (void)arg;
    static const size_t kSlot = 7u;
    CHECK(coro_local_get(g_c, kSlot) == (void*)0x1234);
    coro_local_set(g_c, kSlot, (void*)0x5678);
    CHECK(coro_yield() == CORO_OK);
    /* the value written before the yield must still be there after resuming */
    CHECK(coro_local_get(g_c, kSlot) == (void*)0x5678);
    coro_local_set(g_c, kSlot, NULL);
    CHECK(coro_local_get(g_c, kSlot) == NULL);
}

static void run_tests(void) {
    coro_token tok = CORO_TOKEN_NONE, next = CORO_TOKEN_NONE;

    /* ---------- cancellation lands at the next safe point ---------- */
    CHECK(coro_create(&g_c, NULL, entry_cancellable, NULL) == CORO_OK);
    CHECK(coro_start(g_c, &tok) == CORO_OK);
    CHECK(g_rounds == 1u);
    CHECK(coro_join(g_c) == CORO_EAGAIN);   /* still live */

    CHECK(coro_cancel(g_c) == CORO_OK);
    CHECK(coro_cancel(NULL) == CORO_EINVAL);

    /* the pending request surfaces only when the coroutine is resumed */
    CHECK(coro_resume(g_c, tok, &next) == CORO_OK);
    CHECK(next == CORO_TOKEN_DEAD);
    CHECK(g_cancel_seen == 1);
    CHECK(coro_get_state(g_c) == CORO_DEAD);
    CHECK(coro_join(g_c) == CORO_OK);
    CHECK(coro_destroy(g_c) == CORO_OK);

    /* a cancellation requested before the coroutine ever runs surfaces at the
       very first safe point: the entry never even parks */
    g_cancel_seen = 0;
    CHECK(coro_create(&g_c, NULL, entry_cancellable, NULL) == CORO_OK);
    CHECK(coro_cancel(g_c) == CORO_OK);
    CHECK(coro_start(g_c, &tok) == CORO_OK);
    CHECK(g_cancel_seen == 1);
    CHECK(next == CORO_TOKEN_DEAD);
    CHECK(coro_destroy(g_c) == CORO_OK);

    /* ---------- local slots ---------- */
    CHECK(coro_create(&g_c, NULL, entry_stores_local, NULL) == CORO_OK);
    CHECK(coro_local_set(g_c, 7u, (void*)0x1234) == CORO_OK);
    CHECK(coro_local_get(g_c, CORO_LOCAL_SLOTS) == NULL);       /* out of range */
    CHECK(coro_local_set(g_c, CORO_LOCAL_SLOTS, (void*)1) == CORO_EINVAL);
    CHECK(coro_local_set(g_c, CORO_LOCAL_SLOTS + 100u, (void*)1) == CORO_EINVAL);
    CHECK(coro_local_set(NULL, 0u, (void*)1) == CORO_EINVAL);
    CHECK(coro_local_get(NULL, 0u) == NULL);
    CHECK(coro_local_get(g_c, 0u) == NULL);                     /* unset slot */

    /* the entry parks once at its yield, then finishes on the next drive */
    CHECK(coro_start(g_c, &tok) == CORO_OK);
    CHECK(tok != CORO_TOKEN_DEAD);
    CHECK(coro_get_state(g_c) == CORO_SUSPENDED);
    CHECK(coro_local_get(g_c, 7u) == (void*)0x5678); /* written before the yield */

    CHECK(coro_resume(g_c, tok, &next) == CORO_OK);
    CHECK(next == CORO_TOKEN_DEAD);
    CHECK(coro_get_state(g_c) == CORO_DEAD);
    CHECK(coro_local_get(g_c, 7u) == NULL);          /* cleared by the entry */
    CHECK(coro_destroy(g_c) == CORO_OK);
}

TEST_MAIN()
