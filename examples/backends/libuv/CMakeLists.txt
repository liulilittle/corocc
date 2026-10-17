# corocc -- libuv integration example (stand-alone project).
#
# Not part of the library build/install: it shows how a libuv loop drives a
# LOOP-family runtime through coro_rt_pump, with the doorbell/clock/timer
# supplied by the example's driver.
#
#     cmake -S examples/backends/libuv -B build-libuv -DCMAKE_BUILD_TYPE=Release
#     cmake --build build-libuv && ctest --test-dir build-libuv --output-on-failure
#
# Options:
#   COROCC_UV_TESTS  ON|OFF  build the example's smoke test (default ON)

cmake_minimum_required(VERSION 3.16)
project(corocc_libuv_example CXX)

option(COROCC_UV_TESTS "Build the libuv example smoke test" ON)

set(COROCC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../.."
                 "${CMAKE_CURRENT_BINARY_DIR}/corocc-build"
                 EXCLUDE_FROM_ALL)

find_path(UV_INCLUDE_DIR uv.h)
find_library(UV_LIBRARY NAMES uv libuv)
if (NOT UV_INCLUDE_DIR OR NOT UV_LIBRARY)
    message(FATAL_ERROR
        "libuv not found. Install it (e.g. apt-get install libuv1-dev) or pass "
        "-DUV_INCLUDE_DIR=<dir with uv.h> -DUV_LIBRARY=<libuv>.")
endif()

find_package(Threads REQUIRED)

add_library(corocc_uv INTERFACE)
target_include_directories(corocc_uv INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${UV_INCLUDE_DIR}")
target_link_libraries(corocc_uv INTERFACE corocc ${UV_LIBRARY}
    Threads::Threads ${CMAKE_DL_LIBS})

function(corocc_uv_target name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE corocc_uv)
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED ON)
    if (MSVC)
        target_compile_options(${name} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

if (COROCC_UV_TESTS)
    enable_testing()
    corocc_uv_target(uv_smoke tests/uv_smoke.cpp)
    add_test(NAME uv_smoke COMMAND uv_smoke)
    set_tests_properties(uv_smoke PROPERTIES TIMEOUT 120)
endif()
