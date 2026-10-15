# coro_rt — M:N 运行时与同步原语(L2)

> English | [中文](coro_rt.zh-CN.md)

本文档是 `coro_rt.h` 的 API 参考:M:N 工作窃取运行时,以及与之同头提供的
同步原语——协程互斥锁、条件变量、计数信号量、写者优先读写锁与 FIFO 通道。
所有原语都是完整类型、可按值内嵌、零初始化即可用;运行时本身是不透明句柄。

协程对象(L1)与运行时是两回事:用 `coro_create` 创建的协程可以在没有运行时
的情况下由任意线程手动驱动(见 [coro.zh-CN.md](coro.zh-CN.md))。运行时把
一组协程变成并发负载:worker 线程池共享可运行协程集合,某 worker 队列枯竭时
去窃取工作。

## 运行时

```c
coro_rt_t* rt = coro_rt_create(2);       /* 2 个 worker;0 = 库默认 */
...
coro_rt_wait_all(rt);                    /* 排空 */
coro_rt_destroy(rt);
```

| 入口 | 契约 |
| --- | --- |
| `coro_rt_create(nthreads)` | 用 `nthreads` 个 OS worker 线程创建运行时(0 选硬件线程数,有上限;失败返回 NULL)。 |
| `coro_rt_spawn(rt, attr, fn, arg)` | 创建协程并排队;它在稍后某个 worker 上开始,不在本次调用内运行。结束时运行时回收全部资源(句柄、栈、记账),调用方无需清理。`CORO_OK` / `CORO_EINVAL` / `CORO_ENOMEM`。同一运行时的提交不在内部串行化:多线程提交需在外部自行串行化;复用自运行时池的栈不保证零初始化([threading.zh-CN.md](../threading.zh-CN.md))。 |
| `coro_rt_spawn_h(rt, attr, fn, arg, &out)` | 同上,另返回句柄以便手动管理该协程(查询、取消、join)。结束后用 `coro_rt_collect`(或 `coro_destroy`)释放。 |
| `coro_rt_collect(rt, c)` | 回收受管 spawn:释放句柄、栈与记账。每个 `coro_rt_spawn_h` 恰好一次,如同手动协程的 `coro_destroy`。协程仍存活**或已结束但尚未 retire** 时返回 `CORO_EBUSY`——重试(在协程内先 yield;绝不空转)。可在 `coro_rt_destroy` 之后调用(该路径不触碰运行时)。默认分配器的栈会回到运行时上界受限的复用池而不是解除映射(自定义分配器句柄经其分配器释放)。 |
| `coro_rt_wait_all(rt)` | 运行时自身的排空:worker 持续调度,直到无可运行项、无待发定时器、无中途协程。停在永远等不到的唤醒上的协程会像阻塞 join 一样让运行时保持存活。 |
| `coro_rt_join(rt, c)` | 把**调用它的运行时协程**停放至目标结束;一个目标上的多个等待者全部被释放,各自恰好恢复一次。普通线程或 join 自身句柄返回 `CORO_EAGAIN`;目标已结束(无论句柄是否已回收)返回 `CORO_EDESTROYED`;等待被取消且目标未结束返回 `CORO_ECANCELED`。 |
| `coro_rt_pending(rt)` | 存活协程数(可运行 + 睡眠 + 阻塞,未结束)。为 0 表示运行时空闲、可以安全销毁。 |
| `coro_rt_sleep(rt, deadline)` | 把运行中的协程停放至绝对单调时间 `deadline`(传入 `now + delta`)。取消提前唤醒时返回 `CORO_ECANCELED`;协程外调用返回 `CORO_ESTALE`。 |
| `coro_rt_yield(void)` | 主动让出 CPU:协程回到可运行集合的尾部,稍后被再次调度。协作式轮转。协程外调用返回 `CORO_ESTALE`。 |
| `coro_rt_cancel(rt, c)` | 请求协程在下一安全点停止;睡眠中的协程会被移除期限并重新入队,使安全点立即到来。请求绝不强制切换。任意线程安全。 |
| `coro_rt_destroy(rt)` | 停止全部 worker 并释放运行时。仍有提交未结束的协程时返回 `CORO_EBUSY`;先跑 `coro_rt_wait_all`。仅由未回收的 `coro_rt_spawn_h` 句柄保活的协程不阻止销毁。成功销毁后运行时句柄失效。destroy 具有排他性:并发调用 `coro_rt_spawn`/`coro_rt_spawn_h` 或 `coro_rt_wait_all` 属调用方违规;destroy 会释放复用池仍驻留的全部栈。 |

身份在调度中保持:spawn 到运行时的协程仍可经其 `coro_t` 句柄查询、取消或
join;凭证规则同样适用(至多一条执行流位于协程内),过期 resume 一律拒绝、
绝不重放。

## 等待链协议

五个同步原语都在运行时内停放,而不是阻塞 worker:等待协程在运行时的活动锁下
追加到 FIFO 链并 yield,释放方交接资源并重新入队。由于释放路径在同一把锁下
变更状态、并在释放锁之后才唤醒等待者,唤醒不会丢失,资源也不会被双份交出。
无竞争获取只需一次 compare-and-swap(或单个锁窗口);两条路径都没有轮询、
没有自旋。

阻塞型入口仅限协程(普通线程调用返回 `CORO_ESTALE`);`try` 型入口绝不停放、
任何上下文可调。停放的协程让出 worker,因此单 worker 运行时不会在这些原语上
死锁。

## coro_rt_mutex

```c
struct coro_rt_mutex {
    uint64_t locked; /* 0 空闲,1 被持有(原子) */
    void*    waitq;  /* 停放协程的 FIFO 头 */
};
typedef struct coro_rt_mutex coro_rt_mutex_t;
```

| 入口 | 契约 |
| --- | --- |
| `coro_rt_mutex_init(m)` | 可选:零初始化的互斥锁已可用;仅在复用已用对象时调用。 |
| `coro_rt_mutex_lock(rt, m)` | 获取;被持有时在运行时内停放。`CORO_OK`;运行时协程外 `CORO_ESTALE`;NULL `CORO_EINVAL`。 |
| `coro_rt_mutex_trylock(rt, m)` | 单次裸 compare-and-swap;绝不停放,也绝不报 `CORO_ESTALE`。被持有返回 `CORO_EBUSY`。 |
| `coro_rt_mutex_unlock(rt, m)` | 释放;把锁交给最长等待者(若有)并重新入队。调用方未持有返回 `CORO_ESTATE`;运行时协程外 `CORO_ESTALE`。 |

## coro_rt_cond

```c
struct coro_rt_cond {
    void* waitq; /* 停放协程的 FIFO */
};
typedef struct coro_rt_cond coro_rt_cond_t;
```

| 入口 | 契约 |
| --- | --- |
| `coro_rt_cond_init(c)` | 可选,同互斥锁。 |
| `coro_rt_cond_wait(rt, c, m)` | 原子地释放 `m`、在 `c` 上停放、返回前重新获取 `m`(被唤醒者经同一次交接重获锁,因此单 worker 运行时不会死锁)。等待者按 FIFO 顺序被恢复;不产生虚假唤醒,但调用方仍应重查谓词。运行时协程外 `CORO_ESTALE`。 |
| `coro_rt_cond_notify(rt, c)` | 唤醒最长等待的协程(若有)。任意线程安全。 |
| `coro_rt_cond_broadcast(rt, c)` | 唤醒全部等待协程。任意线程安全。 |

## coro_rt_sem

```c
struct coro_rt_sem {
    uint64_t count; /* token 计数(原子) */
    void*    waitq; /* 停放协程的 FIFO */
};
typedef struct coro_rt_sem coro_rt_sem_t;
```

| 入口 | 契约 |
| --- | --- |
| `coro_rt_sem_init(s, count)` | 以初始 token 数(`>= 0`)准备信号量。零初始化对象即计数 0。 |
| `coro_rt_sem_wait(rt, s)` | 消费一个 token,或在 post 前停放。`CORO_OK`;运行时协程外 `CORO_ESTALE`。 |
| `coro_rt_sem_trywait(rt, s)` | 消费 token 且绝不阻塞。计数为 0 返回 `CORO_EBUSY`。 |
| `coro_rt_sem_post(rt, s)` | 释放一个 token 并唤醒最长等待者(若有)。任意线程安全(Boost 集成使用的单发唤醒通道)。 |
| `coro_rt_sem_count(s)` | 当前 token 数(咨询性)。 |

## coro_rt_rwlock

```c
struct coro_rt_rwlock {
    uint64_t state;    /* bit0 = 写者持有,bit1 = 写者等待,bits[2..63] >> 2 = 读者数 */
    void*    rd_waitq; /* 停放读者的 FIFO */
    void*    wr_waitq; /* 停放写者的 FIFO */
};
typedef struct coro_rt_rwlock coro_rt_rwlock_t;
```

整个锁状态集中在一个原子字里。读者获取是在两个写者位均未置位时才执行的
`state -> state + 4` 单次 compare-and-swap;写者获取是从空闲或写者等待态到
`state -> RW_W` 的单次 compare-and-swap,同一步清掉等待位,因此读者绝不可能
观察到"写者位已置位而等待位尚未清掉"的瞬间。`RW_WW` 恰在写者持有锁或写者
排队时置位,所以写者等待期间到达的读者会被该位挡住。

零初始化锁即为有效空闲锁:`coro_rt_rwlock_init` 只用于重新初始化已用对象。

| 入口 | 契约 |
| --- | --- |
| `coro_rt_rwlock_init(l)` | 可选:复位为空闲态。 |
| `coro_rt_rwlock_rdlock(rt, l)` | 取读持有;写者持有或等待时停放。 |
| `coro_rt_rwlock_tryrdlock(rt, l)` | 取读持有,绝不阻塞。 |
| `coro_rt_rwlock_rdunlock(rt, l)` | 释放读持有;最后一个离开的读者把锁交给最长等待的写者。 |
| `coro_rt_rwlock_wrlock(rt, l)` | 取写持有;否则停放。 |
| `coro_rt_rwlock_trywrlock(rt, l)` | 取写持有,绝不阻塞。 |
| `coro_rt_rwlock_wrunlock(rt, l)` | 释放写持有;交给等待的写者,否则释放全部等待读者。 |

返回码:`CORO_OK`;NULL 为 `CORO_EINVAL`;阻塞型入口在运行时协程外调用为
`CORO_ESTALE`;try 路径为 `CORO_EBUSY`;无匹配持有的解锁为 `CORO_ESTATE`;
读者计数将溢出为 `CORO_ESTATE`;等待被截断为 `CORO_ECANCELED`。被取消的
等待者在放弃之前会重试自己的操作,因此交给它的授予绝不丢失。

## coro_rt_channel

```c
struct coro_rt_channel {
    void**   slots;  /* 排队项的 ring,惰性分配 */
    size_t   cap;    /* 已分配 ring 容量 */
    size_t   len;    /* 排队项数 */
    size_t   head;   /* ring 头索引 */
    size_t   limit;  /* 配置上限,0 = 无界 */
    uint64_t closed; /* 关闭标志 */
    void*    sendq;  /* 停放发送者的 FIFO */
    void*    recvq;  /* 停放接收者的 FIFO */
};
typedef struct coro_rt_channel coro_rt_channel_t;
```

通道承载不透明 `void*` 值:其指向对象的生命周期归调用方。有界通道预分配
ring,无界通道按需增长。每次变更都在运行时的活动锁下进行,握手是一对一的:
每次成功 send 至多唤醒一个停放的接收者,每次成功 recv 至多唤醒一个停放的
发送者,因此排队的值与停放的等待者绝不会互相干等。

零初始化通道即空的无界通道。有界通道最小容量为一;容量 0 表示**无界**,不是
汇合(rendezvous)通道。

| 入口 | 契约 |
| --- | --- |
| `coro_rt_channel_init(ch, cap)` | 准备通道;`cap == 0` 表示无界。 |
| `coro_rt_channel_destroy(ch)` | 释放 ring;仍有排队项或等待者时拒绝(`CORO_EBUSY`)。幂等。 |
| `coro_rt_channel_send(rt, ch, v)` | 入队;有界通道满时停放。 |
| `coro_rt_channel_trysend(rt, ch, v)` | 入队,绝不阻塞(满时 `CORO_EBUSY`)。 |
| `coro_rt_channel_recv(rt, ch, out)` | 出队;通道为空时停放。 |
| `coro_rt_channel_tryrecv(rt, ch, out)` | 出队,绝不阻塞(空时 `CORO_EBUSY`)。 |
| `coro_rt_channel_close(rt, ch)` | 关闭并释放全部等待者;已缓冲项仍可接收。任意线程安全;幂等。 |
| `coro_rt_channel_size(ch)` | 当前排队项数(咨询性)。 |
| `coro_rt_channel_closed(ch)` | 通道是否已关闭。 |

返回码:`CORO_OK`;NULL 为 `CORO_EINVAL`;阻塞型入口在运行时协程外调用为
`CORO_ESTALE`;满时 `trysend` 或空时 `tryrecv` 为 `CORO_EBUSY`;关闭后为
`CORO_ECLOSED`(`recv` 先排空剩余项);无界通道无法增长为 `CORO_ENOMEM`;
等待被截断为 `CORO_ECANCELED`。

## 销毁

互斥锁、条件变量、信号量、读写锁与通道只能在无人持有、无人在其上等待时销毁。
先排空运行时(`coro_rt_wait_all`);对已排空对象的销毁是空操作,对象保持零
初始化。

## 线程安全小结

线程安全以单个运行时为作用域:原语状态只由该运行时调度的协程在其活动锁下
变更,因此可以安全地用于该运行时的任意 worker。字段私有,不得直接触碰。
运行时自身入口的线程表见 [threading.zh-CN.md](../threading.zh-CN.md)。

## 保证

- 互斥:读者绝不可能观察到并发写者,两个写者绝不并行;状态字是两个方向的
  唯一见证。
- 写者优先:排队的写者挡住其后到达的读者,读者风暴无法饿死写者。
- 交接不丢失:被取消的等待者若已获得授予,会在放弃前重试,没有等待者被搁浅、
  没有槽位丢失。
- 唤醒不丢失:检查-停放序列与释放-重入队序列在同一把锁下运行,且每次唤醒都
  发生在锁外。
- 无死锁:停放的协程让出 worker,单 worker 运行时依然前进。

## 验证

| 套件 | 证明什么 | 运行于 |
| --- | --- | --- |
| `test_rt_mn` | M:N 调度本身:spawn、工作窃取、`wait_all`、pending 计数、睡眠期限与单 worker 无死锁契约。 | 原生、ASan、aarch64/QEMU |
| `test_rt_cancel_park` | 在停放安全点被消费的取消:每个停放点都退掉等待状态,协程保持可用、绝不被静默丢失(定时器、mutex/cond/sem、channel、join)。 | 原生、ASan、aarch64/QEMU |
| `test_rt_sync` | 互斥锁、条件变量与信号量的互斥、FIFO 唤醒顺序、交接与取消。 | 原生、ASan、aarch64/QEMU |
| `test_rt_rwlock` | 双向互斥、精确轮次、写者优先、try 与解锁返回码、取消与双写者取消场景。 | 原生、ASan、aarch64/QEMU |
| `test_rt_channel` | MxN 生产者/消费者下不丢、不重、不乱序,有界与无界,try 返回码,关闭与排空,取消,销毁及其幂等。 | 原生、ASan、aarch64/QEMU |
