# coro — Coroutine objects (L1)

> [中文](coro.zh-CN.md) | English

This document is the API and ABI reference for `coro.h`, the coroutine object
layer of corocc. It covers every entry point's contract, the layout the
library relies on, and the guarantees the credential protocol provides.

The coroutine object works without a runtime layer; the M:N scheduler, the
blocking primitives and the C++ wrapper build on exactly these entry points.

## Concepts

| Term | Meaning |
| --- | --- |
| `coro_t` | Opaque handle to a scheduling entity: one state machine plus one execution context (stack + saved frame). |
| Credential / token | A `coro_token` value published by every suspension point. Only its holder may resume the coroutine. |
| Claim | The single compare-and-swap that grants permission to drive a coroutine. Losing it is reported, never retried. |
| Driver | The thread that drives a coroutine. Any thread holding the live credential may drive; the claim is a single compare-and-swap (see Driving from several threads). |
| Resumer | The context the coroutine returns to when it yields or its entry returns. |

## The state word

All state lives in one atomic 64-bit word, `W`, so every transition is a single
compare-and-swap:

| Value | Meaning |
| --- | --- |
| `0` | `CORO_W_NEW`: created, never started. |
| `1` | `CORO_W_RUNNING`: executing; not resumable. |
| `2` | `CORO_W_SWITCHING`: the frame is being saved; not resumable. |
| `UINT64_MAX` | `CORO_W_DEAD`: finished; not resumable. Also the `CORO_TOKEN_DEAD` sentinel. |
| `3 … UINT64_MAX-1` | Parked at a suspension point; the value is the only token that may resume it. |

Tokens come from a per-coroutine monotone sequence starting at 3, so a token
is never reused: a stale token can never be mistaken for the live one (no ABA).

`coro_token` is `uint64_t`. Two values are reserved and never issued as
credentials: `CORO_TOKEN_NONE` (0) and `CORO_TOKEN_DEAD` (`UINT64_MAX`). The
DEAD sentinel aliases the dead state word, so `coro_resume` rejects it before
its claim rather than claiming a finished coroutine.

Because `CORO_TOKEN_DEAD` is returned whenever a coroutine finishes, drive
loops look like:

```c
coro_token tok = CORO_TOKEN_NONE, next = CORO_TOKEN_NONE;
if (coro_start(c, &tok) != CORO_OK) { /* handle */ }
while (next != CORO_TOKEN_DEAD) {
    if (coro_resume(c, tok, &next) != CORO_OK) { /* handle */ }
    tok = next;
}
```

## Constants

| Name | Value | Meaning |
| --- | --- | --- |
| `CORO_DEFAULT_STACK_SIZE` | 256 KiB (`262144`) | Usable stack size selected when `attr->stack_size == 0`; one guard page is mapped below it. |
| `CORO_LOCAL_SLOTS` | `32` | Number of coroutine-local slots (`coro_local_get/set` valid keys are `< CORO_LOCAL_SLOTS`). |
| `CORO_F_NONE` | `0` | The only value accepted in `coro_attr_t.flags` today; any other bit is rejected with `CORO_EINVAL`. |
| `CORO_TOKEN_NONE` | `0` | Reserved credential value: "no suspension to report" (also the initial value of an out-parameter before a successful call). |
| `CORO_TOKEN_DEAD` | `UINT64_MAX` | Sentinel credential: the entry returned and the coroutine is dead. |

## Entry points

### `coro_attr_t coro_attr_default(void)`

Returns `{ stack_size = 0, name = NULL, flags = CORO_F_NONE }`. The structure
may be modified field by field afterwards and discarded after `coro_create`.

### `coro_status coro_create(coro_t** out, const coro_attr_t* attr, coro_fn fn, void* arg)`

Allocates the handle, maps a guard-page-protected stack (`CORO_DEFAULT_STACK_SIZE`
usable bytes plus one guard page when `stack_size` is 0) and builds the entry
frame. Nothing runs until the first drive.

* Preconditions: `out` and `fn` are not NULL; `attr->flags` is `CORO_F_NONE`;
  `attr->stack_size` is 0 or large enough to be served with a guard page.
* Postconditions on success: `*out` receives the handle in state `CORO_NEW`.
* Failure: `CORO_EINVAL`, `CORO_ENOMEM`; `*out` is untouched and no resource
  is retained.
* Threading: safe to call concurrently for distinct handles.
* Exceptions/ABI: `fn` runs on the coroutine's stack; returning from it ends the
  coroutine and returns control to the last resumer — never a process exit.

### `coro_status coro_destroy(coro_t* c)`

Marks the coroutine dead, releases its stack and frees the handle. Must be
called exactly once per `coro_create`, like `free()` on a pointer.

* Preconditions: the coroutine is not running — including not being destroyed
  by itself, which would unmap the stack it runs on.
* Failure: `CORO_EBUSY` (running, mid-switch, or the state changed under the
  caller); `CORO_EINVAL` for NULL. Nothing is released on failure.
* Threading: callable from any thread that can see the handle, provided the
  coroutine is not running; no particular thread is required.

### `coro_state coro_get_state(const coro_t* c)`

Reports `CORO_NEW`, `CORO_READY`, `CORO_RUNNING`, `CORO_SUSPENDED` or
`CORO_DEAD`. NULL returns `CORO_NEW` rather than crashing.

### `coro_status coro_start(coro_t* c, coro_token* next)`

Claims the coroutine from `CORO_W_NEW`, switches to it, and returns once it
parks or finishes.

* On success `*next` receives the credential of the suspension point, or
  `CORO_TOKEN_DEAD` when the entry returned. Written only on success.
* Failure: `CORO_EINVAL` (NULL arguments); `CORO_EBUSY` (running or
  mid-switch); `CORO_ESTALE` (already started and parked: resume it with the
  token you were given instead); `CORO_EDESTROYED` (dead).

### `coro_status coro_resume(coro_t* c, coro_token tok, coro_token* next)`

The only way to wake a coroutine: present the token its last suspension point
published.

* The claim is a single `CAS(tok -> RUNNING)`. On success control transfers; on
  failure the coroutine is untouched and the caller sees why:
  `CORO_EINVAL` (NULL, or `tok` is not a credential), `CORO_EDESTROYED`
  (`CORO_TOKEN_DEAD`, or the state word says dead), `CORO_ESTALE` (a stale or
  never-issued token), `CORO_EBUSY` (running or mid-switch).
* Exactly one holder of a token can claim it: after a successful resume the
  token is consumed and presenting it again fails with `CORO_ESTALE`.
* Settle-after-return: the next credential is published only after the switch
  has fully saved the coroutine's frame, so no observer can see a half-stopped
  coroutine as parked.
* Threading/exception: no lock is held across the switch; the caller must not
  propagate C++ exceptions across it.

### `coro_status coro_yield(void)`

Parks the running coroutine and returns control to its resumer.

* Preconditions: called from inside a coroutine that holds the permission token.
* Returns `CORO_OK` when the coroutine is resumed, `CORO_ECANCELED` when a
  cancellation had been requested (at either safe point: before parking or when
  woken), `CORO_EBUSY` when the permission token was lost to another actor (no
  switch happens), `CORO_ESTALE` when called outside any coroutine.
* A yield that observes a cancellation does not park: the entry function is
  expected to unwind, which completes the coroutine.

### `coro_status coro_cancel(coro_t* c)`

Requests that the coroutine stops at its next safe point. Cooperative: nothing
is preempted, no state transition is forced, and the request is harmless on a
new, parked or dead coroutine. A coroutine that never yields never observes it.
Returns `CORO_OK`, or `CORO_EINVAL` for NULL.

### `coro_status coro_join(coro_t* c)`

Reports whether the coroutine is dead. The manual layer has nothing to park
the caller in and this contract never spins waiting, so a live coroutine
yields `CORO_EAGAIN`; drive it to completion and join afterwards. The blocking
form is provided by the runtime layer (`coro_rt_join`). `CORO_EINVAL` for NULL.

### `void* coro_local_get(const coro_t* c, size_t key)` / `coro_status coro_local_set(coro_t* c, size_t key, void* val)`

Read or write one of `CORO_LOCAL_SLOTS` (32) fixed slots that live on the
coroutine, not on the thread, so they survive suspension and thread migration
and never touch `thread_local`. `key >= CORO_LOCAL_SLOTS` is rejected with
`CORO_EINVAL`; `NULL` handles are rejected the same way, and `coro_local_get`
returns NULL for them. Unset slots read NULL. No state word is involved, so
these calls never block or fail on a live coroutine.

### `size_t coro_pump(void)`

Run every wakeup request waiting in the calling thread's queue and return how
many were served (0 when the queue was empty). The queue is the per-thread
alternative to driving a coroutine inline: requests land there when a wakeup
is addressed to the thread that owns a coroutine rather than sent to whoever
holds the credential. Each queued request is claimed once with the same
compare-and-swap the inline resume uses, so a request that loses the claim is
dropped as a rejected credential instead of driving the coroutine twice. A
coroutine whose entry finishes during a pumped drive publishes
`CORO_TOKEN_DEAD`. Non-blocking; a caller that must serve a known number of
requests should count sweeps, because one sweep can serve many requests.

### `size_t coro_pump_wait(void)`

The blocking form: sleep on the thread's condition variable (never poll)
until a request arrives, then drain the queue exactly like `coro_pump` and
return how many requests the drain served. Must be called from the owning
thread. Use it when the thread has nothing to do but serve coroutines; use
`coro_pump` when the thread has its own work and only wants to sweep the
queue. A request queued for a thread that has already exited fails with
`CORO_EDESTROYED` instead of being lost (the thread's exit hook fails
whatever is still queued).

## Driving from several threads

Any thread that holds a coroutine's live credential may drive it. The claim is
a single compare-and-swap on the state word, so when several threads present
the same credential at once, exactly one of them wins and enters the
coroutine; every other caller is told why (`CORO_EBUSY` for a running or
mid-switch coroutine, `CORO_ESTALE` for a stale credential) and the coroutine
is untouched.

A yield (and the entry-return sentinel) returns control to whichever thread
drove the current run: the drive path records the resumer, so handing a
coroutine to another thread is just a matter of handing over its credential.
There is no owner field and no separate migration entry point, because none is
needed; the coroutine-local slots travel with the coroutine.

Threads that want to process wakeup requests on a chosen thread can use the
per-thread wakeup queue. `coro_pump` sweeps the calling thread's queued
requests and returns how many it served; `coro_pump_wait` sleeps until a
request arrives and then sweeps. Both are safe to call at any time, and a
caller that must serve a known number of requests should count what both
return, because one sweep can serve many requests at once.

A request queued for a thread that has already left fails with
`CORO_EDESTROYED` rather than being lost: the thread's exit hook fails
whatever is still queued, so the requesting side is always released.

## Thread safety summary

| Entry point | Thread safety |
| --- | --- |
| `coro_create` | Concurrent for distinct handles. |
| `coro_destroy` | Any thread; refuses a running coroutine. |
| `coro_get_state`, `coro_join`, `coro_cancel`, `coro_local_*` | Any thread holding the handle. |
| `coro_start`, `coro_resume` | Any thread holding the credential; concurrent claims are resolved by a single compare-and-swap, so one winner drives and the others see `CORO_EBUSY`/`CORO_ESTALE`. |
| `coro_yield` | Inside the running coroutine; transfers control back to the drive path that resumed it. |
| `coro_pump`, `coro_pump_wait` | Any thread that serves a wakeup queue (typically the thread the coroutines belong to); concurrent enqueues are serialised by the queue's lock. |

## Guarantees

* **Exactly once**: for one credential exactly one compare-and-swap can win, so
  at most one execution flow enters a suspension point. Rejected claims are
  reported, never retried (no spinning anywhere in the control path).
* **Never crash**: every invalid or racy call returns a `coro_status`; failures
  leave the state word, the credential and the stack untouched, so calls can be
  retried or routed elsewhere.
* **Overflow stays fatal**: the guard page turns a stack overflow into a fault
  instead of silent corruption.
* **Entry return is safe**: returning from the entry function returns control to
  the last resumer; the process never exits.

## ABI notes

* `coro_t` is an opaque handle: the layout is internal and never exposed.
* `coro_token` is `uint64_t`; `coro_state` and `coro_status` are enumerations
  whose numeric values are stable (`CORO_OK == 0`).
* `coro_attr_t` and `coro_allocator_t` are complete embeddable types; their
  measured layouts (both pointer models) and the full ABI contract are in
  [ABI.md](../ABI.md).
* The library is C99 and C++11 compatible; `coro.h` includes no third-party
  header.

## Measured cost

Reference figures (Linux x86_64, GCC, Ninja, `tests/perf/bench_coro.c`). They
are indicative, not contractual: absolute numbers move with the machine, and
the lifecycle figure is dominated by mapping and first-touching a 256 KiB
guarded stack.

| Measurement | Value |
| --- | --- |
| resume + yield round trip (two switches, one claim, one publish) | ~56 ns |
| create + start + drive to completion + destroy (256 KiB guarded stack) | ~9.5 µs |
| context switch alone (L0, `bench_switch`) | ~11 ns |

## Concurrency verification

The concurrency suites live in `tests/concurrency/`:

| Suite | What it proves | Runs under |
| --- | --- | --- |
| `test_conc_double_resume` | At most one execution flow enters a suspension point when several threads present one credential, over a fixed-seed 200-round delivery stress. | native, ASan, aarch64/QEMU |
| `test_conc_runtime_tsan` | The delivery queue, identity registry and atomics are race-free, from several threads at once. | ThreadSanitizer (CI) |

ThreadSanitizer follows the assembly stack switch through the fiber-switch
annotations (`__tsan_create_fiber`/`__tsan_switch_to_fiber`, see
`src/arch/coro_tsan.h`), so the drive path is instrumentable too: the whole
concurrency surface -- the delivery queue, the identity registry, the atomics
and the coroutine drive path -- is covered race-free by the TSan job, alongside
the behavioural exactly-once suites that run on every platform.
