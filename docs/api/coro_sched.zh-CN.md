# coro_sched — 单线程调度器

> [English](coro_sched.md) | 中文

本文档是 `coro_sched.h` 的 API 参考。用 `coro_create` 创建的协程可以经
`coro_start`/`coro_resume`/`coro_yield` 手动驱动,无需调度器;本层增加的
是一条 run loop:把协程驱动到挂点为止,提供队列以把协程交给调度器而非某个
调用方、阻塞式 join,以及定时器。M:N 运行时([coro_rt.zh-CN.md](coro_rt.zh-CN.md))
是多线程版本,入口形状与本文一致。

## 模型

- **一条线程拥有 run loop。** 调度器在调用线程上创建并绑定;所有驱动与定时器
  到期都发生在该线程。跨线程唤醒(他线程的 `coro_cancel`、别处设置的定时器)
  被投递到 loop 所在线程,绝不由请求方直接执行。
- **loop 绝不空转。** 没有可运行协程时,它睡眠直到本线程收到唤醒或最近一个
  定时器到期。因此 `coro_sched_run` 调用代价低,可反复调用。
- **spawn 是延后的。** `coro_sched_spawn` 只把协程入队,spawn 调用内不执行
  任何用户代码,批量创建因此很便宜。
- **定时器是绝对时间。** 期限是来自 `coro_monotonic_ns` 的单调纳秒时间戳;
  传入 `now + delta`。

## 入口

| 入口 | 契约 |
| --- | --- |
| `coro_sched_t* coro_sched_create(void)` | 创建绑定到调用线程的 run loop。分配失败返回 NULL。 |
| `coro_status coro_sched_destroy(s)` | 释放调度器。其拥有的协程仍存活时以 `CORO_EBUSY` 拒绝——先驱动其结束(`coro_sched_join` / `coro_sched_wait_all`)。NULL 被接受并忽略。 |
| `coro_status coro_sched_spawn(s, attr, fn, arg)` | 创建协程并入队;下一轮 run-loop 开始时启动。`CORO_OK`;调度器或入口为 NULL 时 `CORO_EINVAL`;协程、其栈或队列节点无法分配时 `CORO_ENOMEM`。 |
| `coro_status coro_sched_spawn_h(s, attr, fn, arg, &out)` | 同上,并返回句柄。**句柄归调度器所有**:不得手动销毁(调度器在协程结束时释放它);调度器所有的协程也绝不能手动驱动——若下一轮 drive 输掉认领,loop 会直接销毁它。 |
| `size_t coro_sched_pending(s)` | 存活协程数(可运行 + 睡眠 + 已 join);为 0 表示调度器空闲,可以销毁。 |
| `coro_status coro_sched_run(s)` | 驱动可运行者,直到无可运行且无待发定时器才返回。因后续唤醒而驻停的协程仍归调度器管,不是留在调用内的理由。 |
| `coro_status coro_sched_join(s, c)` | 把**调用它的调度器协程**停放在目标的等待链上,目标结束时返回(主线程侧收口用 `coro_sched_wait_all` 或 `coro_sched_run`)。一个目标上的多个等待者全部被释放,且各自恰好被恢复一次。结束返回 `CORO_OK`;NULL、外来协程或等待自身返回 `CORO_EINVAL`;不在本调度器的协程内调用返回 `CORO_ESTALE`;目标在调用前已结束返回 `CORO_EDESTROYED`。 |
| `coro_status coro_sched_wait_all(s)` | 运行 loop 直到它拥有的每个协程都已结束(join 全部的便捷形式)。 |
| `coro_status coro_sched_cancel(s, c)` | 请求取消;在下一安全点被观察。睡眠中的协程由 loop 所在线程自行找到、移除其期限并重新入队,使安全点立即到来;停放在 join 上的协程维持原样,留给其释放者。任意线程均可调用。请求仅以「待处理标志 + 工作信号」跨线程传递:本调用不向其它线程传递任何调度器节点指针(或其它调度器记账),故不会触及 loop 线程正在并发回收的节点。唯一存活要求是句柄在调用期间有效,这与 `coro_sched_spawn_h` 对其返回句柄提出的要求一致。NULL 或外来调度器不拥有的协程返回 `CORO_EINVAL`。 |
| `coro_status coro_sched_sleep(s, deadline)` | 把运行中的协程停放至绝对单调时间 `deadline`。期限到返回 `CORO_OK`;被取消截断返回 `CORO_ECANCELED`;期限无法挂上时返回 `CORO_ENOMEM`(协程继续运行,未停泊);NULL 返回 `CORO_EINVAL`;协程外调用返回 `CORO_ESTALE`。 |
| `uint64_t coro_monotonic_ns(void)` | 调度器与其定时器使用的单调时钟。只有差值有意义;进程内永不回退。 |

## 示例

```c
#include <corocc/coro_sched.h>
#include <stdio.h>

static void worker(void* arg) {
    (void)arg;
    /* ... 在调度器线程上运行 ... */
}

int main(void) {
    coro_sched_t* s = coro_sched_create();
    if (!s) return 1;

    coro_sched_spawn(s, NULL, worker, NULL);
    coro_sched_wait_all(s);            /* 驱动其结束 */
    coro_sched_destroy(s);
    return 0;
}
```

需要等待特定句柄的协程可在 loop 内 join:

```c
static void waiter(void* arg) {
    coro_t* c = (coro_t*)arg;
    if (coro_sched_join(g_sched, c) == CORO_OK) {
        /* c 已结束 */
    }
}
```

类型化的 C++ 封装(`corocc::scheduler` + `corocc::sched_task<R>`,见
[coro_cpp.zh-CN.md](coro_cpp.zh-CN.md))把同样的流程与结果、异常打包在一起。

## 复用与认领

调度器用与 `coro_start`/`coro_resume` 相同的凭证协议驱动协程:每次驱动一次
认领,过期认领一律拒绝;被定时器唤醒的协程是**重新入队**而非被直接切换,
因此 run loop 始终是该协程唯一的驱动者。

## 线程安全

| 入口 | 线程安全 |
| --- | --- |
| `coro_sched_create`、`coro_sched_destroy`、`coro_sched_run`、`coro_sched_wait_all` | 仅限拥有它的线程。 |
| `coro_sched_spawn`、`coro_sched_spawn_h` | 拥有它的线程(协程被入队到该 loop)。 |
| `coro_sched_pending` | 任意线程;咨询性读取。 |
| `coro_sched_join` | 本调度器调度的协程内部(停放该协程)。 |
| `coro_sched_cancel` | 任意线程;请求投递到 loop 所在线程,协程在下一安全点观察。 |
| `coro_sched_sleep` | 本调度器上的协程内部。 |
| `coro_monotonic_ns` | 纯函数;任意线程。 |
