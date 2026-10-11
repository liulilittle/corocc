# corocc

> [English](README.md) | 中文

`corocc` 是一个零依赖的 C/C++ 协同程序(coroutine)库,只做**有栈协程**
(stackful):每个协程拥有自己的、带保护页的栈,可任意次数挂起与恢复。工程
标准对标顶级 C/C++ 系统库:小而稳的 ABI、文档齐全的公共头、守护页把栈溢出
变成硬错误而非静默内存破坏,并且全部自有代码在 `-Wall -Wextra -Werror`
(MSVC 下 `/W4 /WX`) 下零警告。

## 当前进度

里程碑 1(骨架 + L0 执行上下文)已完成:

| 领域 | 状态 |
| --- | --- |
| 分层 | L0 上下文原语已实现(`coro_stack`、`coro_ctx`);L1 协程对象(含 token 凭证协议)、L2 运行时内核(M:N 工作窃取调度、同步原语、IO 事件循环)、L3 结构化并发、C++11 高级封装为后续里程碑 |
| 架构 | x86_64(SysV)与 aarch64(AAPCS)汇编已 vendored;aarch64 经 `qemu-aarch64` 验证 |
| 保护页 | 每个协程栈映射一个 `PROT_NONE` 守护页,溢出即触发缺页而不是破坏相邻内存 |
| 消毒器 | ASan fiber-switch 钩子(`COROCC_ASAN`);ASan 下全部测试通过,含启用栈用后即返回检测 |
| 与 Boost 共存 | 重命名符号后的汇编可与真实 Boost.Context 同进程链接(`boost_coexist` 测试) |
| 工具链 | GCC / Clang / MSVC,以及基于 CMake toolchain 文件的交叉编译 |

## 环境要求

- CMake >= 3.16,Ninja 或 Make,支持 C99 与 C++11 的编译器。
- 在 x86_64 主机上运行 aarch64 二进制需要 `qemu-aarch64` 与 aarch64 交叉
  工具链 + sysroot(`aarch64-linux-gnu-gcc`)。
- 可选:Boost.Context 头文件与库(仅用于共存测试)。
- 无其他依赖。

## 目录结构

```
include/corocc/    公共头(C99、C++11 兼容):coro_status.h、
                   coro_stack.h、coro_ctx.h
src/coroutine/     实现:status.c、stack.c、ctx.c
src/arch/          vendored Boost.Context 汇编(符号已重命名)
toolchain/         交叉编译 toolchain 文件
tests/             单元测试、Boost 共存测试、性能基准
```

## 构建与测试

### 本机构建(x86_64)

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

预期:`8/8 tests passed` —— `smoke`、`stack`、`ctx`、`ctx_return`、
`ctx_pingpong`、`ctx_overflow`、`ctx_asan`,以及 `boost_coexist`
(仅在安装 Boost.Context 时注册)。

### AddressSanitizer 构建

```sh
cmake -S . -B build-asan -G Ninja -DCOROCC_ASAN=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

### aarch64 交叉构建(qemu 下运行)

```sh
cmake -S . -B build-arm64 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=toolchain/aarch64-linux-gnu.cmake
cmake --build build-arm64
ctest --test-dir build-arm64 --output-on-failure
```

### 性能基准

```sh
./build/tests/bench_switch
# 例:switches=10000000 ns/switch=11.09
```

基准在两个上下文之间往返(每次循环 2 次切换),输出每次切换的纳秒数。项目
Linux CI 主机上的参考值:x86_64 原生(GCC、Ninja)约 **10-11 ns/切换**;
aarch64 在 `qemu-aarch64` 用户态模拟下约 **50-60 ns/切换**(含模拟开销,
该数值随主机负载波动)。

## 预构建产物(零构建引入)

每个 tag 都由 GitHub Actions(`.github/workflows/release.yml`)构建,把预编译
好的静态库与公共头一起打成按三元组分包的归档,挂到该 tag 的 GitHub Release
上,例如 `corocc-x86_64-unknown-linux-gnu-linux.tar.gz`。

```sh
tar xzf corocc-aarch64-unknown-linux-gnu-linux.tar.gz
cc your_app.c \
    -Iaarch64-unknown-linux-gnu/include \
    aarch64-unknown-linux-gnu/lib/libcorocc.a
```

无需 configure、无需构建:下载后把编译器指向归档内的 `include` 与 `lib` 即可。
已发布三元组随"具备 vendored 上下文切换后端的架构"增长;当前状态矩阵见
`scripts/README.md`,`scripts/prebuild-all.sh` 可在本地生成同构布局。

## L0 API 用法

```c
#include <corocc/coro_ctx.h>

static coro_ctx main_ctx, worker;

static void worker_entry(void* arg) {
    /* 运行在带保护页的栈上;从此处返回是安全的 */
}

int main(void) {
    coro_stack stk;
    coro_stack_alloc(&stk, 64 * 1024);
    coro_ctx_init(&worker, &stk, worker_entry, NULL);

    coro_ctx_swap(&main_ctx, &worker);   /* 首次进入 */
    coro_ctx_swap(&main_ctx, &worker);   /* 再次恢复同一上下文 */

    coro_ctx_destroy(&worker);
    coro_stack_free(&stk);
}
```

上下文对象可重复使用:可挂起、恢复任意次数。入口函数返回时,控制权交还给
最后一次切入它的上下文,绝不退出进程。公共头同时可作为 C99 与 C++11 编译,
且不包含任何第三方头。

## 已知限制

- **范围:**目前只交付 L0 层。协程对象与 token 凭证协议、带工作窃取的
  M:N 调度器与 IO 事件循环、结构化并发、C++11 封装均为后续里程碑。
- **架构:**x86_64 与 aarch64 已完成 vendored 与测试;计划中的其余 ISA
  (i386、arm32/armeb、mips、ppc、s390x、riscv 及大端变体)尚未接入构建。
- **线程:**`coro_ctx` 不是线程安全的 —— 同一时刻必须由单一线程切换
  (本层按设计不持锁)。
- **异常:**禁止用 `coro_ctx_swap` 跨上下文边界传播 C++ 异常。
- **交叉构建测试:**Boost 共存测试仅在本机构建中注册。

## 许可证

`corocc` 本体采用 MIT 许可证,见 `LICENSE`。vendored 的 Boost.Context 汇编
仍为 BSL-1.0,原始版权头已保留 —— 见 `THIRD-PARTY.md`。
