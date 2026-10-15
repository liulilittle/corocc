# 构建与集成 corocc

> [English](building.md) | 中文

本文档覆盖 corocc 的全部受支持构建与消费方式:从源码构建(作为子项目或经
`FetchContent`)、消费预构建发布归档、交叉目标构建,以及 Android。受支持
平台矩阵见 [platforms.zh-CN.md](platforms.zh-CN.md);二进制契约见
[ABI.zh-CN.md](ABI.zh-CN.md)。

## 环境要求

- CMake >= 3.16 与 Ninja 或 Make(Windows 上也可用 Visual Studio 生成器)。
- C99 编译器;C++ 封装与 C++ 测试套件需要 C++11 编译器(GCC、Clang 或
  MSVC)。
- 无其他依赖。可选:Boost.Context(共存测试)、Boost.Asio(可选集成头)。

## 从源码构建

### 构建与测试(本机)

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

测试数量公式(普通 Linux 43,安装 Boost.Context 后 `+1`,Release 构建 `+1`,
Windows `-1`)详见仓库 README。

### 构建选项

| 选项 | 默认 | 作用 |
| --- | --- | --- |
| `COROCC_BUILD_TESTS` | `ON` | 注册测试套件(`enable_testing`)。把 corocc 作为子项目内嵌时设为 `OFF`。 |
| `COROCC_ASAN` | `OFF` | 使用 AddressSanitizer 与 fiber 切换钩子构建(见 [platforms.zh-CN.md](platforms.zh-CN.md))。 |
| `COROCC_TSAN` | `OFF` | 使用 ThreadSanitizer 构建(运行时投递队列、身份注册表与原子操作;见 [platforms.zh-CN.md](platforms.zh-CN.md))。 |
| `COROCC_PROBE_NOSPIN` | `OFF` | 统计 compare-and-swap 尝试次数,使零自旋契约可被断言。 |
| `COROCC_LTCG` | `ON` | MSVC Release 构建使用 `/GL` + `/LTCG`(实测的交付配置)。纯 `/O2` 构建可设为 `OFF`。 |

全部自有代码在 `-Wall -Wextra -Werror`(MSVC 下 `/W4 /WX /utf-8`)下零告警
编译;告警即构建失败,这是设计使然。

### AddressSanitizer / ThreadSanitizer 构建

```sh
cmake -S . -B build-asan -G Ninja -DCOROCC_ASAN=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -G Ninja -DCOROCC_TSAN=ON
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure -R tsan
```

## CMake 集成(源码方式)

### `add_subdirectory`

```cmake
cmake_minimum_required(VERSION 3.16)
project(myapp C)

set(COROCC_BUILD_TESTS OFF CACHE BOOL "" FORCE)   # 不注册 corocc 的测试套件
add_subdirectory(third_party/corocc)              # 指向检出目录

add_executable(myapp main.c)
target_link_libraries(myapp PRIVATE corocc)
```

`corocc` target 自带包含路径、按平台的链接接口(例如没有内联 64 位 CAS 的
32 位构建上的 `atomic`),以及 MSVC 上使静态库不残留 `__imp_` 间接的
`COROCC_BUILDING` 定义。

### `FetchContent`

```cmake
include(FetchContent)

set(COROCC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(corocc
  GIT_REPOSITORY https://github.com/liulilittle/corocc.git
  GIT_TAG        main)          # 或某个发布 tag,例如 v0.1.0
FetchContent_MakeAvailable(corocc)

add_executable(myapp main.c)
target_link_libraries(myapp PRIVATE corocc)
```

`FetchContent_MakeAvailable` 需要 CMake 3.14+;项目本身需要 3.16。

## 预构建归档(零构建引入)

每个 tag 发布由 release 工作流附上按三元组打包的归档,以及扁平的
`SHA256SUMS` 文件。Linux 归档命名 `corocc-<tag>-<triple>.tar.gz`;Windows
归档为 `corocc-<tag>-x86_64-pc-windows-msvc.zip`。包布局(tar 顶层为
`<triple>/`):

```
<triple>/
  lib/libcorocc.a
  include/corocc/*.h|.hpp          公共头(含 C++ 封装)
  lib/cmake/corocc/corocc-config.cmake
  lib/cmake/corocc/corocc-config-version.cmake
  examples/cmake-consumer/         最小 find_package 消费示例
  README.txt                       tag、三元组、commit、编译器、日期
```

校验并解包:

```sh
sha256sum -c SHA256SUMS --ignore-missing
tar xzf corocc-<tag>-aarch64-unknown-linux-gnu.tar.gz
```

用裸编译器消费:

```sh
cc your_app.c -Iaarch64-unknown-linux-gnu/include \
              aarch64-unknown-linux-gnu/lib/libcorocc.a
```

经捆绑的 config 包用 CMake 消费:

```sh
cmake -S aarch64-unknown-linux-gnu/examples/cmake-consumer -B build \
      -DCMAKE_PREFIX_PATH="$PWD/aarch64-unknown-linux-gnu"
cmake --build build
```

或在你的 `CMakeLists.txt` 中:

```cmake
find_package(corocc 1.0 REQUIRED CONFIG)
target_link_libraries(myapp PRIVATE corocc::corocc)
```

发布工作流覆盖的 Linux 三元组:`x86_64-unknown-linux-gnu`、
`aarch64-unknown-linux-gnu`、`arm-linux-gnueabihf`、
`i386-unknown-linux-gnu`、`mips-unknown-linux-gnu`、
`mipsel-unknown-linux-gnu`、`mips64-unknown-linux-gnuabi64`、
`mips64el-unknown-linux-gnuabi64`、`mips64-unknown-linux-gnuabin32`、
`mips64el-unknown-linux-gnuabin32`、`powerpc-unknown-linux-gnu`、
`powerpc64-unknown-linux-gnu`、`powerpc64le-unknown-linux-gnu`、
`s390x-unknown-linux-gnu`、`aarch64_be-unknown-linux-gnu`、
`armeb-unknown-linux-gnueabi`、`riscv64-unknown-linux-gnu`、
`riscv32-unknown-linux-gnu`,另加 Windows 的 `x86_64-pc-windows-msvc`。
每个三元组都是独立的 ABI 包:归档不可跨三元组互换。

`scripts/prebuild-all.sh` 可在本地生成同构布局(输出到 `dist/`)。

## 交叉构建(qemu)

一条标准交叉流程;每个受支持目标用各自的工具链文件走同样形状(按 ABI 的
工具链表见 [platforms.zh-CN.md](platforms.zh-CN.md)):

```sh
cmake -S . -B build-arm64 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=toolchain/aarch64-linux-gnu.cmake
cmake --build build-arm64
ctest --test-dir build-arm64 --output-on-failure
```

工具链文件设置了 `CMAKE_CROSSCOMPILING_EMULATOR`,CTest 会自动在对应
`qemu-user` 下运行二进制。一条命令走完整矩阵(日志落在
`scripts/README.md` 所述的日志目录):

```sh
scripts/build-all.sh              # 全部 17 个交叉目标
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

`ANDROID_ABI` 接受标准 NDK ABI(`arm64-v8a`、`x86_64` 等);工具链文件以
API level 24 与静态 C++ STL 为目标。交叉编译与链接受支持;在设备或模拟器上
运行属于消费方步骤(NDK 注记见 [ABI.zh-CN.md](ABI.zh-CN.md))。

## 基准

Release 构建注册性能门禁;直接运行:

```sh
./build/tests/bench_switch          # L0 双向上下文切换
./build/tests/bench_switch_pure     # 纯运行时切换吞吐(8 协程)
./build/tests/bench_rt_mn           # 复合 spawn+yield 负载
./build/tests/bench_coro            # L1 协程生命周期
```

参考值与冻结的口径记录在每个基准旁,概览见 README。

## 日志与站点记录

`scripts/ci/build-test.sh <site>` 默认把 `<site>-{configure,build,ctest}.log`
与工具链记录 `<site>-versions.txt` 写入 `ci-logs/<site>/`(`LOG_DIR` 可覆盖
基目录)。这就是 CI 任务使用的同一驱动,因此本机运行与 CI 运行产出同样的
制品。
