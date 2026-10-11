# corocc build scripts

One-command entry points for building, testing and prebuilding `corocc` on
every planned host. All scripts are POSIX/bash and assume CMake plus a
working C toolchain; cross targets additionally need the corresponding
cross compiler and `qemu-<isa>` from `qemu-user`.

| Script | Purpose |
| --- | --- |
| `build.sh` | Native configure + build + ctest + benchmark. Honors `CC`, `CXX`, `BUILD_DIR`, `CMAKE_ARGS`, `CMAKE_GENERATOR`. |
| `build-all.sh` | Loops over every cross target: builds and tests the ones with a vendored backend, validates the toolchain files of the others. |
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

# every cross ISA that has a vendored backend (qemu runs the tests)
scripts/build-all.sh

# stage dist/<triple>/{lib,include} for distribution
scripts/prebuild-all.sh
```

Logs are written to `.opencode/logs/` (or `$LOG_DIR`).

## Toolchain files (`scripts/toolchains/`)

| File | Target triple | QEMU runner |
| --- | --- | --- |
| `aarch64.cmake` | `aarch64-linux-gnu` | `qemu-aarch64` (canonical file: `toolchain/aarch64-linux-gnu.cmake`) |
| `arm32.cmake` | `arm-linux-gnueabihf` | `qemu-arm` |
| `armeb.cmake` | `arm-linux-gnueabi` | `qemu-armeb` |
| `mips32.cmake` | `mips-linux-gnu` | `qemu-mips` |
| `mips32el.cmake` | `mipsel-linux-gnu` | `qemu-mipsel` |
| `ppc32.cmake` | `powerpc-linux-gnu` | `qemu-ppc` |
| `ppc64.cmake` | `powerpc64-linux-gnu` | `qemu-ppc64` |
| `ppc64le.cmake` | `powerpc64le-linux-gnu` | `qemu-ppc64le` |
| `s390x.cmake` | `s390x-linux-gnu` | `qemu-s390x` |
| `riscv64.cmake` | `riscv64-linux-gnu` | `qemu-riscv64` |
| `android-ndk.cmake` | NDK r26+, `ANDROID_ABI` | on device / emulator |

## Backend status (what `build-all.sh` reports today)

| ISA | Backend vendored | `build-all.sh` result |
| --- | --- | --- |
| x86_64 (SysV ELF) | yes | built and tested natively |
| aarch64 (AAPCS) | yes | built and tested under `qemu-aarch64` |
| arm32 / armeb | pending (milestone M9) | toolchain validated, backend skipped |
| mips32 / mips32el | pending (milestone M9) | toolchain validated, backend skipped |
| ppc32 / ppc64 / ppc64le | pending (milestone M9) | toolchain validated, backend skipped |
| s390x | pending (milestone M9) | toolchain validated, backend skipped |
| riscv64 | pending (milestone M9) | toolchain validated, backend skipped |
| Windows MSVC / MinGW | pending (PE/MASM backend, milestone M9) | not offered as a toolchain file: the vendored assembly is ELF/SysV, and a Windows build needs the PE backend first |
| Android NDK | toolchain only | build after the `arm64-v8a`/`armeabi-v7a` backend lands |

`mips64-n64` and `riscv32` have neither an upstream assembly backend nor a
Debian cross toolchain in the reference image and are therefore not listed.

## Consuming a `prebuild-all.sh` artifact

```sh
cc your_app.c -Idist/x86_64-unknown-linux-gnu/include \
              dist/x86_64-unknown-linux-gnu/lib/libcorocc.a
```
