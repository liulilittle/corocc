# corocc -- asio-host integration example (stand-alone project).
#
# This directory is NOT part of the corocc library build or install: the
# library ships only its own core and its built-in EDSM backend. External
# integrations such as Boost.Asio live here as examples that show how the
# library is used / how a backend can be switched.
#
# The project is self-contained. It re-enters the repository root only to
# build the corocc static library it links against, with the library's own
# tests disabled:
#
#     cmake -S examples/backends/asio-host -B build-asio-host -DCMAKE_BUILD_TYPE=Release
#     cmake --build build-asio-host
#     ctest --test-dir build-asio-host --output-on-failure
#
# Options:
#   COROCC_ASIO_HOST_TESTS  ON|OFF  build the example's tests (default ON)
#   BOOST_INCLUDE_DIR       Boost include directory (auto-detected)
#   COROCC_ASAN             ON to build library + example with AddressSanitizer

cmake_minimum_required(VERSION 3.16)
project(corocc_asio_host_example CXX)

option(COROCC_ASIO_HOST_TESTS "Build the asio-host example tests" ON)

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

add_library(corocc_asio_host INTERFACE)
target_include_directories(corocc_asio_host INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${BOOST_INCLUDE_DIR}")
target_link_libraries(corocc_asio_host INTERFACE corocc Threads::Threads)

# corocc_asio_host_target(<name> <sources...>): one example executable with
# the C++11 baseline and the zero-warning policy.
function(corocc_asio_host_target name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE corocc_asio_host)
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED ON)
    if (MSVC)
        target_compile_options(${name} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

corocc_asio_host_target(asio_host_demo demo/host_echo.cpp)

if (COROCC_ASIO_HOST_TESTS)
    enable_testing()

    corocc_asio_host_target(asio_host_single tests/host_single.cpp)
    corocc_asio_host_target(asio_host_strand tests/host_strand.cpp)
    corocc_asio_host_target(asio_host_stress tests/host_stress.cpp)
    corocc_asio_host_target(asio_host_errors tests/host_errors.cpp)
    corocc_asio_host_target(asio_host_awaitable tests/host_awaitable.cpp)
    corocc_asio_host_target(asio_host_migration tests/host_migration.cpp)
    corocc_asio_host_target(asio_host_abandon tests/host_abandon.cpp)
    corocc_asio_host_target(asio_host_coexist tests/host_coexist.cpp
                                              tests/host_coexist_b.cpp)

    add_test(NAME asio_host_single COMMAND asio_host_single)
    add_test(NAME asio_host_strand COMMAND asio_host_strand)
    add_test(NAME asio_host_stress COMMAND asio_host_stress)
    add_test(NAME asio_host_errors COMMAND asio_host_errors)
    add_test(NAME asio_host_awaitable COMMAND asio_host_awaitable)
    add_test(NAME asio_host_migration COMMAND asio_host_migration)
    add_test(NAME asio_host_abandon COMMAND asio_host_abandon)
    add_test(NAME asio_host_coexist COMMAND asio_host_coexist)

    set_tests_properties(
        asio_host_single asio_host_strand asio_host_stress
        asio_host_errors asio_host_awaitable asio_host_migration
        asio_host_abandon asio_host_coexist
        PROPERTIES TIMEOUT 120)
endif()
