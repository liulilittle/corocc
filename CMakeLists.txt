cmake_minimum_required(VERSION 3.16)
project(corocc C CXX ASM)

option(COROCC_BUILD_TESTS "Build tests" ON)
option(COROCC_ASAN "Build with AddressSanitizer (fiber-switch hooks)" OFF)
option(COROCC_TSAN "Build with ThreadSanitizer (data races in the runtime)" OFF)
option(COROCC_PROBE_NOSPIN "Count compare-and-swap attempts so the zero-spin
contract can be asserted" OFF)
# M9 W1 staging (temporary): on Windows, build the corocc core subset while
# the runtime layer still binds pthread-shaped types from src/scheduler/rt.h
# (the M9 W2 window ports that header). The option and every staging block
# below are removed in M9 W2c, together with the full-target restore.
option(COROCC_M9_W1_STAGE "M9 W1 staging build (Windows core subset; temporary)" OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Zero-warning policy for ALL our code (library and tests). The vendored Boost asm is unaffected (C options).
if (MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

if (COROCC_ASAN)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
    add_compile_definitions(COROCC_ASAN=1)
endif()

if (COROCC_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

if (COROCC_PROBE_NOSPIN)
    add_compile_definitions(COROCC_PROBE_NOSPIN=1)
endif()

add_library(corocc STATIC
    src/coroutine/status.c
    src/coroutine/stack.c
    src/coroutine/ctx.c
    src/coroutine/coro.c
    src/coroutine/coro_runtime.c
    src/scheduler/timer.c
    src/scheduler/sched.c
    src/scheduler/rt.c
    src/scheduler/rt_sync.c
    src/scheduler/rt_scope.c
    src/scheduler/wsdeque.c
    src/scheduler/mpsc.c
    src/platform/coro_plat_locks.c
    src/platform/threads.c
    src/platform/wait.c)
target_include_directories(corocc PUBLIC include)
target_include_directories(corocc PRIVATE src)
if (MSVC)
    # coro_status.h maps COROCC_API to __declspec(dllimport) unless
    # COROCC_BUILDING is defined; the static corocc target must define it for
    # itself and for consumers linking it, or every call site resolves to
    # __imp_* and linking fails with LNK2019 (M9 probe A2). The public header
    # stays unchanged; this is the static-library spelling of its contract.
    target_compile_definitions(corocc PUBLIC COROCC_BUILDING=1)
endif()
if (WIN32 AND COROCC_M9_W1_STAGE)
    # M9 W1 staging (temporary; removed in W2c). The runtime layer binds
    # pthread-shaped types through src/scheduler/rt.h, which the W2 window
    # ports; every TU that includes it stays out of the staged target.
    # rt.c/rt_sync.c/sched.c are the mandated exclusions; rt_scope.c is the
    # L3 integration point (a post-design addition), and wsdeque.c/mpsc.c are
    # the same dependency class (they include rt.h).
    set_source_files_properties(
        src/scheduler/rt.c
        src/scheduler/rt_sync.c
        src/scheduler/sched.c
        src/scheduler/rt_scope.c
        src/scheduler/wsdeque.c
        src/scheduler/mpsc.c
        PROPERTIES HEADER_FILE_ONLY TRUE)
    # coro_atomic.h keeps its R'-frozen MSVC body until the W2 atomic window
    # (fast path, counters); its coro_at_fence currently calls MemoryBarrier(),
    # which <windows.h> would define. Supply the exact winnt.h (x64) spelling
    # to the two staged TUs that include the header, so the frozen file
    # compiles under /W4 /WX without being touched.
    set_source_files_properties(src/coroutine/coro.c src/coroutine/coro_runtime.c
        PROPERTIES COMPILE_DEFINITIONS "MemoryBarrier=__faststorefence")
endif()
if (NOT MSVC)
    target_compile_options(corocc PRIVATE -fvisibility=hidden)   # export only public symbols
endif()
include(cmake/arch_select.cmake)   # appends asm sources to corocc

if (COROCC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
