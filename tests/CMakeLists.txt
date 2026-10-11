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
