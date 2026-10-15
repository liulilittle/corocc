cmake_minimum_required(VERSION 3.16)
project(corocc C CXX ASM)

option(COROCC_BUILD_TESTS "Build tests" ON)
option(COROCC_ASAN "Build with AddressSanitizer (fiber-switch hooks)" OFF)
option(COROCC_TSAN "Build with ThreadSanitizer (data races in the runtime)" OFF)
option(COROCC_PROBE_NOSPIN "Count compare-and-swap attempts so the zero-spin
contract can be asserted" OFF)
# Link-time code generation (MSVC /GL + /LTCG) for Release builds. The M9
# performance gate was measured with it (the design's ladder, section 4) and
# the gate configuration must be the delivery configuration: a Release build
# that ships without it is measurably slower (pure-switch gate: ~8M vs ~20M
# round trips/s). Turn OFF for a plain /O2 build.
option(COROCC_LTCG "Enable link-time code generation in Release builds (MSVC /GL)" ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Zero-warning policy for ALL our C/C++ code (library and tests). The options
# are attached per language so they never reach the assemblers: ml64 rejects
# /W4 /WX /utf-8, and under the Visual Studio generator the directory-level
# options used to leak into the ASM_MASM build customization (MSB3721,
# audit A-2); the vendored Boost asm is unaffected (C options).
if (MSVC)
    # /utf-8: sources are stored as UTF-8 (some comments non-ASCII); without
    # it the compiler reads them in the system code page and warns C4819,
    # which /WX turns fatal.
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/W4;/WX;/utf-8>")
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

if (COROCC_ASAN)
    if (MSVC)
        # cl.exe spells the sanitizer /fsanitize=address; the -fsanitize
        # spelling below is GCC/Clang only and would be rejected by cl/link.
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address)
        add_link_options(-fsanitize=address)
    endif()
    add_compile_definitions(COROCC_ASAN=1)
endif()

if (COROCC_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

if (COROCC_PROBE_NOSPIN)
    add_compile_definitions(COROCC_PROBE_NOSPIN=1)
endif()

if (MSVC AND COROCC_LTCG)
    # Per-config: Debug keeps plain /Od, Release gets /GL + /LTCG. The
    # variable drives the INTERPROCEDURAL_OPTIMIZATION_RELEASE property for
    # every target created below.
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
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
if (NOT MSVC)
    target_compile_options(corocc PRIVATE -fvisibility=hidden)   # export only public symbols
endif()
include(cmake/arch_select.cmake)   # appends asm sources to corocc

if (COROCC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
