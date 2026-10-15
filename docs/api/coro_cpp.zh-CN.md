# coro_cpp -- C++11 封装层(header-only)

> [English](coro_cpp.md) | 中文

本文档是 `include/corocc/coro.hpp` 的 API 参考:C++11 单头封装层。它与 C
API **平行**:只增加 RAII 所有权、类型化结果、chrono 时间与异常"捕获-交付"
边界,不发明第二套调度/取消机制;一切行为都建立在
[coro.md](coro.md)、[coro_rt.md](coro_rt.md)、[coro_scope.md](coro_scope.md)
记录的 C 入口之上。C 消费者零成本:不包含该头的 C 翻译单元逐字节不受影响。

以下构建规则属于契约本身:

- **C++11 基线**:头内不使用任何 C++14+ 设施;以
  `-std=c++11 -pedantic-errors -Wall -Wextra -Werror` 验收,并以
  `-std=c++14/17/20` 做前向验收。MSVC 无 C++11 模式:MSVC 侧基线表述为
  "实现不使用 C++11 之后特性",以 `/std:c++14 /W4 /WX` 验收。
- **`COROCC_CPP_NO_EXCEPTIONS`** 强制降级模式(无 `throw`;`get()`/
  `wait_or_throw()` 删除);未定义时按 `__cpp_exceptions`/`__EXCEPTIONS`/
  `_CPPUNWIND` 自动检测。
- **同二进制同一版本**:`coro.hpp` 即实现,同一二进制的全部 TU 必须包含
  同一版本,且异常模式宏必须一致——混编即 ODR 违反。头内唯一共享状态
  (detached 异常处理器槽)是函数局部 static:**以模块为界**(EXE 与各 DLL
  各一份)。
- 链接方式不变(仍是既有的 `libcorocc.a`/`corocc.lib`):封装不新增符号、
  不新增库、不新增构建步骤。

## 概念表

| 术语 | 含义 |
| --- | --- |
| `corocc::status` | `coro_status` 零开销包装:`ok()/failed()/code()/message()`;双向隐式转换。 |
| `corocc::attrs` | 创建属性构建器(`stack_size`、**拥有**的 `name`、`allocator`);`flags` 不暴露(C 只接受 `CORO_F_NONE`)。 |
| `corocc::monotonic_clock` | `coro_monotonic_ns()` 的 C++11 Clock(ns `duration`,steady)。 |
| 控制块(box) | 每次 spawn 的包装自有堆对象:callable、名字的拥有副本、共享结果状态、哑 C cell 别名。 |
| 快照(`state<R>`) | trampoline 在子结束时发布:码、异常、类型化值、就绪旗标。**包装层的一切读取都走这里**,永不读 C cell。 |
| trampoline | C 层调用的入口:运行用户 callable、捕获一切逃逸、发布快照,**绝不让异常穿越上下文切换**。 |
| detached 处理器 | `detached_exception_handler`——无句柄 `runtime::spawn` 的异常去向;默认 `std::terminate`。 |

## 所有权与寿命

- 每次 spawn 分配**一个包装控制块**(`new (std::nothrow)`,类型化 spawn 另加
  一个共享结果状态)。包装自有元数据 OOM 归一 `CORO_ENOMEM`;**用户**
  callable/参数的存储异常在调用方传播(`std::thread` 规则);一处细化:
  **包装内部**移动/存储 callable 或实参时抛出的 `std::bad_alloc` 归一
  `CORO_ENOMEM`,调用点(实参传入)仍按传播规则。不存在零分配
  C++ spawn——逃生舱是经 `native()` 直接用 C API。
- `coroutine`、`runtime`、`scheduler`、`task`、`sched_task` 是**唯一所有者**
  (move-only)。`cell<R>` 是共享句柄;`scope` 不可拷贝不可移动(C 记录持有
  指向它的回指)。
- **对象寿命 ≤ 其 runtime/scheduler**:触碰 runtime 内部的 `task`/`scope`
  操作(`cancel`、活目标的 `join`/`wait`)必须先于 runtime/scheduler 析构。
  唯一例外是 destroy 之后的 `coro_rt_collect`(D25/V4),包装层的收口路径
  只对"已完成"目标触碰它。
- 析构一律 **drain**:`runtime`/`scheduler` 执行 `wait_all()` + `destroy()`;
  `scope` 执行 `cancel()` + `wait()` + `destroy()`;`task` 取消失败目标并
  收口。drain 可能无界——永远跑不起来的子会让它阻塞(`scheduler` 析构遇
  "parked 子"时 `destroy` 返回 `EBUSY`,析构将其视为**调用方红线**)。
  这与 C 层的调用方责任同源。
- 析构 **SUSPENDED** `coroutine` 合法但**静默遗弃协程帧**:栈上 C++ 对象的
  析构**不会运行**。重要场合请协作式收口:`cancel()` 后驱动 `resume()` 至
  `done()`。debug 构建对遗弃计数(从不 abort)。

## 入口清单

### 值类型

| 入口 | 契约 |
| --- | --- |
| `status(coro_status)` / `operator coro_status` | 双向隐式;`static_assert(sizeof(status)==sizeof(coro_status))`。 |
| `status::ok()/failed()/code()/message()` | `noexcept`;`message()`=`coro_status_str`。 |
| `corocc::error(coro_status)` | 携带状态的 `std::exception`;`what()`=`coro_status_str`。仅在收口点抛出。 |
| `attrs::stack_size(n)/name(s)/allocator(p)` | 链式;`name` 被拥有,临时 `attrs` 交给 spawn 也安全。 |
| `monotonic_clock::now()` | `coro_monotonic_ns()` 的 `time_point`;steady。 |
| `set_detached_exception_handler(h)` | 安装 `h`(null 恢复默认 `std::terminate`),返回旧处理器。 |

### `coroutine`(L1 手动驱动)

| 入口 | 契约 |
| --- | --- |
| `static create(out, f, attrs)` | callable 仅可返回 `void` 或 `coro_status`(void 即 OK)。`out` 非空 → `CORO_ESTATE`。返回 C 集,元数据 OOM 为 `ENOMEM`。 |
| `start()/resume()` | 用内部凭证驱动(单驱动者纪律;高级凭证协议走 `native()`)。返回 C 集。 |
| `cancel()/join()` | C 面(`join`:已死 OK,存活 `EAGAIN`)。 |
| `state()/done()/valid()/native()` | 只读。 |
| `exception()` | 捕获的体异常(未抛出或未结束则为 null)。 |
| `destroy()` | 显式释放;`EBUSY`(运行中)原样返回;二次调用空句柄 `EINVAL`。 |

### `runtime` 与 `task<R>`

| 入口 | 契约 |
| --- | --- |
| `runtime(nthreads)` / `static create(out,n)` / `valid()/native()` | `nthreads==0` 选 C 默认;构造失败即 `valid()==false`(`create` 返回 `CORO_ENOMEM`)。 |
| `spawn(f [, args])` | 分离式:runtime 回收一切;逃逸异常交给 detached 处理器。 |
| `spawn_h(task<R>&, f [, args])` | 受管类型化任务。失败时 `out` 不变;`out` 非空时先收口旧任务。 |
| `pending()/wait_all()/cancel(c)/destroy()` | C 面。 |
| `sleep_for(d)/sleep_until(tp)` | 仅协程内(`ESTALE` 于线程);时长 ceil+饱和;负值/已过期零时长。 |
| `task::wait()` | **目标定向收口**。已完成 → collect 有界重试(协程内 yield,线程侧 1ms)。未完成:协程内 `join`(park 至目标终了;等待**自身**句柄即 self-join,返回 `EAGAIN`);线程侧 1ms collect 重试(5000 次≈5s),耗尽返回 `EBUSY`(可稍后重试)。**永不触碰 `wait_all`**——无关 parked 任务拖不死它。已死目标归一 OK。 |
| `task::join()` | C `coro_rt_join` 原样:协程内 park;线程侧 `EAGAIN`;`EDESTROYED`/`ECANCELED` 同 C。 |
| `task::cancel()` | C 请求;`OK`/`EINVAL`。 |
| `task::get()` | 已完成 → 取值或重抛;否则先 `wait()`;wait 失败抛 `corocc::error`(携带其状态)。**异常通道优先于码通道**。取值会把快照中的结果移出:再次调用读到的是已搬移值。 |
| `task::peek()/exception()/done()` | 就绪门控的快照读取;done 后任意线程只读。 |

`R==void` 与 `R==coro_status` 有专门存储(`coro_status` 不双存);
`task<void>::get()` 仅重抛。

### `scheduler` 与 `sched_task<R>`

| 入口 | 契约 |
| --- | --- |
| `static create(out)/run()/wait_all()/pending()/destroy()` | 绑定创建线程;debug 构建跨线程使用即 abort。 |
| `spawn/spawn_h/sleep_for/sleep_until/cancel` | 同上,作用于单线程 run loop。 |
| `sched_task::wait()` | done → `OK`(句柄已退休,**绝不触碰 C**);否则在调度器协程内 `coro_sched_join`(否则 `ESTALE`),`EDESTROYED` 归一 `OK`,`ECANCELED` 同 C。 |
| `sched_task::cancel()` | done → `OK` 不触 C;非绑定线程 → `ESTALE` 不触 C;否则 C 请求。 |
| `sched_task::get()/peek()/exception()` | 同 `task`;C 句柄归调度器(retire 即毁),包装层 done 旗标挡在每次触碰之前。 |

### `scope` 与 `cell<R>`

| 入口 | 契约 |
| --- | --- |
| `scope(rt)` | 绑定 runtime(或裸 `coro_rt_t*`);`native()` 即 C 记录。 |
| `spawn(f [, args])` | 非类型化子:callable 返回 `void` 或 `coro_status`。**每个子(含非类型化)都注册非 NULL `cell_out`**——异常终结对 C 聚合可见并驱动 `wait_first_error` 早退。 |
| `spawn(cell<R>&, f [, args])` | 类型化子;`out` 仅成功时绑定。 |
| `wait()/wait_all()/wait_first_error()` | C drain;返回聚合/首个错误。 |
| `wait_first_done(first)` | 首个完成;scope 保持存活——**调用方必须跟进 `wait()` 或 `cancel()`**。 |
| `wait_for(d)/wait_until(tp)` | 带期限形式;ceil+饱和;`<=0` 钳到"查一次"期限。 |
| `wait_or_throw()` | 先 drain,再"异常优先"重抛,否则抛 `error(code)`。 |
| `cancel()/cancel_requested()/status()/first_exception()` | `status()` 在 drain 前直读 C partial、drain 后读冻结聚合(**destroy 后仍可读**);`first_exception()` 遍历包装快照。 |
| `destroy()` | `OK`(幂等)/`EBUSY`(有存活子)/`EINVAL`。 |
| `cell<R>::ready()/status()/exception()/peek()/get()/try_get()` | 一切读取走包装快照;`get()` 要求就绪(否则契约违例)。cell 在 scope 销毁后仍有效可读。`get()`/`try_get()` 会把结果从快照移出:再次调用读到的是已搬移值——只取一次。就绪前 `status()` 读作 `CORO_OK`(尚未观察到错误);就绪后为子的最终码。 |

### 同步原语

`mutex`、`condition_variable`、`semaphore`、`rwlock` 为绑定单个 runtime 的
值类型;全部等待入口仅协程内可用(线程侧 `CORO_ESTALE`),`try` 族永不
park。`mutex::lock()/unlock()` 满足 `std::lock_guard`/`std::unique_lock`。
`channel<>`(即 `channel<void*>`)是零分配 C 面(载荷生命周期归调用方);
`channel<T>` 每消息一节点,C 返回集上追加 `CORO_ENOMEM`(载荷存储遵循
callable 规则:bad_alloc 归一,其余异常传播)。两者析构都 close+drain+
destroy。

## ECANCELED 三义(解码状态前必读)

到达 C++ 调用方的 `CORO_ECANCELED` 恰有三种含义。规则:**先查
`first_exception()`(或 `cell::exception()`),再解码状态**。

| 义 | 场景 | 观察者动作 |
| --- | --- | --- |
| ① 取消请求被观察 | 子被 cancel 后自行返回 ECANCELED | 正常取消语义。 |
| ② 异常终结标记 | 子体未捕获异常被 trampoline 捕获,C cell 写 ECANCELED | **`first_exception()`/`cell<R>::exception()` 非空**——这不是取消。 |
| ③ `wait_for`/`wait_until` deadline 先中 | 本 waiter 自己的期限命中(外层返回) | 查 `first_exception()`;为空才是真超时。 |

`wait_or_throw()` 先重抛体异常再考虑码通道;`wait_first_error()` 返回
ECANCELED 时需要同样的消歧。

**C 消费者跨语言注记**:C 程序直接 wait 一个含 C++ 子的 scope 时,未捕获
异常在状态通道表现为 `CORO_ECANCELED`(异常终结)——这正是 C 级
`first_error` 早退所需;C 通道无法携带异常真值,混用面以 C++ 页为准。

## 读 C 记录与 `native()` 红线

- 包装层**永不解引用 C 结果 cell**(`scope_child.value`):C++ 调用方看到
  的码与异常全部来自包装快照。因此 `cell<R>` 与
  `scope::status()/first_exception()` 在 scope 销毁后仍可读。
- `native()` 借出对象存活期内的 C 句柄。包装层拥有的句柄(runtime、
  scheduler、task、scope 子、coroutine)**不得**手工 destroy/collect/驱动:
  调度器句柄在协程结束的 retire 即被释放(另有一条"借出句柄被手驱动"的
  第二释放路径,包装层自身永不触发);手工 collect 的 task 句柄会让包装层
  簿记悬垂。用 `native()` *读*状态、或在包装层拥有的 scope 内混入 C 调用
  点;想要 C 契约时请直接用 C API(并由 C 侧拥有)。

## 示例

```cpp
#include <corocc/coro.hpp>
#include <chrono>

int main() {
    corocc::runtime rt(4);
    if (!rt.valid()) return 1;

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
    s.spawn([] { /* void 子:异常同样进入聚合 */ });
    st = s.wait();
    if (st.code() == CORO_ECANCELED && s.first_exception()) {
        /* 异常终结,不是取消 */
    }
    s.destroy();
    rt.wait_all();
    return 0;
}
```
