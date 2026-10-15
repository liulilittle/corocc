/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * perf/bench_cpp_switch.cpp -- the C++ wrapper's zero-overhead proof.
 *
 * A/B in one binary: the C face drives pre-spawned coroutines through
 * coro_rt_spawn + coro_rt_yield; the C++ face drives the same shape through
 * corocc::runtime::spawn + corocc::yield(). The timed window contains only
 * yield -> requeue -> drive -> resume round trips (spawn/collect happen
 * before the clock starts), the configuration is the pinned one (-O2,
 * ncoro=8, single worker), each face runs REPEATS times and the MEDIAN is
 * the reported figure -- a single run at the 20M/s gate is noise.
 *
 * UNIT (pinned): one round trip = one yield -> requeue -> drive -> resume =
 * TWO context switches, so "M round-trips/s" equals "2M ctx switches/s";
 * both conventions are printed for both faces.
 *
 * GATES (exit nonzero on failure), the design's dual track:
 *   - strict (default): both medians >= 20M round-trips/s and the ratio
 *     C++/C >= 0.95 -- the numbers recorded for the performance ledger;
 *   - loose (--loose, used by the ctest entry `cpp_perf`): both medians
 *     >= 10M round-trips/s and the ratio >= 0.85 -- a coarse-regression
 *     tripwire that still fails a catastrophic regression.
 * The spawn-side cost (per-coroutine, reflecting the one extra control
 * block) is reported next to it with no hard gate. ctest registration is
 * configuration-gated (Release-like and not ASan) by the build file; this
 * binary is always safe to run by hand.
 *
 * usage: bench_cpp_switch [ncoro] [--loose]   (default 8; pass 1 for the
 *                                   hottest single-coroutine round trip)
 */
#include <corocc/coro.hpp>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h> /* Sleep; coro_monotonic_ns comes from coro_sched.h */
#endif

#define SW_ITERS 4000000UL
#define ALLOC_ROUNDS 20000
#define REPEATS 5

namespace {

coro_rt_t* g_c_rt = NULL;
corocc::runtime* g_cpp_rt = NULL;
volatile int g_go = 0;

#define MAX_CORO 16
unsigned long g_switches[MAX_CORO];

void c_spinner(void* pv) {
    const unsigned id = static_cast<unsigned>(reinterpret_cast<size_t>(pv));
    while (!g_go) (void)coro_rt_yield();
    for (unsigned long i = 0; i < SW_ITERS; ++i) {
        if (coro_rt_yield() != CORO_OK) return;
        ++g_switches[id];
    }
}

void quick_entry(void* arg) {
    (void)arg;
}

double now_s() {
#if defined(_WIN32)
    return static_cast<double>(coro_monotonic_ns()) / 1e9;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) +
           static_cast<double>(ts.tv_nsec) / 1e9;
#endif
}

int cmp_double(const void* a, const void* b) {
    const double x = *static_cast<const double*>(a);
    const double y = *static_cast<const double*>(b);
    return (x > y) - (x < y);
}

void window_sleep_50ms() {
#if defined(_WIN32)
    Sleep(50);
#else
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 50 * 1000 * 1000;
    nanosleep(&ts, NULL);
#endif
}

/* One timed C repeat: fresh coroutines, untimed warm-up, pure window.
 * Returns the ns per round trip (0 on failure). */
double pure_repeat_c(int ncoro, unsigned long* out_total) {
    for (int i = 0; i < ncoro; ++i) g_switches[i] = 0;
    for (int i = 0; i < ncoro; ++i) {
        if (coro_rt_spawn(g_c_rt, NULL, c_spinner,
                          reinterpret_cast<void*>(static_cast<size_t>(i))) !=
            CORO_OK) {
            printf("C spawn %d failed\n", i);
            return 0.0;
        }
    }
    window_sleep_50ms();

    g_go = 1;
    const double t0 = now_s();
    const coro_status st = coro_rt_wait_all(g_c_rt);
    const double t1 = now_s();
    g_go = 0;
    if (st != CORO_OK) {
        printf("C wait_all -> %d\n", static_cast<int>(st));
        return 0.0;
    }
    unsigned long total = 0;
    for (int i = 0; i < ncoro; ++i) total += g_switches[i];
    *out_total = total;
    if (!total) return 0.0;
    return (t1 - t0) * 1e9 / static_cast<double>(total);
}

/* One timed C++ repeat: same shape through the wrapper. */
double pure_repeat_cpp(int ncoro, unsigned long* out_total) {
    for (int i = 0; i < ncoro; ++i) g_switches[i] = 0;
    for (int i = 0; i < ncoro; ++i) {
        const unsigned id = static_cast<unsigned>(i);
        if (!g_cpp_rt
                 ->spawn([id]() {
                     while (!g_go) (void)corocc::yield();
                     for (unsigned long k = 0; k < SW_ITERS; ++k) {
                         if (corocc::yield().code() != CORO_OK) return;
                         ++g_switches[id];
                     }
                 })
                 .ok()) {
            printf("C++ spawn %d failed\n", i);
            return 0.0;
        }
    }
    window_sleep_50ms();

    g_go = 1;
    const double t0 = now_s();
    const coro_status st = coro_rt_wait_all(g_cpp_rt->native());
    const double t1 = now_s();
    g_go = 0;
    if (st != CORO_OK) {
        printf("C++ wait_all -> %d\n", static_cast<int>(st));
        return 0.0;
    }
    unsigned long total = 0;
    for (int i = 0; i < ncoro; ++i) total += g_switches[i];
    *out_total = total;
    if (!total) return 0.0;
    return (t1 - t0) * 1e9 / static_cast<double>(total);
}

/* The spawn/collect side, timed separately, no hard gate. */
void alloc_side(int* rc) {
    {
        coro_t* h[ALLOC_ROUNDS];
        const double s0 = now_s();
        for (int i = 0; i < ALLOC_ROUNDS; ++i) {
            if (coro_rt_spawn_h(g_c_rt, NULL, quick_entry, NULL, &h[i]) !=
                CORO_OK) {
                printf("C alloc spawn failed at %d\n", i);
                *rc = 5;
                return;
            }
        }
        const double s1 = now_s();
        if (coro_rt_wait_all(g_c_rt) != CORO_OK) {
            printf("C alloc wait_all failed\n");
            *rc = 6;
            return;
        }
        const double c0 = now_s();
        for (int i = 0; i < ALLOC_ROUNDS; ++i) {
            if (coro_rt_collect(g_c_rt, h[i]) != CORO_OK) {
                printf("C alloc collect failed at %d\n", i);
                *rc = 7;
                return;
            }
        }
        const double c1 = now_s();
        printf("spawn side C  : spawn_h %.0f ns/coro, collect %.0f ns/coro "
               "(%d round)\n",
               (s1 - s0) / ALLOC_ROUNDS * 1e9, (c1 - c0) / ALLOC_ROUNDS * 1e9,
               ALLOC_ROUNDS);
    }
    {
        std::vector<corocc::task<void> > tasks;
        tasks.reserve(ALLOC_ROUNDS);
        const double s0 = now_s();
        for (int i = 0; i < ALLOC_ROUNDS; ++i) {
            corocc::task<void> t;
            if (!g_cpp_rt->spawn_h(t, []() {}).ok()) {
                printf("C++ alloc spawn failed at %d\n", i);
                *rc = 8;
                return;
            }
            tasks.push_back(std::move(t));
        }
        const double s1 = now_s();
        if (coro_rt_wait_all(g_cpp_rt->native()) != CORO_OK) {
            printf("C++ alloc wait_all failed\n");
            *rc = 9;
            return;
        }
        const double c0 = now_s();
        for (int i = 0; i < ALLOC_ROUNDS; ++i) {
            if (!tasks[static_cast<size_t>(i)].wait().ok()) {
                printf("C++ alloc wait/collect failed at %d\n", i);
                *rc = 10;
                return;
            }
        }
        const double c1 = now_s();
        printf("spawn side C++: spawn_h %.0f ns/coro, wait/collect %.0f "
               "ns/coro (%d round)\n",
               (s1 - s0) / ALLOC_ROUNDS * 1e9, (c1 - c0) / ALLOC_ROUNDS * 1e9,
               ALLOC_ROUNDS);
    }
}

} /* namespace */

int main(int argc, char** argv) {
    int ncoro = 8;
    double gate_rt = 20e6;
    double gate_ratio = 0.95;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--loose") == 0) {
            gate_rt = 10e6;
            gate_ratio = 0.85;
        } else {
            ncoro = atoi(argv[i]);
            if (ncoro < 1) ncoro = 1;
            if (ncoro > MAX_CORO) ncoro = MAX_CORO;
        }
    }
    int rc = 0;

    g_c_rt = coro_rt_create(1);
    if (!g_c_rt) {
        printf("C runtime create failed\n");
        return 2;
    }
    corocc::runtime cpp_rt(1);
    if (!cpp_rt.valid()) {
        printf("C++ runtime create failed\n");
        coro_rt_destroy(g_c_rt);
        return 2;
    }
    g_cpp_rt = &cpp_rt;

    double ns_c[REPEATS];
    double ns_cpp[REPEATS];
    unsigned long total_c = 0;
    unsigned long total_cpp = 0;

    printf("bench_cpp_switch: ncoro=%d window=%lu round trips/coro, %d "
           "repeats\n",
           ncoro, SW_ITERS, REPEATS);
    for (int r = 0; r < REPEATS; ++r) {
        ns_c[r] = pure_repeat_c(ncoro, &total_c);
        ns_cpp[r] = pure_repeat_cpp(ncoro, &total_cpp);
        printf("  repeat %d: C %.1f ns/rt (%.1fM rt/s) | C++ %.1f ns/rt "
               "(%.1fM rt/s)\n",
               r + 1, ns_c[r], 1e9 / ns_c[r] / 1e6, ns_cpp[r],
               1e9 / ns_cpp[r] / 1e6);
        if (ns_c[r] == 0.0 || ns_cpp[r] == 0.0) {
            printf("repeat %d failed\n", r + 1);
            coro_rt_destroy(g_c_rt);
            return 3;
        }
    }
    qsort(ns_c, REPEATS, sizeof ns_c[0], cmp_double);
    qsort(ns_cpp, REPEATS, sizeof ns_cpp[0], cmp_double);
    const double med_c = ns_c[REPEATS / 2];
    const double med_cpp = ns_cpp[REPEATS / 2];
    const double rt_c = 1e9 / med_c;
    const double rt_cpp = 1e9 / med_cpp;
    const double ratio = med_c / med_cpp; /* round-trips ratio = C ns / C++ ns */

    printf("median C  : %.1fM round-trips/s (%.1f ns/rt, %.1f ns/ctx "
           "switch; switches=%lu)\n",
           rt_c / 1e6, med_c, med_c / 2.0, total_c);
    printf("median C++: %.1fM round-trips/s (%.1f ns/rt, %.1f ns/ctx "
           "switch; switches=%lu)\n",
           rt_cpp / 1e6, med_cpp, med_cpp / 2.0, total_cpp);
    printf("ratio C++/C (round trips) = %.3f (gate >= %.2f)\n", ratio,
           gate_ratio);

    if (rt_c < gate_rt) {
        printf("GATE FAIL: C median below %.0fM round-trips/s\n",
               gate_rt / 1e6);
        rc = 4;
    }
    if (rt_cpp < gate_rt) {
        printf("GATE FAIL: C++ median below %.0fM round-trips/s\n",
               gate_rt / 1e6);
        rc = 4;
    }
    if (ratio < gate_ratio) {
        printf("GATE FAIL: ratio below %.2f\n", gate_ratio);
        rc = 4;
    }
    if (rc == 0) {
        printf("gate %.0fM round-trips/s PASSED for both faces; ratio "
               "PASSED\n",
               gate_rt / 1e6);
    }

    alloc_side(&rc);

    coro_rt_destroy(g_c_rt);
    return rc;
}
