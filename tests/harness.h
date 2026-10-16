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
/* Equality check that reports the actual and expected values on failure:
   a bare CHECK of `a == b` prints the expression, not the two values, which
   hides the observation a diagnosis needs. Operands are evaluated once each
   and compared in the widest signed integer type (status codes, counters,
   booleans and small sizes all fit); use plain CHECK for comparisons that
   are not value equalities. */
#define CHECK_EQ(actual, expected) do { \
    long long ce_a_ = (long long)(actual); \
    long long ce_e_ = (long long)(expected); \
    if (ce_a_ != ce_e_) { \
        fprintf(stderr, "FAIL %s:%d: %s == %s (actual=%lld, expected=%lld)\n", \
                __FILE__, __LINE__, #actual, #expected, ce_a_, ce_e_); \
        g_failures++; \
    } \
} while (0)
#define TEST_MAIN() int main(void) { run_tests(); \
    if (g_failures) { fprintf(stderr, "%d check(s) failed\n", g_failures); return 1; } \
    printf("ok\n"); return 0; }
#endif
