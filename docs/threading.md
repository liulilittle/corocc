# Threading — model, migration and thread-safety

> [中文](threading.zh-CN.md) | English

This document is the concurrency reference for corocc: which thread may call
what, how a coroutine moves between threads, what the coroutine-local slots
guarantee, and the thread-safety table for every layer. The per-API
contracts remain in the layer references
([coro.md](api/coro.md), [coro_l0.md](api/coro_l0.md),
[coro_rt.md](api/coro_rt.md), [coro_sched.md](api/coro_sched.md),
[coro_scope.md](api/coro_scope.md)); this page puts the threading rules in
one place.

## The model in one paragraph

A coroutine is a scheduling entity, not a thread: it owns a stack and a
saved frame, and it runs whenever some thread **drives** it by presenting
its live credential. The claim is a single compare-and-swap on the
coroutine's state word, so at most one execution flow is ever inside a
coroutine, and a failed claim is reported (`CORO_EBUSY` for a running or
mid-switch coroutine, `CORO_ESTALE` for a stale credential) instead of
being retried. Nothing in the design binds a coroutine to a thread: there is
no owner field, because none is needed.

## Which thread may drive

Any thread that holds the coroutine's live credential may drive it:

```c
/* thread A created the coroutine; hand the token to thread B */
coro_token tok = /* from coro_start / the last coro_resume */;
coro_token next = CORO_TOKEN_NONE;
coro_status s = coro_resume(worker, tok, &next);   /* one claim, one drive */
```

- `coro_start`, `coro_resume`: any thread holding the credential. Concurrent
  claims on one credential are resolved by the compare-and-swap; exactly one
  caller enters the coroutine.
- `coro_get_state`, `coro_join`, `coro_cancel`, `coro_local_get/set`: any
  thread that can see the handle.
- `coro_destroy`: any thread; refuses (with `CORO_EBUSY`) while the
  coroutine is running or mid-switch, wherever that is.
- `coro_yield` (and the entry-return path): transfers control back to the
  **thread that drove the current run** — the drive path records the
  resumer. Handing a coroutine to another thread is therefore just a matter
  of handing over its credential.

Coroutine-local slots (`coro_local_get/set`, `CORO_LOCAL_SLOTS` slots) live
on the coroutine, not on the thread: they survive suspension and travel with
the coroutine across a handover, and they never touch `thread_local`.

## Per-thread wakeup queues

A thread that owns a set of coroutines can take wakeup requests through its
own queue instead of being driven inline:

| Entry point | Contract |
| --- | --- |
| `coro_pump()` | Sweep every request queued for the calling thread's queue; returns how many were served. Non-blocking; 0 when the queue was empty. |
| `coro_pump_wait()` | Sleep until a request arrives, then sweep. The owning thread's blocking serve loop; never polls. |

Because one sweep can serve many requests, a caller that must serve a known
number of requests should count what both calls return. A request queued for
a thread that has already left fails with `CORO_EDESTROYED` (the thread's
exit hook fails whatever is still queued) instead of being lost, so the
requesting side is always released.

## The M:N runtime

`coro_rt_t` is a pool of worker threads sharing runnable coroutines. The
thread-safety rules of its entry points:

| Entry point | Thread safety |
| --- | --- |
| `coro_rt_create` | The constructing thread. |
| `coro_rt_spawn`, `coro_rt_spawn_h` | The thread that manages the runtime; when several threads submit, serialize the submissions externally. The coroutines themselves then run on the worker pool. |
| `coro_rt_destroy`, `coro_rt_wait_all` | The managing thread (drain before destroy; `CORO_EBUSY` while coroutines are live). |
| `coro_rt_collect` | Any thread, on a finished handle — including after `coro_rt_destroy` (that path touches only the coroutine handle and its node). |
| `coro_rt_cancel` | Any thread. The request lands on the target's state word and, if the target is parked on a deadline, is handed to its worker through the wakeup path. |
| `coro_rt_join` | A coroutine scheduled by the runtime (it parks the caller); a plain thread is answered `CORO_EAGAIN`. |
| `coro_rt_pending` | Any thread; advisory read. |
| `coro_rt_sleep`, `coro_rt_yield` | Inside a runtime coroutine (`CORO_ESTALE` outside one). |

Synchronization primitives (`coro_rt_mutex`, `coro_rt_cond`, `coro_rt_sem`,
`coro_rt_rwlock`, `coro_rt_channel`) are scoped to one runtime:

- Blocking entries are coroutine-only: from a plain thread they report
  `CORO_ESTALE` rather than blocking a non-worker.
- The `try` entries never park and are callable from any context (a held
  lock reports `CORO_EBUSY` to a plain thread exactly as to a coroutine).
- `coro_rt_cond_notify`, `coro_rt_cond_broadcast`, `coro_rt_channel_close`,
  `coro_rt_mutex_trylock`, `coro_rt_sem_trywait`, `coro_rt_sem_post` are
  explicitly safe from any thread.
- A parked worker is never blocked: a coroutine that has to wait parks
  inside the runtime and yields its worker, so a single-worker runtime still
  makes progress (and cannot deadlock on these primitives).

The scheduler layer is stricter by design — it is bound to the thread that
created it; see the thread-safety table in
[coro_sched.md](api/coro_sched.md).

## thread_local and coroutines

`thread_local` storage is a property of the **thread**, not of the
coroutine:

- A coroutine that has never left its thread sees the thread's values as any
  other function would.
- After a handover to another thread, the driving thread changes, so
  `thread_local` values observed inside the coroutine change with it. Values
  that must survive suspension and migration belong in coroutine-local slots
  (`coro_local_get/set`) or in the coroutine's own stack frame.
- **s390x exception:** do not rely on `thread_local` inside coroutine bodies
  on s390x at all. gcc's s390x TLS access materializes the thread pointer
  (access registers a0/a1) into a callee-saved general register and may
  reuse it across calls within one function; a context resumed by a
  different thread would dereference the TLS slot of the thread that
  suspended it (and crash once that thread is gone). The s390x toolchain
  therefore compiles with `-fno-stack-protector` (the canary check reads the
  same cached pointer). Other ABIs are unaffected.

## Blocking calls inside coroutines

Coroutines are cooperative: a coroutine that never yields cannot be
cancelled or preempted. Two rules follow for third-party code called from a
coroutine body:

- **Do not block the worker** on OS primitives (`pthread_mutex_lock`,
  `WaitForSingleObject`, blocking I/O, …) while other coroutines are queued
  on the same runtime: a blocked worker is a stalled scheduling slot. Use
  the runtime's own primitives (`coro_rt_mutex`, `coro_rt_sem`,
  `coro_rt_channel`, `coro_rt_sleep`), which park the coroutine and yield
  the worker.
- **No lock may be held across an L0 `coro_ctx_swap`**, and no C++ exception
  may cross it ([coro_l0.md](api/coro_l0.md)). The higher layers already
  respect this; hand-written L0 code must too.

Cancellation stays cooperative in every layer: a request recorded while a
coroutine is parked on a timer is delivered by waking it early, and a
request recorded while it runs is observed at its next safe point. The
Boost.Asio integration has one documented subtlety: a cancellation observed
while waiting for an Asio completion does not cut the wait short — the
pending handler must be allowed to finish while the coroutine frame is
still alive ([boost.md](api/boost.md)).

## Reading the layer references

| Layer | Threading section |
| --- | --- |
| L0 primitives | [coro_l0.md](api/coro_l0.md) |
| L1 coroutine object | [coro.md](api/coro.md) |
| L2 runtime and primitives | [coro_rt.md](api/coro_rt.md) |
| Scheduler | [coro_sched.md](api/coro_sched.md) |
| Scopes | [coro_scope.md](api/coro_scope.md) |
| C++ wrapper | [coro_cpp.md](api/coro_cpp.md) |
