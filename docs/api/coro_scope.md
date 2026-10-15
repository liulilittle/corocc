# coro_scope -- structured concurrency for the M:N runtime (L3)

> English | [中文](coro_scope.zh-CN.md)

This document is the API reference for the L3 structured-concurrency layer:
a scope owns a set of child coroutines, and the wait family only reports done
once every child the scope ever accepted has finished and been collected.

The header is `include/corocc/coro_scope.h`; the implementation lives in
`src/scheduler/rt_scope.c`. The scope object is complete and embeddable by
value; a zero-initialized scope is the OPEN state (verdict section 3.8,
replacing the design draft's opaque typedef: zero-initialization must be a
usable state, so the caller must be able to declare the object).

## Concepts

| Term | Meaning |
| --- | --- |
| Scope | An owner of a dynamic table of children plus one aggregate result. |
| Child | A coroutine spawned through `coro_rt_scope_spawn`; every child is registered before it can run. |
| Result cell | Per-child `coro_status` storage. The installed trampoline is its ONLY writer; the scope never pre-writes a result. |
| Aggregate | The first non-OK child result in completion order, or `CORO_OK`. |
| live | The count of registered children that have not finished; every wait predicate is `live == 0`. |
| State word | `OPEN -> CANCELING -> DRAINED -> DESTROYED`, one atomic; one lock (the runtime's activity lock) guards the table, live, the aggregate and the event chain. |
| Watchdog | A retained spawn (never a child, never counted in live) that claims a `wait_for` deadline exactly once and cancels the scope. |

The wait family has two documented entries: from a coroutine scheduled by the
runtime (the waiter parks on the scope's own event chain -- one parking point
-- and is woken once per park), and from a plain thread (the call blocks the
thread on the runtime's settled condition; it never reports `CORO_EAGAIN`).
No wait uses `coro_rt_join` on its children: the child-exit event chain is
what carries the notification and the ordering the aggregation reads.

## Lifecycle

```c
coro_rt_scope_t s;              /* zero-initialized = OPEN */
memset(&s, 0, sizeof s);        /* or: coro_rt_scope_init(&s) */
```

| Transition | Trigger |
| --- | --- |
| (zero/init) -> OPEN | initialization |
| OPEN -> OPEN | `coro_rt_scope_spawn` success |
| OPEN -> CANCELING | `coro_rt_scope_cancel`, or a `wait_for` watchdog claim |
| OPEN/CANCELING -> DRAINED | the single winner of the drain (a wait whose predicate `live == 0` holds) |
| DRAINED -> DESTROYED | `coro_rt_scope_destroy` (allowed from any state once `live == 0`) |

Illegal transitions: a spawn after CANCELING/DRAINED reports `CORO_ESTATE`; any
operation but destroy after DESTROYED reports `CORO_ESTALE`; destroy with
`live > 0` reports `CORO_EBUSY` and changes nothing. A second destroy reports
`CORO_OK`. The scope object itself belongs to the caller; destroy releases the
child table and resets the scope to the zero state with the DESTROYED
tombstone set.

## Entry points

| Entry point | Contract |
| --- | --- |
| `coro_rt_scope_init(s)` | Bring a fresh scope to OPEN. `CORO_OK` / `CORO_EINVAL` (NULL). |
| `coro_rt_scope_spawn(rt, s, attr, fn, arg, cell_out)` | Register a child; `fn` returns the child's result. `cell_out` (optional) receives `&cell`, valid until destroy; `cell_out == NULL` means "no result" and the child is NOT aggregated. `CORO_OK` / `CORO_EINVAL` / `CORO_ESTATE` / `CORO_ESTALE` / `CORO_ENOMEM` (nothing registered on failure). A spawn that races a cancel still registers (and cancels) its child, so no child escapes cancellation. |
| `coro_rt_scope_cancel(rt, s)` | Ask every live child to stop. Idempotent (`CORO_OK`). Issued outside the activity lock, one `coro_rt_cancel` per live child; the scope writes NO result cell. `CORO_OK` / `CORO_EINVAL` / `CORO_ESTALE` (destroyed) / `CORO_ESTATE` (drained). |
| `coro_rt_scope_wait(rt, s)` | Wait for `live == 0`, transition to DRAINED, collect every child (bounded EBUSY retry), return the aggregate. `CORO_OK` / aggregate / `CORO_EINVAL` / `CORO_ESTALE`. |
| `coro_rt_scope_wait_for(rt, s, ms)` | As wait, with a deadline in milliseconds. Coroutine callers get the once-only watchdog (fail-fast `CORO_ENOMEM` before any child registration); thread callers wait on the deadline themselves. Return-code priority: this waiter's deadline first -> `CORO_ECANCELED`; else the first non-OK child; else `CORO_OK`. |
| `coro_rt_scope_status(s)` | The aggregate (a partial aggregate before the drain). `CORO_OK` / aggregate / `CORO_EINVAL` / `CORO_ESTALE`. |
| `coro_rt_scope_cancel_requested(s)` | `1` while canceling, else `0` (also `0` for NULL). |
| `coro_rt_scope_destroy(s)` | Release a drained scope. `CORO_OK` (idempotent) / `CORO_EBUSY` / `CORO_EINVAL`. Does NOT collect: drain with a wait first. |
| `coro_rt_group_wait_all(rt, s)` | Same as `coro_rt_scope_wait`. |
| `coro_rt_group_wait_first_error(rt, s)` | Return at the first non-OK result, cancel the remaining children, still wait out the drain, return that first error (no error -> as wait_all). |
| `coro_rt_group_wait_first_done(rt, s, first)` | Return the first completed child's result through `*first`; the scope STAYS ALIVE and the caller MUST follow up with `coro_rt_scope_wait` or `coro_rt_scope_cancel` (a destroy without that reports `CORO_EBUSY`: a detectable leak). |

## Caller responsibilities (both are contract, not advice)

1. **The wait is immune to its waiter's own cancellation.** A cancellation
   consumed at the park's safe point detaches the waiter and the wait keeps
   going until the children are drained -- an interrupted wait would leave
   children running with nobody covering them. The wait therefore does NOT
   return `CORO_ECANCELED` for the waiter's own cancellation; plan around its
   completion (or cancel the scope instead).

2. **cancel's reach is C-documented, not runtime-wide.** `coro_rt_scope_cancel`
   only guarantees to interrupt children that are running or sleeping right
   away. A child blocked inside a synchronization primitive observes the
   request only when the event it waits for happens; and if children wait on
   each other while the whole scope is cancelled, the wait may not terminate.
   The caller owns that scenario.

Further caller responsibilities, stated with the same force:

3. **`wait_for`'s drain after a timeout is unbounded by design.** The
   structured invariant is "no child outlives its scope", so a timed-out wait
   still ends on `live == 0`. A child that never observes its cancellation (see
   point 2) keeps that wait alive.

4. **Destroy does not collect.** Children are reclaimed by the wait family
   (`coro_rt_collect` with a bounded EBUSY retry: a child that finished but
   whose retirement has not run yet reports `CORO_EBUSY`). Always drain with a
   wait before destroying; destroying an undrained scope abandons its retained
   child handles, and `CORO_EBUSY` from destroy is the detector.

## Example

```c
#include <corocc/coro_scope.h>

static coro_rt_t* g_rt;

static coro_status work(void* arg) {
    unsigned id = (unsigned)(size_t)arg;
    /* ... */
    return id == 3 ? CORO_ESTALE : CORO_OK;
}

static void waiter(void* arg) {
    coro_rt_scope_t* s = (coro_rt_scope_t*)arg;
    coro_status agg = coro_rt_scope_wait(g_rt, s);
    (void)agg;   /* first non-OK child result, or CORO_OK */
}

int main(void) {
    g_rt = coro_rt_create(4);

    coro_rt_scope_t s;
    memset(&s, 0, sizeof s);
    for (size_t i = 0; i < 8; ++i)
        coro_rt_scope_spawn(g_rt, &s, NULL, work, (void*)i, NULL);

    coro_rt_spawn(g_rt, NULL, waiter, &s);   /* the waiter is a coroutine */
    coro_rt_wait_all(g_rt);

    coro_rt_scope_destroy(&s);
    coro_rt_destroy(g_rt);
    return 0;
}
```

A main-thread close-out is equally supported: call `coro_rt_scope_wait(rt, &s)`
directly from the thread (no coroutine waiter needed), and `wait_for` from the
thread uses its own deadline wait -- no watchdog coroutine is spawned there.

## Model notes

- One lock only: the scope reuses the runtime's activity lock; the state word
  is the only atomic on the scope. No new runtime internals were added for L3.
- The result cell's single writer is the trampoline the scope installs; a
  cancelled child reports its own final value (conventionally
  `CORO_ECANCELED`) and the aggregation reads exactly that value. There is no
  EBUSY sentinel: a child may legitimately return `CORO_EBUSY`.
- The watchdog is a retained spawn with an explicit 32 KiB stack (not the
  256 KiB default); it never enters the child table and never touches `live`.
- A scope is bound to one runtime at its first spawn.
