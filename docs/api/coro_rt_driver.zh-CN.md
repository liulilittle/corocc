# coro_rt_driver — 运行时驱动注册表、attach/detach 与 LOOP pump

> [English](coro_rt_driver.md) | 中文

本文是 `coro_rt_driver.h` 的 API 参考。它让宿主提供运行时的三项宿主需求 ——
**门铃**(wake)、**时钟**(now)、**定时器**(alarm)—— 以替代内置 EDSM 后端
(worker 线程、每 worker 定时器堆、`coro_monotonic_ns`)。未挂驱动时运行时行为
与既往完全一致(`rt->drv == NULL` 即内置路径),故为纯新增能力。

执行模型分两类:

- **SIGNAL**(无 `CORO_DRIVER_FLAG_LOOP`):worker 线程仍在;驱动只提供门铃、时钟
  与(可选)定时。内置与 SIGNAL 可在有协程 park 时热切换。
- **LOOP**(`CORO_DRIVER_FLAG_LOOP`):运行时**不**持有 worker 线程;宿主循环调用
  `coro_rt_pump` 推进。LOOP 运行时由 LOOP 默认驱动创建。

## 驱动 vtable

```c
enum coro_driver_flags { CORO_DRIVER_FLAG_NONE = 0, CORO_DRIVER_FLAG_LOOP = 1u << 0 };

typedef uint64_t (*coro_driver_now_fn)(void* ctx);
typedef void     (*coro_driver_wake_fn)(void* ctx);
typedef void     (*coro_driver_timer_fn)(void* arg);
typedef int      (*coro_driver_timer_arm_fn)(void* ctx, void* req,
                    uint64_t deadline, coro_driver_timer_fn fn, void* arg);
typedef int      (*coro_driver_timer_cancel_fn)(void* ctx, void* req);

struct coro_rt_driver_ops {
    uint32_t                    struct_size;   /* = sizeof(struct) */
    uint32_t                    flags;         /* enum coro_driver_flags */
    coro_driver_now_fn          now;           /* 可空;NULL=内置时钟 */
    coro_driver_wake_fn         wake;          /* 必填 */
    coro_driver_timer_arm_fn    timer_arm;     /* 可空 */
    coro_driver_timer_cancel_fn timer_cancel;  /* timer_arm 非空时必填 */
};
```

| 回调 | 契约 |
| --- | --- |
| `now(ctx)` | 任意线程可调的单调非递减纳秒时钟;仅差值有意义。热切换时新旧必须同尺度(已存 deadline 不做换算)。可空(NULL 用 `coro_monotonic_ns`)。 |
| `wake(ctx)` | 门铃。有新可运行工作/等待者需重扫时从任意线程调用。必须廉价、非阻塞、可并发、幂等;**禁止同步回调进运行时**(运行时调用 `wake` 时不持锁,重入会自死锁)。必填。 |
| `timer_arm(ctx, req, deadline, fn, arg)` | 在绝对 `deadline`(与 `now` 同尺度)恰好投递一次 `fn(arg)`。`req` 为运行时不透明令牌。返回 0 成功。可空。 |
| `timer_cancel(ctx, req)` | 撤销未触发的 alarm;返回后 `fn` 必不再运行(或已运行)。取消成功返回 0,已触发/未知返回非 0。`timer_arm` 非空时必填。 |
| `timer_fn(arg)` | 由驱动在其自身线程投递;运行时仅用它唤醒 worker,不得阻塞循环线程。 |

## 注册表

| 入口 | 契约 |
| --- | --- |
| `coro_rt_driver_register(name, ops, ctx, &out)` | 复制 `ops` 建立实例并进程级注册(线程安全)。`name` 可空(匿名,不可按名查找)。`ctx` 生命周期须覆盖实例使用全程。返回 `CORO_OK`;`CORO_EINVAL`(`out`/`ops` 空、`wake` 空、`struct_size` 小于本构建 vtable、timer 成对不符);`CORO_ENOMEM`;名字重复时 **`CORO_ESTATE`**(名字唯一;任何失败都把 `*out` 置 NULL)。 |
| `coro_rt_driver_unregister(drv)` | 移除并释放实例。被使用时(当前默认驱动,或已 attach 到某运行时)`CORO_EBUSY`;NULL 幂等返回 `CORO_OK`;非本注册表拥有的非空指针返回 `CORO_EINVAL`。 |
| `coro_rt_driver_find(name)` | 按名查找(空入空出);借用指针,直到注销前有效。 |
| `coro_rt_driver_set_default(drv)` | 设置 `coro_rt_create` 读取的进程默认;`drv = NULL` 复位为内置。非空且未注册返回 `CORO_EINVAL`。 |
| `coro_rt_driver_default(void)` | 当前默认,或 NULL(内置)。 |

## attach / detach / 查询

| 入口 | 契约 |
| --- | --- |
| `coro_rt_attach_driver(rt, drv)` | 挂载 `drv`(NULL=detach)。内置 ↔ SIGNAL 可在有协程 park 时切换:运行时会 quiesce worker、按新驱动重挂各自 alarm 再唤醒,故不丢 deadline。涉及 LOOP 的**跨类**切换是实时热切换(B5):threaded → LOOP 会 quiesce 全部 worker 停在 barrier,LOOP 阶段由宿主通过 `coro_rt_pump` 驱动 worker[0];LOOP → threaded 释放被停住的 worker(若运行时在 LOOP 下创建且从未有线程,此时才启动)。两个方向在途的定时 park 都不丢。仅当有 pump 正在运行(或 quiesce 超时回滚)时,进出 LOOP 的切换才返回 `CORO_EBUSY`。`CORO_EINVAL`(NULL 运行时或未注册驱动);同驱动重复挂载返回 `CORO_OK`。 |
| `coro_rt_detach_driver(rt)` | 恢复内置后端。未挂时 `CORO_OK`;在 LOOP 运行时上会把被停住的 worker 释放回线程模式(若运行时在 LOOP 下创建且从未有线程,此时才启动);有 pump 运行时 `CORO_EBUSY`。 |
| `coro_rt_attached_driver(rt)` | 当前驱动,或 NULL(内置)。 |

## LOOP pump

| 入口 | 契约 |
| --- | --- |
| `coro_rt_pump(rt, budget)` | LOOP 运行时的一轮有界推进:先收集到期项、排空 inbox,再从 worker[0] 运行队列驱动至多 `budget` 个可运行节点(`budget == 0` 抽干全部),随即返回;不阻塞。非 LOOP 运行时或已在 pump 中返回 `CORO_EBUSY`;NULL 返回 `CORO_EINVAL`。 |

LOOP 模式下运行时**不**自行挂驱动 alarm:park 的 `coro_rt_sleep` 由下一次在
`rt_now` 越过 deadline 之后的 pump 收集,故宿主循环应在其(d或之后)再次 pump ——
外部后端通过 `timer_arm` 安排这次后续 pump(见 `examples/backends/asio-pump`、
`libuv`、`libevent`)。`coro_rt_wait_all` 与 scope 线程侧等待阻塞在运行时的
settled 条件上,禁止在 pump 循环线程调用。

## 默认路径与构建开关

未挂驱动时每处分派都是一次 NULL 判定,执行原语句:门铃=活动广播,时钟=
`coro_monotonic_ns`,定时=每 worker 定时器堆。定义 `COROCC_RT_DRIVER=0`
(CMake 选项 `COROCC_RT_DRIVER=OFF`)可把该分支整体编译掉(供从不挂驱动的构建);
注册表本身仍可用。

## 后端

| 后端 | 位置 | 循环 |
| --- | --- | --- |
| Boost.Asio | `examples/backends/asio-pump` | `io_context` + `asio::post` / `steady_timer` |
| libuv | `examples/backends/libuv` | `uv_run` + `uv_async` / `uv_timer` |
| libevent | `examples/backends/libevent` | `event_base_dispatch` + `event_active` / `evtimer` |

三者均为独立 CMake 工程(不属于库构建),各自带测试。
