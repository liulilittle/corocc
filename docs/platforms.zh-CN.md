# platforms — 支持矩阵、工具链与验证

> [English](platforms.md) | 中文

本文档是 corocc 的支持矩阵:操作系统、编译器、语言标准、全部架构目标、用于
运行交叉测试的模拟器站点、消毒器支持,以及已知边界。ISA 清单背后的二进制
布局保证见 [ABI.zh-CN.md](ABI.zh-CN.md);构建入口见
[building.zh-CN.md](building.zh-CN.md)。

## 受支持平台

| 维度 | 支持范围 |
| --- | --- |
| 操作系统 | Linux(glibc)、Windows(x64、x86 与 ARM64)、macOS(x86_64 与 Apple Silicon arm64)、Android(编译+链接;NDK r26+) |
| 编译器 | GCC 与 Clang(CI 与本机使用 Ubuntu 24.04 工具链——GCC 13 / Clang 18)、MSVC(Visual Studio 2022,`/W4 /WX`)、Apple clang(Xcode,CI macOS 运行器) |
| 库语言 | C99(头文件可作 C99 与 C++11 编译) |
| 封装语言 | C++11(经 C++20 前向验证;MSVC 基线 `/std:c++14`) |
| 构建系统 | CMake >= 3.16(Ninja 或 Make;Windows 可用 Visual Studio 生成器) |
| 运行时依赖 | 无。可选:Boost.Context(共存测试)、Boost.Asio(可选集成头) |
| 线程 | 原生 OS 线程(pthreads / Win32),无外部依赖 |

不包含 C++ 头的 C 翻译单元不受封装影响;不包含 Boost 头的程序不受集成影响。
详见 [coro_cpp.zh-CN.md](api/coro_cpp.zh-CN.md) 与
[boost.zh-CN.md](api/boost.zh-CN.md)。

## 架构目标

本机目标:

| 三元组 | ISA | 指针模型 | 验证 |
| --- | --- | --- | --- |
| `x86_64-unknown-linux-gnu` | x86-64 SysV ELF | LP64 | CI native 任务(GCC + Clang)且本机全量套件通过 |
| `x86_64-pc-windows-msvc` | x86-64 PE(MASM 后端) | LLP64 | CI Windows 任务且本机全量套件通过 |
| Windows x86(`windows-x86` 任务) | x86 PE(MASM 后端,ml.exe) | ILP32,`uint64_t` 8 字节对齐 | CI x86 任务(VS2022 Win32);32 位二进制在 x64 runner 上经 WoW64 原生运行,全量套件 |
| Windows ARM64(`windows-arm64` 任务) | ARM64 PE(armasm64 后端) | LLP64(与 64 位表布局一致) | CI ARM64 任务(VS2026,ARM64 runner);全量套件在 ARM64 主机原生运行 |
| macOS x86_64(`macos-x64` 任务) | x86-64 Mach-O | LP64 | CI macOS x64 任务(Apple clang,全量套件) |
| macOS arm64(`macos-arm64` 任务) | aarch64 Mach-O | LP64 | CI macOS arm64 任务(Apple clang,全量套件) |

交叉目标——每个都在 CI 中构建并在对应的 `qemu-user` 下执行测试套件;在全部
本机可用的这些工具链上还复跑了 ABI 探针(布局结果见
[ABI.zh-CN.md](ABI.zh-CN.md)):

| 三元组 | ISA / 端序 | 指针模型 | qemu 运行器 | 本机探针 |
| --- | --- | --- | --- | --- |
| `aarch64-unknown-linux-gnu` | aarch64 LE(AAPCS) | LP64 | `qemu-aarch64` | 是(另附全量套件) |
| `aarch64_be-unknown-linux-gnu` | aarch64 BE | LP64 | `qemu-aarch64_be` | 预构建工具链 |
| `arm-linux-gnueabihf` | armv7 LE(硬浮点) | ILP32 | `qemu-arm` | 是 |
| `arm-linux-gnueabi`(armel) | armv5t LE 软浮点(EABI) | ILP32 | `qemu-arm` | 是(另附全量套件) |
| `armeb-unknown-linux-gnueabi` | armv7 BE | ILP32 | `qemu-armeb` | 预构建工具链 |
| `i386-unknown-linux-gnu` | x86 32 位 | ILP32(`uint64_t` 4 字节对齐) | `qemu-i386` | 是(本机 `-m32`) |
| `mips-unknown-linux-gnu` | MIPS o32 BE | ILP32 | `qemu-mips` | 是 |
| `mipsel-unknown-linux-gnu` | MIPS o32 LE | ILP32 | `qemu-mipsel` | — |
| `mips64-unknown-linux-gnuabi64` | MIPS n64 BE | LP64 | `qemu-mips64` | — |
| `mips64el-unknown-linux-gnuabi64` | MIPS n64 LE | LP64 | `qemu-mips64el` | — |
| `mips64-unknown-linux-gnuabin32` | MIPS n32 BE | ILP32 | `qemu-mipsn32` | — |
| `mips64el-unknown-linux-gnuabin32` | MIPS n32 LE | ILP32 | `qemu-mipsn32el` | — |
| `powerpc-unknown-linux-gnu` | ppc32 BE(SysV) | ILP32 | `qemu-ppc` | — |
| `powerpc64-unknown-linux-gnu` | ppc64 BE(ELFv1) | LP64 | `qemu-ppc64` | — |
| `powerpc64le-unknown-linux-gnu` | ppc64 LE(ELFv2) | LP64 | `qemu-ppc64le` | 是 |
| `s390x-unknown-linux-gnu` | s390x BE | LP64 | `qemu-s390x` | 是(另附全量套件) |
| `riscv64-unknown-linux-gnu` | riscv64(RV64GC/LP64D) | LP64 | `qemu-riscv64` | 是 |
| `riscv32-unknown-linux-gnu` | riscv32(RV32GC/ILP32D) | ILP32 | `qemu-riscv32` | 预构建工具链 |
| `loongarch64-unknown-linux-gnu` | loongarch64 LE(LP64D) | LP64 | `qemu-loongarch64` | 是(另附全量套件;vendored 后端,Boost 1.92.0) |
| `sparc64-unknown-linux-gnu` | sparc64 BE(V9) | LP64 | `qemu-sparc64` | 是(另附全量套件;vendored 后端,Boost 1.92.0) |

"预构建工具链"行使用由 `scripts/ci/fetch-toolchain-assets.sh` 拉取并做
sha256 校验的**单树**工具链资产(CI 与本机同一流程):每个 triple 一个 tarball,
顶层为 `corocc-toolchain/<triple>/{bin,...,sysroot/}`,解包到 `/opt`。该资产的
"自足"仅指:`bin/<triple>-gcc` 携带自带 binutils,树内无指向树外的软链,
`-print-sysroot` 可重定位——即**消除原指向 `/usr/bin` 的绝对软链与对宿主交叉
编译器身份的依赖**;它**并未消除**对宿主 C 运行时的依赖:捆库后仍依赖宿主
glibc / `ld-linux`(故这些行保留 apt 运行时包)。全站来源表见
[toolchains.zh-CN.md](toolchains.zh-CN.md)。"本机探针"表示在本次
文档验证中还把 ABI 布局探针编译运行于该工具链之上;"另附全量套件"表示完整
CTest 套件也在本机经 `qemu-user` 跑过。32 位各行的 "ILP32" 默认指 `uint64_t`
8 字节对齐,i386 除外(4 字节对齐),细节以 [ABI.zh-CN.md](ABI.zh-CN.md)
为准。

上下文切换后端 vendored 于 `src/arch/`,由 `cmake/arch_select.cmake` 选择;
手写与衍生后端记录在 `THIRD-PARTY.md`。macOS 属本机构建,使用 Mach-O 后端
(`x86_64_sysv_macho_gas.S` / `arm64_aapcs_macho_gas.S`),宿主机为 Apple 时
经同一组启发式选择。

macOS 平台注记:Darwin 没有 `pthread_condattr_setclock`,条件变量垫片因此
运行在默认实时时钟上,并在每次调用时把单调时钟期限换算为实时时刻
(`ETIMEDOUT` 后重新武装,超时永不提前上报);匿名映射旗标使用 `MAP_ANON`
拼写;守护页溢出让进程以 `SIGBUS` 终止(Darwin 将 `PROT_NONE` 访问报告为
`KERN_PROTECTION_FAILURE`),溢出套件在 Linux 的 `SIGSEGV` 之外同时接受它。

## Android

- 经 `scripts/toolchains/android-ndk.cmake` 提供编译+链接支持(NDK r26+,
  `ANDROID_ABI` 选择目标,API level 24,静态 C++ STL)。
- CI 门禁全部四个 NDK ABI:`arm64-v8a`、`x86_64`、`armeabi-v7a` 与 `x86`
  (各一个构建任务);设备上执行不在模拟器矩阵内。
- 布局(经 NDK clang record-layout dump 实测):`arm64-v8a` 与 `x86_64` 为
  LP64,遵循 [ABI.zh-CN.md](ABI.zh-CN.md) 的 64 位表;`armeabi-v7a` 为
  ILP32、`uint64_t` 8 字节对齐(第一张 ILP32 表);`x86` 为 ILP32、
  `uint64_t` 4 字节对齐(i386 表)。

## 消毒器与插桩构建

| 构建 | 开关 | 范围 | 说明 |
| --- | --- | --- | --- |
| AddressSanitizer | `-DCOROCC_ASAN=ON` | 每次上下文切换前后的 fiber 切换钩子(`__sanitizer_start_switch_fiber`/`finish_switch_fiber`),含栈用后即返回检测。功能套件在消毒器下运行;三个误用检测套件(`coro_exactlyonce`、`coro_crossresume`、`conc_double_resume`)被排除,因为消毒器会拦截它们故意触发的 abort 路径;该配置也不注册性能套件。 | Linux CI 任务;构建选项同时支持 MSVC `/fsanitize=address`。 |
| ThreadSanitizer | `-DCOROCC_TSAN=ON` | 每次上下文切换前后的 fiber 切换注解(`__tsan_create_fiber`/`__tsan_switch_to_fiber`,见 `src/arch/coro_tsan.h`),使 TSan 可跟踪汇编切栈。全量测试套件在消毒器下无数据竞争运行。 | Linux CI 任务;gcc 对 `__atomic_thread_fence` 的 `-Wtsan` 警告被豁免(不作为错误)。 |
| 零自旋探针 | `-DCOROCC_PROBE_NOSPIN=ON` | 统计 compare-and-swap 尝试次数,使零自旋契约可被 `coro_nospin` 套件断言。 | Linux CI 任务。 |

ASan 与 TSan 不能在同一构建中共存;二者都在 CI 的 Linux x86-64 上构建并
运行套件。

## CI 管道

`ci` 工作流(见仓库 README 的徽章)在每次 push 与 pull request 上运行:

| 任务 | 门禁内容 |
| --- | --- |
| `native-gcc`、`native-clang` | 两种编译器下的 Linux x86-64 全量套件。 |
| `native-arm64` | ARM64 Linux 运行器上的全量套件。 |
| `asan`、`tsan`、`nospin` | 上表插桩配置,并断言注册/执行的测试数量下限。 |
| `headers` | 头文件一致性:公开头在严格告警下可独立按 C99 与 C++11 编译。 |
| `cross`(20 个任务) | 每个交叉 ABI 一个任务:交叉工具链构建 + 对应 `qemu-user` 下运行 CTest 套件。 |
| `windows-msvc` | Visual Studio 2022 x64 构建(Debug + Release)与 Release 测试套件。 |
| `windows-x86` | Visual Studio 2022 Win32(x86)构建(Debug + Release)与 Release 测试套件;32 位二进制在 x64 runner 上原生运行。 |
| `windows-arm64` | Visual Studio 2026 ARM64 构建(Debug + Release)与 Release 测试套件,运行于 `windows-11-arm` runner(ARM64 主机;2026-09 起默认 VS2026 镜像)。 |
| `macos-x64`、`macos-arm64` | macOS 原生构建(Apple clang)+ Release 全量测试套件,分别覆盖 Intel(`macos-15-intel`)与 Apple Silicon(`macos-15`)。 |
| `ndk`(2 个任务) | Android NDK 对 `arm64-v8a` 与 `x86_64` 的编译+链接。 |
| `libuv-example`、`libevent-example` | 独立 libuv/libevent 集成示例:配置、构建并运行各示例自带的 CTest 冒烟套件。 |
| `ci-summary` | 汇总各任务;除显式白名单外,被跳过(skipped)的任务会使汇总失败。 |

`perf-report` 在每次 push/PR 上运行(手动或定时触发也运行);report-only(非门禁),
产物为 `perf-report`,发布基准数字(CI 以 loose 门运行)。

## 已知边界

- **核心库没有异步 I/O。** 协程停放于定时器与同步原语,不能停放于文件/套接字
  就绪事件;桥接到事件循环的受支持方式是可选 Boost.Asio 集成
  ([boost.zh-CN.md](api/boost.zh-CN.md))。
- **Windows 测试范围。** 仅 POSIX 的 `conc_runtime_tsan` 套件不在 Windows
  注册,因此 Windows Release 套件为 43 个测试。Windows **Debug** 配置属于
  构建门禁;测试套件在 Release 运行。
- **Boost 共存测试**仅在本机构建注册(交叉构建会把宿主机的 Boost 目标文件
  链入异构架构二进制)。
- **s390x 协程内的 `thread_local`**:gcc 会把线程指针缓存进被调用者保存
  寄存器,而跨线程切换会恢复过期的值;s390x 工具链因此使用
  `-fno-stack-protector`。其他 ABI 不受影响;一般 `thread_local` 契约见
  [threading.zh-CN.md](threading.zh-CN.md)。
- **MIPS N32** 站点排除性能基准(模拟器对该 n32 的支持无法在时间预算内跑完
  基准);功能套件完整。
- **交叉二进制不可跨三元组互换**;每个三元组是独立的静态归档(见
  [building.zh-CN.md](building.zh-CN.md))。
- **Windows API 级别:** Win32 后端使用 SRW 锁与条件变量 API 族外加
  `InitOnceExecuteOnce`(自 Windows Vista 起可用);受验证目标是当前 Windows
  (CI 镜像为 Windows Server 2022)。没有旧版 Windows 目标。

## 在本机复现模拟器矩阵

每个交叉站点都走同一条两步流程(以单站点为例):

```sh
scripts/ci/fetch-toolchain-assets.sh riscv32-linux-gnu   # 仅资产行需要
scripts/build-all.sh --abi riscv32
```

`scripts/build-all.sh` 不带参数会走完 20 个站点的完整矩阵;
`scripts/build-all.sh --abi <名称>` 只跑一个站点。每个站点的日志落在
`scripts/README.md` 所述的日志目录下。
