# threading — 线程模型、迁移与线程安全

> [English](threading.md) | 中文

本文档是 corocc 的并发参考:哪条线程可以调用什么、协程如何在线程之间移动、
协程局部槽保证什么,以及每一层的线程安全表。逐 API 契约仍在各层参考中
([coro.zh-CN.md](api/coro.zh-CN.md)、[coro_l0.zh-CN.md](api/coro_l0.zh-CN.md)、
[coro_rt.zh-CN.md](api/coro_rt.zh-CN.md)、
[coro_sched.zh-CN.md](api/coro_sched.zh-CN.md)、
[coro_scope.zh-CN.md](api/coro_scope.zh-CN.md));本页把线程规则集中在一处。

## 一段话模型

协程是调度实体,不是线程:它拥有栈和保存帧,当某条线程出示其有效凭证去
**驱动**它时运行。认领是状态字上的单次 compare-and-swap,因此至多一条执行流
位于协程之内;认领失败会得到明确回报(运行中/切换中为 `CORO_EBUSY`,过期凭证
为 `CORO_ESTALE`),绝不重试。设计中没有任何东西把协程绑在线程上:没有属主
字段,因为不需要。

## 哪条线程可以驱动

任何持有协程有效凭证的线程都可以驱动它:

```c
/* 线程 A 创建了协程;把 token 交给线程 B */
coro_token tok = /* 来自 coro_start / 最近一次 coro_resume */;
coro_token next = CORO_TOKEN_NONE;
coro_status s = coro_resume(worker, tok, &next);   /* 一次认领,一次驱动 */
```

- `coro_start`、`coro_resume`:持凭证的任意线程。对同一凭证的并发认领由
  compare-and-swap 裁决;恰好一个调用方进入协程。
- `coro_get_state`、`coro_join`、`coro_cancel`、`coro_local_get/set`:任何能
  看到句柄的线程。
- `coro_destroy`:任意线程;协程正在运行或切换中时以 `CORO_EBUSY` 拒绝,
  无论发生在哪条线程。
- `coro_yield`(以及入口返回路径):把控制权交还给**驱动当前这一轮运行的
  线程**——驱动路径记录了恢复者。因此把协程交给另一线程,只需把凭证交给它。

协程局部槽(`coro_local_get/set`,共 `CORO_LOCAL_SLOTS` 个)存在协程上而非
线程上:跨挂起存活,并随协程一起跨线程交接,且绝不触碰 `thread_local`。

## 线程级唤醒队列

自己管理一组协程的线程可以经本线程队列领取唤醒请求,而不是被就地驱动:

| 入口 | 契约 |
| --- | --- |
| `coro_pump()` | 清扫排队给调用线程队列的全部请求;返回服务数量。非阻塞;队列为空返回 0。 |
| `coro_pump_wait()` | 先睡到有请求到达,再清扫。所属线程的阻塞式服务循环;绝不轮询。 |

因为一次清扫可能服务多个请求,需要服务已知数量请求的调用方应同时统计两者
返回值。投递给已退出线程的请求以 `CORO_EDESTROYED` 失败而不是丢失(该线程的
退出钩机会把仍排队的请求全部置失败),因此请求方一定会被释放。

## M:N 运行时

`coro_rt_t` 是一组共享可运行协程的 worker 线程池。其入口的线程安全规则:

| 入口 | 线程安全 |
| --- | --- |
| `coro_rt_create` | 构造它的线程。 |
| `coro_rt_spawn`、`coro_rt_spawn_h` | 管理运行时的线程;多线程提交时由调用方在外部串行化。提交后协程在 worker 池上运行。 |
| `coro_rt_destroy`、`coro_rt_wait_all` | 管理线程(先排空再销毁;有存活协程时报 `CORO_EBUSY`)。destroy 具有排他性:进行期间并发提交或等待属调用方违规(见下方栈复用说明)。 |
| `coro_rt_collect` | 任意线程,针对已完成句柄——包括在 `coro_rt_destroy` 之后(该路径只触碰协程句柄与其记账节点)。 |
| `coro_rt_cancel` | 任意线程。请求落在目标的状态字上;若目标停在期限上,则经唤醒路径交给它的 worker。 |
| `coro_rt_join` | 本运行时调度的协程内部(停放调用者);普通线程得到 `CORO_EAGAIN`。 |
| `coro_rt_pending` | 任意线程;咨询性读取。 |
| `coro_rt_sleep`、`coro_rt_yield` | 运行时协程内部(其外返回 `CORO_ESTALE`)。 |

每个运行时的同步原语(`coro_rt_mutex`、`coro_rt_cond`、`coro_rt_sem`、
`coro_rt_rwlock`、`coro_rt_channel`)作用域限于该运行时:

- 阻塞型入口仅限协程调用:普通线程调用返回 `CORO_ESTALE`,而不是阻塞一个
  非 worker。
- `try` 型入口绝不停放,任何上下文可调(被持有的锁对普通线程与对协程一样
  返回 `CORO_EBUSY`)。
- `coro_rt_cond_notify`、`coro_rt_cond_broadcast`、`coro_rt_channel_close`、
  `coro_rt_mutex_trylock`、`coro_rt_sem_trywait`、`coro_rt_sem_post` 明确
  可从任意线程调用。
- worker 永不被阻塞:需要等待的协程在运行时内停放并让出 worker,因此单
  worker 运行时依然前进(也不会在这些原语上死锁)。

### 协程栈复用

已结束协程的栈不会立刻解除映射,而是经每运行时一个上界受限的池回收,之后的
默认尺寸 spawn 可以复用其一。三条后果属于契约的一部分:

- **复用栈不保证零初始化。** 同一进程(同一信任域)内,协程读取自己未初始化
  过的栈内存时,可能读回上一个已结束协程留在回收栈上的字节。不要把新栈内存
  当成零;照常先初始化。
- **池的范围与寿命。** 只有内置分配器、可用尺寸不小于默认值
  (`CORO_DEFAULT_STACK_SIZE`)的栈会被驻留;更小的自定义栈立即释放。只有默认
  尺寸的 spawn 会从池中取用(驻留项可能大于请求尺寸);retained 与自定义
  分配器路径绝不进入该池。`COROCC_RT_STACK_CACHE_MAX`(编译期;默认 256)限定
  每个运行时驻留映射的数量——超出部分正常释放——`coro_rt_destroy` 释放仍然
  驻留的全部栈。
- **destroy 具有排他性。** 在 `coro_rt_destroy` 进行期间调用 spawn
  (`coro_rt_spawn`、`coro_rt_spawn_h`)或 wait(`coro_rt_wait_all`)属调用方
  违规,禁止使用:必须先把提交方与等待方静默下来。多线程提交同样不在内部
  串行化(见上表)。

调度器层按设计更严格——它绑定到创建它的线程;线程安全表见
[coro_sched.zh-CN.md](api/coro_sched.zh-CN.md)。

## thread_local 与协程

`thread_local` 存储是**线程**的属性,不是协程的属性:

- 从未离开其线程的协程,看到的 TLS 值与任何普通函数无异。
- 交接给另一线程后,驱动线程改变,协程内观察到的 `thread_local` 值随之改变。
  必须跨挂起与迁移存活的应放进协程局部槽(`coro_local_get/set`)或协程自己的
  栈帧。
- **s390x 例外:** s390x 上协程体内完全不要依赖 `thread_local`。gcc 的 s390x
  TLS 访问会把线程指针(访问寄存器 a0/a1)物化进被调用者保存的通用寄存器,
  并可能在同一函数内的多次调用间复用;被另一线程恢复的上下文会解引用挂起它
  的那条线程的 TLS 槽(该线程一旦退出即崩溃)。s390x 工具链因此使用
  `-fno-stack-protector`(金丝雀检查会读同一缓存指针)。其他 ABI 不受影响。

## 协程内的阻塞调用

协程是协作式的:从不 yield 的协程无法被取消或抢占。由此对协程体内调用的
第三方代码有两条规则:

- **不要阻塞 worker**:在同一运行时还有排队协程时用 OS 原语
  (`pthread_mutex_lock`、`WaitForSingleObject`、阻塞 I/O 等)会卡住一个调度
  槽。应使用运行时自身的原语(`coro_rt_mutex`、`coro_rt_sem`、
  `coro_rt_channel`、`coro_rt_sleep`),它们停放协程并让出 worker。
- **L0 的 `coro_ctx_swap` 调用期间不得持锁**,也不得让 C++ 异常穿越它
  ([coro_l0.zh-CN.md](api/coro_l0.zh-CN.md))。高层已经遵守;手写 L0 代码
  同样必须遵守。

取消在每一层都保持协作式:协程停在定时器上时记录的取消请求会提前唤醒它;
运行中记录的请求在下一安全点被观察。Boost.Asio 集成有一个已记录的特殊点:
在等待 Asio 完成期间观察到的取消不会截断等待——未完成的处理器必须在协程帧
仍然存活时被允许跑完([boost.zh-CN.md](api/boost.zh-CN.md))。

## 各层参考索引

| 层 | 线程安全章节 |
| --- | --- |
| L0 原语 | [coro_l0.zh-CN.md](api/coro_l0.zh-CN.md) |
| L1 协程对象 | [coro.zh-CN.md](api/coro.zh-CN.md) |
| L2 运行时与原语 | [coro_rt.zh-CN.md](api/coro_rt.zh-CN.md) |
| 调度器 | [coro_sched.zh-CN.md](api/coro_sched.zh-CN.md) |
| scope | [coro_scope.zh-CN.md](api/coro_scope.zh-CN.md) |
| C++ 封装 | [coro_cpp.zh-CN.md](api/coro_cpp.zh-CN.md) |
