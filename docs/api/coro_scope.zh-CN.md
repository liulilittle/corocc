# coro_scope -- M:N 运行时之上的结构化并发(L3)

> [English](coro_scope.md) | 中文

本文是 L3 结构化并发层的 API 参考:scope 持有一组子协程,wait 族只有在
该 scope 接纳过的每一个子协程都已结束并完成回收之后,才报告完成。

头文件为 `include/corocc/coro_scope.h`,实现位于
`src/scheduler/rt_scope.c`。scope 对象是完整类型、可按值内嵌;零初始化即
OPEN 态(定案 §3.8;设计草案的"不透明 typedef"被取代——零初始化必须可用,
调用方必须能按值声明该对象)。

## 概念

| 术语 | 含义 |
| --- | --- |
| scope | 子表 + 一份聚合结果的持有者。 |
| 子(child) | 经 `coro_rt_scope_spawn` 派生的协程;每个子在任何运行之前先完成登记。 |
| 结果单元(cell) | 每子一份 `coro_status` 存储。scope 安装的 trampoline 是它的**唯一写者**;scope 自身从不预写结果。 |
| 聚合 | 按完成顺序的首个非 OK 子结果,否则 `CORO_OK`。 |
| live | 已登记但未结束的子计数;所有 wait 谓词都是 `live == 0`。 |
| 状态字 | `OPEN -> CANCELING -> DRAINED -> DESTROYED`,单一原子;一把锁(运行时的 act_lock)守护子表、live、聚合与事件链。 |
| 看门狗 | 一个 retained spawn(绝不是子、绝不计入 live),对 `wait_for` 的 deadline 做恰好一次的认领并取消 scope。 |

wait 族有两种成文入口:由该运行时调度的协程调用时,等待者挂在 scope 自己
的事件链上(单挂点)、每次挂起对应恰好一次唤醒;由普通线程调用时,调用阻塞
该线程于运行时的 settled 条件上,**绝不返回 `CORO_EAGAIN`**。任何 wait 都不
对子使用 `coro_rt_join`:子完成经事件链通知,聚合读取所依赖的顺序也由它提供。

## 生命周期

```c
coro_rt_scope_t s;              /* 零初始化 = OPEN */
memset(&s, 0, sizeof s);        /* 或:coro_rt_scope_init(&s) */
```

| 迁移 | 触发 |
| --- | --- |
| (zero/init) -> OPEN | 初始化 |
| OPEN -> OPEN | `coro_rt_scope_spawn` 成功 |
| OPEN -> CANCELING | `coro_rt_scope_cancel`,或 `wait_for` 看门狗认领 |
| OPEN/CANCELING -> DRAINED | drain 的单次 CAS 胜者(谓词 `live == 0` 成立的 wait) |
| DRAINED -> DESTROYED | `coro_rt_scope_destroy`(`live == 0` 后自任意态均允许) |

非法迁移:CANCELING/DRAINED 后 spawn → `CORO_ESTATE`;DESTROYED 后除 destroy
外任何操作 → `CORO_ESTALE`;`live > 0` 时 destroy → `CORO_EBUSY` 且状态不变;
二次 destroy → `CORO_OK`。scope 对象本体归调用方所有;destroy 释放子表并把
scope 复位为零态、置 DESTROYED 墓碑。

## 入口点

| 入口 | 契约 |
| --- | --- |
| `coro_rt_scope_init(s)` | 将全新 scope 置为 OPEN。`CORO_OK` / `CORO_EINVAL`(NULL)。 |
| `coro_rt_scope_spawn(rt, s, attr, fn, arg, cell_out)` | 登记一个子;`fn` 的返回值即子结果。`cell_out`(可选)收到 `&cell`,有效期至 destroy;`cell_out == NULL` 表示"无结果",该子**不参与聚合**。返回 `CORO_OK` / `CORO_EINVAL` / `CORO_ESTATE` / `CORO_ESTALE` / `CORO_ENOMEM`(失败时不留半登记)。与 cancel 竞速的 spawn 仍会登记(并由自己取消)其子——没有子能逃过取消。 |
| `coro_rt_scope_cancel(rt, s)` | 请求每个活子停止。幂等(`CORO_OK`)。在 act_lock 外逐个发出 `coro_rt_cancel`;scope **不写任何结果单元**。返回 `CORO_OK` / `CORO_EINVAL` / `CORO_ESTALE`(已销毁)/ `CORO_ESTATE`(已 drain)。 |
| `coro_rt_scope_wait(rt, s)` | 等待 `live == 0`,转 DRAINED,逐子 collect(EBUSY 有界重试),返回聚合。`CORO_OK` / 聚合码 / `CORO_EINVAL` / `CORO_ESTALE`。 |
| `coro_rt_scope_wait_for(rt, s, ms)` | 同 wait,带毫秒 deadline。协程调用方获恰一次认领的看门狗(在任何子登记前 fail-fast `CORO_ENOMEM`);线程调用方自行等待 deadline。返回码优先级:本 waiter 的 deadline 先命中 → `CORO_ECANCELED`;否则首个非 OK 子;否则 `CORO_OK`。 |
| `coro_rt_scope_status(s)` | 聚合值(drain 前为部分聚合)。`CORO_OK` / 聚合码 / `CORO_EINVAL` / `CORO_ESTALE`。 |
| `coro_rt_scope_cancel_requested(s)` | 取消中返回 `1`,否则 `0`(NULL 也为 `0`)。 |
| `coro_rt_scope_destroy(s)` | 释放已 drain 的 scope。`CORO_OK`(幂等)/ `CORO_EBUSY` / `CORO_EINVAL`。**不 collect**:请先以 wait 收口。 |
| `coro_rt_group_wait_all(rt, s)` | 等价 `coro_rt_scope_wait`。 |
| `coro_rt_group_wait_first_error(rt, s)` | 首个非 OK 结果即返回;取消其余子;仍等满 drain;返回该首个错误(无错误则退化为 wait_all)。 |
| `coro_rt_group_wait_first_done(rt, s, first)` | 经 `*first` 返回首个完成子的结果;scope **保持存活**,调用方**必须**随后 `coro_rt_scope_wait` 或 `coro_rt_scope_cancel`(否则 destroy 报 `CORO_EBUSY`:可检测的泄漏)。 |

## 调用方责任(两条均为契约,非建议)

1. **wait 对自身取消免疫。** 在挂起点安全点被消费的取消会先摘除等待者,wait
   继续直到子全部 drain——被打断的 wait 会让仍在运行的子无人收口。因此 wait
   **不会**因等待者自身被取消而返回 `CORO_ECANCELED`;调用方应围绕其完成做
   规划(或改为取消 scope)。

2. **cancel 的射程是 C-文档级限制,而非运行时级保证。**
   `coro_rt_scope_cancel` 只保证即时打断 running/sleeping 的子;阻塞在同步
   原语上的子仅在其等待事件发生时观察取消;若子互等且整个 scope 被取消,
   wait 可能不终止——该场景的责任归调用方。

同样强制的另外两条:

3. **`wait_for` 超时后的 drain 是无界的(结构化不变量)。** 计时已到的 wait
   仍以 `live == 0` 结束;一个永远观察不到取消的子(见第 2 条)会让该 wait
   一直等待。

4. **destroy 不 collect。** 子由 wait 族回收(`coro_rt_collect` 配 EBUSY
   有界重试:已结束但尚未 retire 的子报 `CORO_EBUSY`)。destroy 前务必先以
   wait 收口;销毁未 drain 的 scope 会遗弃其 retained 子句柄,destroy 的
   `CORO_EBUSY` 就是检测器。

## 示例

```c
#include <corocc/coro_scope.h>

static coro_rt_t* g_rt;

static coro_status work(void* arg) {
    unsigned id = (unsigned)(size_t)arg;
    /* ... */
    return id == 3 ? CORO_ESTALE : CORO_OK;
}

static void waiter(void* arg) {
    coro_rt_scope_t* s = (coro_rt_scope_t*)arg;
    coro_status agg = coro_rt_scope_wait(g_rt, s);
    (void)agg;   /* 首个非 OK 子结果,或 CORO_OK */
}

int main(void) {
    g_rt = coro_rt_create(4);

    coro_rt_scope_t s;
    memset(&s, 0, sizeof s);
    for (size_t i = 0; i < 8; ++i)
        coro_rt_scope_spawn(g_rt, &s, NULL, work, (void*)i, NULL);

    coro_rt_spawn(g_rt, NULL, waiter, &s);   /* 等待者是协程 */
    coro_rt_wait_all(g_rt);

    coro_rt_scope_destroy(&s);
    coro_rt_destroy(g_rt);
    return 0;
}
```

主线程收口同样支持:直接从线程调用 `coro_rt_scope_wait(rt, &s)`(无需协程
等待者);线程版 `wait_for` 用自己的 deadline 等待——不派发看门狗协程。

## 模型说明

- 只有一把锁:scope 复用运行时的 act_lock;状态字是 scope 上唯一的原子。
  L3 未新增任何运行时内部件。
- 结果单元的唯一写者是 scope 安装的 trampoline;被取消的子报告自己的最终值
  (约定为 `CORO_ECANCELED`),聚合读取的正是该值。不存在 EBUSY 哨兵:子合法
  返回 `CORO_EBUSY` 不会被曲解。
- 看门狗是显式 32 KiB 小栈的 retained spawn(非默认 256 KiB);绝不进子表、
  绝不触碰 `live`。
- 一个 scope 在其首次 spawn 时绑定唯一运行时。
