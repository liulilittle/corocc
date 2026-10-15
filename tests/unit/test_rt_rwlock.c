#include "../harness.h"
#include "arch/coro_atomic.h"
#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <string.h>

/* coro_rt_rwlock -- a writer-preferring readers/writer lock on the
 * M:N runtime.
 *
 * Behaviours pinned:
 *   - a zero-initialized lock is valid; no init call is required;
 *   - tryrdlock / trywrlock report CORO_EBUSY instead of blocking, and an
 *     unlock with no matching hold reports CORO_ESTATE;
 *   - the blocking entry points report CORO_ESTALE outside a runtime
 *     coroutine and CORO_EINVAL for NULL;
 *   - readers hold the lock together and a writer holds it alone: the lock's
 *     own state word is the witness of mutual exclusion in both directions;
 *   - a coroutine whose cancellation lands while the lock is taken reports
 *     CORO_ECANCELED, leaves no phantom waiter, and leaves the lock usable;
 *   - two writers queued behind a reader: cancelling the tail one still lets
 *     the surviving writer acquire, and readers are not starved afterwards;
 *   - a single-worker runtime does not deadlock.
 *
 * The overlap counters are atomics: several readers legitimately hold the
 * lock at once, so a plain ++ from concurrent workers would lose updates.
 *
 * The cancellation scenarios are driven by ONE coroutine that owns the lock
 * and lets the others run with a single yield. Because a parked waiter is not
 * re-queued by the worker, the driver always resumes next, so the observed
 * order is deterministic without a flag-polling loop (which, on a single
 * worker, would starve the driver behind the coroutines it spawned).
 */
#define RW_W_BIT ((uint64_t)1u << 0)

#define READERS 6u
#define WRITERS 2u
#define ROUNDS  150u

static coro_rt_rwlock_t g_rw;

static uint64_t g_readers_in;   /* readers inside the critical section */
static uint64_t g_writers_in;   /* writers inside the critical section */
static uint64_t g_max_readers;
static uint64_t g_max_writers;
static uint64_t g_conflict;
static uint64_t g_read_rounds;
static uint64_t g_write_rounds;
static uint64_t g_counter;

static void reader_entry(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    for (unsigned i = 0; i < ROUNDS; ++i) {
        if (coro_rt_rwlock_rdlock(rt, &g_rw) != CORO_OK) return;
        uint64_t n = coro_at_fetch_add(&g_readers_in, 1) + 1;
        if (n > g_max_readers) g_max_readers = n;
        /* Hold the read lock for a while: a writer that acquires in two steps
           would show its RW_W bit inside this window. */
        for (unsigned k = 0; k < 64u; ++k) {
            if (coro_at_load(&g_rw.state) & RW_W_BIT) g_conflict = 1;
            if (coro_at_load(&g_writers_in) != 0) g_conflict = 1;
            coro_yield();
        }
        (void)coro_at_fetch_add(&g_read_rounds, 1);
        coro_at_fetch_add(&g_readers_in, ~(uint64_t)0);
        if (coro_rt_rwlock_rdunlock(rt, &g_rw) != CORO_OK) return;
        if (coro_yield() != CORO_OK) return;
    }
}

static void writer_entry(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    for (unsigned i = 0; i < ROUNDS; ++i) {
        if (coro_rt_rwlock_wrlock(rt, &g_rw) != CORO_OK) return;
        uint64_t n = coro_at_fetch_add(&g_writers_in, 1) + 1;
        if (n > g_max_writers) g_max_writers = n;
        if (coro_at_load(&g_readers_in) != 0) g_conflict = 1;
        (void)coro_at_fetch_add(&g_write_rounds, 1);
        (void)coro_at_fetch_add(&g_counter, 1);
        coro_at_fetch_add(&g_writers_in, ~(uint64_t)0);
        if (coro_rt_rwlock_wrunlock(rt, &g_rw) != CORO_OK) return;
        if (coro_yield() != CORO_OK) return;
    }
}

/* ---- a waiter cancelled while the lock is taken by another ---- */

static coro_t*     g_victim;
static coro_status g_victim_st;
static int         g_victim_got_lock;
static coro_status g_after_wrunlock;
static coro_status g_reacquire;
static coro_status g_second_try;
static coro_status g_final_unlock;

/* Blocks on the write hold; the cancellation is observed at its safe point. */
static void victim_entry(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    g_victim_st = coro_rt_rwlock_wrlock(rt, &g_rw);
    if (g_victim_st == CORO_OK) {
        g_victim_got_lock = 1;
        (void)coro_rt_rwlock_wrunlock(rt, &g_rw);
    }
}

/* Owns the write lock, spawns a writer that must block, cancels it before it
   has run, and observes the whole exchange itself. */
static void holder_entry(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_rwlock_wrlock(rt, &g_rw) != CORO_OK) return;
    if (coro_rt_spawn_h(rt, NULL, victim_entry, rt, &g_victim) != CORO_OK) return;
    /* The victim has not run yet, so its request is latched before it parks. */
    (void)coro_rt_cancel(rt, g_victim);
    /* One yield lets the victim run: it blocks, observes the cancellation and
       abstains, all inside its own wake -- the driver resumes right after. */
    (void)coro_yield();
    g_after_wrunlock = coro_rt_rwlock_wrunlock(rt, &g_rw);
    /* The lock must be free and usable again. */
    g_reacquire = coro_rt_rwlock_trywrlock(rt, &g_rw);
    if (g_reacquire == CORO_OK) {
        g_second_try = coro_rt_rwlock_trywrlock(rt, &g_rw);
        g_final_unlock = coro_rt_rwlock_wrunlock(rt, &g_rw);
    }
}

static void run_cancel_scenario(void) {
    coro_rt_t* rt = coro_rt_create(1);   /* single worker: strict ordering */
    CHECK(rt != NULL);
    memset(&g_rw, 0, sizeof g_rw);
    g_victim = NULL; g_victim_st = CORO_OK; g_victim_got_lock = 0;
    g_after_wrunlock = g_reacquire = g_second_try = g_final_unlock = CORO_OK;

    CHECK(coro_rt_spawn(rt, NULL, holder_entry, rt) == CORO_OK);
    CHECK(coro_rt_wait_all(rt) == CORO_OK);
    /* The lock was still taken, so the cancelled waiter abstained. */
    CHECK(g_victim_st == CORO_ECANCELED);
    CHECK(g_victim_got_lock == 0);
    CHECK(coro_rt_destroy(rt) == CORO_OK);
    /* The lock survived the cancellation intact and reusable. */
    CHECK(g_after_wrunlock == CORO_OK);
    CHECK(g_reacquire == CORO_OK);
    CHECK(g_second_try == CORO_EBUSY);
    CHECK(g_final_unlock == CORO_OK);
}

/* ---- two writers queued behind a reader, only the tail cancelled ------- */

static coro_rt_rwlock_t g_b3;
static coro_t*         g_b3_w1;
static coro_t*         g_b3_w2;
static coro_status     g_b3_w1_st;
static coro_status     g_b3_w2_st;
static int             g_b3_w1_acquired;
static coro_status     g_b3_late_read;

/* Parks behind the reader's read hold, then takes the handoff. */
static void b3_w1_entry(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    g_b3_w1_st = coro_rt_rwlock_wrlock(rt, &g_b3);
    if (g_b3_w1_st == CORO_OK) {
        g_b3_w1_acquired = 1;
        (void)coro_rt_rwlock_wrunlock(rt, &g_b3);
    }
}

/* Cancelled before it runs: blocks and then abstains. */
static void b3_w2_entry(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    g_b3_w2_st = coro_rt_rwlock_wrlock(rt, &g_b3);
    if (g_b3_w2_st == CORO_OK) {
        (void)coro_rt_rwlock_wrunlock(rt, &g_b3);
    }
}

static void b3_controller_entry(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_rwlock_rdlock(rt, &g_b3) != CORO_OK) return;
    /* W1 queues behind the reader. */
    if (coro_rt_spawn_h(rt, NULL, b3_w1_entry, rt, &g_b3_w1) != CORO_OK) return;
    (void)coro_yield();
    /* W2 is the tail: cancel it before it runs. */
    if (coro_rt_spawn_h(rt, NULL, b3_w2_entry, rt, &g_b3_w2) != CORO_OK) return;
    (void)coro_rt_cancel(rt, g_b3_w2);
    (void)coro_yield();   /* W2 blocks, is cancelled, abstains */
    /* Release the reader: the last-reader handoff must go to W1. */
    if (coro_rt_rwlock_rdunlock(rt, &g_b3) != CORO_OK) return;
    (void)coro_yield();   /* W1 takes the handoff and releases */
    (void)coro_yield();
    /* Readers must not be starved by the leftover writer state. */
    g_b3_late_read = coro_rt_rwlock_tryrdlock(rt, &g_b3);
    if (g_b3_late_read == CORO_OK) {
        (void)coro_rt_rwlock_rdunlock(rt, &g_b3);
    }
}

static void run_b3_scenario(void) {
    coro_rt_t* rt = coro_rt_create(1);   /* single worker: strict ordering */
    CHECK(rt != NULL);
    memset(&g_b3, 0, sizeof g_b3);
    g_b3_w1 = NULL; g_b3_w2 = NULL;
    g_b3_w1_st = g_b3_w2_st = CORO_OK;
    g_b3_w1_acquired = 0; g_b3_late_read = CORO_OK;

    CHECK(coro_rt_spawn(rt, NULL, b3_controller_entry, rt) == CORO_OK);
    CHECK(coro_rt_wait_all(rt) == CORO_OK);
    /* The surviving writer still got the lock after the tail was cancelled. */
    CHECK(g_b3_w1_acquired == 1);
    CHECK(g_b3_w1_st == CORO_OK);
    /* The cancelled tail writer abstained: the reader still held the lock. */
    CHECK(g_b3_w2_st == CORO_ECANCELED);
    /* Readers are not starved afterwards. */
    CHECK(g_b3_late_read == CORO_OK);
    CHECK(coro_rt_destroy(rt) == CORO_OK);
}

static void run_tests(void) {
    /* ---- uncontended round trip, zero-initialized ---- */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        coro_rt_rwlock_t lw;
        memset(&lw, 0, sizeof lw);
        CHECK(coro_rt_rwlock_tryrdlock(rt, &lw) == CORO_OK);
        CHECK(coro_rt_rwlock_tryrdlock(rt, &lw) == CORO_OK);
        CHECK(coro_rt_rwlock_trywrlock(rt, &lw) == CORO_EBUSY);
        CHECK(coro_rt_rwlock_rdunlock(rt, &lw) == CORO_OK);
        CHECK(coro_rt_rwlock_rdunlock(rt, &lw) == CORO_OK);
        CHECK(coro_rt_rwlock_rdunlock(rt, &lw) == CORO_ESTATE);
        CHECK(coro_rt_rwlock_trywrlock(rt, &lw) == CORO_OK);
        CHECK(coro_rt_rwlock_tryrdlock(rt, &lw) == CORO_EBUSY);
        CHECK(coro_rt_rwlock_wrunlock(rt, &lw) == CORO_OK);
        CHECK(coro_rt_rwlock_wrunlock(rt, &lw) == CORO_ESTATE);
        CHECK(coro_rt_rwlock_tryrdlock(NULL, &lw) == CORO_EINVAL);
        CHECK(coro_rt_rwlock_tryrdlock(rt, NULL) == CORO_EINVAL);
        CHECK(coro_rt_rwlock_trywrlock(NULL, &lw) == CORO_EINVAL);
        CHECK(coro_rt_rwlock_trywrlock(rt, NULL) == CORO_EINVAL);
        /* Blocking entry points need a runtime coroutine. */
        CHECK(coro_rt_rwlock_rdlock(rt, &lw) == CORO_ESTALE);
        CHECK(coro_rt_rwlock_wrlock(rt, &lw) == CORO_ESTALE);
        CHECK(coro_rt_rwlock_rdlock(NULL, &lw) == CORO_EINVAL);
        CHECK(coro_rt_rwlock_wrlock(rt, NULL) == CORO_EINVAL);
        CHECK(coro_rt_rwlock_rdunlock(rt, NULL) == CORO_EINVAL);
        CHECK(coro_rt_rwlock_wrunlock(rt, NULL) == CORO_EINVAL);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* ---- readers and writers mixed ---- */
    {
        coro_rt_t* rt = coro_rt_create(3);
        CHECK(rt != NULL);
        memset(&g_rw, 0, sizeof g_rw);
        g_readers_in = g_writers_in = 0;
        g_max_readers = g_max_writers = 0;
        g_conflict = g_read_rounds = g_write_rounds = g_counter = 0;
        for (unsigned i = 0; i < READERS; ++i) {
            CHECK(coro_rt_spawn(rt, NULL, reader_entry, rt) == CORO_OK);
        }
        for (unsigned i = 0; i < WRITERS; ++i) {
            CHECK(coro_rt_spawn(rt, NULL, writer_entry, rt) == CORO_OK);
        }
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
        CHECK(g_conflict == 0);               /* exclusion held both ways */
        CHECK(g_max_writers == 1);            /* writers are exclusive */
        CHECK(g_read_rounds == READERS * ROUNDS);
        CHECK(g_write_rounds == WRITERS * ROUNDS);
        CHECK(g_counter == WRITERS * ROUNDS);
    }

    /* ---- single worker: no deadlock ---- */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        memset(&g_rw, 0, sizeof g_rw);
        g_readers_in = g_writers_in = 0;
        g_max_readers = g_max_writers = 0;
        g_conflict = g_read_rounds = g_write_rounds = g_counter = 0;
        for (unsigned i = 0; i < 4u; ++i) {
            CHECK(coro_rt_spawn(rt, NULL, reader_entry, rt) == CORO_OK);
        }
        for (unsigned i = 0; i < 2u; ++i) {
            CHECK(coro_rt_spawn(rt, NULL, writer_entry, rt) == CORO_OK);
        }
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(g_conflict == 0);
        CHECK(g_max_writers == 1);
        CHECK(g_read_rounds == 4u * ROUNDS);
        CHECK(g_write_rounds == 2u * ROUNDS);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* ---- cancellation while the lock is taken ---- */
    run_cancel_scenario();

    /* ---- two writers queued, the tail one cancelled ----------- */
    run_b3_scenario();
}

TEST_MAIN()
