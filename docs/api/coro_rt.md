# coro_rt — the M:N runtime and its synchronization primitives (L2)

> English | [中文](coro_rt.zh-CN.md)

This document is the API reference for `coro_rt.h`, the M:N work-stealing
runtime and the synchronization primitives that live beside it: a coroutine
mutex, a condition variable, a counting semaphore, a writer-preferring
reader/writer lock and a FIFO channel. All primitives are complete,
embeddable by value and zero-initialized; the runtime itself is an opaque
handle.

The coroutine object (L1) is one thing and the runtime is another: a
coroutine created with `coro_create` can be driven by hand, from any thread,
with no runtime present ([coro.md](coro.md)). A runtime turns a set of
coroutines into a concurrent workload: a pool of worker threads shares a
collection of runnable coroutines, stealing work when a worker runs dry.

## The runtime

```c
coro_rt_t* rt = coro_rt_create(2);       /* 2 workers; 0 = the default */
...
coro_rt_wait_all(rt);                    /* drain */
coro_rt_destroy(rt);
```

| Entry point | Contract |
| --- | --- |
| `coro_rt_create(nthreads)` | Create a runtime with `nthreads` OS worker threads (0 selects the hardware thread count, capped; NULL on failure). |
| `coro_rt_spawn(rt, attr, fn, arg)` | Create a coroutine and queue it; it starts on some worker later, not inside this call. On completion the runtime reclaims every resource (handle, stack, bookkeeping), so callers need no cleanup. `CORO_OK` / `CORO_EINVAL` / `CORO_ENOMEM`. Submissions to one runtime are not internally serialized: serialize them externally when several threads submit, and note that a stack reused from the runtime's pool is not zeroed ([threading.md](../threading.md)). |
| `coro_rt_spawn_h(rt, attr, fn, arg, &out)` | Same, plus a handle to manage the coroutine by hand (query, cancel, join). Release it with `coro_rt_collect` (or `coro_destroy`) once it has finished. |
| `coro_rt_collect(rt, c)` | Reclaim a handled spawn: releases the handle, stack and bookkeeping. Exactly once per `coro_rt_spawn_h`, like `coro_destroy` for a manual coroutine. `CORO_EBUSY` while the coroutine is live **or finished but not yet retired** — retry (yield first when inside a coroutine; never spin). May run after `coro_rt_destroy` (that path touches no runtime). A default-allocator stack is released (unmapped) here, like `coro_destroy`; only a non-retained retire parks a stack in the runtime's reuse pool (custom-allocator handles release through their allocator). |
| `coro_rt_wait_all(rt)` | The runtime's own drain: workers keep scheduling until nothing is runnable, no timer is pending and no coroutine is mid-flight. A coroutine parked on a wakeup that never comes keeps the runtime alive, as with a blocking join. |
| `coro_rt_join(rt, c)` | Park the **calling runtime coroutine** until the target ends; several waiters on one target are all released, each resumed exactly once. `CORO_EAGAIN` from a plain thread or when joining the caller's own handle; `CORO_EDESTROYED` when the target already finished (whether or not its handle was collected); `CORO_ECANCELED` when the wait was cancelled and the target had not finished. |
| `coro_rt_pending(rt)` | Live coroutine count (runnable + sleeping + blocked, not yet finished). Zero means the runtime is idle and safe to destroy. |
| `coro_rt_sleep(rt, deadline)` | Park the running coroutine until the absolute monotone `deadline` (pass `now + delta`). `CORO_ECANCELED` when a cancellation wakes it early; `CORO_ENOMEM` when the deadline could not be armed (the coroutine keeps running, nothing was parked); `CORO_ESTALE` outside a coroutine. |
| `coro_rt_yield(void)` | Voluntarily give up the CPU: the coroutine goes to the back of the runnable set and is scheduled again later. Cooperative round-robin. `CORO_ESTALE` outside a coroutine. |
| `coro_rt_cancel(rt, c)` | Ask a coroutine to stop at its next safe point; a sleeping coroutine has its deadline removed and is requeued so the safe point comes now. The request never forces a switch. Safe from any thread. |
| `coro_rt_destroy(rt)` | Stop every worker and release the runtime. `CORO_EBUSY` while coroutines submitted to it are still live; run `coro_rt_wait_all` first. A coroutine kept alive only by an uncollected `coro_rt_spawn_h` handle does not block destruction. After a successful destroy the runtime handle is invalid. A destroy is exclusive: concurrent `coro_rt_spawn`/`coro_rt_spawn_h` or `coro_rt_wait_all` calls are a caller violation, and the destroy releases every stack the reuse pool still holds. |

Identity is preserved across scheduling: a coroutine spawned on a runtime can
still be queried, cancelled or joined through its `coro_t` handle, the same
credential rules apply (at most one execution flow is ever inside it), and a
stale resume is refused, not replayed.

## The wait-chain protocol

All five synchronization primitives park inside the runtime rather than
blocking a worker: a waiting coroutine is appended to a FIFO chain under the
runtime's activity lock and yields, and the releaser hands the resource over
and requeues it. Because the release path mutates state under that same lock
and wakes the waiter only after releasing it, no wake can be lost and no
resource can be double-handed. An uncontended acquire costs a single
compare-and-swap (or a single lock window); there is no poll and no spin in
either path.

The runtime's timer heap carries one short internal spin (`sleepers_lock`): a
sleeping node re-driven by a peer withdraws its deadline from the heap that
actually holds it, and that cross-worker removal is serialized against the
owner's expiry scan. It is a leaf lock, held only for a bounded heap operation
and never on the coroutine control path, so it does not weaken the zero-spin
property asserted by `COROCC_PROBE_NOSPIN` (which counts compare-and-swaps on
the coroutine/control path).

Blocking entries are coroutine-only (`CORO_ESTALE` from a plain thread); the
`try` entries never park and are callable from any context. A parked
coroutine yields its worker, so a single-worker runtime cannot deadlock on
these primitives.

## coro_rt_mutex

```c
struct coro_rt_mutex {
    uint64_t locked; /* 0 free, 1 held (atomic) */
    void*    waitq;  /* FIFO head of parked coroutines */
};
typedef struct coro_rt_mutex coro_rt_mutex_t;
```

| Entry point | Contract |
| --- | --- |
| `coro_rt_mutex_init(m)` | Optional: a zero-initialized mutex is already valid; call only to re-initialize a used object. |
| `coro_rt_mutex_lock(rt, m)` | Acquire, parking inside the runtime if held. `CORO_OK`; `CORO_ESTALE` outside a runtime coroutine; `CORO_EINVAL` for NULL. |
| `coro_rt_mutex_trylock(rt, m)` | One bare compare-and-swap; never parks, never reports `CORO_ESTALE`. `CORO_EBUSY` when held. |
| `coro_rt_mutex_unlock(rt, m)` | Release; hands the lock to the longest-waiting waiter, if any, and requeues it. `CORO_ESTATE` when the caller holds no lock; `CORO_ESTALE` outside a runtime coroutine. |

## coro_rt_cond

```c
struct coro_rt_cond {
    void* waitq; /* FIFO of parked coroutines */
};
typedef struct coro_rt_cond coro_rt_cond_t;
```

| Entry point | Contract |
| --- | --- |
| `coro_rt_cond_init(c)` | Optional, as for the mutex. |
| `coro_rt_cond_wait(rt, c, m)` | Atomically release `m`, park on `c`, and re-acquire `m` before returning (a notified waiter re-acquires through the same handoff, so a single-worker runtime cannot deadlock). Waiters are resumed in FIFO order; spurious wakeups are not produced, but callers should still re-check their predicate. `CORO_ESTALE` outside a runtime coroutine. |
| `coro_rt_cond_notify(rt, c)` | Wake the longest-waiting coroutine, if any. Safe from any thread. |
| `coro_rt_cond_broadcast(rt, c)` | Wake every waiting coroutine. Safe from any thread. |

## coro_rt_sem

```c
struct coro_rt_sem {
    uint64_t count; /* token count (atomic) */
    void*    waitq; /* FIFO of parked coroutines */
};
typedef struct coro_rt_sem coro_rt_sem_t;
```

| Entry point | Contract |
| --- | --- |
| `coro_rt_sem_init(s, count)` | Prepare a semaphore with a starting token count (`>= 0`). A zero-initialized object is count 0. |
| `coro_rt_sem_wait(rt, s)` | Consume one token, or park until a post. `CORO_OK`; `CORO_ESTALE` outside a runtime coroutine. |
| `coro_rt_sem_trywait(rt, s)` | Consume a token without ever blocking. `CORO_EBUSY` when the count is 0. |
| `coro_rt_sem_post(rt, s)` | Release a token and wake the longest-waiting waiter, if any. Safe from any thread (the one-shot wakeup channel used by the Boost integration). |
| `coro_rt_sem_count(s)` | Current token count (advisory). |

## coro_rt_rwlock

```c
struct coro_rt_rwlock {
    uint64_t state;    /* bit0 = writer holds, bit1 = writer waiting, bits[2..63] >> 2 = readers */
    void*    rd_waitq; /* FIFO of parked readers */
    void*    wr_waitq; /* FIFO of parked writers */
};
typedef struct coro_rt_rwlock coro_rt_rwlock_t;
```

The whole lock state lives in one atomic word. A reader acquire is one
compare-and-swap of `state -> state + 4` taken only while neither writer bit
is set; a writer acquire is one compare-and-swap into `state -> RW_W` from the
free or writer-waiting state, which clears the waiting bit in the same step so a
reader can never observe a moment in which the writer bit is set but the
waiting bit was not yet cleared. `RW_WW` is set exactly while a writer holds the
lock or a writer is queued, so a reader that arrives while a writer waits is
blocked by that bit.

A zero-initialized lock is a valid, free lock: `coro_rt_rwlock_init` exists only
to re-initialize a used object.

| Entry point | Contract |
| --- | --- |
| `coro_rt_rwlock_init(l)` | Optional: reset to the free state. |
| `coro_rt_rwlock_rdlock(rt, l)` | Take a read hold, parking while a writer holds or waits. |
| `coro_rt_rwlock_tryrdlock(rt, l)` | Take a read hold without ever blocking. |
| `coro_rt_rwlock_rdunlock(rt, l)` | Release a read hold; the last reader out hands it to the longest-waiting writer. |
| `coro_rt_rwlock_wrlock(rt, l)` | Take the write hold, parking otherwise. |
| `coro_rt_rwlock_trywrlock(rt, l)` | Take the write hold without ever blocking. |
| `coro_rt_rwlock_wrunlock(rt, l)` | Release the write hold; hands it to a waiting writer, else releases every waiting reader. |

Return codes: `CORO_OK`, `CORO_EINVAL` for NULL, `CORO_ESTALE` when a blocking
entry point is called outside a runtime coroutine, `CORO_EBUSY` from a try path,
`CORO_ESTATE` for an unlock with no matching hold, `CORO_ESTATE` when the reader
count would overflow, `CORO_ECANCELED` when a wait is cut short. A cancelled
waiter retries its own operation before abandoning it, so a grant handed to it
is never lost.

## coro_rt_channel

```c
struct coro_rt_channel {
    void**   slots;  /* ring of queued items, lazily allocated */
    size_t   cap;    /* allocated ring capacity */
    size_t   len;    /* queued item count */
    size_t   head;   /* ring head index */
    size_t   limit;  /* configured maximum, 0 = unbounded */
    uint64_t closed; /* close flag */
    void*    sendq;  /* FIFO of parked senders */
    void*    recvq;  /* FIFO of parked receivers */
};
typedef struct coro_rt_channel coro_rt_channel_t;
```

A channel carries opaque `void*` values: the caller owns the lifetime of
whatever they point at. The ring is pre-allocated for a bounded channel and
grown on demand for an unbounded one. Every mutation happens under the
runtime's activity lock, and the handshake is one-to-one: each successful send
wakes at most one parked receiver, each successful receive wakes at most one
parked sender, so a queued value and a parked waiter can never both wait for
each other.

A zero-initialized channel is an empty unbounded channel. A bounded channel's
minimum capacity is one; capacity 0 means **unbounded**, not a rendezvous
channel.

| Entry point | Contract |
| --- | --- |
| `coro_rt_channel_init(ch, cap)` | Prepare the channel; `cap == 0` means unbounded. |
| `coro_rt_channel_destroy(ch)` | Free the ring; refused while items or waiters remain (`CORO_EBUSY`). Idempotent. |
| `coro_rt_channel_send(rt, ch, v)` | Enqueue, parking while a bounded channel is full. |
| `coro_rt_channel_trysend(rt, ch, v)` | Enqueue without ever blocking (`CORO_EBUSY` when full). |
| `coro_rt_channel_recv(rt, ch, out)` | Dequeue, parking while the channel is empty. |
| `coro_rt_channel_tryrecv(rt, ch, out)` | Dequeue without ever blocking (`CORO_EBUSY` when empty). |
| `coro_rt_channel_close(rt, ch)` | Close and release every waiter; buffered items stay receivable. Safe from any thread; idempotent. |
| `coro_rt_channel_size(ch)` | Current number of queued items (advisory). |
| `coro_rt_channel_closed(ch)` | Whether the channel has been closed. |

Return codes: `CORO_OK`, `CORO_EINVAL` for NULL, `CORO_ESTALE` when a blocking
entry point is called outside a runtime coroutine, `CORO_EBUSY` for a full
`trysend` or an empty `tryrecv`, `CORO_ECLOSED` after a close (a `recv` drains
what remains first), `CORO_ENOMEM` when an unbounded channel cannot grow,
`CORO_ECANCELED` when a wait is cut short.

## Destruction

A mutex, a condition, a semaphore, a lock or a channel may be destroyed only
when nothing holds it and nothing waits on it. Drain the runtime first
(`coro_rt_wait_all`); a destroy on a drained object is a no-op that leaves the
object zero-initialized.

## Thread safety summary

Thread safety is scoped to one runtime: the primitives' state is mutated only
by coroutines that the runtime schedules, under the runtime's activity lock,
so they are safe to use from any worker of their runtime. Their fields are
private and must not be touched directly. The runtime's own entry-point
threading table is in [threading.md](../threading.md).

## Guarantees

- Exclusion: a reader never observes a concurrent writer, and two writers never
  run together; the state word is the single witness of both directions.
- Writer preference: a queued writer blocks readers that arrived after it, so a
  reader storm cannot starve a writer.
- No lost handoff: a grant handed to a waiter that is cancelled is retried by
  that waiter before it abstains, so no waiter is stranded and no slot is lost.
- No lost wake: the check-and-park sequence and the release-requeue sequence
  run under the same lock, and every wake happens outside it.
- No deadlock: a parked coroutine yields its worker, so a single-worker runtime
  makes progress.

## Verification

| Suite | What it proves | Runs under |
| --- | --- | --- |
| `test_rt_mn` | The M:N scheduling itself: spawn, work stealing, `wait_all`, pending counts, sleep deadlines and the single-worker no-deadlock contract. | native, ASan, aarch64/QEMU |
| `test_rt_cancel_park` | Cancellation consumed at a park's safe point: every park site retires its wait state, so the coroutine stays usable and is never silently lost (timers, mutex/cond/sem, channel, join). | native, ASan, aarch64/QEMU |
| `test_rt_sync` | Mutex, condition and semaphore exclusion, FIFO wake order, handoff and cancellation. | native, ASan, aarch64/QEMU |
| `test_rt_rwlock` | Exclusion in both directions, exact round counts, writer preference, the try and unlock codes, cancellation and the two-writer cancellation case. | native, ASan, aarch64/QEMU |
| `test_rt_channel` | No lost, duplicated or reordered item over MxN producers and consumers, bounded and unbounded, the try codes, close and drain, cancellation, destroy and its idempotence. | native, ASan, aarch64/QEMU |
