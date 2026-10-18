# coro_sched — the single-thread scheduler

> [中文](coro_sched.zh-CN.md) | English

This document is the API reference for `coro_sched.h`, the optional
single-thread scheduler. A coroutine created with `coro_create` can be
driven by hand with `coro_start`/`coro_resume`/`coro_yield` and needs no
scheduler; what this layer adds is a run loop that drives coroutines until
they park, a queue so a coroutine can be handed to the scheduler instead of
to a caller, blocking joins, and timers. The M:N runtime
([coro_rt.md](coro_rt.md)) is the multi-threaded alternative with the same
shape of entry points.

## Model

- **One thread owns the loop.** A scheduler is created on and bound to the
  calling thread; all drives and timer expiries happen on that thread.
  Cross-thread wakeups (a `coro_cancel`, a timer set elsewhere) are
  delivered to the loop's thread, never executed by the requester.
- **The loop never spins.** When no coroutine is runnable it sleeps until
  either a wakeup is delivered to its thread or the nearest timer expires.
  `coro_sched_run` is therefore cheap to call and safe to call repeatedly.
- **Spawn is deferred.** `coro_sched_spawn` only queues the coroutine; no
  user code runs inside the spawn call, so bulk spawning is cheap.
- **Timers are absolute.** Deadlines are monotone nanosecond timestamps from
  `coro_monotonic_ns`; pass `now + delta`.

## Entry points

| Entry point | Contract |
| --- | --- |
| `coro_sched_t* coro_sched_create(void)` | Create a run loop bound to the calling thread. NULL when allocation fails. |
| `coro_status coro_sched_destroy(s)` | Release the scheduler. Refuses with `CORO_EBUSY` while coroutines it owns are still live — drive them to completion first (`coro_sched_join` / `coro_sched_wait_all`). NULL is accepted and ignored. |
| `coro_status coro_sched_spawn(s, attr, fn, arg)` | Create a coroutine and queue it; it starts on the next run-loop turn. `CORO_OK`; `CORO_EINVAL` for a NULL scheduler or entry; `CORO_ENOMEM` when the coroutine, its stack or the queue node cannot be allocated. |
| `coro_status coro_sched_spawn_h(s, attr, fn, arg, &out)` | Same, plus the handle. **The handle belongs to the scheduler**: it must not be destroyed by hand (the scheduler frees it when the coroutine finishes), and a scheduler-owned coroutine must never be driven by hand — the loop destroys it if its next drive loses the claim. |
| `size_t coro_sched_pending(s)` | Live coroutine count (runnable + sleeping + joined); 0 means the scheduler is idle and can be destroyed. |
| `coro_status coro_sched_run(s)` | Drive runnables until none are left and no timer is pending. A parked coroutine resumed by a future wakeup is the scheduler's business again, not a reason to stay inside the call. |
| `coro_status coro_sched_join(s, c)` | Park the calling **scheduler coroutine** on the target's wait chain and return when it ends (the main-thread close-out is `coro_sched_wait_all` or `coro_sched_run` instead). Several waiters on one target are all released, each resumed exactly once. `CORO_OK` when dead; `CORO_EINVAL` for NULL, a foreign coroutine, or waiting on the caller's own handle; `CORO_ESTALE` when called outside a coroutine of this scheduler; `CORO_EDESTROYED` when the target already finished before the call. |
| `coro_status coro_sched_wait_all(s)` | Run the loop until every coroutine it owns is dead (the convenience form of joining everything). |
| `coro_status coro_sched_cancel(s, c)` | Request cancellation; observed at the next safe point. A sleeping coroutine has its deadline removed and is requeued so the safe point happens now. `CORO_EINVAL` for NULL or a foreign coroutine. The handle must stay valid for the whole call: a scheduler-owned coroutine is released the instant it finishes (its bookkeeping node and the coroutine object live and die together), so if it may finish concurrently, the caller must synchronize — join it first, or guarantee under its own lock that it is not released concurrently. Otherwise the call is undefined behavior and may touch freed scheduler state; this is the same lifetime requirement `coro_sched_spawn_h` states. |
| `coro_status coro_sched_sleep(s, deadline)` | Park the running coroutine until the absolute monotone `deadline`. `CORO_OK` when the deadline passed; `CORO_ECANCELED` when cut short; `CORO_ENOMEM` when the deadline could not be armed (the coroutine keeps running, nothing was parked); `CORO_EINVAL` for NULL; `CORO_ESTALE` outside any coroutine. |
| `uint64_t coro_monotonic_ns(void)` | The monotone clock the scheduler and its timers use. Only differences are meaningful; it never decreases within a process. |

## Example

```c
#include <corocc/coro_sched.h>
#include <stdio.h>

static void worker(void* arg) {
    (void)arg;
    /* ... runs on the scheduler's thread ... */
}

int main(void) {
    coro_sched_t* s = coro_sched_create();
    if (!s) return 1;

    coro_sched_spawn(s, NULL, worker, NULL);
    coro_sched_wait_all(s);            /* drive it to completion */
    coro_sched_destroy(s);
    return 0;
}
```

A coroutine that needs to wait for a specific handle joins it from inside
the loop:

```c
static void waiter(void* arg) {
    coro_t* c = (coro_t*)arg;
    if (coro_sched_join(g_sched, c) == CORO_OK) {
        /* c has finished */
    }
}
```

The typed C++ wrapper (`corocc::scheduler` + `corocc::sched_task<R>`, see
[coro_cpp.md](coro_cpp.md)) packages the same flow with results and
exceptions.

## Recycled contexts

The scheduler drives coroutines with the same credential protocol as
`coro_start`/`coro_resume`: one claim per drive, stale claims rejected, and
a coroutine resumed by a timer is requeued — never switched to directly —
so the run loop stays the only thread that drives it.

## Thread safety

| Entry point | Thread safety |
| --- | --- |
| `coro_sched_create`, `coro_sched_destroy`, `coro_sched_run`, `coro_sched_wait_all` | The owning thread only. |
| `coro_sched_spawn`, `coro_sched_spawn_h` | The owning thread (the coroutine is queued on that loop). |
| `coro_sched_pending` | Any thread; advisory read. |
| `coro_sched_join` | Inside a coroutine scheduled by this scheduler (parks that coroutine inside the loop). |
| `coro_sched_cancel` | Any thread; the request is delivered to the loop's thread and the coroutine observes it at its next safe point. |
| `coro_sched_sleep` | Inside a coroutine on this scheduler. |
| `coro_monotonic_ns` | Pure; any thread. |
