# coro_cpp -- the C++11 wrapper (header-only)

> English | [中文](coro_cpp.zh-CN.md)

This document is the API reference for `include/corocc/coro.hpp`, the C++11
header-only face of corocc. The wrapper is parallel to the C API: it adds
RAII ownership, typed results, chrono durations and an exception
capture-and-deliver boundary, but invents no second scheduling or
cancellation mechanism. Everything it does is built on the C entry points
documented in [coro.md](coro.md), [coro_rt.md](coro_rt.md) and
[coro_scope.md](coro_scope.md); the C consumer pays nothing for it (a C
translation unit that never includes the header is byte-for-byte unaffected).

Build rules that are part of the contract:

- **C++11 baseline.** The header uses no C++14-or-later facility. It is
  verified with `-std=c++11 -pedantic-errors -Wall -Wextra -Werror` and, as a
  forward check, with `-std=c++14/17/20`. MSVC has no C++11 mode: the
  documented MSVC baseline is "the implementation does not use C++11-or-later
  *after* features", accepted with `/std:c++14 /W4 /WX`.
- **`COROCC_CPP_NO_EXCEPTIONS`** forces the degraded mode (no `throw`,
  `get()`/`wait_or_throw()` deleted). `COROCC_CPP_HAS_EXCEPTIONS=1` is
  detected automatically otherwise.
- **One version per binary.** `coro.hpp` is the implementation, so every
  translation unit of a binary must include the same version, and the
  exception mode (the macro above) must agree across the binary: a mixed
  binary is an ODR violation by construction. The only piece of shared
  wrapper state, the detached-exception handler slot, is a function-local
  static: one instance per binary module (EXE and each DLL have their own).
- Link the existing C library exactly as before (`libcorocc.a` /
  `corocc.lib`); the wrapper adds no symbols, no library and no build step.

## Concepts

| Term | Meaning |
| --- | --- |
| `corocc::status` | Zero-overhead `coro_status` wrapper: `ok()`, `failed()`, `code()`, `message()`; converts implicitly both ways. |
| `corocc::attrs` | Creation attributes builder (`stack_size`, owning `name`, `allocator`); `flags` is not exposed (the C API accepts only `CORO_F_NONE`). |
| `corocc::monotonic_clock` | C++11 Clock over `coro_monotonic_ns()` (nanosecond `duration`, steady). |
| Control block (box) | The wrapper's per-spawn heap object: the callable, an owned copy of the name, the shared result state, the dummy C cell alias. |
| Snapshot (`state<R>`) | What the trampoline publishes at child end: code, exception, typed value, readiness flag. **All wrapper reads go here**, never to the C cell. |
| Trampoline | The `extern "C"`-compatible entry the C layer calls; it runs the user callable, catches every escape, publishes the snapshot, and never lets an exception cross the context switch. |
| Detached handler | `detached_exception_handler` -- what happens to an exception escaping a `runtime::spawn` without a handle; `std::terminate` by default. |

## Ownership and lifetime

- Every spawn allocates **one wrapper control block** (`new (std::nothrow)`,
  plus one shared result state for typed spawns). Wrapper metadata that runs
  out of memory normalises to `CORO_ENOMEM`; an exception thrown while
  *storing the user's* callable/arguments propagates to the caller (the
  `std::thread` rule) -- one refinement: a `std::bad_alloc` thrown while
  the wrapper itself moves/stores the callable or arguments normalises to
  `CORO_ENOMEM`, while the call-site hop of the arguments keeps the plain
  propagate rule. There is no zero-allocation C++ spawn -- the escape
  hatch is the C API through `native()`.
- `coroutine`, `runtime`, `scheduler`, `task`, `sched_task` are **unique
  owners** (move-only). `cell<R>` is a shared handle; `scope` is neither
  copyable nor movable (the C record holds a back-pointer to it).
- **Object lifetime <= runtime/scheduler.** `task`/`scope` operations that
  touch runtime internals (`cancel`, `join`, `wait` on a live target) must
  happen before the owning runtime/scheduler is destroyed. The one exception
  is `coro_rt_collect` after destroy, which the wrapper's closeout may reach
  only for a target that already finished.
- Destructors **drain**: `runtime`/`scheduler` do `wait_all()` then
  `destroy()`; `scope` does `cancel()` + `wait()` + `destroy()`; `task`
  cancels an unfinished target and collects it. A drain can be unbounded -- a
  child that can never become runnable makes it block (or, for the scheduler
  dtor with a parked child, makes `destroy` report `EBUSY`, which the dtor
  treats as a caller red line). This is the same caller responsibility the C
  layer documents.
- Destroying a **SUSPENDED** `coroutine` is legal but **silently abandons the
  coroutine frame**: the destructors of C++ objects on that stack do **not**
  run. Cooperate when it matters: `cancel()` then drive `resume()` until
  `done()`. Debug builds count the abandon (never abort).

## Entry points

### Value types

| Entry point | Contract |
| --- | --- |
| `status(coro_status)` / `operator coro_status` | Implicit both ways; `static_assert(sizeof(status) == sizeof(coro_status))`. |
| `status::ok() / failed() / code() / message()` | `noexcept`; `message()` = `coro_status_str`. |
| `corocc::error(coro_status)` | `std::exception` carrying a status; `what()` = `coro_status_str`. Thrown only by the closeout points. |
| `attrs::stack_size(n) / name(s) / allocator(p)` | Chainable; `name` is owned, so a temporary `attrs` handed to a spawn is safe. |
| `monotonic_clock::now()` | `coro_monotonic_ns()` as a `time_point`; steady. |
| `set_detached_exception_handler(h)` | Installs `h` (null restores the default `std::terminate`) and returns the previous handler. |

### `coroutine` (L1, manual drive)

| Entry point | Contract |
| --- | --- |
| `static create(out, f, attrs)` | Builds the coroutine; the callable must return `void` or `coro_status` (void means OK). `out` non-empty -> `CORO_ESTATE`. Returns the C set, `ENOMEM` for metadata OOM. |
| `start() / resume()` | Drive with the internal credential (single-driver discipline; advanced token protocols go through `native()`). The C set. |
| `cancel() / join()` | The C faces (`join`: OK when dead, `EAGAIN` when live). |
| `state() / done() / valid() / native()` | Read-only. |
| `exception()` | The captured body exception (null unless the body threw and finished). |
| `destroy()` | Release; `EBUSY` (running) is reported, a second call is `EINVAL` (empty). |

### `runtime` and `task<R>`

| Entry point | Contract |
| --- | --- |
| `runtime(nthreads)` / `static create(out, n) / valid() / native()` | `nthreads == 0` selects the C default; construction failure is `valid() == false` (or `CORO_ENOMEM` from `create`). |
| `spawn(f [, args])` | Detached: the runtime reclaims everything; an escaping exception goes to the detached handler. |
| `spawn_h(task<R>&, f [, args])` | Managed typed task. On failure `out` is untouched; a live `out` is closed out first. |
| `pending() / wait_all() / cancel(c) / destroy()` | The C faces. |
| `sleep_for(d) / sleep_until(tp)` | Coroutine-only (`ESTALE` elsewhere); the duration is ceil-converted and saturated, a negative or expired deadline parks zero time. |
| `task::wait()` | **Target-directed closeout.** Done -> collect with the bounded retry (yield inside a coroutine, 1 ms outside). Not done: inside a coroutine it joins (parks until the target ends; waiting the calling coroutine's **own** handle is the self-join and reports `EAGAIN`); on a plain thread it rides the retirement window with the bounded 1 ms collect retry (5000 tries, ~5 s) and reports `EBUSY` when spent -- the caller may retry. It never touches `wait_all`, so unrelated parked tasks cannot hold it up. A target that already died normalises to OK. |
| `task::join()` | The C `coro_rt_join`, exactly: from a coroutine it parks; from a thread it reports `EAGAIN`; `EDESTROYED`/`ECANCELED` as documented there. |
| `task::cancel()` | The C request; `OK` / `EINVAL`. |
| `task::get()` | Done -> value or rethrow; otherwise `wait()` first. A failed wait throws `corocc::error` carrying its status. The exception channel wins over the code channel. The value is moved out of the snapshot: a second call reads the moved-from value. |
| `task::peek() / exception() / done()` | Ready-gated snapshot reads; safe from any thread once done. |

`R == void` and `R == coro_status` have dedicated storage (`coro_status` is
not stored twice); `task<void>::get()` rethrows only.

### `scheduler` and `sched_task<R>`

| Entry point | Contract |
| --- | --- |
| `static create(out) / run() / wait_all() / pending() / destroy()` | Bound to the creating thread; debug builds abort on use from another thread. |
| `spawn / spawn_h / sleep_for / sleep_until / cancel` | As above, on the single-thread run loop. |
| `sched_task::wait()` | `done` -> `OK` (the handle is retired; it is never touched); otherwise `coro_sched_join` from a scheduler coroutine (`ESTALE` outside one), `EDESTROYED` normalised to `OK`, `ECANCELED` as the C reports. |
| `sched_task::cancel()` | `done` -> `OK` without touching C; another thread -> `ESTALE` without touching C; otherwise the C request. |
| `sched_task::get() / peek() / exception()` | As `task`; the scheduler owns the C handle and frees it on retirement, so the wrapper's done flag gates every touch. |

### `scope` and `cell<R>`

| Entry point | Contract |
| --- | --- |
| `scope(rt)` | Binds a runtime (or a raw `coro_rt_t*`); `native()` is the C record. |
| `spawn(f [, args])` | Non-typed child: the callable returns `void` or `coro_status`. **Every child -- typed or not -- registers a non-NULL `cell_out`**, so an abnormal end is visible to the C aggregation and drives `wait_first_error`'s early exit. |
| `spawn(cell<R>&, f [, args])` | Typed child; `out` binds only on success. |
| `wait() / wait_all() / wait_first_error()` | The C drains; the return is the aggregate / first error. |
| `wait_first_done(first)` | The first completion; the scope stays alive -- the caller MUST follow up with `wait()` or `cancel()`. |
| `wait_for(d) / wait_until(tp)` | Deadline forms; ceil-converted, saturated, `<= 0` clamps to the check-once deadline. |
| `wait_or_throw()` | Drain, then rethrow the first exception (authoritative) or throw `error(code)`. |
| `cancel() / cancel_requested() / status() / first_exception()` | `status()` reads the C partial aggregate before a drain and the frozen aggregate after (readable even after destroy); `first_exception()` walks the wrapper snapshots. |
| `destroy()` | `OK` (idempotent) / `EBUSY` (children live) / `EINVAL`. |
| `cell<R>::ready() / status() / exception() / peek() / get() / try_get()` | All reads go to the wrapper snapshot; `get()` requires readiness (a contract violation otherwise). Cells stay valid (and readable) after the scope is destroyed. `get()`/`try_get()` move the stored value out of the snapshot: a second call reads the moved-from value -- take the result exactly once. Before readiness `status()` reads `CORO_OK` (nothing observed yet); after readiness it is the child's final code. |

### Synchronization

`mutex`, `condition_variable`, `semaphore`, `rwlock` are value types bound to
one runtime; all waiting entries are coroutine-only (`CORO_ESTALE` from a
plain thread), `try` variants never park (a plain-null `CORO_EBUSY` face).
`mutex::lock()/unlock()` satisfy `std::lock_guard`/`std::unique_lock`.
`channel<>` (that is `channel<void*>`) is the zero-allocation C face (payload
lifetime is the caller's); `channel<T>` allocates one node per message and
tops the C return set with `CORO_ENOMEM` (the payload store follows the
callable rule: `bad_alloc` normalises, other exceptions propagate). Both
destructors close, drain and destroy.

## ECANCELED has three meanings (read this before decoding a status)

The C `CORO_ECANCELED` reaching a C++ caller is ambiguous in exactly three
ways. The rule is: **consult `first_exception()` (or `cell::exception()`)
first, then the code.**

| Meaning | Scene | Observer action |
| --- | --- | --- |
| 1. A cancel request was observed | A child was cancelled and returned ECANCELED itself | Normal cancellation. |
| 2. Abnormal end (the exception marker) | The child body threw and the trampoline caught it; the C cell carries ECANCELED | **`first_exception()` / `cell<R>::exception()` is non-null** -- this was NOT a cancellation. |
| 3. `wait_for`/`wait_until` deadline first | This waiter's own deadline fired (the outer return) | Check `first_exception()`; if null, it really was a timeout. |

`wait_or_throw()` rethrows the body's exception before considering the code;
`wait_first_error()` returning ECANCELED requires the same disambiguation.

**C consumers of a scope with C++ children:** an uncaptured exception is
reported through the C status channel as `CORO_ECANCELED` (an abnormal end),
and that is exactly what makes the C-level `first_error` early exit work. The
C channel cannot carry the exception itself; C++ pages are the reference for
mixed-language surfaces.

## Reading the C records, and `native()` red lines

- The wrapper **never dereferences the C result cell** (`scope_child.value`):
  the code and exception a C++ caller sees come from the wrapper snapshot.
  This is why a `cell<R>` and `scope::status()/first_exception()` remain
  readable after the scope is destroyed.
- `native()` hands out the borrowed C handle for the object's lifetime.
  Handles the wrapper owns (runtime, scheduler, task, scope children,
  coroutines) must not be destroyed, collected or driven by hand: the
  scheduler's handle is freed by its retire path the moment the coroutine
  finishes (a second free path exists if a borrowed scheduler handle is hand
  driven -- the wrapper never triggers it), and a hand-collected task handle
  leaves the wrapper's own bookkeeping dangling. Use `native()` to *read*
  state or to mix C and C++ call sites inside a wrapper-owned scope; use the
  C API directly (with C ownership) when you want the C contract.

## Example

```cpp
#include <corocc/coro.hpp>
#include <chrono>

int main() {
    corocc::runtime rt(4);
    if (!rt.valid()) return 1;

    corocc::task<int> t;
    rt.spawn_h(t, [] { return 7; });
    corocc::status st = t.wait();          /* thread-side targeted closeout */
    if (st.ok()) {
        int v = t.get();
        (void)v;
    }

    corocc::scope s(rt);                   /* every child carries a cell */
    corocc::cell<int> a;
    s.spawn(a, [] { return 40 + 2; });
    s.spawn([] { /* void child: exceptions still enter the aggregate */ });
    st = s.wait();
    if (st.code() == CORO_ECANCELED && s.first_exception()) {
        /* an abnormal end, not a cancellation */
    }
    s.destroy();
    rt.wait_all();
    return 0;
}
```
