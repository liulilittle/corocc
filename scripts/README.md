# corocc build scripts

One-command entry points for building, testing and prebuilding `corocc` on
every supported host. All scripts are POSIX/bash and assume CMake plus a
working C/C++ toolchain; cross targets additionally need the corresponding
cross compiler and the QEMU user-mode emulator for that ISA.

| Script | Purpose |
| --- | --- |
| `build.sh` | Native configure + build + ctest + benchmark. Honors `CC`, `CXX`, `BUILD_DIR`, `CMAKE_ARGS`, `CMAKE_GENERATOR`. |
| `build-all.sh` | Builds and qemu-tests every cross ABI (19 targets) by delegating to `ci/build-test.sh`; supports `--abi <name>` for a single target. |
| `ci/build-test.sh` | Single-site driver shared with the GitHub Actions CI: configure, static build, qemu ctest and the registered/executed population-floor assertion (`--matrix-env`, see below). |
| `prebuild-all.sh` | Builds the supported variants and stages `dist/<triple>/{lib,include}` for drop-in consumption. |
| `ci-local.sh` | Local mirror of the CI pipeline: workflow YAML parse/lint, shellcheck, header conformance, native + cross sites, optional toolchain-asset rehearsal and a release-publish smoke test. |
| `ci/fetch-toolchain-assets.sh` | Fetches, sha256-verifies and unpacks the prebuilt toolchain + sysroot assets for the big-endian and riscv32 targets. |

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

`ci/build-test.sh` writes `<site>-{configure,build,ctest}.log` and a
`<site>-versions.txt` toolchain record under `$LOG_DIR` (default
`ci-logs/<site>/`). `build-all.sh` sets `$LOG_DIR` itself (default
`<repo>/ci-logs`, a repository-local, untracked log directory) and passes it
to the driver, so its per-ABI logs land flat as `ci-logs/<site>-*.log`; set
`LOG_DIR` to choose the base directory.

## `ci/build-test.sh` presets (`--matrix-env`)

Each preset asserts the test population the site must show: the accepted
registered count (raw `ctest -N`) and executed count (the same `-E` filter
the run uses; `cpp_perf` is always excluded from the executed count). A
`|`-separated pair means "either value": the larger one accounts for the
Boost coexistence suite when Boost is installed on the host.

| Preset | Registered (accepted) | Executed (accepted) | Notes |
| --- | --- | --- | --- |
| `cross` | 34 or 44 | 34 or 43 | C-only cross sites; 44/43 when the C++ suites are registered. |
| `native` | 44 or 45 | 43 or 44 | 45/44 when the Boost coexist suite is present. |
| `nospin` | 45 or 46 | 44 or 45 | `COROCC_PROBE_NOSPIN=ON` (+1). |
| `asan` | 43 or 44 | 40 or 41 | `COROCC_ASAN=ON`; the three misuse-detection suites (`coro_exactlyonce`, `coro_crossresume`, `conc_double_resume`) are excluded because the sanitizer intercepts their abort path; perf suites are not registered. |
| `tsan` | 1 | 1 | `COROCC_TSAN=ON` plus `-Wno-error=tsan` (gcc's `-Wtsan` warning on `__atomic_thread_fence` is not an error); runs only the `-R tsan` suite (`conc_runtime_tsan`). |
| `windows` | 42 | 42 | MSVC Release; `cpp_perf` excluded. |

Without `--matrix-env`, the floors come from `ci/site-matrix.env` (the same
table the CI uses); both columns are lower bounds there, so a host carrying
extra suites such as Boost coexistence still passes.

## Toolchain files (`scripts/toolchains/`)

| File | Target triple | QEMU runner |
| --- | --- | --- |
| `aarch64.cmake` | `aarch64-linux-gnu` | `qemu-aarch64` (canonical file: `toolchain/aarch64-linux-gnu.cmake`) |
| `aarch64_be.cmake` | `aarch64_be-linux-gnu` (prebuilt toolchain + sysroot assets) | `qemu-aarch64_be` |
| `i386.cmake` | host `gcc`/`g++` with `-m32` | `qemu-i386` |
| `arm32.cmake` | `arm-linux-gnueabihf` | `qemu-arm` |
| `armeb.cmake` | `armeb-linux-gnueabi` (prebuilt toolchain + sysroot assets) | `qemu-armeb` |
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
| `riscv32.cmake` | `riscv32-linux-gnu` (prebuilt toolchain + sysroot assets) | `qemu-riscv32` |
| `loongarch64.cmake` | `loongarch64-linux-gnu` (`-gcc-13`) | `qemu-loongarch64` |
| `sparc64.cmake` | `sparc64-linux-gnu` (`-gcc-13`) | `qemu-sparc64` |
| `android-ndk.cmake` | NDK r26+, `ANDROID_ABI` | on device / emulator |

The prebuilt-asset rows (aarch64_be, armeb, riscv32) are installed by
`scripts/ci/fetch-toolchain-assets.sh`, which downloads the asset archives,
verifies them against `ci/toolchain-assets.sha256` and unpacks them under
`/opt` (`corocc-toolchain/`, `corocc-sysroot/`).

## Backend notes

All 19 cross-target context-switch backends are vendored under `src/arch/` and
wired into the build; `scripts/build-all.sh` builds and runs each of them under
the matching qemu-user emulator with the population-floor assertion enforced
(C and C++ suites alike). Windows (MSVC x64, PE/MASM backend) is covered by
native Windows builds; macOS x86_64 and arm64 (Mach-O backends) are covered by
native macOS builds -- both without toolchain files.

- `mips32_n32_elf_gas.S` (MIPS N32, both endiannesses) is hand-written for
  corocc -- a derivative of the o32/n64 structure (see `THIRD-PARTY.md`).
- `riscv32_sysv_elf_gas.S` (RV32GC/ILP32D) is a derivative adaptation of the
  vendored riscv64 backend (see `THIRD-PARTY.md`).
- `android-ndk.cmake` covers NDK compile-and-link builds; on-device runs are
  outside the qemu runtime matrix.

## Consuming a `prebuild-all.sh` artifact

```sh
cc your_app.c -Idist/x86_64-unknown-linux-gnu/include \
              dist/x86_64-unknown-linux-gnu/lib/libcorocc.a
```
