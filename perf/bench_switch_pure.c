/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * perf/bench_switch_pure.c -- PURE context-switch throughput.
 *
 * The composite benchmark (bench_rt_mn) folds spawn (mmap + guard page +
 * rt_node + MPSC), wait_all and destroy into the timed window and folds the
 * result by runs*2, so its figure mixes allocation cost with switching.
 * This benchmark separates the two: N coroutines are spawned and brought
 * into their yield loop BEFORE the clock starts; the timed window contains
 * only yield -> worker requeue -> drive -> resume round trips -- no
 * allocation, no system call, no lock acquisition on the hot path (the
 * private deque end is a plain store, the credential is one CAS).
 *
 * UNIT (pinned): the counter increments once per coro_rt_yield ROUND TRIP
 * (one yield -> requeue -> drive -> resume). A round trip is TWO context
 * switches (out and back), so "M round-trips/s" equals "2M ctx switches/s"
 * -- the figures printed by bench_switch_pure and bench_rt_mn are
 * comparable only after this conversion. ns/round-trip is the per-yield
 * latency; ns/context-switch is half of it.
 *
 * DISCIPLINE (pinned measurement protocol):
 *   - configuration: -O2, ncoro=8 (single worker);
 *   - 5 timed repeats; the median is the reported figure and the gate
 *     judgement (a single run at the 20M/s gate is noise: the measured
 *     margin is only a few percent);
 *   - gate: >= 20M round-trips/s (>= 40M ctx switches/s, <= 50ns per
 *     round trip). Falling below the gate exits NONZERO;
 *   - the allocation side (spawn/collect, and destroy once) is timed
 *     separately, after the pure window;
 *   - every collect in the allocation section must return CORO_OK (the
 *     EBUSY retry contract is exercised under wait_all; a failure fails
 *     the benchmark).
 *
 *   usage: bench_switch_pure [ncoro]   (default 8; pass 1 for the hottest
 *                                        single-coroutine round trip)
 */
#define _POSIX_C_SOURCE 200112L
#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_sched.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h> /* Sleep; coro_monotonic_ns is declared by coro_sched.h */
#endif

#define SW_ITERS    4000000UL   /* round trips per coroutine */
#define ALLOC_ROUNDS 20000      /* spawn/collect cost sampling rounds */
#define REPEATS     5           /* timed repeats; the median is reported */
#define GATE_RT_S   20e6        /* gate: round trips per second */

static coro_rt_t* g_rt;
static volatile int g_go;

/* One counter per coroutine, written only by that coroutine itself. */
#define MAX_CORO 16
static unsigned long g_switches[MAX_CORO];

static void spinner(void* pv) {
    unsigned id = (unsigned)(size_t)pv;
    while (!g_go) (void)coro_rt_yield();      /* warm-up: untimed */
    for (unsigned long i = 0; i < SW_ITERS; ++i) {
        if (coro_rt_yield() != CORO_OK) return;
        ++g_switches[id];
    }
}

static void quick_entry(void* arg) { (void)arg; }

static double now_s(void) {
#if defined(_WIN32)
    return (double)coro_monotonic_ns() / 1e9;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

/* One timed repeat: fresh coroutines, untimed warm-up, then the pure
   round-trip window. Returns the ns per round trip (0 on failure). */
static double pure_repeat(int ncoro, unsigned long* out_total) {
    for (int i = 0; i < ncoro; ++i) g_switches[i] = 0;
    for (int i = 0; i < ncoro; ++i) {
        if (coro_rt_spawn(g_rt, NULL, spinner, (void*)(size_t)i) != CORO_OK) {
            printf("spawn %d failed\n", i);
            return 0.0;
        }
    }
    /* let every coroutine reach its warm-up loop (parked, not timed) */
#if defined(_WIN32)
    Sleep(50); /* the same 50 ms quantity the POSIX nanosleep below uses */
#else
    struct timespec ts = {0, 50 * 1000 * 1000};
    nanosleep(&ts, NULL);
#endif

    g_go = 1;
    double t0 = now_s();
    coro_status st = coro_rt_wait_all(g_rt);
    double t1 = now_s();
    g_go = 0;
    if (st != CORO_OK) {
        printf("wait_all -> %d (expected CORO_OK)\n", (int)st);
        return 0.0;
    }

    unsigned long total = 0;
    for (int i = 0; i < ncoro; ++i) total += g_switches[i];
    *out_total = total;
    if (!total) return 0.0;
    return (t1 - t0) * 1e9 / (double)total;
}

int main(int argc, char** argv) {
    int ncoro = argc > 1 ? atoi(argv[1]) : 8;
    if (ncoro < 1) ncoro = 1;
    if (ncoro > MAX_CORO) ncoro = MAX_CORO;
    int rc = 0;

    g_rt = coro_rt_create(1);            /* single worker: the hot path */
    if (!g_rt) { printf("create failed\n"); return 2; }

    /* ---- pure switch: 5 repeats, the median is the report ---- */
    double ns_rt[REPEATS];
    unsigned long total = 0;
    printf("pure switch: ncoro=%d window=%lu round trips/coro, %d repeats\n",
           ncoro, SW_ITERS, REPEATS);
    for (int r = 0; r < REPEATS; ++r) {
        unsigned long rtotal = 0;
        ns_rt[r] = pure_repeat(ncoro, &rtotal);
        total = rtotal;
        printf("  repeat %d: switches=%lu => %.1fM round-trips/s "
               "(%.1f ns/round-trip = 2 ctx switches)\n",
               r + 1, rtotal, 1e9 / ns_rt[r] / 1e6, ns_rt[r]);
        if (ns_rt[r] == 0.0) { printf("repeat %d failed\n", r + 1); return 3; }
    }
    qsort(ns_rt, REPEATS, sizeof ns_rt[0], cmp_double);
    double med = ns_rt[REPEATS / 2];
    double med_rt_s = 1e9 / med;
    printf("pure switch: MEDIAN %.1fM round-trips/s (%.1f ns/round-trip, "
           "%.1f ns/context-switch; switches=%lu)\n",
           med_rt_s / 1e6, med, med / 2.0, total);
    if (med_rt_s < GATE_RT_S) {
        printf("pure switch: BELOW the 20M round-trips/s gate (50ns) -- "
               "GATE FAIL\n");
        rc = 4;
    } else {
        printf("pure switch: gate 20M round-trips/s PASSED\n");
    }

    /* ---- allocation side: spawn + collect + destroy, timed separately ---- */
    {
        coro_t* h[ALLOC_ROUNDS];
        double s0 = now_s();
        for (int i = 0; i < ALLOC_ROUNDS; ++i) {
            if (coro_rt_spawn_h(g_rt, NULL, quick_entry, NULL, &h[i]) != CORO_OK) {
                printf("alloc spawn failed at %d\n", i);
                return 5;
            }
        }
        double s1 = now_s();
        /* every quick coroutine finishes at once; wait then collect all */
        coro_status st = coro_rt_wait_all(g_rt);
        if (st != CORO_OK) { printf("alloc wait_all -> %d\n", (int)st); return 6; }
        double c0 = now_s();
        for (int i = 0; i < ALLOC_ROUNDS; ++i) {
            /* The fixed contract: collect after wait_all succeeds at once;
               a failure is a benchmark failure, not a number to discard. */
            if (coro_rt_collect(g_rt, h[i]) != CORO_OK) {
                printf("alloc collect failed at %d\n", i);
                return 7;
            }
        }
        double c1 = now_s();
        printf("alloc: spawn_h %.0f ns/coro (%d), collect %.0f ns/coro\n",
               (s1 - s0) / ALLOC_ROUNDS * 1e9, ALLOC_ROUNDS,
               (c1 - c0) / ALLOC_ROUNDS * 1e9);
        double d0 = now_s();
        coro_status dst = coro_rt_destroy(g_rt);
        double d1 = now_s();
        printf("destroy -> %d (expect 0) in %.0f ns (one runtime)\n",
               (int)dst, (d1 - d0) * 1e9);
        if (dst != CORO_OK) { printf("destroy failed\n"); return 8; }
    }
    return rc;
}
