# asio-host -- driving corocc coroutines from a Boost.Asio io_context

This directory is a **stand-alone integration example**. It shows how corocc's
stackful coroutines can be driven directly by one or more Boost.Asio
`io_context` threads -- **host-loop mode**: no corocc runtime, no worker
threads; the user's `io_context` *is* the scheduler, and the switch between
"coroutine running" and "coroutine parked" is a manual `coro_yield` /
`coro_resume` handshake.

It is deliberately not part of the corocc library: the library ships its own
core plus its built-in EDSM backend, while external integrations such as this
one live under `examples/` to demonstrate how the library is used and how a
backend can be switched. Nothing here is installed, built or linked by
`include/` or by the library's CMake targets.

```
spawn(executor-or-strand, fn)
      |
      v  posted to the strand; runs on an io_context thread
fn(y, ...)                              <- the entry starts on an io thread
      |
      v  async_read/async_write: arm the completion, then coro_yield()
  ... suspended; the io thread returns to io_context::run() ...
      |
      v  the completion handler runs on the strand
coro_resume()  -> the coroutine continues exactly where it yielded
      |
      v  the entry returns (or throws)
reclaim: coro_destroy() from the resumer, outside the coroutine's stack
```

## Layout

| Path | Contents |
| --- | --- |
| `include/corocc/boost/asio_host.hpp` | the header-only integration (public API) |
| `demo/host_echo.cpp` | a small echo server + client in one process |
| `tests/host_single.cpp` | single io thread: spawn/yield/inline-resume semantics, non-blocking proof |
| `tests/host_strand.cpp` | 4 io threads, one strand per connection, 32 concurrent coroutines, nested spawn |
| `tests/host_stress.cpp` | 10,000 operations over 50 connections |
| `tests/host_errors.cpp` | eof, close-while-parked, refused connect + immediate error, one-op-violation guard |
| `tests/host_awaitable.cpp` | the OS-thread blocking bridge (`await_*`) |
| `tests/host_migration.cpp` | migration stress with a stream that posts its completions from inside the coroutine (the clang >= -O1 regression) |
| `tests/host_abandon.cpp` | the abandoned-coroutine lifecycle guard (pending and parked coroutines at io_context destruction) |
| `tests/host_coexist.cpp` | ODR/coexistence pin with the runtime integration (`corocc/boost/asio.hpp`) |

## API

All of it lives in `corocc::asio_host`, is header-only, and needs only
`<corocc/coro.h>` plus the Boost.Asio headers (the API compiles with Boost
1.66 and later; the io_context lifecycle guard additionally uses the
execution-context query and is compiled in on Boost 1.74+; the tested
baseline is Boost 1.83).

```cpp
#include <corocc/boost/asio_host.hpp>

namespace asio = boost::asio;
typedef asio::strand<asio::io_context::executor_type> strand_t;
typedef corocc::asio_host::yield_context<strand_t> yield_t;

asio::io_context ioc;
strand_t strand(ioc.get_executor());

coro_status cs = corocc::asio_host::spawn(strand, [](yield_t& y) {
    char buf[64];
    boost::system::error_code ec;
    std::size_t n = corocc::asio_host::async_read(y, sock, asio::buffer(buf), ec);
    /* ec/n are the operation outcome; the coroutine continues here on the
       io thread that completed the read */
});
```

- `spawn(executor-or-strand, fn)` -- starts `fn` on the io thread. A plain
  executor is wrapped in a private strand; a strand is used as given. Returns
  `CORO_OK` once the start was posted (`CORO_ENOMEM`/creation status
  otherwise). **Detached by design**: there is no runtime to park a joiner
  in, and a joinable handle would need a cross-thread completion channel --
  exactly the runtime this mode removes. The `io_context` is the completion
  domain: `run()` returns once the last coroutine and its I/O have drained.
- `yield_context<Executor>&` -- the coroutine-side handle passed to the
  entry. `native()` returns the raw `coro_t*`, `executor()` the strand.
- `std::size_t async_read / async_read_some / async_write / async_write_some
  (y, stream, buffers, ec)` -- ec-first (non-throwing) wrappers with the same
  completion conditions as their `boost::asio::` counterparts. The first
  parameter is the coroutine's handle; `ec` reports the Asio result
  (`operation_aborted` when a cancellation was observed, see below). The
  returned count is passed through unchanged: the full buffer size on
  success, and on error whatever the operation delivered before failing
  (0 for the composed read/write; a failing `*_some` normally reports 0 as
  well). After an observed cancellation it is 0.
- `await_read / await_read_some / await_write / await_write_some(stream,
  buffers, ec)` -- the **awaitable bridge** for plain OS threads: the calling
  thread blocks on a condition variable until the operation completes.
  Never call these from a thread that runs the stream's `io_context`: it
  would block the only thread able to complete the operation.

## The strand inheritance rule

`spawn` runs the coroutine inside a strand handler, and every operation binds
its completion to the same strand **on both executor associations a
completion can use**: the ordinary associated executor and the *immediate*
executor -- the association that governs a completion delivered inside its
initiating call. With both associations on the strand, a completion either
runs inline during the initiating call (same thread, while the coroutine's
own frame still holds the strand; the result is recorded and the coroutine
observes it without suspending) or, after the coroutine parks, runs as a
strand handler and resumes the coroutine inline. In both cases the io thread
is never blocked, and the coroutine can never be resumed concurrently with
its own frame.

For contrast, on Boost 1.83 (the tested baseline) an immediate completion is
serialized under the strand even for a handler that carries only the
ordinary association: plain `bind_executor(strand, h)` is delivered
in-strand, but as a posted operation, not inline. Only a bare handler, whose
associated executor falls back to the raw I/O executor, would run it outside
the strand. Associating the strand as the immediate executor keeps the
immediate path in-strand *and inline* by construction, on every Asio
version.

## Why asio calls are made through a call fence

A stackful coroutine's frames migrate between io threads, while the code
compiled into those frames was compiled under the ordinary assumption that an
execution stays on one thread. Boost.Asio keeps per-thread state on the
running thread's stack (the scheduler's `thread_info` allocation cache,
reached through thread-local bookkeeping) and, with clang >= -O1, asio code
inlined into a coroutine frame can leave values derived from that state in
registers or stack slots across `coro_yield()`. After the coroutine migrates,
those values name the previous thread's stack-resident state, which that
thread is concurrently using without synchronization; the result is heap
corruption (reproduced before the fix: heap-use-after-free in strand invoker
scheduling with a 4-thread io_context and a stream that posts its completion
from inside the coroutine; `tests/host_migration.cpp` is the regression).

The header therefore performs every asio call reachable from coroutine
context through an opaque boundary (`detail::call_fenced`): an indirect call
through a volatile function pointer that the optimizer cannot inline through.
The asio machinery runs, allocates and returns entirely inside that call
frame, which begins and ends between two suspensions, so no thread-derived
value can survive into the coroutine's own frames. On top of the structural
fence, the integration associates a plain heap allocator with all of its
handlers (`asio_host_allocator`), so asio's per-thread recycling cache is not
used on the integration's behalf at all (Boost.Asio would otherwise silently
replace `std::allocator` with it).

**Consequence for user code:** do not call Boost.Asio directly from a
coroutine body (for example `asio::post(...)`): asio calls must happen in a
frame that cannot span a suspension. Perform I/O through the operations
above, which are the fenced entry points. This mirrors the corocc core's
documented rule about `thread_local` state inside coroutines.

## Contracts

- **One operation per coroutine at a time.** A second call while one is in
  flight is rejected through `ec`
  (`boost::system::errc::device_or_resource_busy`); it never touches the
  pending operation. The guard is a plain flag and covers the normal
  strand-serialized pattern (a second call issued from the same
  strand/thread); calling an operation for the same coroutine from another
  thread is a data race and unsupported.
- **The `io_context` must be running** on at least one thread, or nothing
  starts and nothing completes (standard Asio contract).
- `stream`/`buffers` must stay alive until the call returns; the operation
  must be the only one outstanding on that stream.
- **Cancellation** (`coro_cancel(y.native())`) is cooperative and does not
  cut a wait short: the pending handler owns the right to resume. End the
  I/O (close/cancel the stream) to complete the wait; a cancellation observed
  at the resume point is reported as `operation_aborted` with zero bytes.
- **Exceptions** escaping the coroutine body end the coroutine: they are
  caught at the coroutine boundary (never unwinding through the context
  switch) and dropped -- use the `ec` channel for reporting.
- **Lifecycle.** Destroy the `io_context` only after `run()` has drained (the
  standard Asio contract). A live coroutine keeps the `io_context` busy; a
  parked coroutine whose I/O never completes keeps it parked (end the I/O to
  finish it). As a guard, the integration registers a service with the owning
  `execution_context`: if the `io_context` is still destroyed while
  coroutines are pending or parked, their stacks and state are reclaimed
  during service shutdown instead of leaking (`tests/host_abandon.cpp`).
  User code cannot rely on destructors of locals inside an abandoned
  coroutine body; the integration's own machinery only keeps trivially
  destructible data on coroutine stacks for the same reason.
  Asio's own lifetime rules apply unchanged on top of this: sockets, timers,
  strands and executors must not outlive their execution_context (destroying
  a `strand` after its `io_context` is undefined behavior with or without
  this integration).
- **Do not query `thread_local` state from inside a coroutine once it has
  migrated between threads** (e.g. `strand::running_in_this_thread()`).
  Stackful coroutines move between threads while their code was compiled
  assuming one; the library documents that user code must not depend on
  `thread_local` inside a coroutine. The mode's own serialization does not
  depend on `thread_local`: it is the strand's mutex/queue state.
- Error-code spelling for "the stream was closed underneath a parked read" is
  platform dependent (Linux: `operation_aborted`; the Windows IOCP backend:
  `ERROR_CONNECTION_ABORTED` (1236)). Tests accept the abort family.

## Build and run

The project is self-contained; it re-enters the repository root only to build
the corocc static library it links against (with the library's own tests
disabled):

```sh
cmake -S examples/backends/asio-host -B build-asio-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-asio-host
ctest --test-dir build-asio-host --output-on-failure
./build-asio-host/asio_host_demo
```

CMake options:

| Option | Meaning |
| --- | --- |
| `COROCC_ASIO_HOST_TESTS` | build the example's tests (default `ON`) |
| `BOOST_INCLUDE_DIR` | Boost include directory (auto-detected; pass it explicitly when Boost is not on the default search path) |
| `COROCC_ASAN` | build library and example with AddressSanitizer |

Windows / MSVC (the runner images carry no Boost, so point the option at a
Boost checkout; the example itself is portable to MSVC and was verified with
VS2022 in Release):

```bat
cmake -S examples\backends\asio-host -B build-asio-host -G "Visual Studio 17 2022" -A x64 ^
      -DBOOST_INCLUDE_DIR=C:\path\to\boost
cmake --build build-asio-host --config Release --parallel 4
ctest --test-dir build-asio-host -C Release --output-on-failure
```

## Verification status

- Linux (WSL Ubuntu 24.04, gcc 13.3 and clang 18, Boost 1.83): the 8-test
  suite is green on gcc, clang and an AddressSanitizer build
  (`-DCOROCC_ASAN=ON`, `detect_leaks=1`), including 10x repeat runs of every
  test on gcc and clang.
- Migration stress (`host_migration`, 2,000 operations per run, hundreds of
  thread migrations per run): 10 runs at clang -O0, -O1, -O2 and -O3 plus
  gcc -O2/-O3 -- 20,000 operations and 11,000+ observed migrations per
  configuration, zero failures. The same test built against the pre-fence
  header shape fails within milliseconds under clang -O2.
- Sanitizers: `host_migration`, `host_abandon` and `host_stress` are clean
  under ASan and LeakSanitizer; `host_abandon` also proves that abandoning
  pending/parked coroutines at `io_context` destruction leaks nothing.
- The header compiles under `-pedantic-errors -Wall -Wextra -Werror` on
  C++11/14/17/20 (gcc and clang) and under `-fno-exceptions/-fno-rtti`.
- Windows (MSVC 19.4x, VS2022, Release, `/W4 /WX`): full 8-test suite green
  with zero warnings, 10x repeat runs clean.
- The ODR/coexistence test links this header together with the runtime
  integration (`corocc/boost/asio.hpp`) and pins a shared instantiation's
  address across translation units.
