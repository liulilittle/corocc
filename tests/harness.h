#ifndef COROCC_TEST_HARNESS_H
#define COROCC_TEST_HARNESS_H
#include <stdio.h>
#include <stdlib.h>

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)
#define TEST_MAIN() int main(void) { run_tests(); \
    if (g_failures) { fprintf(stderr, "%d check(s) failed\n", g_failures); return 1; } \
    printf("ok\n"); return 0; }
#endif
