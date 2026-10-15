# corocc

> [中文](README.zh-CN.md) | English

`corocc` is a dependency-free C/C++ coroutine library for **stackful**
(one-shot-freely resumable, stack-allocating) coroutines. It targets
top-tier systems-engineering standards: a small, well-documented ABI,
guard-page stacks that turn overflow into a hard fault instead of silent
memory corruption, and zero warnings under `-Wall -Wextra -Werror`
(`/W4 /WX` on MSVC) for all first-party code.

## Overview

| Area | What you get |
| --- | --- |
| Layers | L0 context primitives (`coro_stack`, `coro_ctx`), L1 coroutine objects with the token credential protocol, the L2 M:N work-stealing runtime with synchronization primitives (mutex, condition variable, semaphore, rwlock, channel), the L3 structured-concurrency scope layer, a single-thread scheduler, and a header-only C++11 wrapper (`coro.hpp`) |
| Architectures | Native x86_64 (SysV ELF and MSVC PE/MASM) plus 17 cross-compiled ABIs: i386, aarch64/aarch64_be, arm32/armeb, MIPS o32/n32/n64 in both endiannesses, ppc32/ppc64/ppc64le, s390x, riscv32/riscv64; every cross ABI builds and runs its test suite under QEMU user-mode emulation (`scripts/README.md`) |
| Guard pages | Every coroutine stack maps one `PROT_NONE` guard page; overflow faults instead of corrupting neighbours |
| Sanitizers | ASan fiber-switch hooks (`COROCC_ASAN`), including stack-use-after-return detection; a ThreadSanitizer suite covers the runtime's delivery queue, identity registry and atomics |
| Boost coexistence | Symbol-renamed assembly coexists with real Boost.Context in one binary (`boost_coexist` test) |
| Toolchain | GCC, Clang, MSVC (x64) and cross builds via CMake toolchain files; Android NDK compile support |
| Distribution | Per-triple prebuilt static-library archives on tagged releases, with a CMake config package and a zero-dependency consumer example |

## Requirements

- CMake >= 3.16, Ninja or Make, and a C99/C++11 compiler (GCC, Clang or MSVC).
- Optional: Boost.Context headers and library, for the coexistence test.
- To run cross-target binaries on an x86_64 host: the `qemu-user` emulator for
  that ISA plus the matching cross toolchain and sysroot.
  `scripts/build-all.sh` covers all 17 cross targets; the aarch64 flow below
  is the canonical example.
- No other dependencies.

## Layout

```
include/corocc/    public headers (C99, C++11-clean): coro_status.h,
                   coro_stack.h, coro_ctx.h, coro.h, coro_rt.h,
                   coro_sched.h, coro_scope.h, and the wrapper coro.hpp
src/coroutine/     L0/L1 implementation: status.c, stack.c, ctx.c, coro.c,
                   coro_runtime.c (per-thread driver state and wakeup queue)
src/scheduler/     single-thread run loop (sched.c), the M:N work-stealing
                   runtime (rt.c, rt_sync.c, rt_scope.c, timer.c, wsdeque.c,
                   mpsc.c)
src/platform/      threads, locks and condition waits (POSIX/Win32)
src/arch/          context-switch backends (vendored Boost.Context assembly,
                   symbols renamed) plus atomics/TLS/ABI headers
cmake/             arch_select.cmake: per-architecture source selection
toolchain/         canonical cross-compilation toolchain file (aarch64)
scripts/           build/test/cross/prebuild entry points (scripts/README.md)
tests/             unit, C++ (cpp/), concurrency and performance suites
perf/              M:N runtime and pure-switch benchmarks
```

## Build and test

### Native

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: every registered test passes. The suite count depends on the
platform and build type: a plain Linux build registers 43 CTest entries,
`+1` with Boost.Context installed (the `boost_coexist` test), `+1` in a
Release build (the `cpp_perf` throughput gate), and `-1` on Windows (the
POSIX-only `conc_runtime_tsan` suite is not registered there). The suites
cover context switching and guard-page overflow, the coroutine credential
protocol, the single-thread scheduler, the M:N runtime and its primitives,
the scope layer and the C++11 wrapper.

### AddressSanitizer build

```sh
cmake -S . -B build-asan -G Ninja -DCOROCC_ASAN=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

### aarch64 cross build (run under qemu)

```sh
cmake -S . -B build-arm64 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=toolchain/aarch64-linux-gnu.cmake
cmake --build build-arm64
ctest --test-dir build-arm64 --output-on-failure
```

### Benchmark

```sh
./build/tests/bench_switch
# e.g. switches=10000000 ns/switch=11.09
```

The benchmark ping-pongs between two contexts (two switches per iteration)
and reports nanoseconds per switch. Reference figures (Linux x86_64, GCC,
Ninja): about **10-11 ns/switch** natively, and about **50-60 ns/switch** for
aarch64 under `qemu-aarch64` user-mode emulation (emulation overhead
included; the figure moves with host load).

## Prebuilt artifacts (zero-build consumption)

Each tagged release is built by the release workflow
(`.github/workflows/release.yml`), which packs the precompiled static library,
the public headers, a CMake config package and a consumer example into
per-triple archives attached to the GitHub Release, alongside a flat
`SHA256SUMS` file. Linux archives are named `corocc-<tag>-<triple>.tar.gz`;
the Windows archive is `corocc-<tag>-x86_64-pc-windows-msvc.zip`.

```sh
tar xzf corocc-<tag>-aarch64-unknown-linux-gnu.tar.gz
cc your_app.c -Iaarch64-unknown-linux-gnu/include \
              aarch64-unknown-linux-gnu/lib/libcorocc.a
```

No configure or build step is needed: download, point the compiler at the
bundled `include` and `lib`, done. An archive can also be consumed directly
by CMake through the bundled config package:

```sh
cmake -S aarch64-unknown-linux-gnu/examples/cmake-consumer -B build \
      -DCMAKE_PREFIX_PATH="$PWD/aarch64-unknown-linux-gnu"
cmake --build build
```

Linux triples published this way: `x86_64-unknown-linux-gnu`,
`aarch64-unknown-linux-gnu`, `arm-linux-gnueabihf`,
`i386-unknown-linux-gnu`, `mips-unknown-linux-gnu`,
`mipsel-unknown-linux-gnu`, `mips64-unknown-linux-gnuabi64`,
`mips64el-unknown-linux-gnuabi64`, `powerpc-unknown-linux-gnu`,
`powerpc64-unknown-linux-gnu`, `powerpc64le-unknown-linux-gnu`,
`s390x-unknown-linux-gnu` and `riscv64-unknown-linux-gnu`. The Windows
archive is `x86_64-pc-windows-msvc`.

`scripts/prebuild-all.sh` produces an equivalent local layout under `dist/`.

## Using the L0 API

```c
#include <corocc/coro_ctx.h>

static coro_ctx main_ctx, worker;

static void worker_entry(void* arg) {
    /* runs on a guard-page-protected stack; return here is safe */
}

int main(void) {
    coro_stack stk;
    coro_stack_alloc(&stk, 64 * 1024);
    coro_ctx_init(&worker, &stk, worker_entry, NULL);

    coro_ctx_swap(&main_ctx, &worker);   /* enter; worker may swap back */
    coro_ctx_swap(&main_ctx, &worker);   /* resume the same context */

    coro_ctx_destroy(&worker);
    coro_stack_free(&stk);
}
```

A context object is reusable: it may be suspended and resumed any number of
times. Returning from the entry function hands control back to the context
that last swapped into it — never to a process exit. Headers compile as both
C99 and C++11 and pull in no third-party headers.

## Using the L1 coroutine object

```c
#include <corocc/coro.h>

static coro_t* g_worker;

static void worker(void* arg) {
    for (int i = 0; i < 4; ++i) {
        /* ... */
        coro_yield();          /* park here, hand control back to the caller */
    }
    /* returning ends the coroutine */
}

int main(void) {
    coro_token tok = CORO_TOKEN_NONE, next = CORO_TOKEN_NONE;
    coro_create(&g_worker, NULL, worker, NULL);
    coro_start(g_worker, &tok);          /* run until the first suspension */
    while (next != CORO_TOKEN_DEAD) {
        coro_resume(g_worker, tok, &next);   /* needs the token it published */
        tok = next;
    }
    coro_destroy(g_worker);
    return 0;
}
```

Every suspension point publishes a unique token: presenting it is what wakes a
coroutine, a stale token is rejected, and no scheduler is required. Cancellation
is cooperative (`coro_cancel`), values that must survive suspension live in
coroutine-local slots (`coro_local_get/set`), and a coroutine's custom stack
and handle allocator can be supplied per coroutine (`coro_attr_t.allocator`).
The full contract, the state-word encoding and the measured costs are
documented in [docs/api/coro.md](docs/api/coro.md).

## L2 runtime (M:N scheduling and synchronization)

`coro_rt_t` is a work-stealing M:N runtime. `coro_rt_create(n)` starts `n`
worker threads; `coro_rt_spawn`/`coro_rt_spawn_h` submit coroutines (the
managed form reports a handle you collect with `coro_rt_collect`);
`coro_rt_join` parks the caller until a coroutine finishes; `coro_rt_wait_all`
drains the runtime; `coro_rt_sleep`, `coro_rt_yield` and `coro_rt_cancel`
provide cooperative timing and cancellation. Alongside the runtime come its
synchronization primitives, all embeddable by value and zero-initialized: a
mutex, a condition variable, a counting semaphore, a writer-preferring
reader/writer lock and a FIFO channel — the lock and channel are shown here:

```c
#include <corocc/coro_rt.h>

int main(void) {
    coro_rt_t* rt = coro_rt_create(2);

    coro_rt_rwlock_t lock;          /* no init call needed */
    if (coro_rt_rwlock_rdlock(rt, &lock) == CORO_OK)
        coro_rt_rwlock_rdunlock(rt, &lock);

    coro_rt_channel_t chan;
    coro_rt_channel_init(&chan, 8);   /* capacity 0 = unbounded */
    coro_rt_channel_send(rt, &chan, (void*)1);
    void* got = NULL;
    coro_rt_channel_recv(rt, &chan, &got);
    coro_rt_channel_close(rt, &chan);       /* drains, then reports ECLOSED */
    coro_rt_channel_destroy(&chan);

    coro_rt_destroy(rt);
    return 0;
}
```

`coro_rt_rwlock_*` reports `CORO_EBUSY` from the try paths, `CORO_ESTALE`
outside a runtime coroutine, `CORO_ESTATE` for an unlock with no matching hold
and `CORO_ECANCELED` when a wait is cut short. The channel reports
`CORO_EBUSY` for a full `trysend` or an empty `tryrecv`, and `CORO_ECLOSED`
once it is closed and drained. The full contracts live in the headers under
`include/corocc/coro_rt.h`; the lock and channel are also documented in
[docs/api/coro_rt.md](docs/api/coro_rt.md).

## Structured concurrency (L3)

A scope owns a set of child coroutines and gives them one lifetime: the wait
family only reports done once every child the scope accepted has finished and
been collected. The scope object is embeddable by value; a zero-initialized
scope is the OPEN state.

```c
#include <corocc/coro_scope.h>

coro_rt_scope_t s;
memset(&s, 0, sizeof s);                  /* OPEN */
coro_rt_scope_spawn(rt, &s, NULL, work, arg, NULL);   /* child */
coro_rt_spawn(rt, NULL, waiter_entry, &s);            /* waiter coroutine */
coro_rt_wait_all(rt);                     /* the waiter runs wait(rt, &s) */
coro_rt_scope_destroy(&s);                /* after the drain */
```

Both entries of the wait family are supported: from a coroutine the waiter
parks on the scope's event chain; from a plain thread it blocks on the
runtime's condition (never `CORO_EAGAIN`), so a "spawn, then wait from main"
close-out works directly. Two caller responsibilities are contract, not
advice: the wait is immune to its own waiter's cancellation (it always ends on
`live == 0`), and `coro_rt_scope_cancel` only guarantees to interrupt running
or sleeping children -- a child blocked in a sync primitive observes the
request only when its event happens, and children waiting on each other under
a full cancel may keep a wait alive. The full contract, the state machine and
the watchdog design live in [docs/api/coro_scope.md](docs/api/coro_scope.md).

## Driving from several threads

A coroutine is driven from the thread that presents its live credential. The
claim is a single compare-and-swap on the state word: when several threads
race for the same credential, exactly one wins and enters the coroutine; every
other caller receives `CORO_EBUSY` or `CORO_ESTALE` and the coroutine is
untouched. A yield returns to whichever thread drove the current run, so
handing a coroutine to another thread is just a matter of handing over its
credential.

```c
/* any thread holding the live token may drive: */
coro_token next = CORO_TOKEN_NONE;
coro_status s = coro_resume(worker, tok, &next);   /* one claim, one drive */
```

Because coroutine-local slots live on the coroutine rather than on the
thread, they move with it. Threads that own a set of coroutines can also
serve a per-thread wakeup queue: `coro_pump` sweeps the calling thread's
queued requests without blocking and returns how many it served;
`coro_pump_wait` sleeps until a request arrives and then sweeps. Because one
sweep can serve many requests, callers that must serve a fixed number should
count what both return. A request queued for a thread that has already left
fails with `CORO_EDESTROYED` instead of being lost. The full cross-thread
contract and the concurrency verification matrix are in
[docs/api/coro.md](docs/api/coro.md).

## The C++11 wrapper (header-only)

`include/corocc/coro.hpp` is the C++11 face of the same library: RAII
handles (`coroutine`, `runtime`, `scheduler`, `task<R>`, `scope`, `cell<R>`)
over the C entry points, with typed results, chrono durations and an
exception capture-and-deliver boundary. It adds no library and no build
step; a C translation unit that never includes the header is unaffected.

```cpp
#include <corocc/coro.hpp>

int main() {
    corocc::runtime rt(4);                 /* 4 workers; 0 = the default */
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
    st = s.wait();
    if (st.code() == CORO_ECANCELED && s.first_exception()) {
        /* an abnormal end, not a cancellation */
    }
    s.destroy();
    rt.wait_all();
    return 0;
}
```

Read this before decoding a status: on this path `CORO_ECANCELED` has three
meanings (a requested cancel, a fired deadline, an abnormal child end), and
`first_exception()` disambiguates. The full reference -- build rules, the
exception model, ownership/lifetime red lines and the `native()` contract --
is in [docs/api/coro_cpp.md](docs/api/coro_cpp.md).

## Known limitations

- **I/O:** there is no asynchronous I/O completion loop; coroutines park on
  timers and synchronization primitives, not on file or socket readiness.
- **Manual-layer join:** `coro_join` never blocks -- it reports `CORO_EAGAIN`
  while the coroutine is live, and the caller drives it to completion. The
  blocking form lives in the runtime layer (`coro_rt_join`).
- **Threading:** a single `coro_ctx` swap pair is not thread-safe by itself:
  it must be swapped from one thread at a time. The L1 claim guarantees this
  for whole coroutines (see [docs/api/coro.md](docs/api/coro.md)).
- **Exceptions:** `coro_ctx_swap` must not be used to propagate C++
  exceptions across a context boundary.
- **TLS in coroutines on s390x:** do not rely on `thread_local` (TLS)
  variables inside coroutine bodies on s390x -- gcc caches the thread pointer
  in a callee-saved register that context switching restores across threads,
  so the cached value can be stale after a switch. The s390x toolchain file
  therefore compiles with `-fno-stack-protector` (the stack-protector canary
  would read the same cached pointer). Other ABIs are unaffected; the general
  `thread_local` contract is in [docs/api/coro.md](docs/api/coro.md).
- **Cross-build tests:** the Boost-coexistence test is registered for native
  builds only (a cross build would link the host's Boost objects into a
  foreign-architecture binary).

## License

`corocc` is released under the MIT License; see `LICENSE`. The vendored
Boost.Context assembly remains under BSL-1.0 with its original copyright
headers preserved — see `THIRD-PARTY.md`.
