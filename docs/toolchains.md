# toolchains — per-site toolchain sources

> [中文](toolchains.zh-CN.md) | English

This table records, for every corocc CI/release site, where its compiler and
sysroot come from: the runner image, apt cross packages, the sha256-verified
prebuilt `toolchain-v1` assets, Xcode / Visual Studio / the Android NDK. The
`consumer` column cites the exact workflow / matrix lines (line numbers are
computed at generation time). It also carries the two ci.yml jobs that have no
`ci/site-matrix.env` row (`headers`, `perf-report`), whose toolchain is the
runner image like the native sites.

It is generated from the repository's authoritative files by
`scripts/ci/gen-toolchain-table.sh` (edit `ci/toolchain-sources.env` and the
referenced workflows, then re-run the generator; do not hand-edit this file).
The generator verifies that the site set here equals the union of the `ci.yml`
jobs / `cross` / `ndk` matrices and the `ci/site-matrix.env` rows; a mismatch
makes it exit 1. It is not wired into CI.

## Per-site toolchain sources

| site | triple | toolchain source | asset + sha256 (v1 rows only) | consumer |
| --- | --- | --- | --- | --- |
| `cross-aarch64` | `aarch64-unknown-linux-gnu` | apt cross packages: gcc-aarch64-linux-gnu g++-aarch64-linux-gnu | — | ci.yml:211, release.yml:26, site-matrix.env:30 |
| `cross-armhf` | `arm-linux-gnueabihf` | apt cross packages: gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf | — | ci.yml:212, release.yml:27, site-matrix.env:31 |
| `cross-armel` | `arm-linux-gnueabi` | apt cross packages: gcc-13-arm-linux-gnueabi g++-13-arm-linux-gnueabi libc6-dev-armel-cross | — | ci.yml:213, release.yml:28, site-matrix.env:32 |
| `cross-i386` | `i386-unknown-linux-gnu` | host gcc/g++ -m32 (apt: libc6-dev-i386 lib32gcc-13-dev lib32stdc++-13-dev) | — | ci.yml:214, release.yml:29, site-matrix.env:33 |
| `cross-mips_o32` | `mips-unknown-linux-gnu` | apt cross packages: gcc-mips-linux-gnu g++-mips-linux-gnu | — | ci.yml:215, release.yml:30, site-matrix.env:34 |
| `cross-mipsel_o32` | `mipsel-unknown-linux-gnu` | apt cross packages: gcc-mipsel-linux-gnu g++-mipsel-linux-gnu | — | ci.yml:216, release.yml:31, site-matrix.env:35 |
| `cross-mips64_n64` | `mips64-unknown-linux-gnuabi64` | apt cross packages: gcc-13-mips64-linux-gnuabi64 g++-13-mips64-linux-gnuabi64 | — | ci.yml:217, release.yml:32, site-matrix.env:36 |
| `cross-mips64el_n64` | `mips64el-unknown-linux-gnuabi64` | apt cross packages: gcc-13-mips64el-linux-gnuabi64 g++-13-mips64el-linux-gnuabi64 | — | ci.yml:218, release.yml:33, site-matrix.env:37 |
| `cross-ppc32` | `powerpc-unknown-linux-gnu` | apt cross packages: gcc-powerpc-linux-gnu g++-powerpc-linux-gnu | — | ci.yml:219, release.yml:34, site-matrix.env:38 |
| `cross-ppc64` | `powerpc64-unknown-linux-gnu` | apt cross packages: gcc-powerpc64-linux-gnu g++-powerpc64-linux-gnu | — | ci.yml:220, release.yml:35, site-matrix.env:39 |
| `cross-ppc64le` | `powerpc64le-unknown-linux-gnu` | apt cross packages: gcc-powerpc64le-linux-gnu g++-powerpc64le-linux-gnu | — | ci.yml:221, release.yml:36, site-matrix.env:40 |
| `cross-s390x` | `s390x-unknown-linux-gnu` | apt cross packages: gcc-s390x-linux-gnu g++-s390x-linux-gnu | — | ci.yml:222, release.yml:37, site-matrix.env:41 |
| `cross-riscv64` | `riscv64-unknown-linux-gnu` | apt cross packages: gcc-riscv64-linux-gnu g++-riscv64-linux-gnu | — | ci.yml:223, release.yml:38, site-matrix.env:42 |
| `cross-loongarch64` | `loongarch64-unknown-linux-gnu` | apt cross packages: gcc-13-loongarch64-linux-gnu g++-13-loongarch64-linux-gnu libc6-dev-loong64-cross | — | ci.yml:224, release.yml:39, site-matrix.env:43 |
| `cross-sparc64` | `sparc64-unknown-linux-gnu` | apt cross packages: gcc-13-sparc64-linux-gnu g++-13-sparc64-linux-gnu libc6-dev-sparc64-cross | — | ci.yml:225, release.yml:40, site-matrix.env:44 |
| `cross-aarch64_be` | `aarch64_be-unknown-linux-gnu` | prebuilt single-tree toolchain asset (gcc/binutils/glibc versions carried in the asset name; Ubuntu 24.04) + apt runtime packages: gcc-aarch64-linux-gnu | corocc-toolchain-aarch64_be-linux-gnu-gcc13.3.0-binutils2.42-glibc2.39-ubuntu24.04.tar.gz `7f5b96320477ed9dea494ac35fb51b95db3d39e7ed14245cc3e36157526b7b90` | ci.yml:229, release.yml:43, site-matrix.env:46 |
| `cross-armeb` | `armeb-unknown-linux-gnueabi` | prebuilt single-tree toolchain asset (gcc/binutils/glibc versions carried in the asset name; Ubuntu 24.04) + apt runtime packages: gcc-arm-linux-gnueabi g++-arm-linux-gnueabi | corocc-toolchain-armeb-linux-gnueabi-gcc13.3.0-binutils2.42-glibc2.39-ubuntu24.04.tar.gz `16f6a9850fb34245dc8ff343758d116f03fbf28aed9a6ae5d6f33b42246caa5f` | ci.yml:230, release.yml:44, site-matrix.env:47 |
| `cross-riscv32` | `riscv32-unknown-linux-gnu` | prebuilt single-tree toolchain asset (gcc/binutils/glibc versions carried in the asset name; Ubuntu 24.04) + apt runtime packages: gcc-riscv64-linux-gnu g++-riscv64-linux-gnu | corocc-toolchain-riscv32-linux-gnu-gcc13.3.0-binutils2.42-glibc2.39-ubuntu24.04.tar.gz `caca2037f21c3a74ff1d686d54d3631004466234517e5043dd0f162976cfc550` | ci.yml:231, release.yml:45, site-matrix.env:48 |
| `cross-mipsn32` | `mips64-unknown-linux-gnuabin32` | apt cross packages: gcc-13-mips64-linux-gnuabi64 g++-13-mips64-linux-gnuabi64 gcc-13-multilib-mips64-linux-gnuabi64 g++-13-multilib-mips64-linux-gnuabi64 libc6-dev-mipsn32-mips64-cross libn32stdc++-13-dev-mips64-cross | — | ci.yml:233, release.yml:47, site-matrix.env:50 |
| `cross-mipsn32el` | `mips64el-unknown-linux-gnuabin32` | apt cross packages: gcc-13-mips64el-linux-gnuabi64 g++-13-mips64el-linux-gnuabi64 gcc-13-multilib-mips64el-linux-gnuabi64 g++-13-multilib-mips64el-linux-gnuabi64 libc6-dev-mipsn32-mips64el-cross libn32stdc++-13-dev-mips64el-cross | — | ci.yml:234, release.yml:48, site-matrix.env:51 |
| `native-gcc` | `x86_64-unknown-linux-gnu` | runner image toolchain (Ubuntu 24.04; GCC 13 / Clang 18) | — | ci.yml:29, site-matrix.env:24 |
| `native-clang` | `x86_64-unknown-linux-gnu` | runner image toolchain (Ubuntu 24.04; GCC 13 / Clang 18) | — | ci.yml:42, site-matrix.env:25 |
| `native-arm64` | `aarch64-unknown-linux-gnu` | runner image toolchain (Ubuntu 24.04; GCC 13 / Clang 18) | — | ci.yml:56, site-matrix.env:26 |
| `nospin` | `x86_64-unknown-linux-gnu` | runner image toolchain (Ubuntu 24.04; GCC 13 / Clang 18) | — | ci.yml:94, site-matrix.env:27 |
| `asan` | `x86_64-unknown-linux-gnu` | runner image toolchain (Ubuntu 24.04; GCC 13 / Clang 18) | — | ci.yml:69, site-matrix.env:28 |
| `tsan` | `x86_64-unknown-linux-gnu` | runner image toolchain (Ubuntu 24.04; GCC 13 / Clang 18) | — | ci.yml:78, site-matrix.env:29 |
| `headers` | `x86_64-unknown-linux-gnu` | runner image toolchain (Ubuntu 24.04; GCC 13 / Clang 18) | — | ci.yml:102 |
| `perf-report` | `x86_64-unknown-linux-gnu` | runner image toolchain (Ubuntu 24.04; GCC 13 / Clang 18) | — | ci.yml:383 |
| `boost-asio` | `x86_64-unknown-linux-gnu` | runner image toolchain (Ubuntu 24.04; GCC 13 / Clang 18) + apt: libboost-dev clang | — | ci.yml:109, site-matrix.env:52 |
| `asio-host-example` | `x86_64-unknown-linux-gnu` | runner image toolchain (Ubuntu 24.04; GCC 13 / Clang 18) + apt: libboost-dev ninja-build | — | ci.yml:136, site-matrix.env:62 |
| `driver-pump-example` | `x86_64-unknown-linux-gnu` | runner image toolchain (Ubuntu 24.04; GCC 13 / Clang 18) + apt: libboost-dev ninja-build | — | ci.yml:152, site-matrix.env:63 |
| `libuv-example` | `x86_64-unknown-linux-gnu` | runner image toolchain (Ubuntu 24.04; GCC 13 / Clang 18) + apt: libuv1-dev ninja-build | — | ci.yml:168, site-matrix.env:64 |
| `libevent-example` | `x86_64-unknown-linux-gnu` | runner image toolchain (Ubuntu 24.04; GCC 13 / Clang 18) + apt: libevent-dev ninja-build | — | ci.yml:185, site-matrix.env:65 |
| `windows-msvc` | `x86_64-pc-windows-msvc` | Visual Studio 2022 (MSVC x64 / Win32) | — | ci.yml:261, release.yml:69, site-matrix.env:55 |
| `windows-x86` | `i686-pc-windows-msvc` | Visual Studio 2022 (MSVC x64 / Win32) | — | ci.yml:323, site-matrix.env:56 |
| `windows-arm64` | `aarch64-pc-windows-msvc` | Visual Studio 2026 (MSVC / armasm64, ARM64) | — | ci.yml:342, site-matrix.env:57 |
| `macos-x64` | `x86_64-apple-darwin` | Xcode clang (macOS runner image) | — | ci.yml:305, site-matrix.env:53 |
| `macos-arm64` | `arm64-apple-darwin` | Xcode clang (macOS runner image) | — | ci.yml:314, site-matrix.env:54 |
| `ndk-arm64-v8a` | `aarch64-linux-android` | Android NDK r26+ clang (preinstalled; ANDROID_ABI selects the target) | — | ci.yml:369, site-matrix.env:58 |
| `ndk-x86_64` | `x86_64-linux-android` | Android NDK r26+ clang (preinstalled; ANDROID_ABI selects the target) | — | ci.yml:369, site-matrix.env:59 |
| `ndk-armeabi-v7a` | `armv7a-linux-androideabi` | Android NDK r26+ clang (preinstalled; ANDROID_ABI selects the target) | — | ci.yml:369, site-matrix.env:60 |
| `ndk-x86` | `i686-linux-android` | Android NDK r26+ clang (preinstalled; ANDROID_ABI selects the target) | — | ci.yml:369, site-matrix.env:61 |

