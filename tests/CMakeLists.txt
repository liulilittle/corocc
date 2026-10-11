add_executable(test_smoke unit/test_smoke.c)
target_link_libraries(test_smoke PRIVATE corocc)
add_test(NAME smoke COMMAND test_smoke)
