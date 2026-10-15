# corocc build scripts

One-command entry points for building, testing and prebuilding `corocc` on
every supported host. All scripts are POSIX/bash and assume CMake plus a
working C/C++ toolchain; cross targets additionally need the corresponding
cross compiler and the QEMU user-mode emulator for that ISA.

| Script | Purpose |
| --- | --- |
| `build.sh` | Native configure + build + ctest + benchmark. Honors `CC`, `CXX`, `BUILD_DIR`, `CMAKE_ARGS`, `CMAKE_GENERATOR`. |
| `build-all.sh` | Builds and qemu-tests every cross ABI (17 targets) by delegating to `ci/build-test.sh`; supports `--abi <name>` for a single target. |
| `ci/build-test.sh` | Single-ABI driver shared with the M10 CI: configure, static build, qemu ctest and the registered/executed population-floor assertion (`--matrix-env`, see below). |
| `prebuild-all.sh` | Builds the supported variants and stages `dist/<triple>/{lib,include}` for drop-in consumption. |

## Usage

```sh
# native (gcc), then test
scripts/build.sh

# native with clang
CC=clang CXX=clang++ scripts/build.sh

# pass-through cmake options, custom build dir
scripts/build.sh -DCMAKE_BUILD_TYPE=Release
BUILD_DIR=build-rel scripts/build.sh

# every cross ABI (qemu runs the tests, population floors enforced)
scripts/build-all.sh

# a single ABI (same code path as the CI jobs)
scripts/build-all.sh --abi riscv32
scripts/ci/build-test.sh riscv32 \
  --toolchain scripts/toolchains/riscv32.cmake --matrix-env cross

# stage dist/<triple>/{lib,include} for distribution
scripts/prebuild-all.sh
```

Logs are written to `.opencode/logs/` (or `$LOG_DIR`); the per-ABI runner
writes `<abi>-{configure,build,ctest}.log` under `.opencode/logs/ci/`.

## `ci/build-test.sh` presets (`--matrix-env`)

Double column = registered/executed.

| Preset | Population floor | Notes |
| --- | --- | --- |
| `cross` | 34/34 or 44/43 | 44/43 is the pre-guard reality (C++ suites registered unconditionally); 34/34 is the post-guard target. `-E 'perf\|bench'`. |
| `native` | 44/43 or 45/44 | 45/44 when the Boost coexist suite is present. |
| `nospin` | 45\|46 / 44\|45 | `COROCC_PROBE_NOSPIN=ON` (+1). |
| `asan` | 43\|44 / 40\|41 | `COROCC_ASAN=ON`; the three misuse-detection tests are excluded (the sanitizer intercepts their abort path); perf suites are not registered. |
| `tsan` | 1/1 | `COROCC_TSAN=ON` plus `-Wno-error=tsan`; runs `ctest -R tsan` under `setarch -R` (TSan ASLR memory mapping on kernels >= 6.5; avoids the root-only sysctl workaround). |
| `windows` | 42\|43 / 42 | MSVC Release; `cpp_perf` excluded. |

## Toolchain files (`scripts/toolchains/`)

| File | Target triple | QEMU runner |
| --- | --- | --- |
| `aarch64.cmake` | `aarch64-linux-gnu` | `qemu-aarch64` (canonical file: `toolchain/aarch64-linux-gnu.cmake`) |
| `aarch64_be.cmake` | `aarch64_be-linux-gnu` (W4/W5-built toolchain + sysroot) | `qemu-aarch64_be` |
| `i386.cmake` | host `gcc`/`g++` with `-m32` | `qemu-i386` |
| `arm32.cmake` | `arm-linux-gnueabihf` | `qemu-arm` |
| `armeb.cmake` | `armeb-linux-gnueabi` (W4/W5-built toolchain + sysroot) | `qemu-armeb` |
| `mips32.cmake` | `mips-linux-gnu` | `qemu-mips` |
| `mips32el.cmake` | `mipsel-linux-gnu` | `qemu-mipsel` |
| `mips64.cmake` | `mips64-linux-gnuabi64` (`-gcc-13`) | `qemu-mips64` |
| `mips64el.cmake` | `mips64el-linux-gnuabi64` (`-gcc-13`) | `qemu-mips64el` |
| `mipsn32.cmake` | `mips64-linux-gnuabi64` with `-mabi=n32` | `qemu-mipsn32` |
| `mipsn32el.cmake` | `mips64el-linux-gnuabi64` with `-mabi=n32` | `qemu-mipsn32el` |
| `ppc32.cmake` | `powerpc-linux-gnu` | `qemu-ppc` |
| `ppc64.cmake` | `powerpc64-linux-gnu` | `qemu-ppc64` |
| `ppc64le.cmake` | `powerpc64le-linux-gnu` | `qemu-ppc64le` |
| `s390x.cmake` | `s390x-linux-gnu` | `qemu-s390x` |
| `riscv64.cmake` | `riscv64-linux-gnu` | `qemu-riscv64` |
| `riscv32.cmake` | `riscv32-linux-gnu` (W4/W5-built toolchain + sysroot) | `qemu-riscv32` |
| `android-ndk.cmake` | NDK r26+, `ANDROID_ABI` | on device / emulator |

## Backend status

All 17 context-switch backends are vendored under `src/arch/` and wired into
the build; `scripts/build-all.sh` builds and runs each of them under the
matching qemu-user emulator with the population-floor assertion enforced
(C and C++ suites alike). Windows (MSVC x64, PE/MASM backend) is covered by
native Windows builds.

- `mips32_n32_elf_gas.S` (MIPS N32, both endiannesses) is hand-written for
  corocc - a derivative of the o32/n64 structure (see `THIRD-PARTY.md`).
- `riscv32_sysv_elf_gas.S` (RV32GC/ILP32D) is a derivative adaptation of the
  vendored riscv64 backend (see `THIRD-PARTY.md`).
- `android-ndk.cmake` covers NDK compile-only builds; it is outside the qemu
  runtime matrix (no population floor yet).

## Consuming a `prebuild-all.sh` artifact

```sh
cc your_app.c -Idist/x86_64-unknown-linux-gnu/include \
              dist/x86_64-unknown-linux-gnu/lib/libcorocc.a
```
