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
# guard page (SIGSEGV), and the parent asserts that death-by-SIGSEGV is what
# happened. The test itself passes when that holds.
add_executable(test_ctx_overflow unit/test_ctx_overflow.c)
target_link_libraries(test_ctx_overflow PRIVATE corocc)
add_test(NAME ctx_overflow COMMAND test_ctx_overflow)

# ASan build: the hooks make this switch pass without false positives.
add_executable(test_ctx_asan unit/test_ctx_asan.c)
target_link_libraries(test_ctx_asan PRIVATE corocc)
add_test(NAME ctx_asan COMMAND test_ctx_asan)

# M2: coroutine object (L1)
add_executable(test_coro_smoke unit/test_coro_smoke.c)
target_link_libraries(test_coro_smoke PRIVATE corocc)
add_test(NAME coro_smoke COMMAND test_coro_smoke)

add_executable(test_coro_token unit/test_coro_token.c)
target_link_libraries(test_coro_token PRIVATE corocc)
add_test(NAME coro_token COMMAND test_coro_token)

add_executable(test_coro_cancel unit/test_coro_cancel.c)
target_link_libraries(test_coro_cancel PRIVATE corocc)
add_test(NAME coro_cancel COMMAND test_coro_cancel)

# M2 safety suites: exactly-once under concurrency, failure without side
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

# M3: the per-thread runtime as seen through the public API.
add_executable(test_thread_smoke unit/test_thread_smoke.c)
target_link_libraries(test_thread_smoke PRIVATE corocc)
if (NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_thread_smoke PRIVATE Threads::Threads)
endif()
add_test(NAME thread_smoke COMMAND test_thread_smoke)

# M3: cross-thread wakeup delivered to the owning thread.
add_executable(test_coro_crossresume unit/test_coro_crossresume.c)
target_link_libraries(test_coro_crossresume PRIVATE corocc)
if (NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_coro_crossresume PRIVATE Threads::Threads)
endif()
add_test(NAME coro_crossresume COMMAND test_coro_crossresume)

# M3 concurrency suites. Only registered when the platform provides POSIX
# threads. The TSan-visible suite needs the internal headers, so it also gets
# the source root on its include path.
if (NOT WIN32)
    find_package(Threads REQUIRED)
    add_executable(test_conc_double_resume concurrency/test_conc_double_resume.c)
    target_link_libraries(test_conc_double_resume PRIVATE corocc Threads::Threads)
    add_test(NAME conc_double_resume COMMAND test_conc_double_resume)

    add_executable(test_conc_runtime_tsan concurrency/test_conc_runtime_tsan.c)
    target_include_directories(test_conc_runtime_tsan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_conc_runtime_tsan PRIVATE corocc Threads::Threads)
    add_test(NAME conc_runtime_tsan COMMAND test_conc_runtime_tsan)
endif()

# M4: the timer heap and the monotone clock.
add_executable(test_timer unit/test_timer.c)
target_include_directories(test_timer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(test_timer PRIVATE corocc)
add_test(NAME timer COMMAND test_timer)

# M4: the single-thread scheduler.
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

# M:N work-stealing runtime (L2).
if (NOT WIN32)
    find_package(Threads REQUIRED)
    add_executable(test_rt_mn unit/test_rt_mn.c)
    target_include_directories(test_rt_mn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_rt_mn PRIVATE corocc Threads::Threads)
    add_test(NAME rt_mn COMMAND test_rt_mn)

    # P3 sync primitives on the M:N runtime.
    add_executable(test_rt_sync unit/test_rt_sync.c)
    target_include_directories(test_rt_sync PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_rt_sync PRIVATE corocc Threads::Threads)
    add_test(NAME rt_sync COMMAND test_rt_sync)

    # M5: writer-preferring read/write lock on the M:N runtime.
    add_executable(test_rt_rwlock unit/test_rt_rwlock.c)
    target_include_directories(test_rt_rwlock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_rt_rwlock PRIVATE corocc Threads::Threads)
    add_test(NAME rt_rwlock COMMAND test_rt_rwlock)

    # M5: bounded/unbounded channel on the M:N runtime.
    add_executable(test_rt_channel unit/test_rt_channel.c)
    target_include_directories(test_rt_channel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_rt_channel PRIVATE corocc Threads::Threads)
    add_test(NAME rt_channel COMMAND test_rt_channel)

    # L2 hardening set: retained-handle accounting (R2), join-chain
    # atomicity and cancel detach with a usable node afterwards (R3), and
    # -- landing with R4 -- the exactly-once wake-request claim. The
    # cancel-of-a-sync-chain scene (R1) is held back by the W11 verdict and
    # moves to its own task.
    add_executable(test_rt_lifecycle unit/test_rt_lifecycle.c)
    target_include_directories(test_rt_lifecycle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_rt_lifecycle PRIVATE corocc Threads::Threads)
    add_test(NAME rt_lifecycle COMMAND test_rt_lifecycle)
    # Concurrency suite: a lost wakeup must fail, not hang the run forever.
    set_tests_properties(rt_lifecycle PROPERTIES TIMEOUT 120)

    # D23 family: a cancellation consumed at a park's safe point leaves the
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
    # zero-init is OPEN). Step 1 of the L3 TDD sequence.
    add_executable(test_rt_scope_state unit/test_rt_scope_state.c)
    target_link_libraries(test_rt_scope_state PRIVATE corocc Threads::Threads)
    add_test(NAME rt_scope_state COMMAND test_rt_scope_state)
    set_tests_properties(rt_scope_state PROPERTIES TIMEOUT 120)

    # L3 step 2: the child result cell -- the trampoline is its only
    # writer, cell_out=NULL means "no result", the aggregate is the first
    # non-OK in completion order.
    add_executable(test_rt_scope_spawn unit/test_rt_scope_spawn.c)
    target_link_libraries(test_rt_scope_spawn PRIVATE corocc Threads::Threads)
    add_test(NAME rt_scope_spawn COMMAND test_rt_scope_spawn)
    set_tests_properties(rt_scope_spawn PROPERTIES TIMEOUT 120)

    # L3 step 3: the wait predicate -- one parking point on the scope
    # event chain, multi-waiter release with one resume each, immunity to
    # the waiter's own cancellation, and coverage of children registered
    # while a waiter is already parked.
    add_executable(test_rt_scope_wait unit/test_rt_scope_wait.c)
    target_include_directories(test_rt_scope_wait PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(test_rt_scope_wait PRIVATE corocc Threads::Threads)
    add_test(NAME rt_scope_wait COMMAND test_rt_scope_wait)
    set_tests_properties(rt_scope_wait PROPERTIES TIMEOUT 120)

    # L3 step 5: the group three-state family -- wait_all (live==0),
    # first_error (cancel the rest at the first error, still drain, report
    # that error promptly), first_done (report the first completion, scope
    # stays alive: follow-up wait or cancel is mandatory).
    add_executable(test_rt_scope_group unit/test_rt_scope_group.c)
    target_link_libraries(test_rt_scope_group PRIVATE corocc Threads::Threads)
    add_test(NAME rt_scope_group COMMAND test_rt_scope_group)
    set_tests_properties(rt_scope_group PROPERTIES TIMEOUT 120)

    # L3 step 6: wait_for with the watchdog -- fail-fast before any child
    # registration, the timer_fired CAS claim, the deadline-priority return
    # code, and the watchdog's cancel -> join -> collect reclaim on the
    # early exit (it is never a child and never touches live).
    add_executable(test_rt_scope_wait_for unit/test_rt_scope_wait_for.c)
    target_link_libraries(test_rt_scope_wait_for PRIVATE corocc Threads::Threads)
    add_test(NAME rt_scope_wait_for COMMAND test_rt_scope_wait_for)
    set_tests_properties(rt_scope_wait_for PROPERTIES TIMEOUT 120)

    # L3 step 7: the non-coroutine (thread-blocking) entry -- the wait
    # family called from a plain thread blocks on the runtime's settled
    # condition (never EAGAIN), and the thread-side wait_for waits on its
    # own deadline and claims the same once-only timeout.
    add_executable(test_rt_scope_thread unit/test_rt_scope_thread.c)
    target_link_libraries(test_rt_scope_thread PRIVATE corocc Threads::Threads)
    add_test(NAME rt_scope_thread COMMAND test_rt_scope_thread)
    set_tests_properties(rt_scope_thread PROPERTIES TIMEOUT 120)
endif()

# Custom allocator interface (openppp2-style).
add_executable(test_coro_allocator unit/test_coro_allocator.c)
target_link_libraries(test_coro_allocator PRIVATE corocc)
add_test(NAME coro_allocator COMMAND test_coro_allocator)

# M:N runtime benchmark (not a ctest: reports throughput).
if (NOT WIN32)
    find_package(Threads REQUIRED)
    add_executable(bench_rt_mn ../perf/bench_rt_mn.c)
    target_link_libraries(bench_rt_mn PRIVATE corocc Threads::Threads)

    # Pure context-switch throughput (pre-spawned; the timed window holds
    # only yield/drive round trips), plus the spawn/collect cost split.
    add_executable(bench_switch_pure ../perf/bench_switch_pure.c)
    target_link_libraries(bench_switch_pure PRIVATE corocc Threads::Threads)

    # D25 regression probe (perf/probe_collect_race.c): the collect-vs-retire
    # window. Probabilistic red/green -- intentionally NOT a ctest target;
    # run it repeatedly (see .opencode/logs/execR/D25/ for the proof scripts).
    add_executable(probe_collect_race ../perf/probe_collect_race.c)
    target_link_libraries(probe_collect_race PRIVATE corocc Threads::Threads)
endif()
