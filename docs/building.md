# Building and integrating corocc

> [中文](building.zh-CN.md) | English

This document covers every supported way to build and consume corocc: from
source (as a subproject or through `FetchContent`), from the prebuilt release
archives, for cross targets, and for Android. The supported platform matrix
is in [platforms.md](platforms.md); the binary contract is in
[ABI.md](ABI.md).

## Requirements

- CMake >= 3.16 and Ninja or Make (or the Visual Studio generator on
  Windows).
- A C99 compiler; for the C++ wrapper and the C++ test suites a C++11
  compiler (GCC, Clang or MSVC).
- No other dependencies. Optional: Boost.Context for the coexistence test,
  Boost.Asio for the optional integration header.

## From source

### Build and test (native)

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The test-count formula (plain Linux 43, `+1` with Boost.Context, `+1` in a
Release build, `-1` on Windows) is detailed in the repository README.

### Build and test (macOS)

macOS x86_64 and Apple Silicon arm64 are native targets: the Mach-O
context-switch backends are selected automatically from the host
architecture, so no toolchain file is needed (Xcode Command Line Tools
provide the compiler and SDK).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build -E cpp_perf --output-on-failure
```

`-E cpp_perf` mirrors the CI macOS jobs and every other gated site: the
throughput gate is a Linux-site concern on shared runners; the functional
population (43 tests) runs in full. macOS specifics that the build handles
for you: the condition-variable shim converts its monotonic deadlines to the
platform's realtime cond clock, `MAP_ANON` is used for stack mappings, and
guard-page overflow terminates with `SIGBUS`, which the overflow suite
accepts.

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `COROCC_BUILD_TESTS` | `ON` | Register the test suites (`enable_testing`). Turn `OFF` when embedding corocc as a subproject. |
| `COROCC_ASAN` | `OFF` | Build with AddressSanitizer and the fiber-switch hooks (see [platforms.md](platforms.md)). |
| `COROCC_TSAN` | `OFF` | Build with ThreadSanitizer (runtime delivery queue, identity registry and atomics; see [platforms.md](platforms.md)). |
| `COROCC_PROBE_NOSPIN` | `OFF` | Count compare-and-swap attempts so the zero-spin contract can be asserted. |
| `COROCC_LTCG` | `ON` | MSVC Release builds use `/GL` + `/LTCG` (the measured delivery configuration). Turn `OFF` for a plain `/O2` build. |

All first-party code is compiled warning-clean under `-Wall -Wextra -Werror`
(`/W4 /WX /utf-8` on MSVC); a warning is a build failure by design.

### AddressSanitizer / ThreadSanitizer builds

```sh
cmake -S . -B build-asan -G Ninja -DCOROCC_ASAN=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -G Ninja -DCOROCC_TSAN=ON
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure -R tsan
```

## CMake integration (from source)

### `add_subdirectory`

```cmake
cmake_minimum_required(VERSION 3.16)
project(myapp C)

set(COROCC_BUILD_TESTS OFF CACHE BOOL "" FORCE)   # do not register corocc's suites
add_subdirectory(third_party/corocc)              # path to the checkout

add_executable(myapp main.c)
target_link_libraries(myapp PRIVATE corocc)
```

The `corocc` target carries its include path, its per-platform link
interface (for example `atomic` on 32-bit builds without an inline 64-bit
CAS) and, on MSVC, the `COROCC_BUILDING` definition that makes the static
library link without `__imp_` indirection.

### `FetchContent`

```cmake
include(FetchContent)

set(COROCC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(corocc
  GIT_REPOSITORY https://github.com/liulilittle/corocc.git
  GIT_TAG        main)          # or a release tag, e.g. v0.1.0
FetchContent_MakeAvailable(corocc)

add_executable(myapp main.c)
target_link_libraries(myapp PRIVATE corocc)
```

`FetchContent_MakeAvailable` requires CMake 3.14+; the project itself
requires 3.16.

## Prebuilt archives (zero-build consumption)

Each tagged release attaches per-triple archives built by the release
workflow, plus a flat `SHA256SUMS` file. Linux archives are named
`corocc-<tag>-<triple>.tar.gz`; the Windows archive is
`corocc-<tag>-x86_64-pc-windows-msvc.zip`. Package layout (tar top level
`<triple>/`):

```
<Triple>/
  lib/libcorocc.a
  include/corocc/*.h|.hpp          public headers (including the C++ wrapper)
  lib/cmake/corocc/corocc-config.cmake
  lib/cmake/corocc/corocc-config-version.cmake
  examples/cmake-consumer/         minimal find_package consumer
  README.txt                       tag, triple, commit, compiler, date
```

Verify and unpack:

```sh
sha256sum -c SHA256SUMS --ignore-missing
tar xzf corocc-<tag>-aarch64-unknown-linux-gnu.tar.gz
```

Consume with a bare compiler:

```sh
cc your_app.c -Iaarch64-unknown-linux-gnu/include \
              aarch64-unknown-linux-gnu/lib/libcorocc.a
```

Consume with CMake through the bundled config package:

```sh
cmake -S aarch64-unknown-linux-gnu/examples/cmake-consumer -B build \
      -DCMAKE_PREFIX_PATH="$PWD/aarch64-unknown-linux-gnu"
cmake --build build
```

or from your own `CMakeLists.txt`:

```cmake
find_package(corocc 1.0 REQUIRED CONFIG)
target_link_libraries(myapp PRIVATE corocc::corocc)
```

The published Linux triples are `x86_64-unknown-linux-gnu`,
`aarch64-unknown-linux-gnu`, `arm-linux-gnueabihf`, `i386-unknown-linux-gnu`,
`mips-unknown-linux-gnu`, `mipsel-unknown-linux-gnu`,
`mips64-unknown-linux-gnuabi64`, `mips64el-unknown-linux-gnuabi64`,
`mips64-unknown-linux-gnuabin32`, `mips64el-unknown-linux-gnuabin32`,
`powerpc-unknown-linux-gnu`, `powerpc64-unknown-linux-gnu`,
`powerpc64le-unknown-linux-gnu`, `s390x-unknown-linux-gnu`,
`aarch64_be-unknown-linux-gnu`, `armeb-unknown-linux-gnueabi`,
`riscv64-unknown-linux-gnu`, `riscv32-unknown-linux-gnu`,
`loongarch64-unknown-linux-gnu` and `sparc64-unknown-linux-gnu`, plus the
Windows `x86_64-pc-windows-msvc`. Each triple is its own ABI package:
archives are not interchangeable across triples.

`scripts/prebuild-all.sh` produces an equivalent local layout under `dist/`.

## Cross builds (qemu)

One canonical cross flow; every supported target follows the same shape with
its own toolchain file (the per-ABI table is in [platforms.md](platforms.md)):

```sh
cmake -S . -B build-arm64 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=toolchain/aarch64-linux-gnu.cmake
cmake --build build-arm64
ctest --test-dir build-arm64 --output-on-failure
```

The toolchain files set `CMAKE_CROSSCOMPILING_EMULATOR`, so CTest runs the
binaries under the matching `qemu-user` automatically. The full matrix in
one command (logs under the log directory described in
`scripts/README.md`):

```sh
scripts/build-all.sh              # all 17 cross targets
scripts/build-all.sh --abi riscv32
```

## Android

```sh
export ANDROID_NDK=/path/to/android-ndk-r26
cmake -S . -B build-ndk -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=scripts/toolchains/android-ndk.cmake \
      -DANDROID_ABI=arm64-v8a
cmake --build build-ndk
```

`ANDROID_ABI` accepts the standard NDK ABIs (`arm64-v8a`, `x86_64`, …); the
toolchain file targets API level 24 with the static C++ STL. Cross
compilation and linking are supported; running on a device or emulator is a
consumer-side step (see [ABI.md](ABI.md) for the NDK notes).

## Benchmarks

Release builds register the performance gates; run them directly:

```sh
./build/tests/bench_switch          # L0 ping-pong context switch
./build/tests/bench_switch_pure     # pure runtime-switch throughput (8 coroutines)
./build/tests/bench_rt_mn           # composite spawn+yield workload
./build/tests/bench_coro            # L1 coroutine lifecycle
```

Reference figures and the frozen units are documented next to each
benchmark and summarised in the README.

## Logs and per-site records

`scripts/ci/build-test.sh <site>` writes `<site>-{configure,build,ctest}.log`
and a `<site>-versions.txt` toolchain record under `ci-logs/<site>/` by
default (`LOG_DIR` overrides the base directory). This is the same driver
the CI jobs use, so a local run and a CI run produce the same artefacts.
