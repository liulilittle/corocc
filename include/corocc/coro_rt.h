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

struct coro_rt_rwlock {
    uint64_t state;    /**< Private: bit0 = RW_W (writer holds the lock), bit1 =
                            RW_WW (a writer is waiting), bits[2..63] right-
                            shifted by 2 = active reader count. Whole word is
                            read and written with one atomic operation. */
    void*    rd_waitq; /**< Private: FIFO of parked readers. */
    void*    wr_waitq; /**< Private: FIFO of parked writers. */
};

struct coro_rt_channel {
    void**   slots;  /**< Private: ring of queued items (lazily allocated). */
    size_t   cap;    /**< Private: allocated ring capacity. */
    size_t   len;    /**< Private: queued item count. */
    size_t   head;   /**< Private: ring head index. */
    size_t   limit;  /**< Private: configured maximum, 0 for unbounded. */
    uint64_t closed; /**< Private: close flag (atomic). */
    void*    sendq;  /**< Private: FIFO of parked senders. */
    void*    recvq;  /**< Private: FIFO of parked receivers. */
};

/**
 * coro_rt_rwlock_t -- a writer-preferring read/write lock for runtime
 * coroutines.
 *
 * Many readers may hold the lock together, or one writer, never both. The
 * lock is a single 64-bit state word: bit 0 (RW_W) marks a writer holding the
 * lock, bit 1 (RW_WW) marks a writer waiting, and bits 2..63 (shifted right by
 * two) count active readers. Acquiring and releasing are single
 * compare-and-swaps on that word, so the uncontended paths take no lock at
 * all; a coroutine that has to wait parks on one of two FIFO chains and
 * yields, so a worker is never blocked.
 *
 * The lock is writer-preferring: once a writer is queued, new readers wait
 * until it has run, which keeps writers from starving under a reader storm.
 * The flip side -- a reader could in principle starve under a continuous
 * stream of writers -- is the documented trade-off.
 *
 * A lock may be destroyed only when it is unlocked and nothing waits on it
 * (drain the runtime first). It is embeddable by value and a zero-initialized
 * object is valid, so no coro_rt_rwlock_init call is required.
 */
typedef struct coro_rt_rwlock coro_rt_rwlock_t;

/** coro_rt_rwlock_init -- reset a read/write lock to the free state. Optional. */
COROCC_API void coro_rt_rwlock_init(coro_rt_rwlock_t* l);

/**
 * coro_rt_rwlock_rdlock -- take a shared read hold.
 *
 * Succeeds immediately unless a writer holds the lock or is waiting; otherwise
 * the coroutine parks and yields until released.
 *
 * @return CORO_OK on success; CORO_ESTALE when not called from a coroutine on
 *         this runtime; CORO_ESTATE on reader-count overflow; CORO_EINVAL for
 *         NULL; CORO_ECANCELED when the wait is cancelled.
 */
COROCC_API coro_status coro_rt_rwlock_rdlock(coro_rt_t* rt,
                                             coro_rt_rwlock_t* l);

/**
 * coro_rt_rwlock_tryrdlock -- take a read hold without ever blocking.
 *
 * @return CORO_OK when acquired; CORO_EBUSY when a writer holds or waits;
 *         CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_rwlock_tryrdlock(coro_rt_t* rt,
                                                coro_rt_rwlock_t* l);
/**
 * coro_rt_rwlock_rdunlock -- release a shared read hold.
 *
 * Must be called by a current reader. If it was the last reader and a writer
 * is waiting, the longest-waiting writer is woken.
 *
 * @return CORO_OK on success; CORO_ESTATE when the caller holds no read hold;
 *         CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_rwlock_rdunlock(coro_rt_t* rt,
                                               coro_rt_rwlock_t* l);

/**
 * coro_rt_rwlock_wrlock -- take the exclusive write hold.
 *
 * Succeeds immediately when the lock is free, and is granted ahead of new
 * readers once queued. Parks and yields otherwise.
 *
 * @return CORO_OK on success; CORO_ESTALE when not called from a coroutine on
 *         this runtime; CORO_EINVAL for NULL; CORO_ECANCELED when cancelled.
 */
COROCC_API coro_status coro_rt_rwlock_wrlock(coro_rt_t* rt,
                                             coro_rt_rwlock_t* l);

/**
 * coro_rt_rwlock_trywrlock -- take the write hold without ever blocking.
 *
 * @return CORO_OK when acquired; CORO_EBUSY when the lock is held or waited
 *         on; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_rwlock_trywrlock(coro_rt_t* rt,
                                                coro_rt_rwlock_t* l);

/**
 * coro_rt_rwlock_wrunlock -- release the write hold.
 *
 * Hands the lock to the longest-waiting writer if one is queued; otherwise
 * releases every waiting reader (a wake storm bounded by the reader count).
 *
 * @return CORO_OK on success; CORO_ESTATE when the caller holds no write hold;
 *         CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_rwlock_wrunlock(coro_rt_t* rt,
                                               coro_rt_rwlock_t* l);

/**
 * coro_rt_channel_t -- a void* message channel between runtime coroutines.
 *
 * A bounded (capacity > 0) or unbounded (capacity 0) FIFO queue with a single
 * producer/consumer handshake per item: each successful send wakes one parked
 * receiver and each successful recv wakes one parked sender, so the blocked
 * side is resumed exactly once. Items are opaque void* -- the caller owns the
 * lifetime of whatever they point at.
 *
 * The storage is lazily allocated; a bounded channel allocates its ring up
 * front, an unbounded one grows on demand. A channel may be destroyed only
 * after it is closed and drained. Closing lets receivers drain whatever is
 * already queued and then reports CORO_ECLOSED; senders are refused and
 * released immediately. It is embeddable by value; a zero-initialized object
 * is an empty unbounded channel, so no coro_rt_channel_init call is required
 * for that case.
 *
 * Note: a bounded channel's minimum capacity is one; capacity 0 means
 * *unbounded*, not a rendezvous channel.
 */
typedef struct coro_rt_channel coro_rt_channel_t;

/**
 * coro_rt_channel_init -- prepare a channel.
 *
 * @param ch    Channel to initialize.
 * @param cap   Maximum queued items, or 0 for unbounded.
 * @return CORO_OK on success; CORO_ENOMEM when the initial ring cannot be
 *         allocated; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_channel_init(coro_rt_channel_t* ch, size_t cap);
/**
 * coro_rt_channel_destroy -- release a channel's storage.
 *
 * Refuses while items are still queued or coroutines are parked on it
 * (CORO_EBUSY); drain and close first. On success the channel is reset to the
 * zero-initialized state and a second destroy is a no-op.
 *
 * @return CORO_OK on success; CORO_EBUSY when not drained; CORO_EINVAL for
 *         NULL.
 */
COROCC_API coro_status coro_rt_channel_destroy(coro_rt_channel_t* ch);

/**
 * coro_rt_channel_send -- enqueue one value, parking until room is available.
 *
 * @return CORO_OK on success; CORO_ESTALE when not called from a coroutine on
 *         this runtime; CORO_ECLOSED when the channel is closed; CORO_ENOMEM
 *         when an unbounded channel cannot grow; CORO_EINVAL for NULL;
 *         CORO_ECANCELED when the wait is cancelled.
 */
COROCC_API coro_status coro_rt_channel_send(coro_rt_t* rt,
                                            coro_rt_channel_t* ch, void* v);

/**
 * coro_rt_channel_trysend -- enqueue one value without ever blocking.
 *
 * @return CORO_OK on success; CORO_EBUSY when a bounded channel is full;
 *         CORO_ECLOSED when the channel is closed; CORO_ENOMEM on allocation
 *         failure; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_channel_trysend(coro_rt_t* rt,
                                               coro_rt_channel_t* ch, void* v);

/**
 * coro_rt_channel_recv -- dequeue one value, parking until one is available.
 *
 * A closed channel first yields every queued item, then reports CORO_ECLOSED.
 *
 * @return CORO_OK with *out set; CORO_ESTALE when not called from a coroutine
 *         on this runtime; CORO_ECLOSED when the channel is closed and empty;
 *         CORO_EINVAL for NULL; CORO_ECANCELED when the wait is cancelled.
 */
COROCC_API coro_status coro_rt_channel_recv(coro_rt_t* rt,
                                            coro_rt_channel_t* ch, void** out);

/**
 * coro_rt_channel_tryrecv -- dequeue one value without ever blocking.
 *
 * @return CORO_OK with *out set; CORO_EBUSY when empty and open;
 *         CORO_ECLOSED when closed and empty; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_channel_tryrecv(coro_rt_t* rt,
                                               coro_rt_channel_t* ch,
                                               void** out);

/**
 * coro_rt_channel_close -- close the channel and release every waiter.
 *
 * Queued items remain readable; after they are drained recv reports
 * CORO_ECLOSED. Every parked sender and receiver is woken; closing is
 * idempotent. Safe to call from any thread.
 *
 * @return CORO_OK on success; CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_channel_close(coro_rt_t* rt,
                                             coro_rt_channel_t* ch);

/**
 * coro_rt_channel_size -- number of queued items (advisory).
 *
 * @return The queued count, or 0 for NULL.
 */
COROCC_API size_t coro_rt_channel_size(const coro_rt_channel_t* ch);

/**
 * coro_rt_channel_closed -- whether the channel has been closed.
 *
 * @return Non-zero when closed, 0 otherwise (also 0 for NULL).
 */
COROCC_API int coro_rt_channel_closed(const coro_rt_channel_t* ch);

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
 * Refuses while coroutines submitted to this runtime are still live (have
 * not finished), returning CORO_EBUSY. Run coro_rt_wait_all first, or drive
 * everything to completion. A coroutine kept alive only by an uncollected
 * coro_rt_spawn_h handle does not block destruction; collect it before or
 * after. After a successful destroy the runtime handle is invalid.
 *
 * A destroy must be exclusive: concurrent coro_rt_spawn/coro_rt_spawn_h or
 * coro_rt_wait_all calls during the destroy are a caller violation and must
 * not be attempted; quiesce the submitters and waiters first (see
 * docs/threading.md).
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
 * Submissions to one runtime are not internally serialized: when several
 * threads submit to the same runtime, the caller must serialize the calls
 * externally (see docs/threading.md).
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
 * refused with CORO_EBUSY. A coroutine that has finished but whose retirement
 * on the runtime has not completed yet also reports CORO_EBUSY; retry until it
 * succeeds. When retrying from inside a coroutine, do not spin: yield first
 * (for example coro_rt_yield()) and retry after it returns. Collect may run
 * after coro_rt_destroy as well (the contract the destroy note keeps); that
 * path touches no runtime, only the coroutine handle and its node.
 *
 * @param rt  Runtime that owns the coroutine.
 * @param c   Handle from coro_rt_spawn_h.
 * @return CORO_OK on success; CORO_EBUSY if the coroutine is still live or
 *         has finished but not been retired yet -- retry until it succeeds;
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
 * The blocking counterpart of coro_join: it parks the calling coroutine on
 * the target's wait chain and returns when the target ends, so the runtime
 * keeps making progress while the caller waits. Several waiters on one target
 * are all released, and each is resumed exactly once. Only a coroutine the
 * runtime is driving can park this way: a plain thread and a coroutine
 * joining its own handle are refused with CORO_EAGAIN instead of parking.
 *
 * A cancellation requested while the caller is parked is observed at its next
 * safe point, like anywhere else. If the target has finished by then, the
 * wait condition is satisfied and the call reports CORO_OK; otherwise the
 * caller is released with CORO_ECANCELED.
 *
 * CORO_EDESTROYED covers the target that had already finished before the
 * caller parked -- whether or not its handle has been collected yet.
 *
 * @param rt  Runtime that owns the target.
 * @param c   Coroutine to wait for.
 * @return CORO_OK when the coroutine is dead;
 *         CORO_EINVAL for NULL arguments or a coroutine this runtime does not
 *         own;
 *         CORO_EAGAIN when the caller cannot park: called from outside any
 *         runtime coroutine (a plain thread included), or waiting on the
 *         caller's own handle;
 *         CORO_EDESTROYED when the target already finished, whether or not
 *         its handle was collected;
 *         CORO_ECANCELED when the caller's wait was cancelled and the target
 *         had not finished.
 */
COROCC_API coro_status coro_rt_join(coro_rt_t* rt, coro_t* c);

/**
 * coro_rt_pending -- how many coroutines this runtime still owns.
 *
 * Counts runnable, sleeping and blocked coroutines that were submitted and not
 * yet finished. Zero means the runtime is idle and safe to destroy.
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
 *         CORO_ENOMEM when the deadline could not be armed (the coroutine keeps
 *         running, nothing was parked); CORO_ESTALE when called outside a
 *         coroutine; CORO_EINVAL for NULL.
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
 * The acquire is a bare compare-and-swap, so unlike coro_rt_mutex_lock it
 * never parks and never reports CORO_ESTALE: it may be called from any
 * context, and a plain thread outside the runtime can take a free lock (a
 * held lock reports CORO_EBUSY to it exactly as to a coroutine).
 *
 * @return CORO_OK when the lock was acquired by this call;
 *         CORO_EBUSY when the lock is already held;
 *         CORO_EINVAL for NULL.
 */
COROCC_API coro_status coro_rt_mutex_trylock(coro_rt_t* rt,
                                             coro_rt_mutex_t* m);

/**
 * coro_rt_mutex_unlock -- release a coroutine mutex.
 *
 * Hands the lock to the longest-waiting waiter, if any, and requeues it; the
 * caller keeps running. Must be called by the current holder. It does not park,
 * so it carries no coroutine-context requirement: a lock taken with
 * coro_rt_mutex_trylock from a plain thread (which that entry point explicitly
 * allows) can be released from that same thread.
 *
 * @param rt  Runtime that schedules the caller. Must not be NULL.
 * @param m   Mutex held by the caller; must not be NULL.
 * @return CORO_OK on success; CORO_ESTATE when the caller does not hold the
 *         mutex; CORO_EINVAL for NULL.
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
