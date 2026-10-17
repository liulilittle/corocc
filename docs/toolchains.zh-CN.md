# toolchains — 全站工具链来源

> [English](toolchains.md) | 中文

本表记录 corocc 每个 CI/release 站点的编译器与 sysroot 来源:runner 镜像、
apt 交叉包、经 sha256 校验的预编译 `toolchain-v1` 资产、Xcode / Visual Studio /
Android NDK。每行的 `消费位置` 指向确切的 workflow / 矩阵行(行号在生成时计算)。
表中也列出两个没有 `ci/site-matrix.env` 行的 ci.yml 任务(`headers`、`perf-report`),
其工具链同为 runner 镜像。

本表由 `scripts/ci/gen-toolchain-table.sh` 从仓库权威文件生成(改 `ci/toolchain-sources.env`
与相关 workflow 后重跑生成器;**不要手改本文件**)。生成器同时校验本表站点集合等于
`ci.yml`(job、cross、ndk 矩阵)与 `ci/site-matrix.env` 的并集,不一致即 exit 1;
该脚本**不接入 CI**。

## 全站工具链来源表

| 站点 | triple | 工具链来源 | 资产名 + sha256(仅 v1) | 消费位置 |
| --- | --- | --- | --- | --- |
| `cross-aarch64` | `aarch64-unknown-linux-gnu` | apt 交叉包:gcc-aarch64-linux-gnu g++-aarch64-linux-gnu | — | ci.yml:211, release.yml:26, site-matrix.env:30 |
| `cross-armhf` | `arm-linux-gnueabihf` | apt 交叉包:gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf | — | ci.yml:212, release.yml:27, site-matrix.env:31 |
| `cross-armel` | `arm-linux-gnueabi` | apt 交叉包:gcc-13-arm-linux-gnueabi g++-13-arm-linux-gnueabi libc6-dev-armel-cross | — | ci.yml:213, release.yml:28, site-matrix.env:32 |
| `cross-i386` | `i386-unknown-linux-gnu` | 宿主 gcc/g++ -m32(apt:libc6-dev-i386 lib32gcc-13-dev lib32stdc++-13-dev) | — | ci.yml:214, release.yml:29, site-matrix.env:33 |
| `cross-mips_o32` | `mips-unknown-linux-gnu` | apt 交叉包:gcc-mips-linux-gnu g++-mips-linux-gnu | — | ci.yml:215, release.yml:30, site-matrix.env:34 |
| `cross-mipsel_o32` | `mipsel-unknown-linux-gnu` | apt 交叉包:gcc-mipsel-linux-gnu g++-mipsel-linux-gnu | — | ci.yml:216, release.yml:31, site-matrix.env:35 |
| `cross-mips64_n64` | `mips64-unknown-linux-gnuabi64` | apt 交叉包:gcc-13-mips64-linux-gnuabi64 g++-13-mips64-linux-gnuabi64 | — | ci.yml:217, release.yml:32, site-matrix.env:36 |
| `cross-mips64el_n64` | `mips64el-unknown-linux-gnuabi64` | apt 交叉包:gcc-13-mips64el-linux-gnuabi64 g++-13-mips64el-linux-gnuabi64 | — | ci.yml:218, release.yml:33, site-matrix.env:37 |
| `cross-ppc32` | `powerpc-unknown-linux-gnu` | apt 交叉包:gcc-powerpc-linux-gnu g++-powerpc-linux-gnu | — | ci.yml:219, release.yml:34, site-matrix.env:38 |
| `cross-ppc64` | `powerpc64-unknown-linux-gnu` | apt 交叉包:gcc-powerpc64-linux-gnu g++-powerpc64-linux-gnu | — | ci.yml:220, release.yml:35, site-matrix.env:39 |
| `cross-ppc64le` | `powerpc64le-unknown-linux-gnu` | apt 交叉包:gcc-powerpc64le-linux-gnu g++-powerpc64le-linux-gnu | — | ci.yml:221, release.yml:36, site-matrix.env:40 |
| `cross-s390x` | `s390x-unknown-linux-gnu` | apt 交叉包:gcc-s390x-linux-gnu g++-s390x-linux-gnu | — | ci.yml:222, release.yml:37, site-matrix.env:41 |
| `cross-riscv64` | `riscv64-unknown-linux-gnu` | apt 交叉包:gcc-riscv64-linux-gnu g++-riscv64-linux-gnu | — | ci.yml:223, release.yml:38, site-matrix.env:42 |
| `cross-loongarch64` | `loongarch64-unknown-linux-gnu` | apt 交叉包:gcc-13-loongarch64-linux-gnu g++-13-loongarch64-linux-gnu libc6-dev-loong64-cross | — | ci.yml:224, release.yml:39, site-matrix.env:43 |
| `cross-sparc64` | `sparc64-unknown-linux-gnu` | apt 交叉包:gcc-13-sparc64-linux-gnu g++-13-sparc64-linux-gnu libc6-dev-sparc64-cross | — | ci.yml:225, release.yml:40, site-matrix.env:44 |
| `cross-aarch64_be` | `aarch64_be-unknown-linux-gnu` | 预编译 toolchain-v1 资产(gcc 13.3.0 + glibc 2.39)+ apt binutils:gcc-aarch64-linux-gnu | corocc-toolchain-aarch64_be-linux-gnu-gcc13.3.0-glibc2.39.tar.gz `948dc8a3fe6b6c2ac190f9519d4213e24c9688e07fc7aaad1b469bf31bd6c9fd`<br>corocc-sysroot-aarch64_be-linux-gnu-glibc2.39.tar.gz `c180c00cdcbbd76b522551163e57fde575628c1ad81e96a075d54d0dd74e198b` | ci.yml:229, release.yml:43, site-matrix.env:46 |
| `cross-armeb` | `armeb-unknown-linux-gnueabi` | 预编译 toolchain-v1 资产(gcc 13.3.0 + glibc 2.39)+ apt binutils:gcc-arm-linux-gnueabi g++-arm-linux-gnueabi | corocc-toolchain-armeb-linux-gnueabi-gcc13.3.0-glibc2.39.tar.gz `51159f7f78d566e718e5b5fef7c62465bc426619dd75d8c62b647a3c2aec5dbe`<br>corocc-sysroot-armeb-linux-gnueabi-glibc2.39.tar.gz `06ad1445643ebd37fbc9fbebab1eee59781c1597a60cbf8afa507fb624f2322d` | ci.yml:230, release.yml:44, site-matrix.env:47 |
| `cross-riscv32` | `riscv32-unknown-linux-gnu` | 预编译 toolchain-v1 资产(gcc 13.3.0 + glibc 2.39)+ apt binutils:gcc-riscv64-linux-gnu g++-riscv64-linux-gnu | corocc-toolchain-riscv32-linux-gnu-gcc13.3.0-glibc2.39.tar.gz `570b27094990f9b4b237794a9477f2e1c82f999dd74ed705e8219bf33de3acb6`<br>corocc-sysroot-riscv32-linux-gnu-glibc2.39.tar.gz `0f578639f7089c15ba5299b85a6b198787c6761417c0bfd0382e752fb4ef4337` | ci.yml:231, release.yml:45, site-matrix.env:48 |
| `cross-mipsn32` | `mips64-unknown-linux-gnuabin32` | apt 交叉包:gcc-13-mips64-linux-gnuabi64 g++-13-mips64-linux-gnuabi64 gcc-13-multilib-mips64-linux-gnuabi64 g++-13-multilib-mips64-linux-gnuabi64 libc6-dev-mipsn32-mips64-cross libn32stdc++-13-dev-mips64-cross | — | ci.yml:233, release.yml:47, site-matrix.env:50 |
| `cross-mipsn32el` | `mips64el-unknown-linux-gnuabin32` | apt 交叉包:gcc-13-mips64el-linux-gnuabi64 g++-13-mips64el-linux-gnuabi64 gcc-13-multilib-mips64el-linux-gnuabi64 g++-13-multilib-mips64el-linux-gnuabi64 libc6-dev-mipsn32-mips64el-cross libn32stdc++-13-dev-mips64el-cross | — | ci.yml:234, release.yml:48, site-matrix.env:51 |
| `native-gcc` | `x86_64-unknown-linux-gnu` | runner 镜像工具链(Ubuntu 24.04;GCC 13 / Clang 18) | — | ci.yml:29, site-matrix.env:24 |
| `native-clang` | `x86_64-unknown-linux-gnu` | runner 镜像工具链(Ubuntu 24.04;GCC 13 / Clang 18) | — | ci.yml:42, site-matrix.env:25 |
| `native-arm64` | `aarch64-unknown-linux-gnu` | runner 镜像工具链(Ubuntu 24.04;GCC 13 / Clang 18) | — | ci.yml:56, site-matrix.env:26 |
| `nospin` | `x86_64-unknown-linux-gnu` | runner 镜像工具链(Ubuntu 24.04;GCC 13 / Clang 18) | — | ci.yml:94, site-matrix.env:27 |
| `asan` | `x86_64-unknown-linux-gnu` | runner 镜像工具链(Ubuntu 24.04;GCC 13 / Clang 18) | — | ci.yml:69, site-matrix.env:28 |
| `tsan` | `x86_64-unknown-linux-gnu` | runner 镜像工具链(Ubuntu 24.04;GCC 13 / Clang 18) | — | ci.yml:78, site-matrix.env:29 |
| `headers` | `x86_64-unknown-linux-gnu` | runner 镜像工具链(Ubuntu 24.04;GCC 13 / Clang 18) | — | ci.yml:102 |
| `perf-report` | `x86_64-unknown-linux-gnu` | runner 镜像工具链(Ubuntu 24.04;GCC 13 / Clang 18) | — | ci.yml:383 |
| `boost-asio` | `x86_64-unknown-linux-gnu` | runner 镜像工具链(Ubuntu 24.04;GCC 13 / Clang 18) + apt:libboost-dev clang | — | ci.yml:109, site-matrix.env:52 |
| `asio-host-example` | `x86_64-unknown-linux-gnu` | runner 镜像工具链(Ubuntu 24.04;GCC 13 / Clang 18) + apt:libboost-dev ninja-build | — | ci.yml:136, site-matrix.env:62 |
| `driver-pump-example` | `x86_64-unknown-linux-gnu` | runner 镜像工具链(Ubuntu 24.04;GCC 13 / Clang 18) + apt:libboost-dev ninja-build | — | ci.yml:152, site-matrix.env:63 |
| `libuv-example` | `x86_64-unknown-linux-gnu` | runner 镜像工具链(Ubuntu 24.04;GCC 13 / Clang 18) + apt:libuv1-dev ninja-build | — | ci.yml:168, site-matrix.env:64 |
| `libevent-example` | `x86_64-unknown-linux-gnu` | runner 镜像工具链(Ubuntu 24.04;GCC 13 / Clang 18) + apt:libevent-dev ninja-build | — | ci.yml:185, site-matrix.env:65 |
| `windows-msvc` | `x86_64-pc-windows-msvc` | Visual Studio 2022(MSVC x64 / Win32) | — | ci.yml:261, release.yml:69, site-matrix.env:55 |
| `windows-x86` | `i686-pc-windows-msvc` | Visual Studio 2022(MSVC x64 / Win32) | — | ci.yml:323, site-matrix.env:56 |
| `windows-arm64` | `aarch64-pc-windows-msvc` | Visual Studio 2026(MSVC / armasm64,ARM64) | — | ci.yml:342, site-matrix.env:57 |
| `macos-x64` | `x86_64-apple-darwin` | Xcode clang(macOS runner 镜像) | — | ci.yml:305, site-matrix.env:53 |
| `macos-arm64` | `arm64-apple-darwin` | Xcode clang(macOS runner 镜像) | — | ci.yml:314, site-matrix.env:54 |
| `ndk-arm64-v8a` | `aarch64-linux-android` | Android NDK r26+ clang(预装;ANDROID_ABI 选择目标) | — | ci.yml:369, site-matrix.env:58 |
| `ndk-x86_64` | `x86_64-linux-android` | Android NDK r26+ clang(预装;ANDROID_ABI 选择目标) | — | ci.yml:369, site-matrix.env:59 |
| `ndk-armeabi-v7a` | `armv7a-linux-androideabi` | Android NDK r26+ clang(预装;ANDROID_ABI 选择目标) | — | ci.yml:369, site-matrix.env:60 |
| `ndk-x86` | `i686-linux-android` | Android NDK r26+ clang(预装;ANDROID_ABI 选择目标) | — | ci.yml:369, site-matrix.env:61 |

