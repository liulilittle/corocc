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
