# corocc -- libevent integration example (stand-alone project).
#
# Not part of the library build/install: it shows how a libevent event_base
# drives a LOOP-family runtime through coro_rt_pump, with the doorbell/clock/
# timer supplied by the example's driver.
#
#     cmake -S examples/backends/libevent -B build-libevent -DCMAKE_BUILD_TYPE=Release
#     cmake --build build-libevent && ctest --test-dir build-libevent --output-on-failure
#
# Options:
#   COROCC_EVENT_TESTS  ON|OFF  build the example's smoke test (default ON)

cmake_minimum_required(VERSION 3.16)
project(corocc_libevent_example CXX)

option(COROCC_EVENT_TESTS "Build the libevent example smoke test" ON)

set(COROCC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../.."
                 "${CMAKE_CURRENT_BINARY_DIR}/corocc-build"
                 EXCLUDE_FROM_ALL)

find_path(EVENT_INCLUDE_DIR event2/event.h)
find_library(EVENT_LIBRARY NAMES event libevent)
find_library(EVENT_PTHREADS_LIBRARY NAMES event_pthreads libevent_pthreads)
if (NOT EVENT_INCLUDE_DIR OR NOT EVENT_LIBRARY)
    message(FATAL_ERROR
        "libevent not found. Install it (e.g. apt-get install libevent-dev) or "
        "pass -DEVENT_INCLUDE_DIR=... -DEVENT_LIBRARY=...")
endif()

find_package(Threads REQUIRED)

add_library(corocc_event INTERFACE)
target_include_directories(corocc_event INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${EVENT_INCLUDE_DIR}")
target_link_libraries(corocc_event INTERFACE corocc ${EVENT_LIBRARY}
    ${EVENT_PTHREADS_LIBRARY} Threads::Threads)

function(corocc_event_target name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE corocc_event)
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED ON)
    if (MSVC)
        target_compile_options(${name} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

if (COROCC_EVENT_TESTS)
    enable_testing()
    corocc_event_target(event_smoke tests/event_smoke.cpp)
    add_test(NAME event_smoke COMMAND event_smoke)
    set_tests_properties(event_smoke PROPERTIES TIMEOUT 120)
endif()
