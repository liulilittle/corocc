# corocc — libevent 集成示例

用 libevent 的 `event_base`(`event_base_dispatch`)作为宿主,驱动 corocc 的
**LOOP 类**运行时:运行时不持有 worker 线程,宿主循环不断调用
`coro_rt_pump` 推进协程。

## 形态

- **驱动**(`include/corocc/event/event_pump.hpp`):实现 corocc 驱动 vtable
  - `now`:`clock_gettime(CLOCK_MONOTONIC)`(纳秒);
  - `wake`:对一个 `EV_PERSIST` 用户事件 `event_active`(先经
    `evthread_use_pthreads()` 打开线程安全);
  - `timer_arm` / `timer_cancel`:单个 `evtimer`,单发。
- **runtime**:`coro_rt_driver_register` → `coro_rt_driver_set_default` →
  `coro_rt_create`(LOOP 默认驱动 ⇒ 不启线程)。
- **循环**:`coro_rt_pump(rt, 0)` 抽干;仍有存活协程时用 `timer_arm` 排一次
  0.5ms 后续 pump(运行时在 LOOP 模式不自行挂 alarm);抽干后
  `event_base_loopbreak` 结束 `event_base_dispatch`。

## 构建与测试(独立工程)

```sh
sudo apt-get install -y libevent-dev       # 或等价的 libevent 开发包
cmake -S examples/backends/libevent -B build-libevent -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-libevent -j2
ctest --test-dir build-libevent --output-on-failure
```

该目录不进入库的构建/安装;自带冒烟测试 `event_smoke`。

## 注意

- 计算 `coro_rt_sleep` 的绝对 deadline 必须用 `pump_runner::now_ns()`(驱动时钟)。
- 跨线程 `wake` 依赖 `evthread_use_pthreads()`(链接 `event_pthreads`)。
- 同进程默认驱动是全局的;示例退出时复位为 NULL。
