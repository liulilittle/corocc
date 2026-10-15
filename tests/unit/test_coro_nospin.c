#include "../harness.h"
#include <corocc/coro.h>

/* Zero spin.
 *
 * The control path (start/resume/yield) must perform a fixed, small number of
 * compare-and-swap attempts -- never a retry loop, never a busy wait. The
 * library counts every CAS it performs when built with COROCC_PROBE_NOSPIN,
 * which lets this test pin the accounting down:
 *
 *   claim             1   the single attempt that grants or denies the drive
 *   yield claim       1   taken by the coroutine when it parks
 *   publish           1   the resumer settles the coroutine after the switch
 *   destroy           1   the single transition to dead
 *
 * A rejected claim is the sharpest part of the contract: exactly one attempt,
 * no spin. Everything else must sum to the documented totals.
 */

#ifdef COROCC_PROBE_NOSPIN
extern unsigned long coro_probe_cas_count;

static coro_t* g_c;

/* Parks twice, then returns. */
static void entry_two_rounds(void* arg) {
    (void)arg;
    CHECK(coro_yield() == CORO_OK);
    CHECK(coro_yield() == CORO_OK);
}

static void run_tests(void) {
    coro_token tok = CORO_TOKEN_NONE, next = CORO_TOKEN_NONE;
    unsigned long before = 0, after = 0;

    CHECK(coro_create(&g_c, NULL, entry_two_rounds, NULL) == CORO_OK);

    /* a rejected claim: exactly one attempt, no retry */
    before = coro_probe_cas_count;
    CHECK(coro_resume(g_c, 77u, &next) == CORO_ESTALE);
    after = coro_probe_cas_count;
    CHECK(after - before == 1ul);

    /* start: claim + yield claim + publish */
    before = coro_probe_cas_count;
    CHECK(coro_start(g_c, &tok) == CORO_OK);
    after = coro_probe_cas_count;
    CHECK(after - before == 3ul);

    /* resume into the second park: claim + yield claim + publish */
    before = coro_probe_cas_count;
    CHECK(coro_resume(g_c, tok, &next) == CORO_OK);
    after = coro_probe_cas_count;
    CHECK(after - before == 3ul);

    /* final drive: the entry returns, so no yield claim -- claim + publish */
    before = coro_probe_cas_count;
    CHECK(coro_resume(g_c, next, &next) == CORO_OK);
    after = coro_probe_cas_count;
    CHECK(after - before == 2ul);
    CHECK(next == CORO_TOKEN_DEAD);

    /* destroy: the single transition to dead */
    before = coro_probe_cas_count;
    CHECK(coro_destroy(g_c) == CORO_OK);
    after = coro_probe_cas_count;
    CHECK(after - before == 1ul);
}

TEST_MAIN()
#else
static void run_tests(void) {
    CHECK(0 && "build with -DCOROCC_PROBE_NOSPIN=ON to run the zero-spin probe");
}
TEST_MAIN()
#endif
