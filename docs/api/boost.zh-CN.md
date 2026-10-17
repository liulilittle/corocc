# boost — Boost.Asio 集成(header-only)

> [English](boost.md) | 中文

本文档是 `include/corocc/boost/asio.hpp` 的 API 参考。该可选头把一次
Boost.Asio 异步操作包装成"在 corocc 运行时协程内以阻塞形态调用"。

头文件完全独立:没有其他 corocc 头包含它,库本身也从不依赖 Boost。不包含它
的程序不受影响——不解析任何 Boost 头、不引用任何符号、不改动任何构建设置。
包含它需要 Boost.Asio 头、corocc 运行时(`<corocc/coro_rt.h>`)以及 C++11 或
更高模式的编译器。

```cpp
#include <corocc/coro_rt.h>
#include <corocc/boost/asio.hpp>
#include <boost/asio.hpp>
```

## 工作模型

1. Asio 操作**在调用协程所在线程上发起**(只要同一 stream 上没有第二个未完成
   操作,Asio 允许从任意线程发起)。
2. 完成处理器在完成该操作的任意 `io_context` 线程上运行——无论有没有
   strand——把结果写进协程栈上的单发锁存器(latch),并以
   `coro_rt_sem_post`(任意线程可调)投递一个 token。
3. 协程在 `coro_rt_sem_wait` 中停放,token 到达时由某个运行时 worker 唤醒。
   **完成线程绝不触碰协程栈**;恢复走运行时自身唤醒路径,因此交接在构造上
   即 strand 安全 + 线程安全。
4. wait 返回锁存器中的结果:传输字节数,以及经 `ec` 得到的 Asio 结果(成功、
   操作错误,或驻停期间观察到的取消对应的 `operation_aborted`)。

包装器自身不引入任何堆分配:latch 是栈局部对象,`coro_rt_sem_t` 是可内嵌的
值类型。

## 入口

四个函数模板均在 `corocc::asio` 命名空间;每个返回 `std::size_t`(传输字节数,
错误/取消时为 0),经尾部 `ec` 引用报告 Asio 结果。

| 入口 | 完成条件(与同名 `boost::asio::` 函数一致) |
| --- | --- |
| `std::size_t async_read(coro_rt_t* rt, AsyncReadStream& stream, const MutableBufferSequence& buffers, boost::system::error_code& ec)` | 仅在**整个**缓冲区序列读满或操作失败后返回。成功返回完整缓冲区大小。 |
| `std::size_t async_read_some(coro_rt_t* rt, AsyncReadStream& stream, const MutableBufferSequence& buffers, boost::system::error_code& ec)` | 读到至少一个字节(或操作失败)即返回。 |
| `std::size_t async_write(coro_rt_t* rt, AsyncWriteStream& stream, const ConstBufferSequence& buffers, boost::system::error_code& ec)` | 仅在**整个**缓冲区序列写完或操作失败后返回。 |
| `std::size_t async_write_some(coro_rt_t* rt, AsyncWriteStream& stream, const ConstBufferSequence& buffers, boost::system::error_code& ec)` | 写出至少一个字节(或操作失败)即返回。 |

`stream` 可以是任意 Asio stream 类型(socket、SSL stream 等);`buffers` 是
任意 Asio 缓冲区序列。模板参数由实参推导,与 `boost::asio::` 自由函数用法一致。

## 契约

- **协程上下文。** 只在 `rt` 调度的协程内调用(`coro_rt_create` 运行时;C++
  封装经 `corocc::runtime::native()` 提供句柄)。在运行时协程外调用无法停放,
  函数会改为阻塞调用线程等待锁存器直至操作完成,并以同样方式报告结果——安全,
  但属应避免的编程错误(协程等待被降级为线程阻塞)。
- **`io_context` 必须在至少一条线程上运行**,否则完成永不到达、协程一直驻停。
  这是标准 Asio 契约,本头文件无法修复。
- **生命周期。** `stream` 与 `buffers` 必须存活(读操作还须保持不变)到调用
  返回;且该 stream 上只能有这一个未完成操作。两者都是 Asio 本身的要求。
- **取消。** 驻停期间经 `coro_rt_cancel` 发出的取消请求**不会立即生效**:取消
  只有在同时结束 I/O(取消或关闭 socket)时才健全,wait 会把取消折叠为"继续
  等待完成"——因为未完成的处理器仍拥有写锁存器的权利,绝不能比协程帧活得更久。
  完成的到达时刻,先前观察到的取消以 `operation_aborted` + 0 字节报告。
- **错误通道。** 所有失败——Asio 错误与观察到的取消——都经 `ec` 报告(字节数
  为 0),从不以 `coro_status` 形式暴露;内部信号量 wait 的 `coro_status` 由
  包装器自行消费。

## 示例

```cpp
#include <corocc/coro.hpp>          /* 可选:类型化封装 */
#include <corocc/boost/asio.hpp>

#include <boost/asio.hpp>
#include <thread>

int main() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket sock(io);

    corocc::runtime rt(2);          /* 2 个 worker 线程驱动全部协程
                                       (协程数量无上限,仅受内存约束) */
    coro_rt_t* crt = rt.native();   /* 包装器需要的裸句柄 */

    /* ... 在某个线程连接/接受 sock;在别处运行 io ... */
    std::thread io_thread([&] { io.run(); });

    char buf[256];
    boost::system::error_code ec;
    std::size_t n = 0;

    rt.spawn([&] {
        n = corocc::asio::async_read(crt, sock, boost::asio::buffer(buf), ec);
        /* io 线程完成读后,本协程在某个运行时 worker 上继续 */
    });
    rt.wait_all();

    io_thread.join();
    io.stop();
    return 0;
}
```

`rt(2)` 只是两枚 worker 线程;它不限制可创建的协程数量——协程按需增长,
仅受内存约束,worker 只负责驱动。

## 集成示例:host-loop 模式(examples/backends/asio-host)

本头文件的"无运行时"对应物以**独立示例**形式发布,不属于库本体:
`examples/backends/asio-host/`——不安装、不进库默认 CMake target。那里协程
直接由 Boost.Asio 的 `io_context` 与 strand 驱动:没有 corocc 运行时,没有
worker 线程:

    spawn -> 协程入口在 io 线程开始 -> coro_yield 把 io 线程交还 run()
    -> 完成处理器内联恢复协程(coro_resume)-> 栈在协程之外回收

- `corocc::asio_host::spawn(executor 或 strand, fn)`(detached 语义,返回
  `coro_status`);ec-first 的 `async_read` / `async_read_some` /
  `async_write` / `async_write_some` 以协程的 `yield_context` 为第一参数;
  `await_*` 桥让普通 OS 线程以阻塞形态等待同一批操作。
- 每个完成都同时绑定协程 strand 的普通 executor 关联与 immediate executor
  关联,因此内联恢复绝不可能与协程帧竞争,io 线程也永不阻塞。
- 协程上下文中发起的每一次 asio 调用都经过一个不透明调用栅栏(优化器无法
  内联穿透的间接调用),因此协程在线程间迁移时,asio 的线程局部状态不可能
  被跨挂起点缓存;集成自己的处理器还显式携带堆分配器,而不使用 asio 的
  线程级回收缓存。`tests/host_migration.cpp` 就是针对"内联 asio 机制在
  clang >= -O1 下曾导致堆损坏"的回归测试。
- 构建与运行:

  ```sh
  cmake -S examples/backends/asio-host -B build-asio-host -DCMAKE_BUILD_TYPE=Release
  cmake --build build-asio-host
  ctest --test-dir build-asio-host --output-on-failure
  ```

  契约、strand 继承规则、平台说明与测试清单见
  `examples/backends/asio-host/README.md`。示例完全独立,对本头文件与库本体
  的用户没有任何影响。

## 构建说明

- header-only:把 corocc 的 `include/` 加入包含路径(或链接库以取得头中调用的
  运行时入口),并以 `-std=c++11` 或更高编译。
- MSVC 没有 C++11 模式;使用 `/std:c++14` 或更高——即 C++ 封装所记录的同一
  MSVC 基线([coro_cpp.zh-CN.md](coro_cpp.zh-CN.md))。
- 本头仅限 C++;用 C 编译器包含会被干净拒绝
  (`#error "corocc/boost/asio.hpp is a C++ header; compile as C++11 or
  later"`)。
- corocc 不 vendored、不链接 Boost;集成使用你包含路径上已有的 Boost.Asio。
  核心库中 vendored 的上下文切换汇编与本头无关。
