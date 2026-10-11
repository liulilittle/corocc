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
| Layers | L0 context primitives implemented (`coro_stack`, `coro_ctx`); L1 coroutine objects, L2 runtime/scheduler, L3 structured concurrency and the C++11 wrapper are planned follow-ups |
| Architectures | x86_64 (SysV) and aarch64 (AAPCS) vendored assembly; aarch64 verified under `qemu-aarch64` |
| Guard pages | Every coroutine stack maps one `PROT_NONE` guard page; overflow faults instead of corrupting neighbours |
| Sanitizers | ASan fiber-switch hooks (`COROCC_ASAN`); clean under ASan, including stack-use-after-return detection |
| Boost coexistence | Symbol-renamed assembly coexists with real Boost.Context in one binary (`boost_coexist` test) |
| Toolchain | GCC, Clang, MSVC and cross builds via CMake toolchain files |

## Requirements

- CMake >= 3.16, Ninja or Make, a C99 and C++11 compiler.
- To run the aarch64 binaries on an x86_64 host: `qemu-aarch64` plus an
  aarch64 sysroot + cross toolchain (`aarch64-linux-gnu-gcc`).
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

## Known limitations

- **Scope:** only the L0 layer ships today. Coroutine objects with the token
  credential protocol, the M:N scheduler with work stealing and the IO loop,
  structured concurrency and the C++11 wrapper are follow-up milestones.
- **Architectures:** x86_64 and aarch64 are vendored and tested; the rest of
  the planned ISA matrix (i386, arm32/armeb, mips, ppc, s390x, riscv,
  big-endian variants) is not yet wired into the build.
- **Threading:** a `coro_ctx` is not thread-safe — it must be swapped from a
  single thread at a time (no internal locks by design at this layer).
- **Exceptions:** `coro_ctx_swap` must not be used to propagate C++
  exceptions across a context boundary.
- **Cross-build tests:** the Boost-coexistence test is only registered for
  native builds.

## License

`corocc` is released under the MIT License; see `LICENSE`. The vendored
Boost.Context assembly remains under BSL-1.0 with its original copyright
headers preserved — see `THIRD-PARTY.md`.
