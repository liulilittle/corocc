# boost — Boost.Asio integration (header-only)

> [中文](boost.zh-CN.md) | English

This document is the API reference for `include/corocc/boost/asio.hpp`, an
optional header-only integration that gives a Boost.Asio asynchronous
operation the shape of a blocking call made from inside a corocc runtime
coroutine.

The header is standalone: no other corocc header includes it, and the
library itself never depends on Boost. A program that does not include it is
unaffected — no Boost header is parsed, no symbol is referenced, no build
setting changes. Including it requires the Boost.Asio headers, corocc's
runtime (`<corocc/coro_rt.h>`), and a compiler in C++11 mode or later.

```cpp
#include <corocc/coro_rt.h>
#include <corocc/boost/asio.hpp>
#include <boost/asio.hpp>
```

## Model

1. The Asio operation is initiated **on the calling coroutine's thread**
   (Asio allows initiation from any thread as long as no second operation is
   outstanding on the same stream).
2. The completion handler runs on whatever `io_context` thread finishes the
   operation — with or without a strand — writes the outcome into a one-shot
   latch living on the coroutine's stack, and posts a single token with
   `coro_rt_sem_post` (callable from any thread).
3. The coroutine parks in `coro_rt_sem_wait` and is woken on some runtime
   worker when the token arrives. **The completion thread never touches the
   coroutine's stack**; the resume travels through the runtime's own wakeup
   path, so the handoff is strand-safe and thread-safe by construction.
4. The wait returns the outcome stored in the latch: the number of bytes
   transferred and, through `ec`, the Asio result (success, the operation's
   error, or `operation_aborted` when a pending cancellation was observed
   while waiting).

No heap allocation is added by the wrappers themselves: the latch is a stack
local and `coro_rt_sem_t` is an embeddable value type.

## Entry points

All four are function templates in `namespace corocc::asio`; each returns
`std::size_t` (bytes transferred, `0` on error/cancel) and reports the Asio
result through the trailing `ec` reference.

| Entry point | Completion condition (identical to its `boost::asio::` counterpart) |
| --- | --- |
| `std::size_t async_read(coro_rt_t* rt, AsyncReadStream& stream, const MutableBufferSequence& buffers, boost::system::error_code& ec)` | Returns only after the **whole** buffer sequence is filled or the operation fails. On success the return value is the full buffer size. |
| `std::size_t async_read_some(coro_rt_t* rt, AsyncReadStream& stream, const MutableBufferSequence& buffers, boost::system::error_code& ec)` | Returns as soon as at least one byte was read (or the operation fails). |
| `std::size_t async_write(coro_rt_t* rt, AsyncWriteStream& stream, const ConstBufferSequence& buffers, boost::system::error_code& ec)` | Returns only after the **entire** buffer sequence was written or the operation fails. |
| `std::size_t async_write_some(coro_rt_t* rt, AsyncWriteStream& stream, const ConstBufferSequence& buffers, boost::system::error_code& ec)` | Returns as soon as at least one byte was written (or the operation fails). |

`stream` is any Asio stream type (socket, SSL stream, …); `buffers` is any
Asio buffer sequence. The template parameters are inferred, exactly as for
the `boost::asio::` free functions.

## Contracts

- **Coroutine context.** Call these only from inside a coroutine scheduled
  by `rt` (a `coro_rt_create` runtime; the C++ wrapper hands out the handle
  via `corocc::runtime::native()`). A call made outside a runtime coroutine
  cannot park; the function then blocks the calling thread on the latch
  until the operation completes and reports the outcome the same way —
  safe, but a programming error worth avoiding (it turns a coroutine wait
  into a thread block).
- **The `io_context` must be running** on at least one thread, or the
  completion never arrives and the coroutine stays parked. That is the
  standard Asio contract; this header cannot repair it.
- **Lifetimes.** `stream` and `buffers` must stay alive (and unchanged, for
  reads) until the call returns, and the operation must be the only one
  outstanding on that stream. Both are plain Asio requirements.
- **Cancellation.** A cancellation requested through `coro_rt_cancel` while
  the coroutine is parked here is **not acted on immediately**: cancellation
  is only sane together with ending the I/O (cancel or close the socket),
  and the wait folds the cancellation into "keep waiting for the
  completion", because the pending handler still owns the right to write the
  latch and must never outlive the coroutine frame. When the completion does
  arrive, a cancellation observed earlier is reported as
  `operation_aborted` with zero bytes.
- **Error channel.** All failures — Asio errors and the observed
  cancellation — are reported through `ec` (with a `0` byte count), never as
  a `coro_status`. The `coro_status` of the internal semaphore wait is
  consumed by the wrapper.

## Example

```cpp
#include <corocc/coro.hpp>          /* optional: the typed wrapper */
#include <corocc/boost/asio.hpp>

#include <boost/asio.hpp>
#include <thread>

int main() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket sock(io);

    corocc::runtime rt(2);          /* worker pool drives the coroutines */
    coro_rt_t* crt = rt.native();   /* the raw handle the wrappers take */

    /* ... connect/accept sock on some thread; run io somewhere ... */
    std::thread io_thread([&] { io.run(); });

    char buf[256];
    boost::system::error_code ec;
    std::size_t n = 0;

    rt.spawn([&] {
        n = corocc::asio::async_read(crt, sock, boost::asio::buffer(buf), ec);
        /* runs on some runtime worker after the io thread completed the read */
    });
    rt.wait_all();

    io_thread.join();
    io.stop();
    return 0;
}
```

## Build notes

- Header-only: add corocc's `include/` to the include path (or link the
  library for the runtime entry points the header calls) and compile with
  `-std=c++11` or later.
- MSVC has no C++11 mode; use `/std:c++14` or later — the same MSVC
  baseline the C++ wrapper documents ([coro_cpp.md](coro_cpp.md)).
- The header is C++-only and rejected cleanly under a C compiler
  (`#error "corocc/boost/asio.hpp is a C++ header; compile as C++11 or
  later"`).
- Boost is not vendored or linked by corocc; the integration uses whatever
  Boost.Asio you already have on the include path. The vendored
  context-switch assembly lives in the core library and is unrelated to this
  header.
