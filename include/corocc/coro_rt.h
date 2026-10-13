/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * coro_rt.h -- the M:N work-stealing runtime (L2).
 *
 * The coroutine object (L1) is one thing and the runtime is another: a
 * coroutine created with coro_create can be driven by hand, from any thread,
 * with no runtime present. A runtime is an optional layer that turns a set of
 * coroutines into a concurrent workload: a pool of worker threads shares a
 * collection of runnable coroutines, stealing work from one another when a
 * worker runs dry, so N coroutines make progress on M workers.
 *
 * Coroutines submitted to a runtime keep their identity as coroutine objects.
 * The runtime only schedules them. In particular:
 *
 *   - a coroutine can be spawned on a runtime and still be queried, cancelled
 *     or joined through its coro_t handle (coro_sched_* API family);
 *   - the same credential rules apply: at most one execution flow is ever
 *     inside a coroutine, and a stale resume is refused, not replayed;
 *   - when a spawned coroutine finishes, the runtime reclaims its memory
 *     (handle, stack and bookkeeping node) unless the caller asked to keep the
 *     handle via coro_rt_spawn_h and collects it explicitly.
 *
 * This header is conservative: it compiles as C99 and C++11 and includes no
 * third-party header.
 */
#ifndef COROCC_CORO_RT_H
#define COROCC_CORO_RT_H

#include <stddef.h>
#include <stdint.h>

#include "coro.h"

#ifdef __cplusplus
extern "C" {
#endif

/** coro_rt_t -- an M:N work-stealing runtime (opaque). */
typedef struct coro_rt coro_rt_t;

/**
 * Synchronization primitive objects are complete so that callers may embed
 * them by value (on the stack or inside a pool) with no heap allocation and
 * no init call; a zero-initialized object is valid. The fields are private:
 * treat them as opaque and drive them only through the coro_rt_* API below.
 */
struct coro_rt_mutex {
    uint64_t locked; /**< Private: 0 free, 1 held (atomic). */
    void*    waitq;  /**< Private: FIFO head of parked coroutines. */
};

struct coro_rt_cond {
    void*    waitq;  /**< Private: FIFO of parked coroutines. */
};

struct coro_rt_sem {
    uint64_t count;  /**< Private: token count (atomic). */
    void*    waitq;  /**< Private: FIFO of parked coroutines. */
};

/**
 * coro_rt_create -- create a runtime with a pool of worker threads.
 *
 * Workers are real OS threads: each runs its own run queue, steals runnable
 * coroutines from the others when its queue is empty, and sleeps without
 * polling when the runtime has nothing to do. `nthreads` workers are started;
 * a variety of worker counts exercises the work-stealing paths.
 *
 * @param nthreads  Number of worker threads. 0 selects a sensible default
 *                  (the number of hardware threads, capped).
 * @return The new runtime, or NULL when the threads could not be started or
 *         memory for the workers ran out.
 */
COROCC_API coro_rt_t* coro_rt_create(size_t nthreads);

/**
 * coro_rt_destroy -- stop every worker and release the runtime.
 *
 * Refuses while coroutines the runtime has NOT yet collected are still live
 * (returns CORO_EBUSY). Run coro_rt_wait_all first, or drive everything to
 * completion. After a successful destroy the runtime handle is invalid.
 *
 * @param rt  Runtime to destroy; NULL is accepted and ignored.
 * @return CORO_OK on success; CORO_EBUSY when coroutines are still live.
 */
COROCC_API coro_status coro_rt_destroy(coro_rt_t* rt);

/**
 * coro_rt_spawn -- create a coroutine and queue it for execution.
 *
 * The coroutine starts on some worker as soon as a worker has a free slot; it
 * does not run inside this call. When it finishes, the runtime reclaims every
 * resource (handle, stack, bookkeeping node), so callers need no cleanup.
 *
 * @param rt     Runtime to queue on. Must not be NULL.
 * @param attr   Attributes, or NULL for the defaults.
 * @param fn     Entry point, called as fn(arg). Must not be NULL.
 * @param arg    Opaque argument forwarded to fn.
 * @return CORO_OK on success; CORO_EINVAL for NULL arguments; CORO_ENOMEM when
 *         a coroutine, its stack or its bookkeeping node could not be
 *         allocated.
 */
COROCC_API coro_status coro_rt_spawn(coro_rt_t* rt, const coro_attr_t* attr,
                                     coro_fn fn, void* arg);

/**
 * coro_rt_spawn_h -- spawn and additionally report a handle to manage it.
 *
 * The handle lets the caller query, cancel, join or destroy the coroutine by
 * hand, as with any other coro_t. It stays valid until the coroutine finishes;
 * before that the runtime's scheduler never touches it outside the credential
 * protocol. When the coroutine finishes, the caller releases it with
 * coro_rt_collect (or simply coro_destroy), after which the handle is invalid.
 *
 * @param rt     Runtime to queue on. Must not be NULL.
 * @param attr   Attributes, or NULL for the defaults.
 * @param fn     Entry point, called as fn(arg). Must not be NULL.
 * @param arg    Opaque argument forwarded to fn.
 * @param out    Receives the handle. Must not be NULL.
 * @return CORO_OK on success; CORO_EINVAL for NULL arguments; CORO_ENOMEM when
 *         a coroutine, its stack or its bookkeeping node could not be
 *         allocated.
 */
COROCC_API coro_status coro_rt_spawn_h(coro_rt_t* rt, const coro_attr_t* attr,
                                       coro_fn fn, void* arg, coro_t** out);

/**
 * coro_rt_collect -- reclaim a coroutine spawned with coro_rt_spawn_h.
 *
 * Releases the handle, stack and bookkeeping of a finished coroutine. Must be
 * called exactly once per handled spawn, like coro_destroy for a manual
 * coroutine; calling it on a coroutine that is still runnable or running is
 * refused with CORO_EBUSY.
 *
 * @param rt  Runtime that owns the coroutine.
 * @param c   Handle from coro_rt_spawn_h.
 * @return CORO_OK on success; CORO_EBUSY if the coroutine is still live;
 *         CORO_EINVAL for NULL arguments.
 */
COROCC_API coro_status coro_rt_collect(coro_rt_t* rt, coro_t* c);

/**
 * coro_rt_wait_all -- run until every coroutine submitted to this runtime has
 * finished.
 *
 * The runtime's own drain: workers keep scheduling until nothing is runnable,
 * no timer is pending and no coroutine is mid-flight, then return. Coroutines
 * that still park and wait for a wakeup that never comes keep the runtime
 * alive, as with a blocking join.
 *
 * @param rt  Runtime to drain. Must not be NULL.
 * @return CORO_OK on success; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_wait_all(coro_rt_t* rt);

/**
 * coro_rt_join -- park the caller until a coroutine on the runtime finishes.
 *
 * The blocking counterpart of coro_join: it parks the calling coroutine (or,
 * outside any coroutine, blocks the calling thread) on the target's wait chain
 * and returns when the target ends, so the runtime keeps making progress while
 * the caller waits. Several waiters on one target are all released, and each
 * is resumed exactly once.
 *
 * @param rt  Runtime that owns the target.
 * @param c   Coroutine to wait for.
 * @return CORO_OK when the coroutine is dead;
 *         CORO_EINVAL for NULL arguments or a coroutine this runtime does not
 *         own;
 *         CORO_EDESTROYED when the target already finished and was collected.
 */
COROCC_API coro_status coro_rt_join(coro_rt_t* rt, coro_t* c);

/**
 * coro_rt_pending -- how many coroutines this runtime still owns.
 *
 * Counts runnable, sleeping and blocked coroutines that were submitted and not
 * yet collected. Zero means the runtime is idle and safe to destroy.
 *
 * @param rt  Runtime to inspect; NULL returns 0.
 * @return The live coroutine count.
 */
COROCC_API size_t coro_rt_pending(const coro_rt_t* rt);

/**
 * coro_rt_sleep -- park the running coroutine until a deadline passes.
 *
 * Must be called from inside a coroutine being scheduled by this runtime. The
 * deadline is an absolute monotone timestamp; pass now + delta. The coroutine
 * leaves the run queue until its deadline, and the next worker to pick it up
 * resumes it exactly once.
 *
 * Cancellation is observed as elsewhere: a cancellation requested before the
 * deadline wakes the sleep early and the call returns CORO_ECANCELED.
 *
 * @param rt          Runtime that schedules the coroutine.
 * @param deadline    Absolute monotone nanoseconds.
 * @return CORO_OK when the deadline passed; CORO_ECANCELED when cut short;
 *         CORO_ESTALE when called outside a coroutine; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_sleep(coro_rt_t* rt, uint64_t deadline);

/**
 * coro_rt_yield -- voluntarily give up the CPU and re-queue the coroutine.
 *
 * Must be called from inside a coroutine. The coroutine goes to the back of
 * the runtime's runnable set and is scheduled again when a worker reaches it,
 * so yield is cooperative round-robin, exactly like coro_yield but scoped to
 * the runtime.
 *
 * @return CORO_OK after the coroutine was scheduled again; CORO_ESTALE when
 *         called outside any coroutine.
 */
COROCC_API coro_status coro_rt_yield(void);

/**
 * coro_rt_cancel -- ask a coroutine scheduled by this runtime to stop.
 *
 * The coroutine observes the request at its next safe point. If it is
 * sleeping, its deadline is removed and it is requeued so the safe point is
 * reached now instead of at the deadline. The request itself never forces a
 * switch, exactly as coro_cancel documents.
 *
 * @param rt  Runtime that owns the coroutine.
 * @param c   Coroutine to cancel.
 * @return CORO_OK on success; CORO_EINVAL for NULL arguments or a coroutine
 *         this runtime does not own.
 */
COROCC_API coro_status coro_rt_cancel(coro_rt_t* rt, coro_t* c);

/**
 * coro_rt_mutex_t -- a fair-ish coroutine mutex scoped to a runtime.
 *
 * Mutual exclusion among coroutines scheduled by one runtime. The lock itself
 * is a word: the uncontended acquire (trylock) is a single compare-and-swap
 * with no system call, and only a coroutine that loses the race parks on a
 * wait chain inside the runtime and yields, so a worker never blocks on the
 * mutex. Unlock hands the lock to the longest-waiting waiter; a waiting
 * coroutine is resumed exactly once.
 *
 * A mutex may be destroyed only when no coroutine holds it and no coroutine
 * is waiting on it (the runtime must be drained first); it is embeddable by
 * value and zero-initialized, so it needs no coro_rt_mutex_init call.
 */
typedef struct coro_rt_mutex coro_rt_mutex_t;

/**
 * coro_rt_mutex_init -- prepare a mutex for use.
 *
 * Optional: a zero-initialized coro_rt_mutex_t is already valid. Call this
 * only to re-initialize an object after it was used.
 */
COROCC_API void coro_rt_mutex_init(coro_rt_mutex_t* m);

/**
 * coro_rt_mutex_lock -- acquire a coroutine mutex from inside a coroutine.
 *
 * If the mutex is free the lock is taken immediately (one CAS, no thread
 * blocking). Otherwise the calling coroutine parks inside the runtime until a
 * unlocker hands it the lock, and returns once it holds it. Because parking
 * happens through the scheduler, a single-core runtime cannot deadlock: a
 * blocked coroutine yields its worker to run other coroutines.
 *
 * @param rt  Runtime that schedules the caller. Must not be NULL.
 * @param m   Mutex to acquire; must not be NULL.
 * @return CORO_OK when the lock is held; CORO_ESTALE when not called from a
 *         coroutine scheduled by this runtime; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_mutex_lock(coro_rt_t* rt, coro_rt_mutex_t* m);

/**
 * coro_rt_mutex_trylock -- acquire a mutex without ever blocking.
 *
 * @return CORO_OK when the lock was acquired by this call;
 *         CORO_EBUSY when the mutex is held by another coroutine;
 *         CORO_ESTALE when called outside a runtime coroutine;
 *         CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_mutex_trylock(coro_rt_t* rt,
                                             coro_rt_mutex_t* m);

/**
 * coro_rt_mutex_unlock -- release a coroutine mutex.
 *
 * Hands the lock to the longest-waiting waiter, if any, and requeues it; the
 * caller keeps running. Must be called by the current holder.
 *
 * @param rt  Runtime that schedules the caller. Must not be NULL.
 * @param m   Mutex held by the caller; must not be NULL.
 * @return CORO_OK on success; CORO_ESTALE when called outside a runtime
 *         coroutine; CORO_ESTATE when the caller does not hold the mutex;
 *         CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_mutex_unlock(coro_rt_t* rt,
                                            coro_rt_mutex_t* m);

/**
 * coro_rt_cond_t -- a condition variable for runtime-scheduled coroutines.
 *
 * A condition waits on an associated mutex: coro_rt_cond_wait releases the
 * mutex, parks the coroutine, and re-acquires the mutex before returning.
 * Waiters on one condition are resumed in FIFO order by the notify calls;
 * the mutex is handed off so a single-core runtime cannot deadlock.
 *
 * A condition may be destroyed only when nothing blocks on it; it is
 * embeddable by value and zero-initialized.
 */
typedef struct coro_rt_cond coro_rt_cond_t;

/**
 * coro_rt_cond_init -- prepare a condition variable for use.
 *
 * Optional: a zero-initialized coro_rt_cond_t is already valid.
 */
COROCC_API void coro_rt_cond_init(coro_rt_cond_t* c);

/**
 * coro_rt_cond_wait -- atomically release @p m, park, and re-acquire @p m.
 *
 * The caller must hold @p m. The coroutine releases the mutex, parks on the
 * condition, and when notified re-acquires the mutex (parking again until the
 * handoff completes) before returning. Spurious wakeups are not produced, but
 * callers should still re-check their predicate.
 *
 * @param rt  Runtime that schedules the caller.
 * @param c   Condition to wait on.
 * @param m   Mutex the caller holds.
 * @return CORO_OK on a notify; CORO_ESTALE when called outside a runtime
 *         coroutine; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_cond_wait(coro_rt_t* rt, coro_rt_cond_t* c,
                                         coro_rt_mutex_t* m);

/**
 * coro_rt_cond_notify -- wake the longest-waiting coroutine, if any.
 *
 * The notified waiter re-acquires its mutex before continuing. Safe to call
 * from any thread.
 *
 * @return CORO_OK on success; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_cond_notify(coro_rt_t* rt, coro_rt_cond_t* c);

/**
 * coro_rt_cond_broadcast -- wake every waiting coroutine.
 *
 * @return CORO_OK on success; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_cond_broadcast(coro_rt_t* rt,
                                              coro_rt_cond_t* c);

/**
 * coro_rt_sem_t -- a counting semaphore for runtime-scheduled coroutines.
 *
 * A wait that finds no token parks inside the runtime and yields; a post
 * increments the count and wakes the longest-waiting coroutine. Embeddable by
 * value and zero-initialized (count 0).
 */
typedef struct coro_rt_sem coro_rt_sem_t;

/**
 * coro_rt_sem_init -- prepare a semaphore with a starting count.
 *
 * @param count  Initial token count (>= 0).
 */
COROCC_API void coro_rt_sem_init(coro_rt_sem_t* s, size_t count);

/**
 * coro_rt_sem_wait -- consume one token, or park until coro_rt_sem_post.
 *
 * @return CORO_OK when a token was consumed; CORO_ESTALE when called outside
 *         a runtime coroutine; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_sem_wait(coro_rt_t* rt, coro_rt_sem_t* s);

/**
 * coro_rt_sem_trywait -- consume a token without ever blocking.
 *
 * @return CORO_OK when a token was consumed; CORO_EBUSY when the count is 0;
 *         CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_sem_trywait(coro_rt_t* rt, coro_rt_sem_t* s);

/**
 * coro_rt_sem_post -- release a token and wake one waiter, if any.
 *
 * @return CORO_OK on success; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_sem_post(coro_rt_t* rt, coro_rt_sem_t* s);

/**
 * coro_rt_sem_count -- current token count (advisory).
 *
 * @return The count, or 0 for NULL.
 */
COROCC_API size_t coro_rt_sem_count(const coro_rt_sem_t* s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COROCC_CORO_RT_H */