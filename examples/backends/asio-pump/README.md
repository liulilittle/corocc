# corocc — asio-pump 集成示例

用 Boost.Asio 的 `io_context` 作为宿主事件循环,驱动 corocc 的 **LOOP 类**运行时:
运行时不持有 worker 线程,宿主循环不断调用 `coro_rt_pump` 推进协程。

## 形态

- **驱动**(`include/corocc/boost/asio_pump.hpp`):实现 corocc 驱动 vtable
  - `now`:`std::chrono::steady_clock`,纳秒;
  - `wake`:合并的 `asio::post`(把 pump 投递回 io_context),任意线程可调、幂等;
  - `timer_arm` / `timer_cancel`:单发 `asio::steady_timer`。
- **runtime**:`coro_rt_driver_register` → `coro_rt_driver_set_default` →
  `coro_rt_create`(LOOP 默认驱动 ⇒ 不启线程,单个 pump 上下文)。
- **循环**:`coro_rt_pump(rt, 0)` 抽干可运行节点;若仍有存活协程,示例用
  `timer_arm` 安排一次短间隔的后续 pump(运行时在 LOOP 模式不自行挂 alarm),
  从而观察到 `coro_rt_sleep` 的到期。

## 构建与测试(独立工程)

```sh
cmake -S examples/backends/asio-pump -B build-asio-pump -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-asio-pump -j2
ctest --test-dir build-asio-pump --output-on-failure --timeout 300
```

需要 Boost 头(如 Ubuntu 的 `libboost-dev`)。该目录不进入库的构建/安装。

测试集(自带 ctest,与库默认套件无关):

| 测试 | 覆盖 |
|---|---|
| `pump_single` | 单 io 线程下 yield/sleep 协程全部完成,deadline 区间合理 |
| `pump_io` | 协程发起真实异步读写,完成回调经 `wake` 唤醒 pump,端到端回声 |
| `pump_stress` | 多协程 × 多轮 sleep/yield,零丢失 |
| `pump_cancel` | LOOP 模式下对 timed park 的 `coro_rt_cancel` |
| `pump_mixed` | 内置(worker)运行时与 LOOP 运行时同进程并存互不干扰 |

## 注意

- 同进程的默认驱动是全局的:示例通过 `set_default` 建立 LOOP 运行时,退出时
  复位为 NULL。要同时保留内置运行时,请在其创建后再设置 LOOP 默认(见
  `pump_mixed`)。
- 计算 `coro_rt_sleep` 的绝对 deadline 必须使用 `pump_runner::now_ns()`
  (驱动时钟),不要混用其它时钟(`rt_now` 会分派到驱动 `now`)。
