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
| 分层 | L0-L3 已实现:上下文原语(`coro_stack`、`coro_ctx`)、协程对象、M:N 工作窃取运行时(含同步原语)、结构化并发 scope 层;header-only C++11 封装(`coro.hpp`,M8)与 Windows(MSVC)移植(M9)均已落地 |
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

## L1 协程对象用法

```c
#include <corocc/coro.h>

static coro_t* g_worker;

static void worker(void* arg) {
    for (int i = 0; i < 4; ++i) {
        /* ... */
        coro_yield();          /* 在此挂起,把控制权交还调用方 */
    }
    /* 返回即结束协程 */
}

int main(void) {
    coro_token tok = CORO_TOKEN_NONE, next = CORO_TOKEN_NONE;
    coro_create(&g_worker, NULL, worker, NULL);
    coro_start(g_worker, &tok);              /* 运行到第一个挂点 */
    while (next != CORO_TOKEN_DEAD) {
        coro_resume(g_worker, tok, &next);   /* 须出示它发布的凭证 */
        tok = next;
    }
    coro_destroy(g_worker);
    return 0;
}
```

每个挂点都发布唯一 token:出示它才能唤醒协程,旧 token 一律拒绝,且无需任何调度
器。取消是协作式的(`coro_cancel`),需要跨挂起存活的值应放在协程局部槽
(`coro_local_get/set`),协程归属于首个驱动它的线程。完整契约、状态字编码与实测
成本见 [docs/api/coro.zh-CN.md](docs/api/coro.zh-CN.md)。

## 多线程

协程归属于首个驱动它的线程。任何其他线程发起的唤醒会被**投递**给该属主:请求入队,
请求方阻塞等待属主记录的结果——因此协程绝不会被不属于它的线程进入。

```c
/* 线程 A(属主):停在 coro_pump_wait,直到有活可用 */
coro_pump_wait();          /* 睡眠,然后服务全部已排队请求 */

/* 线程 B:请求唤醒 —— 被投递给 A,由 A 执行 */
coro_token next = CORO_TOKEN_NONE;
coro_status s = coro_resume(worker, tok, &next);   /* 等待 A 的结果 */
```

`coro_pump` 以非阻塞方式清扫队列(属主自有工作时应使用),并返回服务了多少个请求;
`coro_pump_wait` 先睡眠再清扫。由于一次清扫可能服务多个请求,需要服务固定数量请求的
调用方应统计两者返回值。投递给已退出属主的请求会返回 `CORO_EDESTROYED` 而非丢失。

把协程**迁移**到另一线程(而非投递给属主)尚未提供:它依赖调度器的工作窃取结构,
随该里程碑交付。完整跨线程契约与并发测试矩阵见
[docs/api/coro.zh-CN.md](docs/api/coro.zh-CN.md)。

## L2 运行时同步原语

M:N 运行时还提供写者优先的读写锁与 FIFO 通道,两者均可按值内嵌、零初始化即可用:

```c
#include <corocc/coro_rt.h>

int main(void) {
    coro_rt_t* rt = coro_rt_create(2);

    coro_rt_rwlock_t lock;               /* 无需初始化调用 */
    if (coro_rt_rwlock_rdlock(rt, &lock) == CORO_OK)
        coro_rt_rwlock_rdunlock(rt, &lock);

    coro_rt_channel_t chan;
    coro_rt_channel_init(&chan, 8);      /* cap==0 表示无界 */
    coro_rt_channel_send(rt, &chan, (void*)1);
    void* got = NULL;
    coro_rt_channel_recv(rt, &chan, &got);
    coro_rt_channel_close(rt, &chan);    /* 先排空,再报 ECLOSED */
    coro_rt_channel_destroy(&chan);

    coro_rt_destroy(rt);
    return 0;
}
```

rwlock 的 try 路径返回 `CORO_EBUSY`,非运行时协程内调用返回 `CORO_ESTALE`,
无匹配持有的解锁返回 `CORO_ESTATE`,等待被取消返回 `CORO_ECANCELED`;
channel 在满时 `trysend` 与空时 `tryrecv` 返回 `CORO_EBUSY`,
关闭并排空后返回 `CORO_ECLOSED`。完整契约见
`include/corocc/coro_rt.h`。

## 结构化并发(L3)

scope 持有一组子协程并赋予它们同一条生命线:wait 族只有在该 scope 接纳过
的每一个子都已结束并完成回收之后,才报告完成。scope 对象可按值内嵌;零初
始化即 OPEN 态。

```c
#include <corocc/coro_scope.h>

coro_rt_scope_t s;
memset(&s, 0, sizeof s);                  /* OPEN */
coro_rt_scope_spawn(rt, &s, NULL, work, arg, NULL);   /* 子 */
coro_rt_spawn(rt, NULL, waiter_entry, &s);            /* 等待者协程 */
coro_rt_wait_all(rt);                     /* 等待者内执行 wait(rt, &s) */
coro_rt_scope_destroy(&s);                /* drain 之后 */
```

wait 族有两种入口:协程调用时等待者挂在 scope 事件链上;普通线程调用时阻塞
于运行时条件变量(**绝不报 `CORO_EAGAIN`**)——"主线程 spawn 后收口"可直接
使用。两条调用方责任为契约而非建议:wait 对自身取消免疫(永远以
`live == 0` 结束);`coro_rt_scope_cancel` 只保证即时打断 running/sleeping
的子——阻塞在同步原语上的子仅在其等待事件发生时观察取消,子互等 + 全取消
可能使 wait 不终止。完整契约、状态机与看门狗设计见
[docs/api/coro_scope.zh-CN.md](docs/api/coro_scope.zh-CN.md)。

## C++11 封装(header-only)

`include/corocc/coro.hpp` 是同一库的 C++11 门面:在 C 入口点之上提供 RAII
句柄(`coroutine`、`runtime`、`scheduler`、`task<R>`、`scope`、`cell<R>`)、
类型化结果、chrono 时长与异常捕获-投递边界。不增加任何库或构建步骤;
不含该头文件的 C 翻译单元不受影响。

```cpp
#include <corocc/coro.hpp>

int main() {
    corocc::runtime rt(4);                 /* 4 个 worker;0 = 库默认 */
    corocc::task<int> t;
    rt.spawn_h(t, [] { return 7; });
    corocc::status st = t.wait();          /* 线程侧目标定向收口 */
    if (st.ok()) {
        int v = t.get();
        (void)v;
    }

    corocc::scope s(rt);                   /* 每个子都带 cell */
    corocc::cell<int> a;
    s.spawn(a, [] { return 40 + 2; });
    st = s.wait();
    if (st.code() == CORO_ECANCELED && s.first_exception()) {
        /* 异常终结,不是取消 */
    }
    s.destroy();
    rt.wait_all();
    return 0;
}
```

解码状态前必读:该路径上 `CORO_ECANCELED` 有三义(请求取消 / 期限触发 /
子异常终结),用 `first_exception()` 消歧。完整参考(构建规则、异常模型、
所有权与寿命红线、`native()` 契约)见
[docs/api/coro_cpp.zh-CN.md](docs/api/coro_cpp.zh-CN.md)。

## 已知限制

- **范围:**目前交付 L0-L3 层(上下文原语、含 token 凭证协议的协程对象、
  带工作窃取的 M:N 运行时与同步原语、结构化并发 scope 层),以及
  header-only C++11 封装(`coro.hpp`,M8)与 Windows(MSVC)移植(M9)。
  IO 事件循环仍为后续里程碑。
- **架构:**x86_64 与 aarch64 已完成 vendored 与测试;计划中的其余 ISA
  (i386、arm32/armeb、mips、ppc、s390x、riscv 及大端变体)尚未接入构建。
- **线程:**`coro_ctx` 不是线程安全的 —— 同一时刻必须由单一线程切换
  (本层按设计不持锁)。
- **异常:**禁止用 `coro_ctx_swap` 跨上下文边界传播 C++ 异常。
- **交叉构建测试:**Boost 共存测试仅在本机构建中注册。

## 许可证

`corocc` 本体采用 MIT 许可证,见 `LICENSE`。vendored 的 Boost.Context 汇编
仍为 BSL-1.0,原始版权头已保留 —— 见 `THIRD-PARTY.md`。
