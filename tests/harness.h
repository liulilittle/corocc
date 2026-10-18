#ifndef COROCC_TEST_HARNESS_H
#define COROCC_TEST_HARNESS_H
#include <stdio.h>
#include <stdlib.h>

/* Windows / MSVC Debug: a CRT assertion (_CRT_ASSERT/_CRT_ERROR) or an
 * abort() normally opens a modal "Debug Error!" dialog. In an unattended
 * run that dialog blocks the whole desktop -- including any AI session
 * driving it -- and the test never returns, so ctest hangs instead of
 * failing with a readable message. Redirect every debug report to stderr
 * and keep abort()'s message on stderr while clearing report-fault (which
 * is what raises the dialog): an assertion or abort then lands in the
 * captured output plus a non-zero exit code, never a window.
 *
 * Debug-only on purpose: a Release build must behave exactly as before
 * (the CI Windows jobs run Release ctest; Debug is a build-only gate). The
 * _Crt* entry points are no-ops in a non-debug CRT anyway, and gating on
 * _DEBUG keeps the SIGABRT handler off the Release path too. */
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <process.h>
#include <signal.h>

static void corocc_test_on_abort(int sig) {
    (void)sig;
    fputs("corocc-test: SIGABRT (abort() called)\n", stderr);
    fflush(stderr);
    /* Do not return: returning re-enters abort()'s message-box path. */
    _exit(3);
}

static int __cdecl corocc_test_quiet_crt(void) {
    (void)_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    /* Write the abort message, but never call report-fault. */
    (void)_set_abort_behavior(_WRITE_ABORT_MSG,
                              _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, corocc_test_on_abort);
    return 0;
}

/* Run before main: the CRT walks the .CRT$XCU section and calls every
 * pointer there. The pointer is per-TU static, so the ODR-style two-TU
 * binary gets two idempotent calls, not a duplicate symbol. */
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU"))
static int(__cdecl* corocc_test_quiet_crt_ctor_)(void) = corocc_test_quiet_crt;

#define COROCC_TEST_EARLY() ((void)corocc_test_quiet_crt())
#else
#define COROCC_TEST_EARLY() ((void)0)
#endif

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
#define TEST_MAIN() int main(void) { COROCC_TEST_EARLY(); run_tests(); \
    if (g_failures) { fprintf(stderr, "%d check(s) failed\n", g_failures); return 1; } \
    printf("ok\n"); return 0; }
#endif
