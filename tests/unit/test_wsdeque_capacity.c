/*
 * tests/unit/test_wsdeque_capacity.c -- fixed-ring capacity invariant.
 *
 * The owner-only Chase-Lev ring must never wrap over a live slot: a push past
 * WSDEQUE_CAPACITY is refused (wsdeque_push returns 0) until a pop frees a
 * slot. A wrapped ring would strand the oldest entry's delivery reference and
 * let a later pop read the covering node twice.
 *
 * Only the deque layer is exercised -- no runtime, no worker, a bounded number
 * of pointer slots. The node payload is opaque: the deque stores the pointer
 * alone, so raw allocations with distinct addresses stand in for rt_node.
 */
#include "../harness.h"

#include "scheduler/wsdeque.h"

#include <stdlib.h>

static int contains(rt_node** seen, size_t n, rt_node* x) {
    for (size_t i = 0; i < n; ++i) {
        if (seen[i] == x) return 1;
    }
    return 0;
}

/* Drain the deque and assert it held exactly `expect` distinct nodes. */
static void drain_is_distinct(wsdeque* d, size_t expect, const char* tag) {
    rt_node** seen = (rt_node**)calloc(expect ? expect : 1, sizeof *seen);
    size_t popped = 0, distinct = 0, dup = 0;
    rt_node* n;
    CHECK(seen != NULL);
    if (!seen) return;
    while ((n = wsdeque_pop(d)) != NULL) {
        if (contains(seen, distinct, n)) ++dup;
        else seen[distinct++] = n;
        ++popped;
    }
    if (dup != 0 || distinct != expect || popped != expect) {
        fprintf(stderr, "%s: popped=%zu distinct=%zu dup=%zu (want %zu)\n",
                tag, popped, distinct, dup, expect);
    }
    CHECK_EQ(dup, 0);
    CHECK_EQ(distinct, expect);
    CHECK_EQ(popped, expect);
    free(seen);
}

static void run_tests(void) {
    enum { N = (int)WSDEQUE_CAPACITY + 1 };
    void** pool = (void**)malloc(sizeof(void*) * (size_t)N);
    CHECK(pool != NULL);
    if (!pool) return;
    for (int i = 0; i < N; ++i) pool[i] = malloc(1); /* distinct addresses */

    /* Scenario 1: fill exactly, then the next push is refused (no wrap). */
    {
        wsdeque d;
        wsdeque_init(&d);
        for (size_t i = 0; i < WSDEQUE_CAPACITY; ++i) {
            CHECK_EQ(wsdeque_push(&d, (rt_node*)pool[i]), 1);
        }
        CHECK_EQ(wsdeque_size(&d), WSDEQUE_CAPACITY);
        CHECK_EQ(wsdeque_full(&d), 1);
        CHECK_EQ(wsdeque_push(&d, (rt_node*)pool[WSDEQUE_CAPACITY]), 0);
        CHECK_EQ(wsdeque_size(&d), WSDEQUE_CAPACITY);
        drain_is_distinct(&d, WSDEQUE_CAPACITY, "overflow-refused");
        CHECK_EQ(wsdeque_full(&d), 0);
    }

    /* Scenario 2: one pop frees exactly one slot; the refused push then lands. */
    {
        wsdeque d;
        wsdeque_init(&d);
        for (size_t i = 0; i < WSDEQUE_CAPACITY; ++i) {
            CHECK_EQ(wsdeque_push(&d, (rt_node*)pool[i]), 1);
        }
        CHECK(wsdeque_pop(&d) != NULL);
        CHECK_EQ(wsdeque_full(&d), 0);
        CHECK_EQ(wsdeque_push(&d, (rt_node*)pool[WSDEQUE_CAPACITY]), 1);
        CHECK_EQ(wsdeque_full(&d), 1);
        drain_is_distinct(&d, WSDEQUE_CAPACITY, "recycle-one-slot");
    }

    /* Scenario 3: drain and refill across the wrap boundary, repeatedly. */
    {
        wsdeque d;
        wsdeque_init(&d);
        for (int cycle = 0; cycle < 3; ++cycle) {
            for (size_t i = 0; i < WSDEQUE_CAPACITY; ++i) {
                CHECK_EQ(wsdeque_push(&d, (rt_node*)pool[i]), 1);
            }
            CHECK_EQ(wsdeque_push(&d, (rt_node*)pool[WSDEQUE_CAPACITY]), 0);
            CHECK(wsdeque_pop(&d) != NULL);
            CHECK_EQ(wsdeque_push(&d, (rt_node*)pool[WSDEQUE_CAPACITY]), 1);
            drain_is_distinct(&d, WSDEQUE_CAPACITY, "wrap-refill");
        }
    }

    for (int i = 0; i < N; ++i) free(pool[i]);
    free(pool);
}

TEST_MAIN()
