/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * coro_scope.h -- structured concurrency for the M:N runtime (L3).
 *
 * A scope owns a set of child coroutines and gives them one lifetime: every
 * child is registered at spawn, every child's result is aggregated, and the
 * scope only reports done once every child it ever accepted has finished.
 * That is the structured-concurrency invariant the wait family enforces --
 * wait returns only when the scope's live count reached zero, so no child
 * outlives the wait that covers it.
 *
 * Design contract, the short form:
 *
 *   - one lock: every scope state transition runs under the runtime's
 *     activity lock (the same lock the sync primitives use); the state word
 *     itself is an atomic;
 *   - one parking point: a waiter parks on the scope's own event chain, at
 *     most one chain at a time, and is woken exactly once per park;
 *   - the trampoline is the only writer of a child's result cell: the scope
 *     never pre-writes a result (a second writer would race the trampoline),
 *     and a cancel is a request to the child, not a result;
 *   - collect's gate is retirement (RT_DONE): a child that finished but has
 *     not been retired yet reports CORO_EBUSY, and the scope's finish paths
 *     retry with a yield in between, never by spinning;
 *   - wait does not use coro_rt_join; children notify the scope through the
 *     event chain, which is what provides the ordering the aggregation reads.
 *
 * The header is conservative: it compiles as C99 and C++11 and includes no
 * third-party header.
 */
#ifndef COROCC_CORO_SCOPE_H
#define COROCC_CORO_SCOPE_H

#include <stddef.h>
#include <stdint.h>

#include "coro_rt.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * coro_rt_scope_t -- a structured-concurrency scope.
 *
 * The object is complete so a caller can embed it by value (on the stack or
 * inside a pool) with no heap allocation and no create call, exactly like
 * the synchronization objects in coro_rt.h: a zero-initialized scope is
 * the OPEN state, which coro_rt_scope_init makes explicit. The fields are
 * private: treat them as opaque and drive the scope only through the
 * coro_rt_scope_* and coro_rt_group_* API below.
 * coro_rt_scope_destroy returns the object to the zero state with the
 * DESTROYED tombstone set, so use after destroy is refused, not undefined.
 */
struct coro_rt_scope_t {
    uint64_t state;       /**< Private: OPEN=0, CANCELING=1, DRAINED=2,
                               DESTROYED=3 (atomic). */
    void*    rt;          /**< Private: bound runtime (atomic, set once). */
    void**   children;    /**< Private: child record table. */
    size_t   nchildren;   /**< Private: table entries in use. */
    size_t   cap;         /**< Private: table entries allocated. */
    size_t   live;        /**< Private: children not yet finished; guarded
                               by the runtime's activity lock. */
    uint64_t status;      /**< Private: aggregate result code; guarded by
                               the activity lock. */
    void*    waitq;       /**< Private: scope event chain head; guarded by
                               the activity lock. */
    uint64_t first_done;  /**< Private: 1 once any child has finished. */
    uint64_t first_value; /**< Private: that child's result code. */
    uint64_t timer_fired; /**< Private: watchdog claim (atomic). */
};

typedef struct coro_rt_scope_t coro_rt_scope_t;

/**
 * coro_scope_fn -- entry point of a scoped child.
 *
 * The return value becomes the child's result cell: the trampoline the
 * scope installs writes it, and it is the only writer. A child that returns
 * after observing a cancellation normally reports CORO_ECANCELED itself;
 * the scope never overwrites that value.
 */
typedef coro_status (*coro_scope_fn)(void* arg);

/**
 * coro_rt_scope_init -- bring a scope to the OPEN state.
 *
 * @return CORO_OK on success; CORO_EINVAL for NULL. Call it once, on a
 *         fresh scope: a scope with live children must be drained and
 *         destroyed first.
 */
COROCC_API coro_status coro_rt_scope_init(coro_rt_scope_t* s);

/**
 * coro_rt_scope_spawn -- register a child on this scope.
 *
 * The child runs on the runtime under the scope's trampoline: when fn
 * returns, its value is written to the child's result cell, the scope's live
 * count drops, and every waiter is notified through the scope event chain.
 *
 * Spawning is refused once the scope left OPEN: CORO_ESTATE while canceling
 * or drained, CORO_ESTALE once destroyed. A spawn that races a cancel still
 * registers (and the scope cancels) its child, so the wait family always
 * covers every child the scope accepted -- no child escapes cancellation by
 * winning a registration race.
 *
 * @param rt       Runtime that schedules the child. Must not be NULL.
 * @param s        Scope to register on. Must not be NULL.
 * @param attr     Child attributes, or NULL for the defaults.
 * @param fn       Child entry point. Must not be NULL.
 * @param arg      Opaque argument forwarded to fn.
 * @param cell_out Optional: receives the address of the child's result
 *                 cell, valid until the scope is destroyed. NULL is allowed
 *                 and means "no result": the child is then not aggregated
 *                 (an explicit opt-out, not an omission).
 * @return CORO_OK on success; CORO_EINVAL for NULL arguments;
 *         CORO_ESTATE when the scope is canceling or drained;
 *         CORO_ESTALE when the scope is destroyed; CORO_ENOMEM when the
 *         child table could not grow (nothing is registered in that case).
 */
COROCC_API coro_status coro_rt_scope_spawn(coro_rt_t* rt, coro_rt_scope_t* s,
                                           const coro_attr_t* attr,
                                           coro_scope_fn fn, void* arg,
                                           coro_status** cell_out);

/**
 * coro_rt_scope_cancel -- ask every child of this scope to stop.
 *
 * Idempotent: the OPEN -> CANCELING transition happens once, further calls
 * report CORO_OK. Each live child receives one coro_rt_cancel, issued
 * outside the activity lock. The scope never writes a result cell: a
 * cancelled child reports its own final value (conventionally
 * CORO_ECANCELED), and the aggregation reads that value, so there is no
 * second writer and no overwritten result.
 *
 * Caller responsibility:
 * cancel only interrupts children that are running or sleeping right away.
 * A child blocked inside a synchronization primitive observes the request
 * only when the event it waits for happens; and if children wait on each
 * other while the whole scope is cancelled, wait may not terminate -- the
 * caller owns that scenario.
 *
 * @return CORO_OK on success (including the idempotent repeat);
 *         CORO_EINVAL for NULL; CORO_ESTALE when destroyed; CORO_ESTATE
 *         when the scope is already drained.
 */
COROCC_API coro_status coro_rt_scope_cancel(coro_rt_t* rt, coro_rt_scope_t* s);

/**
 * coro_rt_scope_wait -- wait until every child of this scope has finished.
 *
 * Two entries, both documented:
 *
 *   - from a coroutine scheduled by this runtime the waiter parks on the
 *     scope's event chain and yields, so the runtime keeps making
 *     progress; the wait is immune to the waiter's own cancellation (a
 *     cancellation consumed at the park's safe point detaches the waiter
 *     and the wait continues until the live count reaches zero -- an
 *     interrupted wait would leave children running with nobody covering
 *     them);
 *   - from a plain thread the call blocks that thread on the runtime's
 *     settled condition until the same live==0 predicate holds, then
 *     drains. It never reports CORO_EAGAIN: the "spawn, then wait from
 *     main" pattern is supported directly.
 *
 * After the drain the scope is DRAINED, every child has been collected,
 * and the aggregate result is returned. The aggregate is the first non-OK
 * result in completion order, or CORO_OK. A child spawned without a
 * result cell does not contribute.
 *
 * @return CORO_OK or the aggregate child status; CORO_EINVAL for NULL;
 *         CORO_ESTALE when the scope is destroyed.
 */
COROCC_API coro_status coro_rt_scope_wait(coro_rt_t* rt, coro_rt_scope_t* s);

/**
 * coro_rt_scope_wait_for -- wait with a deadline, in milliseconds.
 *
 * From a coroutine, a watchdog coroutine (a retained spawn with a
 * deliberately small stack, never registered as a child) sleeps until the
 * deadline and then claims the scope's timeout exactly once, under the
 * activity lock, by canceling every child. From a plain thread no
 * watchdog is needed: the thread waits on the deadline itself (a timed
 * condition wait) and claims the same timeout. Either way the wait still
 * ends only when the live count reaches zero -- the structured invariant
 * holds, and draining after a timeout is unbounded by design, a caller
 * responsibility documented alongside the cancel limitation above.
 *
 * Return-code priority: this waiter's deadline hit first -> CORO_ECANCELED;
 * otherwise the first non-OK child result; otherwise CORO_OK.
 *
 * @param rt          Runtime that schedules the scope's children.
 * @param s           Scope to wait for.
 * @param timeout_ms  Deadline in milliseconds from now.
 * @return CORO_OK, the aggregate child status, or CORO_ECANCELED on
 *         timeout; CORO_EINVAL for NULL; CORO_ESTALE when destroyed;
 *         CORO_ENOMEM when the watchdog could not be spawned (fail-fast,
 *         before any child is registered; the thread entry cannot fail
 *         this way).
 */
COROCC_API coro_status coro_rt_scope_wait_for(coro_rt_t* rt,
                                              coro_rt_scope_t* s,
                                              uint64_t timeout_ms);

/**
 * coro_rt_scope_status -- the scope's aggregate result.
 *
 * The first non-OK child result in completion order, or CORO_OK. Reading
 * it before the scope is drained observes a partial aggregate.
 *
 * @return The aggregate; CORO_EINVAL for NULL; CORO_ESTALE when destroyed.
 */
COROCC_API coro_status coro_rt_scope_status(const coro_rt_scope_t* s);

/**
 * coro_rt_scope_cancel_requested -- whether a cancel is in effect.
 *
 * @return 1 while the scope is canceling, 0 otherwise (and 0 for NULL).
 */
COROCC_API int coro_rt_scope_cancel_requested(const coro_rt_scope_t* s);

/**
 * coro_rt_scope_destroy -- release a drained scope.
 *
 * Refuses while children are live (CORO_EBUSY). Does not collect children:
 * collect runs in the wait family, so drain the scope with a wait call
 * first -- a scope destroyed without waiting abandons its retained child
 * handles. On success the children table is released and the scope is reset
 * with the DESTROYED tombstone set; the scope object itself belongs to the
 * caller and is not freed. A second destroy is CORO_OK.
 *
 * @return CORO_OK on success (including the repeat); CORO_EBUSY while
 *         children are live; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_scope_destroy(coro_rt_scope_t* s);

/**
 * coro_rt_group_wait_all -- wait for every child; same as scope_wait.
 *
 * @return As coro_rt_scope_wait.
 */
COROCC_API coro_status coro_rt_group_wait_all(coro_rt_t* rt,
                                              coro_rt_scope_t* s);

/**
 * coro_rt_group_wait_first_error -- wait for the first child error.
 *
 * Returns as soon as one child reports a non-OK result: the remaining
 * children are cancelled (the scope's cancel), the wait then continues
 * until every child has finished, and the first error is returned. With no
 * error at all the call behaves like wait_all and reports CORO_OK. Both
 * entries (coroutine and plain thread) are supported, as for scope_wait.
 *
 * @return The first non-OK child status, or CORO_OK; CORO_EINVAL for NULL;
 *         CORO_ESTALE when destroyed.
 */
COROCC_API coro_status coro_rt_group_wait_first_error(coro_rt_t* rt,
                                                      coro_rt_scope_t* s);

/**
 * coro_rt_group_wait_first_done -- wait for the first child to finish.
 *
 * Returns that child's result through *first. The scope stays alive and
 * usable: the caller MUST follow up with coro_rt_scope_wait or
 * coro_rt_scope_cancel, otherwise coro_rt_scope_destroy reports CORO_EBUSY
 * -- a detectable leak, not a silent hang. Both entries (coroutine and
 * plain thread) are supported.
 *
 * @param first Receives the first completed child's result. Must not be
 *              NULL.
 * @return CORO_OK with *first set; CORO_EINVAL for NULL; CORO_ESTALE when
 *         destroyed.
 */
COROCC_API coro_status coro_rt_group_wait_first_done(coro_rt_t* rt,
                                                     coro_rt_scope_t* s,
                                                     coro_status* first);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COROCC_CORO_SCOPE_H */
