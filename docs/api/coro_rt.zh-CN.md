# coro_rt — M:N 运行时同步原语(L2)

> [English](coro_rt.md) | 中文

本文档是 M:N 工作窃取运行时两个 L2 同步原语的 API 参考:写者优先的读写锁
(`coro_rt_rwlock`)与 FIFO 通道(`coro_rt_channel`)。二者均定义于 `coro_rt.h`,
与运行时本体同头文件。

状态:**里程碑 M5**。M4 随交付的互斥锁、条件变量与计数信号量使用同一套等待链协议;
本文档仅覆盖 M5 新增的两个原语。

## 概念

| 术语 | 含义 |
| --- | --- |
| 持有 | 对锁的兴趣:一个写者,或任意多个读者,二者绝不同时。 |
| 写者优先 | 已排队的写者会挡住其后到达的读者,使写者不被读者流饿死。 |
| 元素 | 在通道中传递的不透明 `void*` 值。 |
| 无界 | 容量 0:环形缓冲按需增长,发送者永不阻塞。 |

两个原语的等待都在运行时**内部**挂起,而非阻塞 worker 线程:等待协程在运行时的活动锁
下追加进 FIFO 等待链并让出,释放方移交资源后重新入队。由于释放路径在同一把锁下变更状态、
并在解锁之后才唤醒等待者,唤醒不会丢失,资源不会被重复移交。无竞争的获取只需一次
compare-and-swap,或一次锁窗口;两条路径都没有轮询、没有自旋。

## coro_rt_rwlock

```c
struct coro_rt_rwlock {
    uint64_t state;    /* bit0 = 写者持有,bit1 = 写者等待,bits[2..63] 右移 2 = 读者数 */
    void*    rd_waitq; /* 读者等待 FIFO */
    void*    wr_waitq; /* 写者等待 FIFO */
};
typedef struct coro_rt_rwlock coro_rt_rwlock_t;
```

全部锁状态集中于一个原子字。读者获取是对 `state -> state + 4` 的单次
compare-and-swap,且仅在两个写者位均未置位时进行;写者获取是从自由态或写者等待态对
`state -> RW_W` 的单次 compare-and-swap,并在同一步内清掉等待位,因此读者不可能观察到
「写者位已置位而等待位尚未清除」的中间态。`RW_WW` 恰在写者持有或有写者排队期间置位,
故写者等待后到达的读者会被该位挡住。

零初始化的锁即有效、自由的锁:`coro_rt_rwlock_init` 仅用于重新初始化已用过的对象。

| 入口 | 契约 |
| --- | --- |
| `coro_rt_rwlock_init(l)` | 可选:复位为自由态。 |
| `coro_rt_rwlock_rdlock(rt, l)` | 取读持有,写者持有或等待期间挂起。 |
| `coro_rt_rwlock_tryrdlock(rt, l)` | 取读持有,绝不阻塞。 |
| `coro_rt_rwlock_rdunlock(rt, l)` | 释放读持有;最后一个读者离开时移交给最长等待的写者。 |
| `coro_rt_rwlock_wrlock(rt, l)` | 取写持有,否则挂起。 |
| `coro_rt_rwlock_trywrlock(rt, l)` | 取写持有,绝不阻塞。 |
| `coro_rt_rwlock_wrunlock(rt, l)` | 释放写持有;移交给等待写者,否则放行全部等待读者。 |

返回码:`CORO_OK`;NULL 为 `CORO_EINVAL`;阻塞入口在非运行时协程内调用为
`CORO_ESTALE`;try 路径为 `CORO_EBUSY`;无匹配持有的解锁为 `CORO_ESTATE`;
读者计数将溢出为 `CORO_ESTATE`;等待被取消为 `CORO_ECANCELED`。被取消的等待者
在放弃前会重试自身操作,故移交给它的授权不会丢失。

## coro_rt_channel

```c
struct coro_rt_channel {
    void**   slots;  /* 排队元素的环形缓冲,懒分配 */
    size_t   cap;    /* 已分配容量 */
    size_t   len;    /* 已排队元素数 */
    size_t   head;   /* 环形头索引 */
    size_t   limit;  /* 配置上限,0 = 无界 */
    uint64_t closed; /* 关闭标记 */
    void*    sendq;  /* 发送者等待 FIFO */
    void*    recvq;  /* 接收者等待 FIFO */
};
typedef struct coro_rt_channel coro_rt_channel_t;
```

通道承载不透明 `void*` 值:调用方自行负责其所指对象的生命周期。有界通道的环形缓冲
预分配,无界者按需增长。所有变更都在运行时的活动锁下进行,握手为一对一:每次成功发送
至多唤醒一个等待接收者,每次成功接收至多唤醒一个等待发送者,故已入队的元素与等待者
不会互相空等。

零初始化的通道即空的、无界的通道。

| 入口 | 契约 |
| --- | --- |
| `coro_rt_channel_init(ch, cap)` | 准备通道;`cap == 0` 表示无界。 |
| `coro_rt_channel_destroy(ch)` | 释放环形缓冲;仍有元素或等待者时拒绝(`CORO_EBUSY`)。幂等。 |
| `coro_rt_channel_send(rt, ch, v)` | 入队,有界通道满时挂起。 |
| `coro_rt_channel_trysend(rt, ch, v)` | 入队,绝不阻塞。 |
| `coro_rt_channel_recv(rt, ch, out)` | 出队,通道空时挂起。 |
| `coro_rt_channel_tryrecv(rt, ch, out)` | 出队,绝不阻塞。 |
| `coro_rt_channel_close(rt, ch)` | 关闭并放行全部等待者;已缓冲元素仍可读取。 |
| `coro_rt_channel_size(ch)` | 当前排队元素数(咨询性)。 |
| `coro_rt_channel_closed(ch)` | 通道是否已关闭。 |

返回码:`CORO_OK`;NULL 为 `CORO_EINVAL`;阻塞入口在非运行时协程内调用为
`CORO_ESTALE`;满时 `trysend`、空时 `tryrecv` 为 `CORO_EBUSY`;关闭后为
`CORO_ECLOSED`(`recv` 先排空剩余元素);无界通道无法增长为 `CORO_ENOMEM`;
等待被取消为 `CORO_ECANCELED`。

## 销毁

锁、条件、信号量与通道仅在无持有者、无等待者时方可销毁。先排空运行时
(`coro_rt_wait_all`);对已排空对象的销毁是空操作,并使对象回到零初始化状态。

## 线程安全小结

两个原语都限定在一个运行时内:其状态仅由该运行时调度的协程在活动锁下变更,
故可安全用于该运行时的任一 worker。字段均为私有,不得直接访问。

## 保证

- 互斥:读者不会观察到并发写者,两个写者也不会同时运行;状态字是双向的唯一见证。
- 写者优先:已排队的写者挡住其后到达的读者,读者流不会饿死写者。
- 授权不丢失:移交给后被取消的等待者的授权,会先被该等待者重试,故无等待者被搁置、
  无槽位被丢失。
- 唤醒不丢失:检查-挂起序列与释放-重排序列在同一把锁下进行,且每次唤醒都发生在锁外。
- 无死锁:被挂起的协程会让出其 worker,故单 worker 运行时也能推进。

## 验证

| 套件 | 证明内容 | 运行环境 |
| --- | --- | --- |
| `test_rt_rwlock` | 双向互斥、精确轮次计数、写者优先、try 与解锁返回码、取消以及双写者取消反例。 | 原生、ASan、aarch64/QEMU |
| `test_rt_channel` | MxN 生产者/消费者下无丢失、无重复、无乱序,有界与无界、try 返回码、关闭与排空、取消、销毁及其幂等。 | 原生、ASan、aarch64/QEMU |
