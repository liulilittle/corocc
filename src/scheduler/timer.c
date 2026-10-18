/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/scheduler/timer.c -- monotone clock plus a binary min-heap of deadlines.
 *
 * The heap keeps node pointers in a growable array and orders them by deadline,
 * with each node remembering its own position so removing a deadline that has
 * not fired yet (cancellation) is O(log n) rather than a scan. The soonest
 * deadline is always at the top, so a run loop learns its next wakeup in
 * constant time and sleeps instead of polling.
 */
#include "scheduler/timer.h"

#include "arch/coro_atomic.h"

#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#if defined(_WIN32)
/* QueryPerformanceCounter, calibrated once. The frequency is constant for the
   life of the process, so two threads racing to fill the cache both write the
   same value; QPC is monotone and is not affected by wall-clock adjustments,
   which is the contract coro_monotonic_ns promises. */
static uint64_t coro_qpc_freq; /* 0 until calibrated */

static void coro_qpc_calibrate(void) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    coro_qpc_freq = (uint64_t)f.QuadPart;
}

uint64_t coro_monotonic_ns(void) {
    LARGE_INTEGER c;
    uint64_t q;
    if (coro_qpc_freq == 0) coro_qpc_calibrate();
    QueryPerformanceCounter(&c);
    q = (uint64_t)c.QuadPart;
    /* Split conversion: no 128-bit division on this path. Precision is
       limited by QPF (the tick quantum), not by the arithmetic. */
    return (q / coro_qpc_freq) * 1000000000ull +
           ((q % coro_qpc_freq) * 1000000000ull) / coro_qpc_freq;
}
#else
uint64_t coro_monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

void coro_timer_heap_init(coro_timer_heap* h) {
    if (!h) return;
    h->nodes = NULL;
    h->count = 0;
    h->cap = 0;
}

void coro_timer_heap_destroy(coro_timer_heap* h) {
    if (!h) return;
    free(h->nodes);
    h->nodes = NULL;
    h->count = 0;
    h->cap = 0;
}

static int less(const coro_timer_heap* h, size_t a, size_t b) {
    return h->nodes[a]->deadline < h->nodes[b]->deadline;
}

static void swap_at(coro_timer_heap* h, size_t a, size_t b) {
    coro_timer_node* n = h->nodes[a];
    h->nodes[a] = h->nodes[b];
    h->nodes[b] = n;
    h->nodes[a]->index = a;
    h->nodes[b]->index = b;
}

static void sift_up(coro_timer_heap* h, size_t i) {
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!less(h, i, parent)) return;
        swap_at(h, i, parent);
        i = parent;
    }
}

static void sift_down(coro_timer_heap* h, size_t i) {
    for (;;) {
        size_t left = 2 * i + 1;
        size_t right = left + 1;
        size_t best = i;
        if (left < h->count && less(h, left, best)) best = left;
        if (right < h->count && less(h, right, best)) best = right;
        if (best == i) return;
        swap_at(h, i, best);
        i = best;
    }
}

static int grow(coro_timer_heap* h) {
    if (h->count < h->cap) return 0;
    size_t cap = h->cap ? h->cap * 2 : 16;
    coro_timer_node** grown =
        (coro_timer_node**)realloc(h->nodes, cap * sizeof *grown);
    if (!grown) return -1;
    h->nodes = grown;
    h->cap = cap;
    return 0;
}

int coro_timer_heap_push(coro_timer_heap* h, coro_timer_node* n) {
    if (!h || !n) return -1;
    if (grow(h) != 0) return -1; /* out of memory: the caller sees a missing timer */
    n->index = h->count;
    coro_atp_store((volatile uintptr_t*)&n->heap, (uintptr_t)h);
    h->nodes[h->count] = n;
    ++h->count;
    sift_up(h, n->index);
    return 0;
}

coro_timer_node* coro_timer_heap_top(const coro_timer_heap* h) {
    if (!h || h->count == 0) return NULL;
    return h->nodes[0];
}

coro_timer_node* coro_timer_heap_pop(coro_timer_heap* h) {
    if (!h || h->count == 0) return NULL;
    coro_timer_node* top = h->nodes[0];
    coro_atp_store((volatile uintptr_t*)&top->heap, 0);
    --h->count;
    if (h->count > 0) {
        h->nodes[0] = h->nodes[h->count];
        h->nodes[0]->index = 0;
        sift_down(h, 0);
    }
    return top;
}

int coro_timer_heap_remove(coro_timer_heap* h, coro_timer_node* n) {
    if (!h || !n) return -1;
    size_t i = n->index;
    /* A node that is not in this heap either points past the live entries or
       holds a stale index; both are refused so a double removal is caught. */
    if (i >= h->count || h->nodes[i] != n) return -1;

    --h->count;
    if (i < h->count) {
        h->nodes[i] = h->nodes[h->count];
        h->nodes[i]->index = i;
        sift_down(h, i);
        sift_up(h, i);
    }
    coro_atp_store((volatile uintptr_t*)&n->heap, 0);
    return 0;
}
