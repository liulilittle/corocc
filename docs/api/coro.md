# coro — Coroutine objects (L1)

> [中文](coro.zh-CN.md) | English

This document is the API and ABI reference for `coro.h`, the coroutine object
layer of corocc. It covers every entry point's contract, the layout the
library relies on, and the guarantees the credential protocol provides.

Status: **milestone M2**. The coroutine object works without a runtime layer;
the M:N scheduler, blocking primitives and the C++ wrapper build on exactly
these entry points.

## Concepts

| Term | Meaning |
| --- | --- |
| `coro_t` | Opaque handle to a scheduling entity: one state machine plus one execution context (stack + saved frame). |
| Credential / token | A `coro_token` value published by every suspension point. Only its holder may resume the coroutine. |
| Claim | The single compare-and-swap that grants permission to drive a coroutine. Losing it is reported, never retried. |
| Driver / owner | The thread that drives a coroutine. The thread that first drives it owns it (see Ownership). |
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
  coroutine is not running; it does not require ownership.

### `coro_state coro_get_state(const coro_t* c)`

Reports `CORO_NEW`, `CORO_READY`, `CORO_RUNNING`, `CORO_SUSPENDED` or
`CORO_DEAD`. NULL returns `CORO_NEW` rather than crashing.

### `coro_status coro_start(coro_t* c, coro_token* next)`

Claims the coroutine from `CORO_W_NEW`, switches to it, and returns once it
parks or finishes.

* On success `*next` receives the credential of the suspension point, or
  `CORO_TOKEN_DEAD` when the entry returned. Written only on success.
* Failure: `CORO_EINVAL` (NULL arguments); `CORO_EBUSY` (running/mid-switch, or
  another thread owns the coroutine — see Ownership); `CORO_ESTALE` (already
  started and parked: resume it with the token you were given instead);
  `CORO_EDESTROYED` (dead).

### `coro_status coro_resume(coro_t* c, coro_token tok, coro_token* next)`

The only way to wake a coroutine: present the token its last suspension point
published.

* The claim is a single `CAS(tok -> RUNNING)`. On success control transfers; on
  failure the coroutine is untouched and the caller sees why:
  `CORO_EINVAL` (NULL, or `tok` is not a credential), `CORO_EDESTROYED`
  (`CORO_TOKEN_DEAD`, or the state word says dead), `CORO_ESTALE` (a stale or
  never-issued token), `CORO_EBUSY` (running/mid-switch, or another thread owns
  the coroutine).
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

Reports whether the coroutine is dead. With no runtime layer there is nothing
to park the caller in and this contract never spins waiting, so a live
coroutine yields `CORO_EAGAIN`; drive it to completion and join afterwards. A
blocking join ships with the scheduler. `CORO_EINVAL` for NULL.

### `void* coro_local_get(const coro_t* c, size_t key)` / `coro_status coro_local_set(coro_t* c, size_t key, void* val)`

Read or write one of `CORO_LOCAL_SLOTS` (32) fixed slots that live on the
coroutine, not on the thread, so they survive suspension and thread migration
and never touch `thread_local`. `key >= CORO_LOCAL_SLOTS` is rejected with
`CORO_EINVAL`; `NULL` handles are rejected the same way, and `coro_local_get`
returns NULL for them. Unset slots read NULL. No state word is involved, so
these calls never block or fail on a live coroutine.

## Ownership

The thread that first drives a coroutine owns it, and only the owner may drive
it. A start or resume from another thread returns `CORO_EBUSY` and hands the
credential back, because switching into a context that belongs to a thread the
library does not control would corrupt memory. Delivering a coroutine to
another thread (or migrating it) is the runtime milestone's job.

## Thread safety summary

| Entry point | Thread safety |
| --- | --- |
| `coro_create` | Concurrent for distinct handles. |
| `coro_destroy` | Any thread; refuses a running coroutine. |
| `coro_get_state`, `coro_join`, `coro_cancel`, `coro_local_*` | Any thread holding the handle. |
| `coro_start`, `coro_resume`, `coro_yield` | Owner thread only; concurrent claims are resolved by a single compare-and-swap. |

## Guarantees

* **Exactly once**: for one credential exactly one compare-and-swap can win, so
  at most one execution flow enters a suspension point. Rejected claims are
  reported, never retried (no spinning anywhere in the control path).
* **Never crash**: every invalid or racy call returns a `coro_status`; failures
  leave the state word, the credential and the stack untouched, so calls can be
  retried or routed elsewhere.
* **Overflow stays fatal**: the guard page turns a stack overflow into a fault
  instead of silent corruption (see the L0 documentation).
* **Entry return is safe**: returning from the entry function returns control to
  the last resumer; the process never exits.

## ABI notes

* `coro_t` is an opaque handle: the layout is internal and never exposed.
* `coro_token` is `uint64_t`; `coro_state` and `coro_status` are enumerations
  whose numeric values are stable (`CORO_OK == 0`).
* The library is C99 and C++11 compatible; `coro.h` includes no third-party
  header.

## Measured cost

Figures below are from the project's Linux CI host (x86_64, GCC, Ninja,
`tests/perf/bench_coro.c`). They are indicative, not contractual: absolute
numbers move with the machine, and the lifecycle figure is dominated by mapping
and first-touching a 256 KiB guarded stack.

| Measurement | Value |
| --- | --- |
| resume + yield round trip (two switches, one claim, one publish) | ~56 ns |
| create + start + drive to completion + destroy (256 KiB guarded stack) | ~9.5 µs |
