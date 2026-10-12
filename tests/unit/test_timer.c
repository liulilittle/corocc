#include "../harness.h"
#include "scheduler/timer.h"

#include <string.h>

/* Milestone M4 Task 1: the monotone clock and the timer heap.
 *
 * The run loop leans on two properties: time never goes backwards, and the
 * soonest deadline is always at the top. Removal is the interesting case --
 * a cancelled sleep must leave the heap before it fires, and removing the same
 * node twice must be refused rather than corrupt the ordering.
 */

#define N 1000u

static coro_timer_node g_nodes[N];
static coro_timer_heap g_heap;
static unsigned long g_state = 0x12345678u;

static unsigned long rnd(void) {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 7;
    g_state ^= g_state << 17;
    return g_state;
}

static void run_tests(void) {
    uint64_t now = coro_monotonic_ns();
    uint64_t later = coro_monotonic_ns();

    /* the clock is monotone and advancing */
    CHECK(now != 0);
    CHECK(later >= now);

    /* ---------- ordering ---------- */
    for (unsigned i = 0; i < N; ++i) {
        g_nodes[i].deadline = now + 1u + (rnd() % 1000000u);
        g_nodes[i].index = 0;
        coro_timer_heap_push(&g_heap, &g_nodes[i]);
    }
    CHECK(g_heap.count == N);

    /* pop order is non-decreasing, and every node comes out exactly once */
    static int seen[N];
    uint64_t prev = 0;
    for (unsigned i = 0; i < N; ++i) {
        coro_timer_node* n = coro_timer_heap_pop(&g_heap);
        CHECK(n != NULL);
        CHECK(n->deadline >= prev);
        prev = n->deadline;
        size_t slot = (size_t)(n - g_nodes);
        CHECK(slot < N);
        CHECK(seen[slot] == 0);
        seen[slot] = 1;
    }
    CHECK(coro_timer_heap_pop(&g_heap) == NULL);
    CHECK(coro_timer_heap_top(&g_heap) == NULL);
    CHECK(g_heap.count == 0u);

    /* ---------- same-instant deadlines ---------- */
    {
        uint64_t due = now + 500u;
        for (unsigned i = 0; i < 10u; ++i) {
            g_nodes[i].deadline = due;
            g_nodes[i].index = 0;
            coro_timer_heap_push(&g_heap, &g_nodes[i]);
        }
        CHECK(g_heap.count == 10u);
        CHECK(coro_timer_heap_top(&g_heap)->deadline == due);
        /* every same-instant node is reachable from the top in a batch */
        unsigned batch = 0;
        while (coro_timer_heap_top(&g_heap) &&
               coro_timer_heap_top(&g_heap)->deadline == due) {
            CHECK(coro_timer_heap_pop(&g_heap) != NULL);
            ++batch;
        }
        CHECK(batch == 10u);
        CHECK(g_heap.count == 0u);
    }

    /* ---------- removal from the middle and its refusal ---------- */
    {
        for (unsigned i = 0; i < 8u; ++i) {
            g_nodes[i].deadline = now + (uint64_t)(i + 1u) * 1000u;
            g_nodes[i].index = 0;
            coro_timer_heap_push(&g_heap, &g_nodes[i]);
        }
        /* remove the fourth and the last, then the rest pops in order */
        static int gone[8];
        CHECK(coro_timer_heap_remove(&g_heap, &g_nodes[3]) == 0);
        gone[3] = 1;
        CHECK(coro_timer_heap_remove(&g_heap, &g_nodes[7]) == 0);
        gone[7] = 1;
        /* a second removal of the same node is refused */
        CHECK(coro_timer_heap_remove(&g_heap, &g_nodes[3]) != 0);
        /* so is removing a node after it has left the heap */
        CHECK(coro_timer_heap_remove(&g_heap, &g_nodes[4]) == 0);
        gone[4] = 1;
        CHECK(coro_timer_heap_remove(&g_heap, &g_nodes[4]) != 0);

        prev = 0;
        unsigned left = 0;
        while (coro_timer_heap_top(&g_heap)) {
            coro_timer_node* n = coro_timer_heap_pop(&g_heap);
            size_t slot = (size_t)(n - g_nodes);
            CHECK(slot < 8u);
            CHECK(n->deadline >= prev);
            prev = n->deadline;
            CHECK(gone[slot] == 0); /* 3 and 7 never come back */
            ++left;
        }
        CHECK(left == 5u);
    }

    /* ---------- randomised removal stress ----------
       Removing from the middle must re-establish the heap invariant, or the
       pop order silently degrades. This is the case a handful of hand-picked
       removals can miss. */
    {
        static coro_timer_node rn[200];
        static int rgone[200];
        memset(rgone, 0, sizeof rgone);
        for (unsigned i = 0; i < 200u; ++i) {
            rn[i].deadline = now + 1u + (rnd() % 100000u);
            rn[i].index = 0;
            coro_timer_heap_push(&g_heap, &rn[i]);
        }
        CHECK(g_heap.count == 200u);
        /* remove a pseudo-random half */
        for (unsigned k = 0; k < 100u; ++k) {
            unsigned idx = (unsigned)(rnd() % 200u);
            if (!rgone[idx]) {
                if (coro_timer_heap_remove(&g_heap, &rn[idx]) == 0) rgone[idx] = 1;
                else CHECK(rgone[idx] == 1); /* already removed: refused */
            }
        }
        /* everything left comes out in non-decreasing deadline order */
        prev = 0;
        unsigned left = 0;
        while (coro_timer_heap_top(&g_heap)) {
            coro_timer_node* n = coro_timer_heap_pop(&g_heap);
            size_t slot = (size_t)(n - rn);
            CHECK(slot < 200u);
            CHECK(n->deadline >= prev);
            prev = n->deadline;
            CHECK(rgone[slot] == 0);
            ++left;
        }
        /* every node is either removed or came out exactly once */
        unsigned out = 0;
        for (unsigned i = 0; i < 200u; ++i) {
            if (rgone[i]) ++out;
        }
        CHECK(out + left == 200u);
        CHECK(g_heap.count == 0u);
    }

    /* ---------- empty-heap edges ---------- */
    CHECK(coro_timer_heap_top(&g_heap) == NULL);
    CHECK(coro_timer_heap_pop(&g_heap) == NULL);
    CHECK(coro_timer_heap_remove(&g_heap, &g_nodes[0]) != 0);

    coro_timer_heap_destroy(&g_heap);
}

TEST_MAIN()
