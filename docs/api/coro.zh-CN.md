# coro — 协程对象(L1)

> [English](coro.md) | 中文

本文档是 corocc 协程对象层 `coro.h` 的 API 与 ABI 参考,覆盖每个入口的契约、
库依赖的布局,以及凭证协议提供的保证。

状态:**里程碑 M2**。协程对象无运行时亦可独立使用;M:N 调度器、阻塞原语与
C++ 封装都基于这些入口构建。

## 概念

| 术语 | 含义 |
| --- | --- |
| `coro_t` | 调度实体的不透明句柄:一个状态机 + 一个执行上下文(栈 + 保存的寄存器帧)。 |
| 凭证 / token | 由每个挂点发布的 `coro_token` 值;仅持有者可恢复该协程。 |
| 认领 | 授予驱动权限的那一次 compare-and-swap;失败即上报,绝不重试。 |
| 驱动者 / 属主 | 驱动协程的线程。首个驱动它的线程即属主(见「属主」一节)。 |
| 恢复者 | 协程让出或入口返回时回到的上下文。 |

## 状态字

全部状态集中于一个原子 64 位字 `W`,因此每次状态转移都是单次 CAS:

| 值 | 含义 |
| --- | --- |
| `0` | `CORO_W_NEW`:已创建,从未启动。 |
| `1` | `CORO_W_RUNNING`:执行中;不可恢复。 |
| `2` | `CORO_W_SWITCHING`:正在保存帧;不可恢复。 |
| `UINT64_MAX` | `CORO_W_DEAD`:已结束;不可恢复。同时也是 `CORO_TOKEN_DEAD` 哨兵。 |
| `3 … UINT64_MAX-1` | 已停在某个挂点;该值即唯一可恢复它的 token。 |

token 来自每协程的单调序列(自 3 起),故永不重复:旧 token 永不可能被误认为
当前 token(无 ABA)。

`coro_token` 为 `uint64_t`。两个保留值永不作为凭证发出:`CORO_TOKEN_NONE`(0)
与 `CORO_TOKEN_DEAD`(`UINT64_MAX`)。DEAD 哨兵与死状态字同值,因此
`coro_resume` 在认领**之前**拒绝它,而非把已结束协程当作挂起恢复。

由于协程结束时返回 `CORO_TOKEN_DEAD`,驱动循环形如:

```c
coro_token tok = CORO_TOKEN_NONE, next = CORO_TOKEN_NONE;
if (coro_start(c, &tok) != CORO_OK) { /* 处理 */ }
while (next != CORO_TOKEN_DEAD) {
    if (coro_resume(c, tok, &next) != CORO_OK) { /* 处理 */ }
    tok = next;
}
```

## 入口

### `coro_attr_t coro_attr_default(void)`

返回 `{ stack_size = 0, name = NULL, flags = CORO_F_NONE }`。返回后可按字段修改,
`coro_create` 之后即可丢弃。

### `coro_status coro_create(coro_t** out, const coro_attr_t* attr, coro_fn fn, void* arg)`

分配句柄、映射带保护页的栈(`stack_size` 为 0 时为 `CORO_DEFAULT_STACK_SIZE`
可用字节 + 一个保护页)、构造入口帧。首次驱动之前不执行任何用户代码。

* 前置:`out`、`fn` 非 NULL;`attr->flags` 为 `CORO_F_NONE`;`attr->stack_size`
  为 0 或足够配一个保护页。
* 成功后置:`*out` 收到句柄,状态 `CORO_NEW`。
* 失败:`CORO_EINVAL`、`CORO_ENOMEM`;`*out` 不被写入,不残留资源。
* 线程:不同句柄可并发创建。
* 异常/ABI:`fn` 在协程栈上运行;从它返回即结束协程并把控制权交还最后一个
  恢复者 —— 绝不退出进程。

### `coro_status coro_destroy(coro_t* c)`

把协程置死、释放其栈并释放句柄。每个 `coro_create` 必须恰好调用一次,如同对
指针调用一次 `free()`。

* 前置:协程未在运行 —— 包括不得由协程销毁自身(那会解除自己正在运行的栈的映射)。
* 失败:`CORO_EBUSY`(运行中、切换中,或状态在调用期间被他人改变);
  `CORO_EINVAL`(NULL)。失败时什么都不释放。
* 线程:任何能看到该句柄的线程均可调用,只要协程未运行;不要求持有属主身份。

### `coro_state coro_get_state(const coro_t* c)`

报告 `CORO_NEW`/`CORO_READY`/`CORO_RUNNING`/`CORO_SUSPENDED`/`CORO_DEAD`。
NULL 返回 `CORO_NEW` 而非崩溃。

### `coro_status coro_start(coro_t* c, coro_token* next)`

自 `CORO_W_NEW` 认领协程,切入,并在其挂起或结束时返回。

* 成功时 `*next` 收到挂点的凭证,入口已返回则为 `CORO_TOKEN_DEAD`。仅在成功
  时写入。
* 失败:`CORO_EINVAL`(NULL 参数);`CORO_EBUSY`(运行中/切换中,或协程属他线程,
  见「属主」);`CORO_ESTALE`(已启动并挂起:应改用所持 token `coro_resume`);
  `CORO_EDESTROYED`(已死)。

### `coro_status coro_resume(coro_t* c, coro_token tok, coro_token* next)`

唤醒协程的唯一途径:出示其上一个挂点发布的 token。

* 认领为单次 `CAS(tok -> RUNNING)`。成功则转移控制;失败则协程不被改动,调用方
  得到原因:`CORO_EINVAL`(NULL,或 `tok` 不是凭证)、`CORO_EDESTROYED`
  (`CORO_TOKEN_DEAD`,或状态字已表示死亡)、`CORO_ESTALE`(旧/未签发的 token)、
  `CORO_EBUSY`(运行中/切换中,或协程属他线程)。
* 一个 token 只能被一个持有者认领:恢复成功后该 token 即被消费,再次出示以
  `CORO_ESTALE` 失败。
* 落定在返回之后:新凭证只在切换**完整保存**协程帧之后发布,故任何观察者都不
  可能把半停稳的协程看成挂起。
* 线程/异常:切换期间不持锁;调用方不得跨切换传播 C++ 异常。

### `coro_status coro_yield(void)`

挂起当前协程,把控制权交还其恢复者。

* 前置:在持有许可 token 的协程内调用。
* 返回:被恢复时 `CORO_OK`;期间收到取消请求时 `CORO_ECANCELED`(两个安全点
  皆查:驻留前与被唤醒时);许可 token 已被他方夺走时 `CORO_EBUSY`(不发生切换);
  在协程外调用时 `CORO_ESTALE`。
* 观察到取消的 yield 不驻留:入口函数应顺势收尾,协程随即结束。

### `coro_status coro_cancel(coro_t* c)`

请求协程在下一个安全点停止。协作式:不抢占、不强制状态转移,对新创建、挂起或
已死的协程都无害。从不 yield 的协程永远观察不到取消。返回 `CORO_OK`,NULL 返回
`CORO_EINVAL`。

### `coro_status coro_join(coro_t* c)`

报告协程是否已死。无运行时可停放调用方,且本契约绝不为等待而自旋,故活协程返回
`CORO_EAGAIN`——先驱动其结束再 join。阻塞式 join 随调度器里程碑提供。NULL 返回
`CORO_EINVAL`。

### `void* coro_local_get(const coro_t* c, size_t key)` / `coro_status coro_local_set(coro_t* c, size_t key, void* val)`

读写存于协程(而非线程)的 `CORO_LOCAL_SLOTS`(32)个固定槽位之一,因此跨挂起与线程
迁移保持,且绝不触碰 `thread_local`。`key >= CORO_LOCAL_SLOTS` 与 NULL 句柄一律
`CORO_EINVAL`;`coro_local_get` 对 NULL 返回 NULL。未写槽读出 NULL。此类调用不
涉及状态字,永不平滑失败。

## 属主

首个驱动协程的线程即其属主,且只有属主可驱动它。其他线程的 start/resume 会被
**投递**:请求进入属主的队列,请求方阻塞等待属主记录的结果。请求方线程上不发生
任何切栈,因此协程绝不会被不属于它的线程进入,也不会有线程持留另一线程的栈引用。

`coro_pump` 清扫属主队列并返回服务了多少个请求;`coro_pump_wait` 先睡眠至请求
到达再清扫。二者都只能由属主线程调用,且随时可调。需要服务已知数量请求的调用方
应同时统计两者返回值——因为一次清扫可能服务多个请求。

向属主已退出的协程投递会返回 `CORO_EDESTROYED` 而非丢失:属主的退出钩机会把队列
中残留的请求全部置失败,请求方一定会被释放。

### 把协程迁移到另一线程

本里程碑不提供。投递的替代是**迁移**:认领协程、成为新属主并在此运行。这需要调度器
的工作窃取结构来保证迁移期间状态一致,故随运行时可期待。`coro.h` 将其记为已知缺口,
而非假装投递路径已覆盖。

## 线程安全小结

| 入口 | 线程安全 |
| --- | --- |
| `coro_create` | 不同句柄可并发。 |
| `coro_destroy` | 任意线程;拒绝运行中的协程。 |
| `coro_get_state`、`coro_join`、`coro_cancel`、`coro_local_*` | 持句柄的任意线程。 |
| `coro_start`、`coro_resume` | 属主线程直接驱动;异线程的请求被投递给属主,调用方等待结果。两种路径下并发认领都由单次 CAS 裁决。 |
| `coro_yield` | 仅属主线程;把控制权交还其恢复者。 |
| `coro_pump`、`coro_pump_wait` | 仅属主线程;并发入队由队列锁串行化。 |

## 保证

* **恰好一次**:一个凭证只可能被一次 CAS 赢得,故任一挂点至多被一条执行流进入。
  被拒的认领只上报、不重试(控制路径全程零自旋)。
* **绝不崩溃**:一切非法/竞争调用返回 `coro_status`;失败保持状态字、凭证、栈
  不变,可安全重试或改走其他处置。
* **溢出仍致命**:保护页把栈溢出变成缺页而非静默破坏(见 L0 文档)。
* **入口返回安全**:入口函数返回即把控制权交还最后一个恢复者;进程绝不退出。

## ABI 备注

* `coro_t` 为不透明句柄:布局在内部,不对外暴露。
* `coro_token` 为 `uint64_t`;`coro_state`/`coro_status` 为枚举,取值稳定
  (`CORO_OK == 0`)。
* 库为 C99 与 C++11 兼容;`coro.h` 不含任何第三方头。

## 实测成本

以下数字取自项目 Linux CI 主机(x86_64、GCC、Ninja、`tests/perf/bench_coro.c`)。
仅供参考而非契约:绝对值随机器变化,且生命周期数字主要由"映射并首次触碰 256 KiB
保护页栈"主导。

| 测量项 | 数值 |
| --- | --- |
| resume + yield 往返(两次切换 + 一次认领 + 一次发布) | ~56 ns |
| create + start + 驱动至结束 + destroy(256 KiB 保护页栈) | ~9.5 µs |
| 单纯上下文切换(L0,`bench_switch`) | ~11 ns |

## 并发验证

并发套件位于 `tests/concurrency/`:

| 套件 | 证明什么 | 运行于 |
| --- | --- | --- |
| `test_conc_double_resume` | 多线程出示同一凭证时至多一条执行流进入挂点,含固定种子 200 轮投递压测。 | 原生、ASan、aarch64/QEMU |
| `test_conc_runtime_tsan` | 递送队列、身份注册表与原子操作在多线程并发下无数据竞争。 | ThreadSanitizer(CI) |

ThreadSanitizer 无法跟踪汇编执行的切栈,故驱动路径不可插桩;并发工作因此拆为「可插桩
部分」与「行为部分」。行为部分(恰好一次、拒绝码、协程仍可驱动)在其他每个 job 中运行。

TSan job 不在本项目的 WSL 镜像上运行(TSan 自身在此中止——最小 pthread 程序同样失败),
故由 CI 验证并记录为"本地未验证";`scripts/ci-local.sh` 如实说明而非宣称通过。
