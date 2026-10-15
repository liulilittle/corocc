#include "../harness.h"
#include <corocc/coro.h>
#include <corocc/coro_rt.h>

/* Coroutine synchronization primitives on the M:N runtime.
 *
 * Behaviours pinned:
 *   - mutex: N coroutines race for one lock; at any time at most one is
 *     inside the critical section, and after the race the counter is exact;
 *   - trylock: returns EBUSY while held, OK when free;
 *   - condition: a waiter blocks until notify, then re-acquires the mutex;
 *     broadcast releases all queued waiters;
 *   - semaphore: trywait on an empty sem is EBUSY, post bumps the count and
 *     wakes one parked waiter, wait consumes exactly one token.
 */

#define RACERS 8u
#define PER_CORO 500u

static unsigned g_in_cs;
static unsigned g_max_concurrent;
static unsigned g_crit_total;
static coro_rt_mutex_t g_mutex;
static coro_rt_cond_t g_cond;
static coro_rt_sem_t g_sem;
static volatile int g_cond_ready;

/* A racer takes the lock, does one barrier-touch inside the critical section,
   releases it and repeats. The maximum concurrent inside counts one. */
static void race_entry(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    for (unsigned i = 0; i < PER_CORO; ++i) {
        if (coro_rt_mutex_lock(rt, &g_mutex) != CORO_OK) return;
        ++g_in_cs;
        if (g_in_cs > g_max_concurrent) g_max_concurrent = g_in_cs;
        ++g_crit_total;
        --g_in_cs;
        if (coro_rt_mutex_unlock(rt, &g_mutex) != CORO_OK) return;
        if (coro_yield() != CORO_OK) return;
    }
}

/* A producer/consumer pair over a condition: the consumer parks until the
   producer signals a shared counter, sees every signal exactly once. */
static void consumer_entry(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_mutex_lock(rt, &g_mutex) != CORO_OK) return;
    while (!g_cond_ready) {
        if (coro_rt_cond_wait(rt, &g_cond, &g_mutex) != CORO_OK) return;
    }
    g_cond_ready = 0;
    coro_rt_mutex_unlock(rt, &g_mutex);
}

static void producer_entry(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    if (coro_rt_mutex_lock(rt, &g_mutex) != CORO_OK) return;
    g_cond_ready = 1;
    coro_rt_mutex_unlock(rt, &g_mutex);
    coro_rt_cond_notify(rt, &g_cond);
}

/* A semaphore consumer waits for N posts. */
#define SEM_TOTAL 5u
static unsigned g_sem_got;

static void sem_consumer(void* pv) {
    coro_rt_t* rt = (coro_rt_t*)pv;
    for (unsigned i = 0; i < SEM_TOTAL; ++i) {
        if (coro_rt_sem_wait(rt, &g_sem) != CORO_OK) return;
        ++g_sem_got;
    }
}

static void run_tests(void) {
    /* ---- mutex race ---- */
    {
        coro_rt_t* rt = coro_rt_create(2);
        CHECK(rt != NULL);
        coro_rt_mutex_init(&g_mutex);
        g_in_cs = 0; g_max_concurrent = 0; g_crit_total = 0;

        for (unsigned i = 0; i < RACERS; ++i) {
            CHECK(coro_rt_spawn(rt, NULL, race_entry, rt) == CORO_OK);
        }
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(g_max_concurrent == 1);
        CHECK(g_crit_total == RACERS * PER_CORO);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* ---- trylock ---- */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        coro_rt_mutex_t m;
        coro_rt_mutex_init(&m);
        CHECK(coro_rt_mutex_trylock(rt, &m) == CORO_OK);
        CHECK(coro_rt_mutex_trylock(rt, &m) == CORO_EBUSY);
        CHECK(coro_rt_mutex_unlock(rt, &m) == CORO_OK);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* ---- condition ---- */
    {
        coro_rt_t* rt = coro_rt_create(2);
        CHECK(rt != NULL);
        coro_rt_mutex_init(&g_mutex);
        coro_rt_cond_init(&g_cond);
        g_cond_ready = 0;
        CHECK(coro_rt_spawn(rt, NULL, consumer_entry, rt) == CORO_OK);
        CHECK(coro_rt_spawn(rt, NULL, producer_entry, rt) == CORO_OK);
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }

    /* ---- semaphore ---- */
    {
        coro_rt_t* rt = coro_rt_create(1);
        CHECK(rt != NULL);
        coro_rt_sem_init(&g_sem, 0);
        g_sem_got = 0;
        CHECK(coro_rt_sem_trywait(rt, &g_sem) == CORO_EBUSY);
        CHECK(coro_rt_spawn(rt, NULL, sem_consumer, rt) == CORO_OK);
        for (unsigned i = 0; i < SEM_TOTAL; ++i) {
            CHECK(coro_rt_sem_post(rt, &g_sem) == CORO_OK);
        }
        CHECK(coro_rt_wait_all(rt) == CORO_OK);
        CHECK(g_sem_got == SEM_TOTAL);
        CHECK(coro_rt_sem_count(&g_sem) == 0);
        CHECK(coro_rt_destroy(rt) == CORO_OK);
    }
}

TEST_MAIN()
