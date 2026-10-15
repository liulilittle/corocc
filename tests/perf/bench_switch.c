/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * tests/perf/bench_switch.c -- context-switch cost benchmark.
 *
 * Ping-pongs between the main context and one coroutine: each loop iteration
 * performs one swap main->coroutine and one swap coroutine->main (two
 * context switches per iteration). Reports ns per switch measured with the
 * library's own monotone clock, coro_monotonic_ns (src/scheduler/timer.h is
 * reachable by relative path here; the tests have no src/ on their include
 * path).
 */
#include <corocc/coro_ctx.h>

#include "../../src/scheduler/timer.h"

#include <stdio.h>

#define ITERATIONS 5000000u

static coro_ctx work_ctx;
static coro_ctx main_ctx;
static volatile unsigned long g_count = 0;

/* Resumed by every main->coroutine swap; yields back to main each time. */
static void entry(void* arg) {
    (void)arg;
    while (g_count < (unsigned long)ITERATIONS) {
        ++g_count;
        coro_ctx_swap(&work_ctx, &main_ctx);
    }
}

static double now_ns(void) {
    return (double)coro_monotonic_ns();
}

int main(void) {
    coro_stack stk;
    if (coro_stack_alloc(&stk, 64 * 1024) != CORO_OK) {
        return 1;
    }
    coro_ctx_init(&work_ctx, &stk, entry, NULL);

    double t0 = now_ns();
    while (g_count < (unsigned long)ITERATIONS) {
        coro_ctx_swap(&main_ctx, &work_ctx);
    }
    double t1 = now_ns();

    /* One loop iteration = one main->coro swap + one coro->main swap. */
    double total_switches = 2.0 * (double)ITERATIONS;
    double ns_per_switch = (t1 - t0) / total_switches;
    printf("switches=%.0f ns/switch=%.2f\n", total_switches, ns_per_switch);

    coro_stack_free(&stk);
    return 0;
}
