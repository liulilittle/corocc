add_executable(test_smoke unit/test_smoke.c)
target_link_libraries(test_smoke PRIVATE corocc)
add_test(NAME smoke COMMAND test_smoke)

add_executable(test_stack unit/test_stack.c)
target_link_libraries(test_stack PRIVATE corocc)
add_test(NAME stack COMMAND test_stack)

add_executable(test_ctx unit/test_ctx.c)
target_link_libraries(test_ctx PRIVATE corocc)
add_test(NAME ctx COMMAND test_ctx)

add_executable(test_ctx_return unit/test_ctx_return.c)
target_link_libraries(test_ctx_return PRIVATE corocc)
add_test(NAME ctx_return COMMAND test_ctx_return)

# Repeated round trips through one pair of context objects: guards against
# saved-frame slots being overwritten with the resumer's frame.
add_executable(test_ctx_pingpong unit/test_ctx_pingpong.c)
target_link_libraries(test_ctx_pingpong PRIVATE corocc)
add_test(NAME ctx_pingpong COMMAND test_ctx_pingpong)

# The overflow is exercised in a child process: the child faults on the
# guard page (SIGSEGV on Linux, SIGBUS on Darwin's protection-fault
# delivery), and the parent asserts that death-by-signal is what
# happened. The test itself passes when that holds.
add_executable(test_ctx_overflow unit/test_ctx_overflow.c)
target_link_libraries(test_ctx_overflow PRIVATE corocc)
add_test(NAME ctx_overflow COMMAND test_ctx_overflow)

# ASan build: the hooks make this switch pass without false positives.
add_executable(test_ctx_asan unit/test_ctx_asan.c)
target_link_libraries(test_ctx_asan PRIVATE corocc)
add_test(NAME ctx_asan COMMAND test_ctx_asan)

# coroutine object (L1)
add_executable(test_coro_smoke unit/test_coro_smoke.c)
target_link_libraries(test_coro_smoke PRIVATE corocc)
add_test(NAME coro_smoke COMMAND test_coro_smoke)

add_executable(test_coro_token unit/test_coro_token.c)
target_link_libraries(test_coro_token PRIVATE corocc)
add_test(NAME coro_token COMMAND test_coro_token)

add_executable(test_coro_cancel unit/test_coro_cancel.c)
target_link_libraries(test_coro_cancel PRIVATE corocc)
add_test(NAME coro_cancel COMMAND test_coro_cancel)

# safety suites: exactly-once under concurrency, failure without side
# effects, the zero-spin probe (probe counter only with COROCC_PROBE_NOSPIN)
# and a deterministic stress run.
add_executable(test_coro_exactlyonce unit/test_coro_exactlyonce.c)
target_link_libraries(test_coro_exactlyonce PRIVATE corocc)
if (NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_coro_exactlyonce PRIVATE Threads::Threads)
endif()
add_test(NAME coro_exactlyonce COMMAND test_coro_exactlyonce)

add_executable(test_coro_rights unit/test_coro_rights.c)
target_link_libraries(test_coro_rights PRIVATE corocc)
add_test(NAME coro_rights COMMAND test_coro_rights)

add_executable(test_coro_stress unit/test_coro_stress.c)
target_link_libraries(test_coro_stress PRIVATE corocc)
add_test(NAME coro_stress COMMAND test_coro_stress)

if (COROCC_PROBE_NOSPIN)
    add_executable(test_coro_nospin unit/test_coro_nospin.c)
    target_link_libraries(test_coro_nospin PRIVATE corocc)
    add_test(NAME coro_nospin COMMAND test_coro_nospin)
endif()

# Coexistence with the real Boost.Context in one binary (proves the symbol
# renaming took effect). Registered only for native builds: cross builds
# would link the host's Boost objects into a foreign-arch binary, and a cross
# CI host has no Boost toolchain for the target anyway.
# (Plain find_path/find_library is used instead of find_package(Boost): the
#  Ubuntu Boost CMake toolset-detection errors out under GCC.)
find_path(BOOST_CONTEXT_INCLUDE boost/context/fiber.hpp)
find_library(BOOST_CONTEXT_LIB NAMES boost_context)
find_library(BOOST_SYSTEM_LIB NAMES boost_system)
if (BOOST_CONTEXT_INCLUDE AND BOOST_CONTEXT_LIB AND NOT CMAKE_CROSSCOMPILING)
    add_executable(test_boost_coexist coexist/test_boost_coexist.cpp)
    target_include_directories(test_boost_coexist PRIVATE ${BOOST_CONTEXT_INCLUDE})
    target_link_libraries(test_boost_coexist PRIVATE corocc
        ${BOOST_CONTEXT_LIB} ${BOOST_SYSTEM_LIB})
    add_test(NAME boost_coexist COMMAND test_boost_coexist)
endif()

# Context-switch cost benchmark. Not a test: it reports a figure instead of
# asserting one, so it is excluded from ctest.
add_executable(bench_switch perf/bench_switch.c)
target_link_libraries(bench_switch PRIVATE corocc)

# Coroutine lifecycle benchmark (round trip and create/destroy cost).
add_executable(bench_coro perf/bench_coro.c)
target_link_libraries(bench_coro PRIVATE corocc)

# the per-thread runtime as seen through the public API.
add_executable(test_thread_smoke unit/test_thread_smoke.c)
target_link_libraries(test_thread_smoke PRIVATE corocc)
if (NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_thread_smoke PRIVATE Threads::Threads)
endif()
add_test(NAME thread_smoke COMMAND test_thread_smoke)

# cross-thread wakeup delivered to the owning thread.
add_executable(test_coro_crossresume unit/test_coro_crossresume.c)
target_link_libraries(test_coro_crossresume PRIVATE corocc)
if (NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_coro_crossresume PRIVATE Threads::Threads)
endif()
add_test(NAME coro_crossresume COMMAND test_coro_crossresume)

# concurrency suites. Both are built on every platform now (the sources
# go through tests/palt_threads.h); the TSan-visible suite below is the one
# exception. The TSan suite needs the internal headers, so it also gets the
# source root on its include path.
add_executable(test_conc_double_resume concurrency/test_conc_double_resume.c)
target_link_libraries(test_conc_double_resume PRIVATE corocc)
if (NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_conc_double_resume PRIVATE Threads::Threads)
endif()
add_test(NAME conc_double_resume COMMAND test_conc_double_resume)

# Registered on POSIX only. MSVC has no ThreadSanitizer; the purpose of this
# suite is TSan visibility, which a Win32 build cannot provide —— building it
# there would yield false coverage.
if (NOT WIN32)
    find_package(Threads REQUIRED)
    add_executable(test_conc_runtime_tsan concurrency/test_conc_runtime_tsan.c)
    target_include_directories(test_conc_runtime_tsan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_conc_runtime_tsan PRIVATE corocc Threads::Threads)
    add_test(NAME conc_runtime_tsan COMMAND test_conc_runtime_tsan)
endif()

# the timer heap and the monotone clock.
add_executable(test_timer unit/test_timer.c)
target_include_directories(test_timer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(test_timer PRIVATE corocc)
add_test(NAME timer COMMAND test_timer)

# the single-thread scheduler.
add_executable(test_sched_spawn unit/test_sched_spawn.c)
target_link_libraries(test_sched_spawn PRIVATE corocc)
add_test(NAME sched_spawn COMMAND test_sched_spawn)

add_executable(test_sched_sleep unit/test_sched_sleep.c)
target_link_libraries(test_sched_sleep PRIVATE corocc)
add_test(NAME sched_sleep COMMAND test_sched_sleep)

    add_executable(test_sched_join unit/test_sched_join.c)
    target_include_directories(test_sched_join PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_sched_join PRIVATE corocc)
    add_test(NAME sched_join COMMAND test_sched_join)

# M:N work-stealing runtime (L2). These suites build on Windows too -- the
# runtime layer uses the platform lock/condition shim, not pthreads.
# Threads::Threads carries -pthread on POSIX only; on MSVC FindThreads
# yields an empty imported target (no libraries to add).
find_package(Threads REQUIRED)
add_executable(test_rt_mn unit/test_rt_mn.c)
    target_include_directories(test_rt_mn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_rt_mn PRIVATE corocc Threads::Threads)
    add_test(NAME rt_mn COMMAND test_rt_mn)

    # sync primitives on the M:N runtime.
    add_executable(test_rt_sync unit/test_rt_sync.c)
    target_include_directories(test_rt_sync PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_rt_sync PRIVATE corocc Threads::Threads)
    add_test(NAME rt_sync COMMAND test_rt_sync)

    # writer-preferring read/write lock on the M:N runtime.
    add_executable(test_rt_rwlock unit/test_rt_rwlock.c)
    target_include_directories(test_rt_rwlock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_rt_rwlock PRIVATE corocc Threads::Threads)
    add_test(NAME rt_rwlock COMMAND test_rt_rwlock)

    # bounded/unbounded channel on the M:N runtime.
    add_executable(test_rt_channel unit/test_rt_channel.c)
    target_include_directories(test_rt_channel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_rt_channel PRIVATE corocc Threads::Threads)
    add_test(NAME rt_channel COMMAND test_rt_channel)

    # L2 hardening set: retained-handle accounting, join-chain atomicity
    # and cancel detach with a usable node afterwards, and the exactly-once
    # wake-request claim. The cancel-of-a-sync-chain scene is held back (it
    # stays a documented caller-level limitation instead).
    add_executable(test_rt_lifecycle unit/test_rt_lifecycle.c)
    target_include_directories(test_rt_lifecycle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_rt_lifecycle PRIVATE corocc Threads::Threads)
    add_test(NAME rt_lifecycle COMMAND test_rt_lifecycle)
    # Concurrency suite: a lost wakeup must fail, not hang the run forever.
    set_tests_properties(rt_lifecycle PROPERTIES TIMEOUT 120)

    # A cancellation consumed at a park's safe point leaves the
    # node usable -- every park site retires the park marker, so the next
    # plain yield requeues the coroutine, and a cancelled sleep takes its
    # deadline out of the timer heap (no second drive).
    add_executable(test_rt_cancel_park unit/test_rt_cancel_park.c)
    target_include_directories(test_rt_cancel_park PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_rt_cancel_park PRIVATE corocc Threads::Threads)
    add_test(NAME rt_cancel_park COMMAND test_rt_cancel_park)
    set_tests_properties(rt_cancel_park PROPERTIES TIMEOUT 120)

    # L3 structured concurrency: the scope state machine and the per-API
    # return codes that guard it (illegal transitions, destroy idempotence,
    # zero-init is OPEN).
    add_executable(test_rt_scope_state unit/test_rt_scope_state.c)
    target_link_libraries(test_rt_scope_state PRIVATE corocc Threads::Threads)
    add_test(NAME rt_scope_state COMMAND test_rt_scope_state)
    set_tests_properties(rt_scope_state PROPERTIES TIMEOUT 120)

    # The child result cell: the trampoline is its only writer,
    # cell_out=NULL means "no result", the aggregate is the first non-OK in
    # completion order.
    add_executable(test_rt_scope_spawn unit/test_rt_scope_spawn.c)
    target_link_libraries(test_rt_scope_spawn PRIVATE corocc Threads::Threads)
    add_test(NAME rt_scope_spawn COMMAND test_rt_scope_spawn)
    set_tests_properties(rt_scope_spawn PROPERTIES TIMEOUT 120)

    # The wait predicate: one parking point on the scope event chain,
    # multi-waiter release with one resume each, immunity to the waiter's
    # own cancellation, and coverage of children registered while a waiter
    # is already parked.
    add_executable(test_rt_scope_wait unit/test_rt_scope_wait.c)
    target_include_directories(test_rt_scope_wait PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_rt_scope_wait PRIVATE corocc Threads::Threads)
    add_test(NAME rt_scope_wait COMMAND test_rt_scope_wait)
    set_tests_properties(rt_scope_wait PROPERTIES TIMEOUT 120)

    # The group three-state family: wait_all (live==0), first_error (cancel
    # the rest at the first error, still drain, report that error promptly),
    # first_done (report the first completion, scope stays alive: follow-up
    # wait or cancel is mandatory).
    add_executable(test_rt_scope_group unit/test_rt_scope_group.c)
    target_link_libraries(test_rt_scope_group PRIVATE corocc Threads::Threads)
    add_test(NAME rt_scope_group COMMAND test_rt_scope_group)
    set_tests_properties(rt_scope_group PROPERTIES TIMEOUT 120)

    # wait_for with the watchdog: fail-fast before any child registration,
    # the timer_fired CAS claim, the deadline-priority return code, and the
    # watchdog's cancel -> join -> collect reclaim on the early exit (it is
    # never a child and never touches live).
    add_executable(test_rt_scope_wait_for unit/test_rt_scope_wait_for.c)
    target_link_libraries(test_rt_scope_wait_for PRIVATE corocc Threads::Threads)
    add_test(NAME rt_scope_wait_for COMMAND test_rt_scope_wait_for)
    set_tests_properties(rt_scope_wait_for PROPERTIES TIMEOUT 120)

    # The non-coroutine (thread-blocking) entry: the wait family called
    # from a plain thread blocks on the runtime's settled condition (never
    # EAGAIN), and the thread-side wait_for waits on its own deadline and
    # claims the same once-only timeout.
    add_executable(test_rt_scope_thread unit/test_rt_scope_thread.c)
    target_link_libraries(test_rt_scope_thread PRIVATE corocc Threads::Threads)
    add_test(NAME rt_scope_thread COMMAND test_rt_scope_thread)
    set_tests_properties(rt_scope_thread PROPERTIES TIMEOUT 120)

    # B1: the process-level runtime-driver registry (pure C, portable across
    # every site -- the concurrency leg uses tests/palt_threads.h). The vtable
    # and hot-plug suites follow in B2.
    add_executable(test_rt_driver_registry unit/test_rt_driver_registry.c)
    target_include_directories(test_rt_driver_registry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_rt_driver_registry PRIVATE corocc Threads::Threads)
    add_test(NAME rt_driver_registry COMMAND test_rt_driver_registry)

# Custom allocator interface.
add_executable(test_coro_allocator unit/test_coro_allocator.c)
target_link_libraries(test_coro_allocator PRIVATE corocc)
add_test(NAME coro_allocator COMMAND test_coro_allocator)

# M:N runtime benchmark (not a ctest: reports throughput). Built on Windows
# too; the timed sources are platform-portable.
find_package(Threads REQUIRED)
add_executable(bench_rt_mn ../perf/bench_rt_mn.c)
target_link_libraries(bench_rt_mn PRIVATE corocc Threads::Threads)

# Pure context-switch throughput (pre-spawned; the timed window holds
# only yield/drive round trips), plus the spawn/collect cost split.
add_executable(bench_switch_pure ../perf/bench_switch_pure.c)
target_link_libraries(bench_switch_pure PRIVATE corocc Threads::Threads)

# Collect-vs-retire race regression probe (perf/probe_collect_race.c).
# Probabilistic red/green -- intentionally NOT a ctest target; run it
# repeatedly to observe convergence.
add_executable(probe_collect_race ../perf/probe_collect_race.c)
target_link_libraries(probe_collect_race PRIVATE corocc Threads::Threads)
if (MSVC)
    # getenv is flagged C4996 by the UCRT under /W4 /WX; the probe only reads
    # one environment switch (COLLECT_SPIN).
    target_compile_definitions(probe_collect_race PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# The C++11 wrapper suites and the cpp_perf gate are native-only; cross
# sites stay C-only (cross=34). This wraps the whole C++ wrapper segment
# below without touching any target or condition inside it.
if(NOT CMAKE_CROSSCOMPILING)

# ---- the C++11 wrapper suites (header-only; the C library is unchanged) ----
# Every wrapper target pins CXX_STANDARD 11 + REQUIRED. MSVC has no
# C++11 mode; there the accepted baseline is the default /std:c++14 under
# /W4 /WX (the header avoids post-C++11 facilities, not just post-C++11
# flags).
function(corocc_add_cpp_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE corocc Threads::Threads)
    set_target_properties(${name} PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

corocc_add_cpp_test(cpp_types      cpp/test_cpp_types.cpp)
corocc_add_cpp_test(cpp_coroutine  cpp/test_cpp_coroutine.cpp)
corocc_add_cpp_test(cpp_runtime    cpp/test_cpp_runtime.cpp)
corocc_add_cpp_test(cpp_scheduler  cpp/test_cpp_scheduler.cpp)
corocc_add_cpp_test(cpp_scope      cpp/test_cpp_scope.cpp)
corocc_add_cpp_test(cpp_sync       cpp/test_cpp_sync.cpp)
# The exception suite carries the death tests (self-destroy while running,
# detached exception with the default handler, cell::get() before the child
# finished); fork on POSIX, CreateProcess re-exec on Windows.
corocc_add_cpp_test(cpp_exceptions cpp/test_cpp_exceptions.cpp)
corocc_add_cpp_test(cpp_interop    cpp/test_cpp_interop.cpp)

# The ODR pin: two TUs including the same header (opposite include orders),
# function-local statics land on the same address, and noexcept/sizeof stay
# identical across TUs.
add_executable(cpp_odr cpp/test_cpp_odr.cpp cpp/test_cpp_odr_b.cpp)
target_link_libraries(cpp_odr PRIVATE corocc Threads::Threads)
set_target_properties(cpp_odr PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
add_test(NAME cpp_odr COMMAND cpp_odr)
set_tests_properties(cpp_odr PROPERTIES TIMEOUT 120)

# The C++ A/B switch benchmark. The measured gate is defined on a
# Release-like, non-ASan build; in other configurations the target is still
# built but the ctest entry is not registered. The ctest entry runs the
# loose gate (--loose: >=10M round trips/s both faces, ratio >= 0.85 -- a
# coarse-regression tripwire); the strict gate (>=20M, ratio >= 0.95) stays
# the manual/bench-section run. RUN_SERIAL keeps the measurement free of
# sibling-test interference.
add_executable(bench_cpp_switch ../perf/bench_cpp_switch.cpp)
target_link_libraries(bench_cpp_switch PRIVATE corocc Threads::Threads)
set_target_properties(bench_cpp_switch PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
if (NOT COROCC_ASAN AND (CMAKE_BUILD_TYPE STREQUAL "Release" OR
                         CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR
                         CMAKE_BUILD_TYPE STREQUAL "MinSizeRel"))
    add_test(NAME cpp_perf COMMAND bench_cpp_switch --loose)
    set_tests_properties(cpp_perf PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()

endif()  # NOT CMAKE_CROSSCOMPILING (cross sites are C-only, cross=34)
