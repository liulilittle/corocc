# coro_l0 — 状态码、守护页栈与执行上下文(L0)

> [English](coro_l0.md) | 中文

本文档是 corocc L0 基础层的 API 与 ABI 参考:`coro_status.h`(所有可失败
入口返回的结果码类型,以及 `COROCC_API` 可见性宏)、`coro_stack.h`(带守护页
的协程栈)与 `coro_ctx.h`(执行上下文:栈 + 保存的寄存器帧)。三个头文件均
可作 C99 与 C++11 编译、不含任何第三方头,是整个库每一层的地基。

下文的结构体布局属于公开 ABI 的一部分;各指针模型上的 `sizeof`/`offsetof`
实测值见 [ABI.md](../ABI.zh-CN.md)。

## coro_status —— 结果码

每个可失败入口都返回 `coro_status`;库在平常错误路径上绝不 abort。凡函数
契约声明"失败不改变对象状态"之处,调用都可以安全重试或改走其他处置。
`CORO_OK` 为 `0`,因此 `if (status)` 即有效的失败判断。

| 值 | 含义 |
| --- | --- |
| `CORO_OK` | 成功。 |
| `CORO_EINVAL` | 参数非法(NULL、未初始化对象、尺寸不可用、键越界)。 |
| `CORO_ENOMEM` | 内存或后备资源分配失败。 |
| `CORO_EBUSY` | 目标正忙(运行中、已满、未排空),操作被拒。 |
| `CORO_ESTATE` | 对象当前状态不允许该操作。 |
| `CORO_ECANCELED` | 操作被取消请求中止(协作式:在安全点观察)。 |
| `CORO_EDESTROYED` | 目标已结束/已销毁。 |
| `CORO_ESTALE` | 所引用的凭证或代际过期(如被二次恢复的 token);或该调用要求处于本运行时的协程内而在其外调用。 |
| `CORO_EAGAIN` | 暂时性状态;调用方可重试。也是 `coro_join` 对活协程的非阻塞答复。 |
| `CORO_ECLOSED` | 通道已关闭,无法再交换(已入队项先排空)。 |

数值稳定且与上表顺序一致:表内序号即数值(`CORO_OK == 0` … `CORO_ECLOSED
== 9`),此后新增的码只追加在末尾,绝不插入。各入口小节分别列出其实际可能
返回的子集。

### `const char* coro_status_str(coro_status status)`

返回命名该码的静态只读 NUL 结尾字符串(如 `"CORO_OK"`)。指针永不为 NULL,
在进程整个生命周期内有效;不得释放或修改。越界值映射到通用的 `"unknown"`
字符串,而不是未定义行为。

## coro_stack —— 带守护页的栈

`coro_stack` 持有一段连续内存,低地址端有一个硬件守护页。由于全部受支持
ABI 的栈都向下生长,溢出会写入可用区之外的守护页并触发缺页,而不是静默破坏
相邻内存。类型是完整定义,调用方可以直接按值内嵌(置于栈上或池中),无需
堆分配;字段为私有,只能经 API 使用。

```c
typedef struct coro_stack {
    void  *base;        /* 私有:映射区最低地址(守护页) */
    size_t total;       /* 私有:映射总字节数(守护页 + 可用区) */
    void  *lo;          /* 私有:可用区最低地址(base + 守护页大小) */
    size_t usable;      /* 私有:可用栈字节数(不含守护页) */
    size_t guard_bytes; /* 私有:守护页大小(字节) */
} coro_stack;
```

| 入口 | 契约 |
| --- | --- |
| `coro_stack_alloc(stk, size)` | 把 `size` 向上取整到整页,映射相应可用字节 + 一个守护页。`size == 0` 选默认值。成功 `CORO_OK`;`stk` 为 NULL 或尺寸不足以配守护页 `CORO_EINVAL`;无法取得页大小或映射/守护保护失败 `CORO_ENOMEM`。失败时 `stk` 保持零初始化,不得再传给 `coro_stack_free`。 |
| `coro_stack_free(stk)` | 解除整段映射并把对象零初始化,可复用或再次安全释放。幂等:对零初始化对象释放是空操作;NULL 被接受。释放时栈不得正被任何上下文使用。 |
| `coro_stack_size(stk)` | 可用字节数(不含守护页);NULL/未分配返回 0。 |
| `coro_stack_avail(stk)` | 当前低于用水位的可用字节;属咨询性指标(不超过 `coro_stack_size`)。NULL/未分配返回 0。 |

线程安全:不同 `coro_stack` 对象可并发分配与释放;同一对象不得被两条线程同时
使用。

## coro_ctx —— 执行上下文

`coro_ctx` 把入口函数及其参数绑定到一个 `coro_stack`;两个上下文间切换即
保存一方的被调用者保存寄存器、恢复另一方的。它不做系统调用、不持锁——这是
库的最底层。

```c
typedef struct coro_ctx {
    void*             fctx;      /* 私有:保存的寄存器帧(后端内部) */
    const coro_stack* stack;     /* 私有:借用的栈范围(消毒器钩子等) */
    void            (*entry)(void*); /* 私有:协程入口函数 */
    void*             arg;       /* 私有:原样转发给入口的参数 */
    struct coro_ctx*  back;      /* 私有:恢复者上下文(哨兵目标) */
    void*             asan_fake; /* 私有:消毒器伪栈保存句柄;其余情形不用 */
} coro_ctx;
```

| 入口 | 契约 |
| --- | --- |
| `coro_ctx_init(cx, stk, entry, arg)` | 在活栈上构造入口帧,使首次切入时调用 `entry(arg)`。上下文借用 `stk`(栈须比上下文长寿且保持不变)。成功返回 `0`;`cx`、`stk` 或 `entry` 为 NULL 时返回非零的 `CORO_EINVAL`。**注意返回类型是普通 `int`**,不是 `coro_status`。前置:`cx` 未处于活动状态(先销毁)。失败时 `cx` 保持零值。 |
| `coro_ctx_destroy(cx)` | 释放保存的帧并清零对象。幂等(NULL 被接受);不释放栈。前置:`cx` 不是当前运行上下文,且不会再被切入。 |
| `coro_ctx_swap(from, to)` | 保存 `from` 的被调用者保存状态、恢复 `to` 的,在 `to` 中继续执行;待某上下文再切回 `from` 时,该调用"从 `from` 返回"。必须按 nothrow 编译:禁止用它跨边界传播 C++ 异常。前置:两个上下文均活动且互异;调用期间不持任何锁;同一对上下文同一时刻只由一条线程切换。 |

## 示例

```c
#include <corocc/coro_ctx.h>

static coro_ctx main_ctx, worker;

static void worker_entry(void* arg) {
    (void)arg;
    /* 运行在带守护页的栈上;从此处返回是安全的 */
}

int main(void) {
    coro_stack stk;
    if (coro_stack_alloc(&stk, 64 * 1024) != CORO_OK) return 1;
    if (coro_ctx_init(&worker, &stk, worker_entry, NULL) != 0) return 1;

    coro_ctx_swap(&main_ctx, &worker);   /* 首次进入 */
    coro_ctx_swap(&main_ctx, &worker);   /* 再次恢复同一上下文 */

    coro_ctx_destroy(&worker);
    coro_stack_free(&stk);
    return 0;
}
```

上下文对象可重复使用:可挂起、恢复任意次数。入口函数返回时,控制权交还给
最后一次切入它的上下文,绝不退出进程。

## 保证

- **溢出仍致命**:守护页把栈溢出变成缺页,而非静默破坏。
- **入口返回安全**:入口函数返回即把控制权交还最后一个恢复者;进程绝不退出。
- **无隐藏资源**:上下文只持有借用的栈指针与保存帧——切换路径上没有系统调用、
  没有锁、没有分配。
- **可移植布局**:全部字段都是指针或 `size_t`;LP64 与 LLP64 布局逐字节一致,
  ILP32 按同一规则缩放,详见 [ABI.md](../ABI.zh-CN.md)。

## 线程安全

| 入口 | 线程安全 |
| --- | --- |
| `coro_stack_alloc`、`coro_stack_free` | 不同对象可并发;同一对象同一时刻仅一条线程。 |
| `coro_stack_size`、`coro_stack_avail` | 只读;对象稳定时任意线程可调。 |
| `coro_ctx_init`、`coro_ctx_destroy` | 每个上下文同一时刻仅一条线程;上下文不得并发处于活动/使用中。 |
| `coro_ctx_swap` | 每个切换对同一时刻仅一条线程:单个 `coro_ctx` 切换对本身不是线程安全的。L1 协程层的凭证认领保证了整个协程层面的该约束(见 [coro.zh-CN.md](coro.zh-CN.md))。 |
| `coro_status_str` | 纯函数;任意线程可调。 |
