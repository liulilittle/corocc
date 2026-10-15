#include <corocc/coro.h>
#include <corocc/coro_rt.h>

#include "../src/scheduler/timer.h" /* coro_monotonic_ns; no src/ on the include path */

#include <stdio.h>
#include <stdlib.h>

#define COROS 200u
#define ITERS 100u

static unsigned g_runs[COROS];

static void entry_count(void* pv) {
    unsigned id = (unsigned)(size_t)pv;
    for (unsigned i = 0; i < ITERS; ++i) {
        if (coro_yield() != CORO_OK) return;
        ++g_runs[id];
    }
}

static uint64_t now_ns(void) {
    return coro_monotonic_ns();
}

int main(void) {
    for (size_t nthreads = 1; nthreads <= 4; nthreads *= 2) {
        coro_rt_t* rt = coro_rt_create(nthreads);
        if (!rt) { fprintf(stderr, "create(%zu) failed\n", nthreads); return 1; }
        for (unsigned i = 0; i < COROS; ++i) g_runs[i] = 0;
        uint64_t t0 = now_ns();
        for (unsigned i = 0; i < COROS; ++i) {
            if (coro_rt_spawn(rt, NULL, entry_count, (void*)(size_t)i) != CORO_OK) {
                fprintf(stderr, "spawn failed\n"); return 1;
            }
        }
        coro_status st = coro_rt_wait_all(rt);
        uint64_t t1 = now_ns();
        if (st != CORO_OK) { fprintf(stderr, "wait_all=%d\n", (int)st); return 1; }
        unsigned total = 0;
        for (unsigned i = 0; i < COROS; ++i) total += g_runs[i];
        uint64_t elapsed_ns = t1 - t0;
        double switches = (double)total * 2.0;
        double s = (double)elapsed_ns / 1e9;
        printf("workers=%zu total_runs=%u elapsed=%.3fs switches/s=%.0f ns/switch=%.1f\n",
               nthreads, total, s, switches / s,
               elapsed_ns ? (double)elapsed_ns / switches : 0.0);
        coro_rt_destroy(rt);
    }
    return 0;
}
