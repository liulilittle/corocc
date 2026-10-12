#ifndef COROCC_SCHEDULER_TIMER_H
#define COROCC_SCHEDULER_TIMER_H

/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * src/scheduler/timer.h -- monotone clock and a heap of expiry deadlines.
 *
 * The scheduler needs two things a coroutine library must not invent: a clock
 * that never jumps backwards (wall-clock time can step, a monotone source
 * cannot), and an ordered container of deadlines where the next expiry is
 * always at the top, so a run loop can sleep until something is due instead of
 * polling.
 *
 * This header is INTERNAL: it is not installed and is not part of the public
 * API or ABI. It compiles as C99.
 */
#include <stdint.h>
#include <stddef.h>

/** coro_timer_node -- one deadline; the owner embeds it by value. */
typedef struct coro_timer_node {
    uint64_t deadline;   /**< Absolute monotone nanoseconds. */
    size_t   index;      /**< PRIVATE: position in its heap, for O(log n) removal. */
} coro_timer_node;

/** coro_timer_heap -- deadlines ordered soonest first. */
typedef struct coro_timer_heap {
    struct coro_timer_node** nodes; /**< PRIVATE: heap of node pointers. */
    size_t                   count; /**< PRIVATE: live node count. */
    size_t                   cap;   /**< PRIVATE: allocated slots. */
} coro_timer_heap;

/**
 * coro_monotonic_ns -- current time from a monotone source.
 *
 * @return Nanoseconds from an unspecified epoch; only differences are
 *         meaningful. Never decreases within a process.
 */
uint64_t coro_monotonic_ns(void);

/** coro_timer_heap_init -- prepare an empty heap. */
void coro_timer_heap_init(coro_timer_heap* h);

/** coro_timer_heap_destroy -- release a heap's storage (nodes are embedded). */
void coro_timer_heap_destroy(coro_timer_heap* h);

/**
 * coro_timer_heap_push -- schedule a node at its deadline.
 *
 * The node must not already be in a heap; its deadline must already be set.
 */
void coro_timer_heap_push(coro_timer_heap* h, coro_timer_node* n);

/** coro_timer_heap_top -- soonest deadline, NULL when empty. */
coro_timer_node* coro_timer_heap_top(const coro_timer_heap* h);

/**
 * coro_timer_heap_pop -- remove and return the soonest node.
 *
 * Returns NULL when the heap is empty. Several nodes sharing the soonest
 * deadline need repeated calls; the caller batches them.
 */
coro_timer_node* coro_timer_heap_pop(coro_timer_heap* h);

/**
 * coro_timer_heap_remove -- drop a node that has not expired yet.
 *
 * Used by cancellation, where a sleeping coroutine's deadline must leave the
 * heap before it fires. Removing a node that is not in this heap is refused,
 * so a double removal cannot corrupt the ordering.
 *
 * @return 0 on success, non-zero when the node is not in this heap.
 */
int coro_timer_heap_remove(coro_timer_heap* h, coro_timer_node* n);

#endif /* COROCC_SCHEDULER_TIMER_H */
