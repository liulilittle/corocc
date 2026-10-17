# Platforms — supported matrix, toolchains, verification

> [中文](platforms.zh-CN.md) | English

This document is the support matrix of corocc: operating systems, compilers,
language standards, every architecture target, the emulator sites used to
run cross tests, sanitizer support, and the known boundaries. The binary
layout guarantees behind the ISA list live in [ABI.md](ABI.md); the build
entry points live in [building.md](building.md).

## Supported platforms

| Axis | Supported |
| --- | --- |
| Operating systems | Linux (glibc), Windows (x64, x86 and ARM64), macOS (x86_64 and Apple Silicon arm64), Android (compile and link; NDK r26+) |
| Compilers | GCC and Clang (the Ubuntu 24.04 toolchains — GCC 13 / Clang 18 — in CI and local runs), MSVC (Visual Studio 2022, `/W4 /WX`), Apple clang (Xcode, CI macOS runners) |
| Library language | C99 (headers compile as C99 and C++11) |
| Wrapper language | C++11 (verified through C++20 as a forward check; MSVC baseline `/std:c++14`) |
| Build system | CMake >= 3.16 (Ninja or Make; Visual Studio generator on Windows) |
| Runtime dependencies | None. Optional: Boost.Context (coexistence test), Boost.Asio (optional integration header) |
| Threading | Native OS threads (pthreads / Win32), no external dependency |

A C translation unit that never includes the C++ header is unaffected by the
wrapper; a program that never includes the Boost header is unaffected by the
integration. See [coro_cpp.md](api/coro_cpp.md) and
[boost.md](api/boost.md).

## Architecture targets

Native targets:

| Triple | ISA | Pointer model | Verification |
| --- | --- | --- | --- |
| `x86_64-unknown-linux-gnu` | x86-64 SysV ELF | LP64 | CI native jobs (GCC + Clang) and full suite locally |
| `x86_64-pc-windows-msvc` | x86-64 PE (MASM backend) | LLP64 | CI Windows job and full suite locally |
| Windows x86 (`windows-x86` job) | x86 PE (MASM backend, ml.exe) | ILP32, 8-byte `uint64_t` | CI x86 job (VS2022 Win32); the 32-bit binaries run natively on the x64 runner through WoW64, full suite |
| Windows ARM64 (`windows-arm64` job) | ARM64 PE (armasm64 backend) | LLP64 (layout-identical to the 64-bit table) | CI ARM64 job (VS2026, ARM64 runner); full suite runs natively on the ARM64 host |
| macOS x86_64 (`macos-x64` job) | x86-64 Mach-O | LP64 | CI macOS x64 job (Apple clang, full suite) |
| macOS arm64 (`macos-arm64` job) | aarch64 Mach-O | LP64 | CI macOS arm64 job (Apple clang, full suite) |

Cross targets — each one is built and its test suite executed under
`qemu-user` in CI, and the ABI probe re-runs on every one of these toolchains
available locally (see [ABI.md](ABI.md) for the layout results):

| Triple | ISA / endianness | Pointer model | qemu runner | Local probe |
| --- | --- | --- | --- | --- |
| `aarch64-unknown-linux-gnu` | aarch64 LE (AAPCS) | LP64 | `qemu-aarch64` | yes (plus full suite) |
| `aarch64_be-unknown-linux-gnu` | aarch64 BE | LP64 | `qemu-aarch64_be` | prebuilt toolchain |
| `arm-linux-gnueabihf` | armv7 LE (hard-float) | ILP32 | `qemu-arm` | yes |
| `arm-linux-gnueabi` (armel) | armv5t LE soft-float (EABI) | ILP32 | `qemu-arm` | yes (plus full suite) |
| `armeb-unknown-linux-gnueabi` | armv7 BE | ILP32 | `qemu-armeb` | prebuilt toolchain |
| `i386-unknown-linux-gnu` | x86 32-bit | ILP32 (4-byte `uint64_t`) | `qemu-i386` | yes (host `-m32`) |
| `mips-unknown-linux-gnu` | MIPS o32 BE | ILP32 | `qemu-mips` | yes |
| `mipsel-unknown-linux-gnu` | MIPS o32 LE | ILP32 | `qemu-mipsel` | — |
| `mips64-unknown-linux-gnuabi64` | MIPS n64 BE | LP64 | `qemu-mips64` | — |
| `mips64el-unknown-linux-gnuabi64` | MIPS n64 LE | LP64 | `qemu-mips64el` | — |
| `mips64-unknown-linux-gnuabin32` | MIPS n32 BE | ILP32 | `qemu-mipsn32` | — |
| `mips64el-unknown-linux-gnuabin32` | MIPS n32 LE | ILP32 | `qemu-mipsn32el` | — |
| `powerpc-unknown-linux-gnu` | ppc32 BE (SysV) | ILP32 | `qemu-ppc` | — |
| `powerpc64-unknown-linux-gnu` | ppc64 BE (ELFv1) | LP64 | `qemu-ppc64` | — |
| `powerpc64le-unknown-linux-gnu` | ppc64 LE (ELFv2) | LP64 | `qemu-ppc64le` | yes |
| `s390x-unknown-linux-gnu` | s390x BE | LP64 | `qemu-s390x` | yes (plus full suite) |
| `riscv64-unknown-linux-gnu` | riscv64 (RV64GC/LP64D) | LP64 | `qemu-riscv64` | yes |
| `riscv32-unknown-linux-gnu` | riscv32 (RV32GC/ILP32D) | ILP32 | `qemu-riscv32` | prebuilt toolchain |
| `loongarch64-unknown-linux-gnu` | loongarch64 LE (LP64D) | LP64 | `qemu-loongarch64` | yes (plus full suite; vendored backend, Boost 1.92.0) |
| `sparc64-unknown-linux-gnu` | sparc64 BE (V9) | LP64 | `qemu-sparc64` | yes (plus full suite; vendored backend, Boost 1.92.0) |

"Prebuilt toolchain" rows use the sha256-verified toolchain/sysroot assets
fetched by `scripts/ci/fetch-toolchain-assets.sh` (same flow in CI and
locally). "Local probe" means the ABI layout probe was additionally compiled
and run on that toolchain during the documentation verification pass;
"plus full suite" means the complete CTest suite also ran locally under
`qemu-user`. For the 32-bit rows, "ILP32" means 8-byte `uint64_t` alignment
except i386 (4-byte), exactly as detailed in [ABI.md](ABI.md).

The context-switch backends are vendored under `src/arch/` and selected by
`cmake/arch_select.cmake`; the hand-written and derivative backends are
documented in `THIRD-PARTY.md`. macOS is a native build using the Mach-O
backends (`x86_64_sysv_macho_gas.S` / `arm64_aapcs_macho_gas.S`), selected by
the same heuristics when the host is Apple.

macOS platform notes: `pthread_condattr_setclock` does not exist on Darwin,
so the condition-variable shim runs on the default realtime clock and
converts its monotonic deadlines per call (re-arming after `ETIMEDOUT`, so a
timeout is never reported early); `MAP_ANON` is used as the spelling of the
anonymous mapping flag; and a guard-page overflow terminates with `SIGBUS`
(Darwin reports `PROT_NONE` accesses as `KERN_PROTECTION_FAILURE`), which
the overflow suite accepts alongside Linux's `SIGSEGV`.

## Android

- Compile-and-link support through `scripts/toolchains/android-ndk.cmake`
  (NDK r26+, `ANDROID_ABI` selects the target, API level 24, static C++ STL).
- CI gates all four NDK ABIs: `arm64-v8a`, `x86_64`, `armeabi-v7a` and
  `x86` (one build job each); on-device execution is outside the emulator
  matrix.
- Layouts (measured with the NDK clang record-layout dump): `arm64-v8a` and
  `x86_64` are LP64 and follow the 64-bit layout table in
  [ABI.md](ABI.md); `armeabi-v7a` is ILP32 with 8-byte `uint64_t` (the
  first ILP32 table); `x86` is ILP32 with 4-byte `uint64_t` (the i386
  table).

## Sanitizers and instrumented builds

| Build | Switch | Scope | Notes |
| --- | --- | --- | --- |
| AddressSanitizer | `-DCOROCC_ASAN=ON` | Fiber-switch hooks (`__sanitizer_start_switch_fiber`/`finish_switch_fiber`) around every context switch, including stack-use-after-return detection. The functional suite runs under the sanitizer; the three misuse-detection suites (`coro_exactlyonce`, `coro_crossresume`, `conc_double_resume`) are excluded because the sanitizer intercepts their deliberate abort path, and the performance suites are not registered in this configuration. | Linux CI job; MSVC `/fsanitize=address` is also supported by the build option. |
| ThreadSanitizer | `-DCOROCC_TSAN=ON` | The runtime's delivery queue, identity registry and atomics, exercised by the `conc_runtime_tsan` suite. | TSan cannot follow an assembly stack switch, so the drive path itself is not instrumentable; see [coro.md](api/coro.md) for how the verification is split. Linux CI job; gcc's `-Wtsan` on `__atomic_thread_fence` is waived (not an error). |
| Zero-spin probe | `-DCOROCC_PROBE_NOSPIN=ON` | Counts compare-and-swap attempts so the zero-spin contract can be asserted by the `coro_nospin` suite. | Linux CI job. |

ASan and TSan are mutually exclusive in one build; both build and run the
suite on Linux x86-64 in CI.

## CI pipeline

The `ci` workflow (see the badge in the repository README) runs, on every
push and pull request:

| Job | What it gates |
| --- | --- |
| `native-gcc`, `native-clang` | Full suite on Linux x86-64 with both compilers. |
| `native-arm64` | Full suite on an ARM64 Linux runner. |
| `asan`, `tsan`, `nospin` | The instrumented configurations above, with registered/executed test-population floors asserted. |
| `headers` | Header conformance: public headers compile standalone as C99 and C++11 under strict warnings. |
| `cross` (20 jobs) | One job per cross ABI: build with the cross toolchain and run the CTest suite under the matching `qemu-user`. |
| `windows-msvc` | Visual Studio 2022 x64 build (Debug + Release) and the Release test suite. |
| `windows-x86` | Visual Studio 2022 Win32 (x86) build (Debug + Release) and the Release test suite; the 32-bit binaries run natively on the x64 runner. |
| `windows-arm64` | Visual Studio 2026 ARM64 build (Debug + Release) and the Release test suite on the `windows-11-arm` runner (ARM64 host, VS2026 image since the 2026-09 migration). |
| `macos-x64`, `macos-arm64` | Native macOS builds (Apple clang) and the full Release test suite on Intel (`macos-15-intel`) and Apple Silicon (`macos-15`). |
| `ndk` (2 jobs) | Android NDK compile+link for `arm64-v8a` and `x86_64`. |
| `libuv-example`, `libevent-example` | Stand-alone libuv/libevent integration examples: configure, build and run each example's own CTest smoke suite. |
| `ci-summary` | Aggregates the jobs; a skipped job fails the summary unless explicitly whitelisted. |

`perf-report` runs on every push and pull request (and on manual or scheduled
runs). It is report-only (never gates) and publishes the benchmark numbers as
the `perf-report` artifact (CI runs the gates in loose mode).

## Known boundaries

- **No asynchronous I/O in the core.** Coroutines park on timers and
  synchronization primitives, not on file or socket readiness; the optional
  Boost.Asio integration ([boost.md](api/boost.md)) is the supported way to
  bridge to an event loop.
- **Windows test scope.** The POSIX-only `conc_runtime_tsan` suite is not
  registered on Windows, so the Windows Release suite is 43 tests. The
  Windows **Debug** configuration is part of the build gate; the test suite
  runs in Release.
- **Boost coexistence test** is registered for native builds only (a cross
  build would link host Boost objects into a foreign-architecture binary).
- **`thread_local` inside coroutines on s390x**: gcc caches the thread
  pointer in a callee-saved register that a cross-thread switch would
  restore stale; the s390x toolchain therefore compiles with
  `-fno-stack-protector`. Other ABIs are unaffected; the general
  `thread_local` contract is in [threading.md](threading.md).
- **MIPS N32** sites exclude the performance benchmarks (the emulator's n32
  support does not run them within the time budget); the functional suite is
  complete.
- **Cross binaries are not interchangeable** across triples; each triple is
  its own static archive (see [building.md](building.md)).
- **Windows API level:** the Win32 backend uses the SRW-lock and
  condition-variable API family plus `InitOnceExecuteOnce` (available since
  Windows Vista); the verified target is current Windows (the CI image is
  Windows Server 2022). There is no legacy-Windows target.

## Reproducing the emulator matrix locally

Every cross site follows the same two-step flow (one site shown):

```sh
scripts/ci/fetch-toolchain-assets.sh riscv32-linux-gnu   # only for the asset rows
scripts/build-all.sh --abi riscv32
```

`scripts/build-all.sh` with no arguments walks the entire 20-site matrix;
`scripts/build-all.sh --abi <name>` runs one site. The per-site logs land
under the log directory described in `scripts/README.md`.
