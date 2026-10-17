# coro_rt_driver — the runtime driver registry, attach/detach and the LOOP pump

> English | [中文](coro_rt_driver.zh-CN.md)

This document is the API reference for `coro_rt_driver.h`, the layer that lets a
host provide the runtime's three host needs — **doorbell** (wake), **clock**
(now) and **timers** (alarm) — instead of the built-in EDSM backend (worker
threads, per-worker timer heap, `coro_monotonic_ns`). With no driver attached a
runtime behaves exactly as before (`rt->drv == NULL` is the built-in path), so
this is a pure addition.

There are two execution families:

- **SIGNAL** (no `CORO_DRIVER_FLAG_LOOP`): worker threads stay; the driver
  supplies the doorbell, the clock and (optionally) the alarm. Built-in and
  SIGNAL drivers may be hot-swapped while coroutines are parked.
- **LOOP** (`CORO_DRIVER_FLAG_LOOP`): the runtime owns **no** worker threads; the
  host loop drives progress by calling `coro_rt_pump`. A LOOP runtime is created
  from a LOOP process-default driver.

## Driver vtable

```c
enum coro_driver_flags { CORO_DRIVER_FLAG_NONE = 0, CORO_DRIVER_FLAG_LOOP = 1u << 0 };

typedef uint64_t (*coro_driver_now_fn)(void* ctx);
typedef void     (*coro_driver_wake_fn)(void* ctx);
typedef void     (*coro_driver_timer_fn)(void* arg);
typedef int      (*coro_driver_timer_arm_fn)(void* ctx, void* req,
                    uint64_t deadline, coro_driver_timer_fn fn, void* arg);
typedef int      (*coro_driver_timer_cancel_fn)(void* ctx, void* req);

struct coro_rt_driver_ops {
    uint32_t                    struct_size;   /* = sizeof(struct) */
    uint32_t                    flags;         /* enum coro_driver_flags */
    coro_driver_now_fn          now;           /* optional; NULL = built-in clock */
    coro_driver_wake_fn         wake;          /* mandatory */
    coro_driver_timer_arm_fn    timer_arm;     /* optional */
    coro_driver_timer_cancel_fn timer_cancel;  /* required with timer_arm */
};
```

| Callback | Contract |
| --- | --- |
| `now(ctx)` | Monotone, non-decreasing nanoseconds from any thread; only differences matter. A driver swapped in must keep the same scale as the one it replaces (the stored deadlines are not rescaled). Optional (NULL uses `coro_monotonic_ns`). |
| `wake(ctx)` | The doorbell. Called from any thread when new runnable work appears or a sleeper may need to rescan. Cheap, non-blocking, concurrency-safe, idempotent; must **not** call back into the runtime synchronously (the runtime holds no lock when it calls `wake`, and re-entering would self-deadlock). Mandatory. |
| `timer_arm(ctx, req, deadline, fn, arg)` | Fire `fn(arg)` exactly once at the absolute `deadline` (same scale as `now`). `req` is an opaque per-worker token owned by the runtime. Returns 0 on success. Optional. |
| `timer_cancel(ctx, req)` | Withdraw a not-yet-fired alarm. After it returns `fn` will not run (or already ran). Returns 0 when it cancelled, non-zero when the alarm had fired / is unknown. Required when `timer_arm` is set. |
| `timer_fn(arg)` | Delivered by the driver on its own thread. The runtime uses it only to wake workers; it must not block the loop thread. |

## Registry

| Entry point | Contract |
| --- | --- |
| `coro_rt_driver_register(name, ops, ctx, &out)` | Copy `ops` into a new instance and register it process-wide (thread-safe). `name` may be NULL (anonymous, not findable). `ctx` must outlive the instance's use. Returns `CORO_OK`; `CORO_EINVAL` for a NULL `out`/`ops`, a NULL `wake`, a `struct_size` smaller than this build's vtable, or a `timer_arm`/`timer_cancel` mismatch; `CORO_ENOMEM`; **`CORO_ESTATE` when `name` is already registered** (names are unique; `*out` is set to NULL on every failure). |
| `coro_rt_driver_unregister(drv)` | Remove and free an instance. `CORO_EBUSY` while it is in use (the current process default, or attached to a runtime); `CORO_OK` for NULL (idempotent); `CORO_EINVAL` for a non-NULL pointer the registry does not own. |
| `coro_rt_driver_find(name)` | Look up by name (NULL in, NULL out); borrowed pointer, valid until unregister. |
| `coro_rt_driver_set_default(drv)` | Select the process default read by `coro_rt_create`; `drv = NULL` resets to the built-in path. `CORO_EINVAL` when `drv` is non-NULL and not registered. |
| `coro_rt_driver_default(void)` | The current default, or NULL (built-in). |

## Attach / detach / query

| Entry point | Contract |
| --- | --- |
| `coro_rt_attach_driver(rt, drv)` | Attach `drv` (NULL = detach). Built-in ↔ SIGNAL may be swapped even with coroutines parked: the runtime quiesces the workers, re-arms each pending alarm against the new driver and wakes them, so no deadline is lost. A **cross-family** switch involving LOOP is refused with `CORO_EBUSY` (v1; runtime cross-class switching is a later phase) — a LOOP runtime must be created from a LOOP default. `CORO_EINVAL` for a NULL runtime or an unregistered driver; same-driver re-attach is `CORO_OK`. |
| `coro_rt_detach_driver(rt)` | Restore the built-in backend. `CORO_OK` when nothing is attached; `CORO_EBUSY` on a LOOP runtime (its threads were never started). |
| `coro_rt_attached_driver(rt)` | The attached driver, or NULL (built-in). |

## The LOOP pump

| Entry point | Contract |
| --- | --- |
| `coro_rt_pump(rt, budget)` | One bounded turn of a LOOP runtime: drain expirations, the inbox and worker[0]'s run queue up to `budget` runnable nodes (`budget == 0` drains everything runnable), then return. Never blocks. `CORO_EBUSY` when the runtime is not a LOOP runtime or a pump is already running on it; `CORO_EINVAL` for NULL. |

In LOOP mode the runtime does not arm the driver timer itself: a parked
`coro_rt_sleep` is collected by the next pump once `rt_now` passes its deadline,
so the host loop is expected to pump again at (or after) that deadline — an
external backend schedules that follow-up through `timer_arm` (see
`examples/backends/asio-pump`, `libuv`, `libevent`). `coro_rt_wait_all` and the
scope thread-side waits block on the runtime's settled condition and must not be
called from the pump loop thread.

## Default path and the build switch

With no driver attached, every dispatch is a single NULL test that executes the
original statement: the doorbell is the activity broadcast, the clock is
`coro_monotonic_ns` and the alarm is the per-worker timer heap. Defining
`COROCC_RT_DRIVER=0` (the CMake option `COROCC_RT_DRIVER=OFF`) compiles those
branches out entirely for builds that never attach a driver; the registry itself
remains available.

## Backends

| Backend | Location | Loop |
| --- | --- | --- |
| Boost.Asio | `examples/backends/asio-pump` | `io_context` + `asio::post` / `steady_timer` |
| libuv | `examples/backends/libuv` | `uv_run` + `uv_async` / `uv_timer` |
| libevent | `examples/backends/libevent` | `event_base_dispatch` + `event_active` / `evtimer` |

Each is a stand-alone CMake project (not part of the library build) with its own
tests.
