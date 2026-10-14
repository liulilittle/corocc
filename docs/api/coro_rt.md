# coro_rt — M:N runtime synchronization primitives (L2)

> English | [中文](coro_rt.zh-CN.md)

This document is the API reference for the two L2 synchronization primitives of
the M:N work-stealing runtime: a writer-preferring reader/writer lock
(`coro_rt_rwlock`) and a FIFO channel (`coro_rt_channel`). Both live in
`coro_rt.h` together with the runtime itself.

Status: **milestone M5**. The mutex, condition variable and counting semaphore
that ship with M4 use the same wait-chain protocol; this document covers only
the two primitives added in M5.

## Concepts

| Term | Meaning |
| --- | --- |
| Hold | An interest in the lock: one writer, or any number of readers, never both. |
| Writer preference | A queued writer blocks readers that arrive after it, so a writer cannot be starved by a stream of readers. |
| Item | An opaque `void*` value traveling through a channel. |
| Unbounded | Capacity 0: the ring grows on demand and never blocks a sender. |

The two primitives park inside the runtime rather than blocking a worker: a
waiting coroutine is appended to a FIFO chain under the runtime's activity lock
and yields, and the releaser hands the resource over and requeues it. Because
the release path mutates state under that same lock and wakes the waiter only
after releasing it, no wake can be lost and no resource can be double-handed.
An uncontended acquire costs a single compare-and-swap, or a single lock
window; there is no poll and no spin in either.

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
compare-and-swap of `state -> state + 4` taken only while neither writer bit is
set; a writer acquire is one compare-and-swap into `state -> RW_W` from the
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
    size_t   head;    /* ring head index */
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

A zero-initialized channel is an empty unbounded channel.

| Entry point | Contract |
| --- | --- |
| `coro_rt_channel_init(ch, cap)` | Prepare the channel; `cap == 0` means unbounded. |
| `coro_rt_channel_destroy(ch)` | Free the ring; refused while items or waiters remain (`CORO_EBUSY`). Idempotent. |
| `coro_rt_channel_send(rt, ch, v)` | Enqueue, parking while a bounded channel is full. |
| `coro_rt_channel_trysend(rt, ch, v)` | Enqueue without ever blocking. |
| `coro_rt_channel_recv(rt, ch, out)` | Dequeue, parking while the channel is empty. |
| `coro_rt_channel_tryrecv(rt, ch, out)` | Dequeue without ever blocking. |
| `coro_rt_channel_close(rt, ch)` | Close and release every waiter; buffered items stay receivable. |
| `coro_rt_channel_size(ch)` | Current number of queued items (advisory). |
| `coro_rt_channel_closed(ch)` | Whether the channel has been closed. |

Return codes: `CORO_OK`, `CORO_EINVAL` for NULL, `CORO_ESTALE` when a blocking
entry point is called outside a runtime coroutine, `CORO_EBUSY` for a full
`trysend` or an empty `tryrecv`, `CORO_ECLOSED` after a close (a `recv` drains
what remains first), `CORO_ENOMEM` when an unbounded channel cannot grow,
`CORO_ECANCELED` when a wait is cut short.

## Destruction

A lock, a condition, a semaphore or a channel may be destroyed only when
nothing holds it and nothing waits on it. Drain the runtime first
(`coro_rt_wait_all`); a destroy on a drained object is a no-op that leaves the
object zero-initialized.

## Thread safety summary

Both primitives are scoped to one runtime: their state is mutated only by
coroutines that the runtime schedules, under the runtime's activity lock, so
they are safe to use from any worker of their runtime. Their fields are private
and must not be touched directly.

## Guarantees

- Exclusion: a reader never observes a concurrent writer, and two writers never
  run together; the state word is the single witness of both directions.
- Writer preference: a queued writer blocks readers that arrived after it, so a
  reader storm cannot starve a writer.
- No lost handoff: a grant handed to a waiter that is cancelled is retried by
  that waiter before it abstains, so no waiter is stranded and no slot is lost.
- No lost wake: the check-and-park sequence and the release-queeue sequence run
  under the same lock, and every wake happens outside it.
- No deadlock: a parked coroutine yields its worker, so a single-worker runtime
  makes progress.

## Verification

| Suite | What it proves | Runs under |
| --- | --- | --- |
| `test_rt_rwlock` | Exclusion in both directions, exact round counts, writer preference, the try and unlock codes, cancellation and the two-writer cancellation case. | native, ASan, aarch64/QEMU |
| `test_rt_channel` | No lost, duplicated or reordered item over MxN producers and consumers, bounded and unbounded, the try codes, close and drain, cancellation, destroy and its idempotence. | native, ASan, aarch64/QEMU |
