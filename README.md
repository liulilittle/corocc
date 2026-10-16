# corocc

> [中文](README.zh-CN.md) | English

[![ci](https://github.com/liulilittle/corocc/actions/workflows/ci.yml/badge.svg)](https://github.com/liulilittle/corocc/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

`corocc` is a dependency-free C/C++ coroutine library for **stackful**
(one-shot-freely resumable, stack-allocating) coroutines. It targets
top-tier systems-engineering standards: a small, documented
[ABI](docs/ABI.md), guard-page stacks that turn overflow into a hard fault
instead of silent memory corruption, and zero warnings under
`-Wall -Wextra -Werror` (`/W4 /WX` on MSVC) for all first-party code.

## Overview

| Area | What you get |
| --- | --- |
| Layers | L0 context primitives (`coro_stack`, `coro_ctx`), L1 coroutine objects with the token credential protocol, the L2 M:N work-stealing runtime with synchronization primitives (mutex, condition variable, semaphore, rwlock, channel), the L3 structured-concurrency scope layer, a single-thread scheduler, and a header-only C++11 wrapper (`coro.hpp`) |
| Architectures | Native x86_64 (SysV ELF, MSVC PE/MASM and Mach-O), native Windows x86 (MSVC PE/MASM) and macOS arm64, plus 20 cross-compiled ABIs: i386, aarch64/aarch64_be, arm32/armeb/armel (armel = soft-float EABI), MIPS o32/n32/n64 in both endiannesses, ppc32/ppc64/ppc64le, s390x, riscv32/riscv64, loongarch64, sparc64; every cross ABI builds and runs its test suite under QEMU user-mode emulation |
| Guard pages | Every coroutine stack maps one `PROT_NONE` guard page; overflow faults instead of corrupting neighbours |
| Sanitizers | ASan fiber-switch hooks (`COROCC_ASAN`), including stack-use-after-return detection; a ThreadSanitizer suite covers the runtime's delivery queue, identity registry and atomics |
| Boost coexistence | Symbol-renamed assembly coexists with real Boost.Context in one binary (`boost_coexist` test); an optional header-only Boost.Asio integration gives sockets a blocking shape inside coroutines |
| Toolchain | GCC, Clang, MSVC (x64 and x86), Apple clang (macOS x86_64/arm64) and cross builds via CMake toolchain files; Android NDK compile-and-link support |
| Distribution | Per-triple prebuilt static-library archives on tagged releases, with a CMake config package and a zero-dependency consumer example |

## Platform support at a glance

| Axis | Supported |
| --- | --- |
| OS | Linux (glibc), Windows x64 and x86, macOS x86_64 and arm64 (Apple Silicon), Android (NDK, compile+link) |
| Compilers | GCC, Clang, MSVC (VS2022, x64 and x86), Apple clang |
| Language | C99 library; C++11 wrapper and optional header |
| ISA targets | x86_64 (SysV ELF + MSVC PE + macOS Mach-O), i386 (SysV ELF and MSVC PE 32-bit Windows), aarch64 (+BE) including macOS arm64, armv7 hard-float (+BE) and armv5t soft-float (armel), MIPS o32/n32/n64 (+LE), ppc32/ppc64/ppc64le, s390x, riscv32/riscv64, loongarch64, sparc64 |

The full matrix (with per-target CI jobs, emulator runners and known
boundaries) is in [docs/platforms.md](docs/platforms.md).

## Quick start

### CMake (FetchContent)

```cmake
include(FetchContent)
set(COROCC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(corocc
  GIT_REPOSITORY https://github.com/liulilittle/corocc.git
  GIT_TAG        main)
FetchContent_MakeAvailable(corocc)

target_link_libraries(myapp PRIVATE corocc)
```

`add_subdirectory(third_party/corocc)` works the same way; put the cache
variable first. Full integration recipes (prebuilt archives, cross builds,
Android) are in [docs/building.md](docs/building.md).

### Build and test (native)

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

### aarch64 cross build (run under qemu)

```sh
cmake -S . -B build-arm64 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=toolchain/aarch64-linux-gnu.cmake
cmake --build build-arm64
ctest --test-dir build-arm64 --output-on-failure
```

Every cross target follows this shape; `scripts/build-all.sh` walks all 17
of them (see [docs/building.md](docs/building.md)).

## Requirements

- CMake >= 3.16, Ninja or Make, and a C99/C++11 compiler (GCC, Clang or MSVC).
- Optional: Boost.Context headers and library, for the coexistence test.
- To run cross-target binaries on an x86_64 host: the `qemu-user` emulator for
  that ISA plus the matching cross toolchain and sysroot.
- No other dependencies.

## Layout

```
include/corocc/    public headers (C99, C++11-clean): coro_status.h,
                   coro_stack.h, coro_ctx.h, coro.h, coro_rt.h,
                   coro_sched.h, coro_scope.h, the wrapper coro.hpp,
                   and the optional boost/asio.hpp integration
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
docs/              ABI, platform matrix, threading and full API references
```

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
`arm-linux-gnueabi`, `i386-unknown-linux-gnu`, `mips-unknown-linux-gnu`,
`mipsel-unknown-linux-gnu`, `mips64-unknown-linux-gnuabi64`,
`mips64el-unknown-linux-gnuabi64`, `mips64-unknown-linux-gnuabin32`,
`mips64el-unknown-linux-gnuabin32`, `powerpc-unknown-linux-gnu`,
`powerpc64-unknown-linux-gnu`, `powerpc64le-unknown-linux-gnu`,
`s390x-unknown-linux-gnu`, `aarch64_be-unknown-linux-gnu`,
`armeb-unknown-linux-gnueabi`, `riscv64-unknown-linux-gnu`,
`riscv32-unknown-linux-gnu`, `loongarch64-unknown-linux-gnu` and
`sparc64-unknown-linux-gnu`. The Windows archive is
`x86_64-pc-windows-msvc`.

`scripts/prebuild-all.sh` produces an equivalent local layout under `dist/`.

## Performance

Reference figures measured with the repository's benchmarks on Linux x86-64
(GCC 13, `-O2`; indicative, not contractual — absolute numbers move with the
machine):

| Measurement | Figure |
| --- | --- |
| L0 context switch, main ⇄ coroutine ping-pong (`tests/bench_switch`) | ~10-11 ns/switch |
| Pure runtime switch throughput, 8 coroutines (`tests/bench_switch_pure`) | ~20-22M yield round-trips/s (~46-49 ns per round-trip, ~23-25 ns per context switch) |
| Composite spawn+yield workload, 1 worker (`tests/bench_rt_mn`) | ~12M switches/s (82 ns/switch; printed unit: one yield = two switches) |
| L1 resume+yield round trip (`tests/bench_coro`) | ~56 ns |
| create + start + drive to completion + destroy (256 KiB guarded stack) | ~9.5 µs |
| aarch64 under `qemu-aarch64` user-mode emulation (`tests/bench_switch`) | ~50-60 ns/switch (emulation overhead included) |

Reproduce with `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release &&
cmake --build build && ./build/tests/bench_switch`.

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
C99 and C++11 and pull in no third-party headers. The status codes, the
guarded stack and the context object are documented in
[docs/api/coro_l0.md](docs/api/coro_l0.md).

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
`include/corocc/coro_rt.h` and in [docs/api/coro_rt.md](docs/api/coro_rt.md).

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
`coro_pump_wait` sleeps until a request arrives and then sweeps. A request
queued for a thread that has already left fails with `CORO_EDESTROYED`
instead of being lost. The full cross-thread contract and the concurrency
verification matrix are in [docs/api/coro.md](docs/api/coro.md); the
threading model, migration rules and `thread_local` notes are in
[docs/threading.md](docs/threading.md).

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

## Asynchronous I/O (optional Boost.Asio bridge)

`include/corocc/boost/asio.hpp` gives a Boost.Asio operation the shape of a
blocking call made from inside a runtime coroutine — no heap allocation, and
the completion thread never touches the coroutine's stack:

```cpp
#include <corocc/coro_rt.h>
#include <corocc/boost/asio.hpp>
#include <boost/asio.hpp>

boost::system::error_code ec;
std::size_t n = corocc::asio::async_read(rt.native(), sock,
                                         boost::asio::buffer(buf), ec);
```

The four wrappers (`async_read`, `async_read_some`, `async_write`,
`async_write_some`) and their cancellation/strand contract are documented in
[docs/api/boost.md](docs/api/boost.md).

## Documentation

| Document | Contents |
| --- | --- |
| [docs/building.md](docs/building.md) | Build options, CMake integration, prebuilt archives, cross and Android flows, benchmarks |
| [docs/platforms.md](docs/platforms.md) | Support matrix: OS/compiler/ISA targets, qemu sites, sanitizers, known boundaries |
| [docs/ABI.md](docs/ABI.md) | The ABI contract: stability promise, measured layout tables, visibility, versioning |
| [docs/threading.md](docs/threading.md) | Threading model, cross-thread migration, wakeup queues, `thread_local` rules |
| [docs/api/coro_l0.md](docs/api/coro_l0.md) | L0 reference: status codes, guarded stacks, execution contexts |
| [docs/api/coro.md](docs/api/coro.md) | L1 reference: the coroutine object and the credential protocol |
| [docs/api/coro_rt.md](docs/api/coro_rt.md) | L2 reference: the M:N runtime and its synchronization primitives |
| [docs/api/coro_sched.md](docs/api/coro_sched.md) | The single-thread scheduler reference |
| [docs/api/coro_scope.md](docs/api/coro_scope.md) | L3 reference: structured concurrency |
| [docs/api/coro_cpp.md](docs/api/coro_cpp.md) | The C++11 wrapper reference |
| [docs/api/boost.md](docs/api/boost.md) | The optional Boost.Asio integration |
| [scripts/README.md](scripts/README.md) | Build, test, cross and prebuild scripts |
| [THIRD-PARTY.md](THIRD-PARTY.md) | Vendored components and licenses |

## Known limitations

- **I/O:** the core library has no asynchronous I/O completion loop;
  coroutines park on timers and synchronization primitives, not on file or
  socket readiness. The optional Boost.Asio integration
  ([docs/api/boost.md](docs/api/boost.md)) bridges to an event loop.
- **Manual-layer join:** `coro_join` never blocks -- it reports `CORO_EAGAIN`
  while the coroutine is live, and the caller drives it to completion. The
  blocking form lives in the runtime layer (`coro_rt_join`).
- **Threading:** a single `coro_ctx` swap pair is not thread-safe by itself:
  it must be swapped from one thread at a time. The L1 claim guarantees this
  for whole coroutines (see [docs/api/coro.md](docs/api/coro.md) and
  [docs/threading.md](docs/threading.md)).
- **Exceptions:** `coro_ctx_swap` must not be used to propagate C++
  exceptions across a context boundary.
- **TLS in coroutines on s390x:** do not rely on `thread_local` (TLS)
  variables inside coroutine bodies on s390x -- gcc caches the thread pointer
  in a callee-saved register that context switching restores across threads,
  so the cached value can be stale after a switch. The s390x toolchain file
  therefore compiles with `-fno-stack-protector`. Other ABIs are unaffected;
  the general `thread_local` contract is in
  [docs/threading.md](docs/threading.md).
- **Cross-build tests:** the Boost-coexistence test is registered for native
  builds only (a cross build would link the host's Boost objects into a
  foreign-architecture binary).

## License

`corocc` is released under the MIT License; see `LICENSE`. The vendored
Boost.Context assembly remains under BSL-1.0 with its original copyright
headers preserved — see `THIRD-PARTY.md`.
