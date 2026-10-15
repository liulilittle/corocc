/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * perf/probe_collect_race.c -- collect-vs-retire race regression probe
 * (NOT a ctest target).
 *
 * Red/green proof of the collect-vs-retire window: coro_rt_collect must gate
 * on RT_DONE (set by the retire under act_lock), never on the coroutine's
 * own DEAD state alone -- the settle publishes DEAD before the worker retires
 * the node, and freeing in that window made retire_node touch freed memory
 * (ASan heap-use-after-free).
 *
 * This probe maximizes the window hit rate: a single worker, an immediately
 * returning child, and a busy poll for DEAD followed by collect. On the
 * pre-fix gate the race reproduces (probabilistically: roughly 5% of runs
 * under ASan abort); on the fixed gate every round converges, with the
 * documented EBUSY retry for the rare finished-but-not-yet-retired round.
 *
 *   usage: probe_collect_race [rounds]   (default 2000)
 *   env:   COLLECT_SPIN=1 -> collect retries with no back-off (maximum
 *                            pressure)
 *
 * It is intentionally NOT registered as a ctest: the red signal is
 * probabilistic, so a single run is neither red nor green evidence -- run it
 * repeatedly (the scripts under tests/CMakeLists.txt describe the driver).
 */
#define _POSIX_C_SOURCE 200112L
#include <corocc/coro.h>
#include <corocc/coro_rt.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h> /* Sleep */
#endif

static void quick(void* arg) { (void)arg; }

static void nap_us(unsigned us) {
    if (getenv("COLLECT_SPIN")) return;   /* maximum collect pressure mode */
#if defined(_WIN32)
    /* Windows timer granularity is milliseconds; round the microsecond
       request up so a real (never busy) sleep is taken. */
    Sleep((DWORD)((us + 999u) / 1000u));
#else
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = (long)us * 1000L;
    nanosleep(&ts, NULL);
#endif
}

int main(int argc, char** argv) {
    unsigned rounds = argc > 1 ? (unsigned)atoi(argv[1]) : 2000;
    unsigned ebusy = 0, collected = 0, retried = 0, max_retries = 0;
    coro_rt_t* rt = coro_rt_create(1);
    if (!rt) { printf("create failed\n"); return 2; }
    for (unsigned i = 0; i < rounds; ++i) {
        coro_t* h = NULL;
        if (coro_rt_spawn_h(rt, NULL, quick, NULL, &h) != CORO_OK) {
            printf("spawn failed at %u\n", i);
            return 3;
        }
        /* busy-poll: the tightest DEAD -> collect gap achievable */
        for (unsigned s = 0; s < 200000000u && coro_get_state(h) != CORO_DEAD; ++s) {
        }
        if (coro_get_state(h) != CORO_DEAD) {
            printf("child never reached DEAD at round %u\n", i);
            return 4;
        }
        coro_status st = coro_rt_collect(rt, h);
        unsigned tries = 0;
        /* The fixed contract: a finished-but-not-yet-retired coroutine
           reports EBUSY; the retire completes promptly, so retrying (with a
           cooperative back-off, never a busy-wait) converges. */
        while (st == CORO_EBUSY) {
            if (++tries > 1000000u) break;
            nap_us(50);
            st = coro_rt_collect(rt, h);
        }
        if (st == CORO_OK) {
            ++collected;
            if (tries) { ++retried; if (tries > max_retries) max_retries = tries; }
        } else if (st == CORO_EBUSY) {
            ++ebusy;   /* never converged: a permanent EBUSY would land here */
        } else {
            printf("collect -> %d at round %u\n", (int)st, i);
            return 5;
        }
    }
    printf("race: rounds=%u collected=%u retried=%u max_retries=%u "
           "converge_fail=%u destroy=%d\n",
           rounds, collected, retried, max_retries, ebusy,
           (int)coro_rt_destroy(rt));
    return 0;
}
