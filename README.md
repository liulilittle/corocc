# corocc

> [中文](README.zh-CN.md) | English

`corocc` is a dependency-free C/C++ coroutine library for **stackful**
(one-shot-freely resumable, stack-allocating) coroutines. It targets
top-tier systems-engineering standards: a small, well-documented ABI,
guard-page stacks that turn overflow into a hard fault instead of silent
memory corruption, and zero warnings under `-Wall -Wextra -Werror`
(`/W4 /WX` on MSVC) for all first-party code.

## Status

Milestone 1 (skeleton + L0 execution contexts) is complete:

| Area | State |
| --- | --- |
| Layers | L0-L3 implemented: context primitives (`coro_stack`, `coro_ctx`), coroutine objects, the M:N work-stealing runtime with synchronization primitives, and the structured-concurrency scope layer; the header-only C++11 wrapper (`coro.hpp`, M8) and the Windows (MSVC) port (M9) have landed |
| Architectures | 17 context-switch backends vendored: x86_64 (SysV), i386, aarch64/aarch64_be, arm32/armeb, MIPS o32/n32/n64 in both endiannesses, ppc32/ppc64/ppc64le, s390x, riscv32/riscv64; the qemu-user cross matrix is green on every ABI (see `scripts/README.md`) |
| Guard pages | Every coroutine stack maps one `PROT_NONE` guard page; overflow faults instead of corrupting neighbours |
| Sanitizers | ASan fiber-switch hooks (`COROCC_ASAN`); clean under ASan, including stack-use-after-return detection |
| Boost coexistence | Symbol-renamed assembly coexists with real Boost.Context in one binary (`boost_coexist` test) |
| Toolchain | GCC, Clang, MSVC and cross builds via CMake toolchain files |

## Requirements

- CMake >= 3.16, Ninja or Make, a C99 and C++11 compiler.
- To run cross-target binaries on an x86_64 host: the `qemu-user` emulator for
  that ISA plus the matching cross toolchain + sysroot. `scripts/build-all.sh`
  covers all 17 ABIs; the canonical aarch64 flow is shown below.
- Optional: Boost.Context headers/library, for the coexistence test.
- No other dependencies.

## Layout

```
include/corocc/    public headers (C99, C++11-clean): coro_status.h,
                   coro_stack.h, coro_ctx.h
src/coroutine/     implementation: status.c, stack.c, ctx.c
src/arch/          vendored Boost.Context assembly, symbols renamed
toolchain/         cross-compilation toolchain files
tests/             unit tests, the boost-coexistence test, the benchmark
```

## Build and test

### Native (x86_64)

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: `8/8 tests passed` — `smoke`, `stack`, `ctx`, `ctx_return`,
`ctx_pingpong`, `ctx_overflow`, `ctx_asan`, and `boost_coexist` (the last
only when Boost.Context is installed).

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
and reports nanoseconds per switch. Reference measurements from the project's
Linux CI host: about **10-11 ns/switch** natively on x86_64 (GCC, Ninja), and
about **50-60 ns/switch** for aarch64 under `qemu-aarch64` user-mode emulation
(emulation overhead included; the figure moves with host load).

## Prebuilt artifacts (zero-build consumption)

Every tag is built by GitHub Actions (`.github/workflows/release.yml`), which
packs the precompiled static library together with the public headers into
per-triple archives attached to the GitHub Release, for example
`corocc-x86_64-unknown-linux-gnu-linux.tar.gz`.

```sh
tar xzf corocc-aarch64-unknown-linux-gnu-linux.tar.gz
cc your_app.c \
    -Iaarch64-unknown-linux-gnu/include \
    aarch64-unknown-linux-gnu/lib/libcorocc.a
```

No configure or build step is needed: download, point the compiler at the
bundled `include` and `lib`, done. The set of published triples grows with the
architectures that have a vendored context-switch backend; `scripts/README.md`
carries the current status matrix, and `scripts/prebuild-all.sh` produces the
same layout locally.

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
coroutine-local slots (`coro_local_get/set`), and a coroutine belongs to the
thread that first drove it. The full contract, the state-word encoding and the
measured costs are documented in [docs/api/coro.md](docs/api/coro.md).

## L2 runtime primitives

The M:N runtime also ships a writer-preferring reader/writer lock and a FIFO
channel, both embeddable by value and zero-initialized:

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
once it is closed and drained. A full contract lives in the headers under
`include/corocc/coro_rt.h`.

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

## Multithreading

A coroutine belongs to the thread that first drove it. A wakeup requested from
any other thread is *delivered* to that owner: the request is queued and the
requesting thread waits for the result the owner records, so no coroutine is
ever entered by a thread that does not belong to it.

```c
/* thread A (the owner): parks in coro_pump_wait until work arrives */
coro_pump_wait();          /* sleeps, then serves every queued request */

/* thread B: asks for a wakeup -- delivered to A, and A runs it */
coro_token next = CORO_TOKEN_NONE;
coro_status s = coro_resume(worker, tok, &next);   /* waits for A's result */
```

`coro_pump` sweeps the queue without blocking (useful when the owner has its own
work) and returns how many requests it served; `coro_pump_wait` sleeps first.
Because one sweep can serve many requests, callers that must serve a fixed
number should count what both return. A request queued for an owner that has
left fails with `CORO_EDESTROYED` instead of being lost.

Migrating a coroutine to another thread (rather than delivering to its owner)
is not provided yet: it needs the scheduler's work-stealing structures and
arrives with that milestone. See [docs/api/coro.md](docs/api/coro.md) for the
full cross-thread contract and the concurrency test matrix.

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

- **Scope:** the L0-L3 layers ship today (context primitives, coroutine
  objects with the token credential protocol, the M:N work-stealing runtime
  with its synchronization primitives and the structured-concurrency scope
  layer), together with the header-only C++11 wrapper (`coro.hpp`, M8) and
  the Windows (MSVC) port (M9). The IO loop is a follow-up milestone.
- **Architectures:** all 17 backends are vendored, wired into the build and
  green under qemu-user (i386, arm32/armeb, MIPS o32/n32/n64 little- and
  big-endian, ppc32/ppc64/ppc64le, s390x, riscv32/riscv64, aarch64/aarch64_be);
  Windows (MSVC x64) is covered natively. Toolchain files and the per-ABI
  runner: `scripts/README.md`, `scripts/ci/build-test.sh`.
- **Threading:** a `coro_ctx` is not thread-safe — it must be swapped from a
  single thread at a time (no internal locks by design at this layer).
- **Exceptions:** `coro_ctx_swap` must not be used to propagate C++
  exceptions across a context boundary.
- **TLS in coroutines on s390x:** do not rely on `thread_local` (TLS)
  variables inside coroutine bodies on s390x — gcc caches the thread pointer
  in a callee-saved register that context switching restores across threads,
  so the cached value can be stale after a switch (the s390x build therefore
  also uses `-fno-stack-protector`). The general `thread_local` contract is in
  [docs/api/coro.md](docs/api/coro.md); the s390x specifics are in
  `.opencode/logs/isa/exec/S390X-TLS-note.md`. Other ABIs are unaffected.
- **Cross-build tests:** the Boost-coexistence test is only registered for
  native builds.

## License

`corocc` is released under the MIT License; see `LICENSE`. The vendored
Boost.Context assembly remains under BSL-1.0 with its original copyright
headers preserved — see `THIRD-PARTY.md`.
