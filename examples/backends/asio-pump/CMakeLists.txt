# corocc -- asio-pump integration example (stand-alone project).
#
# This directory is NOT part of the corocc library build or install: it shows
# how a host event loop (Boost.Asio's io_context) drives a LOOP-family runtime
# by calling coro_rt_pump, with the runtime's doorbell/clock/timer supplied by
# the example's driver.
#
# The project is self-contained. It re-enters the repository root only to build
# the corocc static library it links against, with the library's own tests
# disabled:
#
#     cmake -S examples/backends/asio-pump -B build-asio-pump -DCMAKE_BUILD_TYPE=Release
#     cmake --build build-asio-pump
#     ctest --test-dir build-asio-pump --output-on-failure
#
# Options:
#   COROCC_ASIO_PUMP_TESTS  ON|OFF  build the example's tests (default ON)
#   BOOST_INCLUDE_DIR       Boost include directory (auto-detected)
#   COROCC_ASAN             ON to build library + example with AddressSanitizer

cmake_minimum_required(VERSION 3.16)
project(corocc_asio_pump_example CXX)

option(COROCC_ASIO_PUMP_TESTS "Build the asio-pump example tests" ON)

# Build the corocc library from the repository root, without its tests.
set(COROCC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../.."
                 "${CMAKE_CURRENT_BINARY_DIR}/corocc-build"
                 EXCLUDE_FROM_ALL)

find_path(BOOST_INCLUDE_DIR boost/asio.hpp)
if (NOT BOOST_INCLUDE_DIR)
    message(FATAL_ERROR
        "Boost.Asio headers not found. Install Boost (e.g. libboost-dev) or "
        "pass -DBOOST_INCLUDE_DIR=<dir containing boost/asio.hpp>.")
endif()

find_package(Threads REQUIRED)

if (COROCC_ASAN)
    if (MSVC)
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address)
        add_link_options(-fsanitize=address)
    endif()
endif()

add_library(corocc_asio_pump INTERFACE)
target_include_directories(corocc_asio_pump INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${BOOST_INCLUDE_DIR}")
target_link_libraries(corocc_asio_pump INTERFACE corocc Threads::Threads)

# corocc_asio_pump_target(<name> <sources...>): one example executable with the
# C++11 baseline and the zero-warning policy.
function(corocc_asio_pump_target name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE corocc_asio_pump)
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED ON)
    if (MSVC)
        target_compile_options(${name} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

if (COROCC_ASIO_PUMP_TESTS)
    enable_testing()

    corocc_asio_pump_target(pump_single tests/pump_single.cpp)
    corocc_asio_pump_target(pump_io tests/pump_io.cpp)
    corocc_asio_pump_target(pump_stress tests/pump_stress.cpp)
    corocc_asio_pump_target(pump_cancel tests/pump_cancel.cpp)
    corocc_asio_pump_target(pump_mixed tests/pump_mixed.cpp)

    add_test(NAME pump_single COMMAND pump_single)
    add_test(NAME pump_io COMMAND pump_io)
    add_test(NAME pump_stress COMMAND pump_stress)
    add_test(NAME pump_cancel COMMAND pump_cancel)
    add_test(NAME pump_mixed COMMAND pump_mixed)

    set_tests_properties(
        pump_single pump_io pump_stress pump_cancel pump_mixed
        PROPERTIES TIMEOUT 120)
endif()
