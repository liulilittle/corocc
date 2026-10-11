cmake_minimum_required(VERSION 3.16)
project(corocc C CXX ASM)

option(COROCC_BUILD_TESTS "Build tests" ON)
option(COROCC_ASAN "Build with AddressSanitizer (fiber-switch hooks)" OFF)
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

add_library(corocc STATIC
    src/coroutine/status.c
    src/coroutine/stack.c
    src/coroutine/ctx.c
    src/coroutine/coro.c)
target_include_directories(corocc PUBLIC include)
target_include_directories(corocc PRIVATE src)
if (NOT MSVC)
    target_compile_options(corocc PRIVATE -fvisibility=hidden)   # export only public symbols
endif()
include(cmake/arch_select.cmake)   # appends asm sources to corocc

if (COROCC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
