# corocc

> [English](README.md) | 中文

`corocc` 是一个零依赖的 C/C++ 协同程序(coroutine)库,只做**有栈协程**
(stackful):每个协程拥有自己的、带守护页的栈,可任意次数挂起与恢复。工程
标准对标顶级 C/C++ 系统库:小而稳的 ABI、文档齐全的公共头、守护页把栈溢出
变成硬错误而非静默内存破坏,并且全部自有代码在 `-Wall -Wextra -Werror`
(MSVC 下 `/W4 /WX`) 下零警告。

## 概览

| 领域 | 能力 |
| --- | --- |
| 分层 | L0 上下文原语(`coro_stack`、`coro_ctx`)、L1 协程对象(供 token 凭证协议)、L2 M:N 工作窃取运行时(含互斥锁、条件变量、计数信号量、读写锁、通道等同步原语)、L3 结构化并发 scope 层、单线程调度器,以及 header-only 的 C++11 封装(`coro.hpp`) |
| 架构 | 原生 x86_64(SysV ELF 与 MSVC PE/MASM),另有 17 个交叉编译 ABI:i386、aarch64/aarch64_be、arm32/armeb、MIPS o32/n32/n64 双端序、ppc32/ppc64/ppc64le、s390x、riscv32/riscv64;每个交叉 ABI 都在 QEMU 用户态模拟下构建并运行完整测试(见 `scripts/README.md`) |
| 守护页 | 每个协程栈映射一个 `PROT_NONE` 守护页,溢出即触发缺页而不是破坏相邻内存 |
| 消毒器 | ASan fiber-switch 钩子(`COROCC_ASAN`),含栈用后即返回检测;ThreadSanitizer 套件覆盖运行时的投递队列、身份注册表与原子操作 |
| 与 Boost 共存 | 重命名符号后的汇编可与真实 Boost.Context 同进程链接(`boost_coexist` 测试) |
| 工具链 | GCC / Clang / MSVC(x64),以及基于 CMake toolchain 文件的交叉编译;支持 Android NDK 编译 |
| 分发 | 每个 tag 发布按三元组打包的预编译静态库归档,内含 CMake config 包与零依赖消费示例 |

## 环境要求

- CMake >= 3.16,Ninja 或 Make,支持 C99/C++11 的编译器(GCC、Clang 或 MSVC)。
- 可选:Boost.Context 头文件与库(仅用于共存测试)。
- 在 x86_64 主机上运行交叉目标二进制需要对应 ISA 的 `qemu-user` 模拟器,
  以及匹配的交叉工具链 + sysroot。`scripts/build-all.sh` 覆盖全部 17 个交叉
  目标;下文 aarch64 流程即标准示例。
- 无其他依赖。

## 目录结构

```
include/corocc/    公共头(C99、C++11 兼容):coro_status.h、
                   coro_stack.h、coro_ctx.h、coro.h、coro_rt.h、
                   coro_sched.h、coro_scope.h,以及封装头 coro.hpp
src/coroutine/     L0/L1 实现:status.c、stack.c、ctx.c、coro.c、
                   coro_runtime.c(线程级驱动状态与唤醒队列)
src/scheduler/     单线程 run loop(sched.c)与 M:N 工作窃取运行时
                   (rt.c、rt_sync.c、rt_scope.c、timer.c、wsdeque.c、
                   mpsc.c)
src/platform/      线程、锁与条件等待(POSIX/Win32)
src/arch/          上下文切换后端(vendored Boost.Context 汇编,符号已
                   重命名)及原子/TLS/ABI 辅助头
cmake/             arch_select.cmake:按架构选择源文件
toolchain/         标准交叉编译 toolchain 文件(aarch64)
scripts/           构建/测试/交叉/预构建入口(见 scripts/README.md)
tests/             单元、C++(cpp/)、并发与性能套件
perf/              M:N 运行时与纯切换基准
```

## 构建与测试

### 本机构建

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

预期:全部注册测试通过。数量随平台与构建类型变化:普通 Linux 构建注册
43 个 CTest 条目,安装 Boost.Context 后 `+1`(`boost_coexist` 测试),
Release 构建再 `+1`(`cpp_perf` 吞吐门禁),Windows 上 `-1`(仅 POSIX 的
`conc_runtime_tsan` 套件不在 Windows 注册)。套件覆盖上下文切换与守护页
溢出、协程凭证协议、单线程调度器、M:N 运行时及其同步原语、scope 层与
C++11 封装。

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

基准在两个上下文之间往返(每次循环 2 次切换),输出每次切换的纳秒数。参考值
(Linux x86_64、GCC、Ninja):原生约 **10-11 ns/切换**;aarch64 在
`qemu-aarch64` 用户态模拟下约 **50-60 ns/切换**(含模拟开销,该数值随主机
负载波动)。

## 预构建产物(零构建引入)

每个 tag 的发布由 release 工作流(`.github/workflows/release.yml`)完成:把
预编译静态库、公共头、CMake config 包与消费示例打成按三元组分包的归档,连同
扁平 `SHA256SUMS` 一起挂到 GitHub Release。Linux 归档命名为
`corocc-<tag>-<triple>.tar.gz`,Windows 归档为
`corocc-<tag>-x86_64-pc-windows-msvc.zip`。

```sh
tar xzf corocc-<tag>-aarch64-unknown-linux-gnu.tar.gz
cc your_app.c -Iaarch64-unknown-linux-gnu/include \
              aarch64-unknown-linux-gnu/lib/libcorocc.a
```

无需 configure、无需构建:下载后把编译器指向归档内的 `include` 与 `lib` 即可。
归档内的 CMake config 包也支持直接以 `find_package` 方式消费:

```sh
cmake -S aarch64-unknown-linux-gnu/examples/cmake-consumer -B build \
      -DCMAKE_PREFIX_PATH="$PWD/aarch64-unknown-linux-gnu"
cmake --build build
```

已发布的 Linux 三元组:`x86_64-unknown-linux-gnu`、
`aarch64-unknown-linux-gnu`、`arm-linux-gnueabihf`、
`i386-unknown-linux-gnu`、`mips-unknown-linux-gnu`、
`mipsel-unknown-linux-gnu`、`mips64-unknown-linux-gnuabi64`、
`mips64el-unknown-linux-gnuabi64`、`powerpc-unknown-linux-gnu`、
`powerpc64-unknown-linux-gnu`、`powerpc64le-unknown-linux-gnu`、
`s390x-unknown-linux-gnu` 与 `riscv64-unknown-linux-gnu`;Windows 归档为
`x86_64-pc-windows-msvc`。

`scripts/prebuild-all.sh` 可在本地生成同构布局(输出到 `dist/`)。

## L0 API 用法

```c
#include <corocc/coro_ctx.h>

static coro_ctx main_ctx, worker;

static void worker_entry(void* arg) {
    /* 运行在带守护页的栈上;从此处返回是安全的 */
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

每个挂点都发布唯一 token:出示它才能唤醒协程,旧 token 一律拒绝,且无需任何
调度器。取消是协作式的(`coro_cancel`);需要跨挂起存活的值应放在协程局部槽
(`coro_local_get/set`);也可以为单个协程指定自定义栈/句柄分配器
(`coro_attr_t.allocator`)。完整契约、状态字编码与实测成本见
[docs/api/coro.zh-CN.md](docs/api/coro.zh-CN.md)。

## L2 运行时(M:N 调度与同步)

`coro_rt_t` 是工作窃取的 M:N 运行时。`coro_rt_create(n)` 启动 `n` 个 worker
线程;`coro_rt_spawn`/`coro_rt_spawn_h` 提交协程(受管形式返回句柄,用
`coro_rt_collect` 回收);`coro_rt_join` 停放调用方直到目标协程结束;
`coro_rt_wait_all` 排空运行时;`coro_rt_sleep`、`coro_rt_yield`、
`coro_rt_cancel` 提供协作式定时与取消。运行时之上提供一组同步原语,全部可按
值内嵌、零初始化即可用:互斥锁、条件变量、计数信号量、写者优先的读写锁,
以及 FIFO 通道——后两者示例如下:

```c
#include <corocc/coro_rt.h>

int main(void) {
    coro_rt_t* rt = coro_rt_create(2);

    coro_rt_rwlock_t lock;          /* 无需初始化调用 */
    if (coro_rt_rwlock_rdlock(rt, &lock) == CORO_OK)
        coro_rt_rwlock_rdunlock(rt, &lock);

    coro_rt_channel_t chan;
    coro_rt_channel_init(&chan, 8);   /* 容量 0 = 无界 */
    coro_rt_channel_send(rt, &chan, (void*)1);
    void* got = NULL;
    coro_rt_channel_recv(rt, &chan, &got);
    coro_rt_channel_close(rt, &chan);       /* 先排空,再报 ECLOSED */
    coro_rt_channel_destroy(&chan);

    coro_rt_destroy(rt);
    return 0;
}
```

rwlock 的 try 路径返回 `CORO_EBUSY`,非运行时协程内调用返回 `CORO_ESTALE`,
无匹配持有的解锁返回 `CORO_ESTATE`,等待被取消返回 `CORO_ECANCELED`;
channel 在满时 `trysend` 与空时 `tryrecv` 返回 `CORO_EBUSY`,
关闭并排空后返回 `CORO_ECLOSED`。完整契约见 `include/corocc/coro_rt.h`;
读写锁与通道的详解另见 [docs/api/coro_rt.zh-CN.md](docs/api/coro_rt.zh-CN.md)。

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

## 跨线程驱动

协程由出示其有效凭证的线程驱动。认领是状态字上的单次 compare-and-swap:
多线程争抢同一凭证时,恰好一个胜出并进入协程,其余调用方收到 `CORO_EBUSY`
或 `CORO_ESTALE`,协程保持原状。yield 会回到驱动当前这一轮运行的线程,因此
把协程交给另一线程,只需把凭证交给它。

```c
/* 任何持有有效 token 的线程都可以驱动: */
coro_token next = CORO_TOKEN_NONE;
coro_status s = coro_resume(worker, tok, &next);   /* 一次认领,一次驱动 */
```

协程局部槽存在协程上而非线程上,因此随协程一起迁移。自己管理一组协程的线程
还可以使用线程级唤醒队列:`coro_pump` 非阻塞地清扫本线程的排队请求并返回
服务数量;`coro_pump_wait` 先睡眠直到请求到达再清扫。由于一次清扫可能服务
多个请求,需要服务固定数量请求的调用方应统计两者返回值。投递给已退出线程的
请求以 `CORO_EDESTROYED` 失败而非丢失。完整跨线程契约与并发验证矩阵见
[docs/api/coro.zh-CN.md](docs/api/coro.zh-CN.md)。

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

- **I/O:**目前没有异步 I/O 完成循环;协程可停放于定时器与同步原语,但不能
  停放于文件/套接字就绪事件。
- **手动层 join:**`coro_join` 不阻塞——协程存活期间报告 `CORO_EAGAIN`,由
  调用方驱动其结束。阻塞式 join 位于运行时层(`coro_rt_join`)。
- **线程:**单个 `coro_ctx` 切换对本身不是线程安全的:同一时刻只能由一条
  线程切换。L1 的认领机制保证了整个协程层面的这一约束(见
  [docs/api/coro.zh-CN.md](docs/api/coro.zh-CN.md))。
- **异常:**禁止用 `coro_ctx_swap` 跨上下文边界传播 C++ 异常。
- **s390x 协程内 TLS:**s390x 上协程体内不要依赖 `thread_local`(TLS)变量
  —— gcc 将线程指针缓存在被保存寄存器中,而上下文切换会跨线程恢复该寄存器,
  切换后缓存值可能失效。s390x 的 toolchain 文件因此使用
  `-fno-stack-protector`(栈保护金丝雀同样会读该缓存指针)。其他 ABI 不受
  影响;一般 `thread_local` 契约见
  [docs/api/coro.zh-CN.md](docs/api/coro.zh-CN.md)。
- **交叉构建测试:**Boost 共存测试仅在本机构建中注册(交叉构建会把宿主机的
  Boost 目标文件链入异构架构二进制)。

## 许可证

`corocc` 本体采用 MIT 许可证,见 `LICENSE`。vendored 的 Boost.Context 汇编
仍为 BSL-1.0,原始版权头已保留 —— 见 `THIRD-PARTY.md`。
