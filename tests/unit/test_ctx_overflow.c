#include "../harness.h"
#include <corocc/coro_ctx.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static void deep(void* a) {
    volatile char buf[128 * 1024]; /* far larger than the 64 KiB stack */
    size_t i;
    /* Volatile stores: no optimizer may drop the frame or the writes (the
       Release /LTCG build eliminated a memset through a cast that discarded
       the volatile qualifier, frame and all). The first stores land below
       the usable region and fault on the guard page, which is the entire
       point of this test. */
    for (i = 0; i < sizeof buf; ++i) buf[i] = 1;
    (void)a;
}

/* Runs the overflowing coroutine; never returns if the guard page works. */
static void run_overflowing_coroutine(void) {
    coro_stack stk;
    coro_ctx c, m;
    coro_stack_alloc(&stk, 64 * 1024); /* far smaller than buf */
    coro_ctx_init(&c, &stk, deep, NULL);
    coro_ctx_swap(&m, &c); /* expected to fault on the guard page */
    _exit(2);              /* reached only if the guard did NOT catch it */
}

#if defined(_WIN32)

/*
 * Windows has no fork; the child runs the same executable again with an
 * environment marker set, which the child detects before the harness runs.
 * The guard page turns the overflow into an access violation, so the child
 * dies with exit code 0xC0000005 (STATUS_ACCESS_VIOLATION) -- the Windows
 * analogue of death-by-SIGSEGV. An ASan-instrumented build may instead
 * fail-fast with 0xC0000409 (STATUS_STACK_BUFFER_OVERRUN) or abort with
 * exit code 3, both of which still prove the fault happened. A normal exit
 * means the guard failed.
 */
#define COROCC_OVERFLOW_CHILD_ENV "COROCC_CTX_OVERFLOW_CHILD"

/* Literal CHECK(0 && ...)/CHECK(1) forms trip MSVC /W4 C4127 (constant
   conditional); this helper keeps the same tally without a constant. */
static void fail_msg(const char* msg) {
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);
    ++g_failures;
}

static void run_tests(void) {
    char exe[MAX_PATH];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;

    if (GetEnvironmentVariableA(COROCC_OVERFLOW_CHILD_ENV, NULL, 0) != 0) {
        /* The child branch: runs the overflowing coroutine and never comes
           back (the callee ends in _exit(2)). Written as an if/else so no
           statement follows the non-returning call: the /LTCG inliner
           proves the non-return and flags any trailing statement as
           unreachable code (C4702, fatal under /WX). */
        run_overflowing_coroutine();
    } else {
        if (GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe) == 0) {
            fail_msg("GetModuleFileName failed");
            return;
        }
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        memset(&pi, 0, sizeof pi);
        SetEnvironmentVariableA(COROCC_OVERFLOW_CHILD_ENV, "1");
        if (!CreateProcessA(exe, NULL, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            fail_msg("CreateProcess failed");
            return;
        }
        SetEnvironmentVariableA(COROCC_OVERFLOW_CHILD_ENV, NULL);
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        /* The overflow must be FATAL, never a silent return. Accepted shapes:
           - 0xC0000005 STATUS_ACCESS_VIOLATION: the PAGE_NOACCESS guard faulted;
           - an ASan build may report 0xC0000409 STATUS_STACK_BUFFER_OVERRUN or
             abort (exit code 3) on the same fault. */
        {
            int accepted = (code == 0xC0000005u);
#if defined(COROCC_ASAN)
            accepted = accepted || (code == 0xC0000409u) || (code == 3u);
#endif
            if (!accepted) {
                fail_msg("coroutine returned instead of faulting - guard page broken");
            }
        }
    }
}

#else /* !_WIN32 */

static void run_tests(void) {
    pid_t child = fork();
    if (child == 0) {
        run_overflowing_coroutine();
        _exit(2); /* unreachable */
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child) {
        CHECK(0 && "waitpid failed");
        return;
    }
    /* The overflow must be FATAL, never a silent return. Two death shapes are
       acceptable because both prove the guard page caught the write:
         - death by SIGSEGV: the PROT_NONE page faulted directly;
         - death by SIGABRT: under an ASan-instrumented build (the
           abort_on_error=1 convention this suite is run with) the sanitizer
           intercepts the same fault, reports it and aborts.
       A nonzero exit status is accepted for sanitizers that report through
       their own exit code instead. Only a normal exit means the guard
       failed. */
    if (WIFSIGNALED(status)) {
        CHECK(WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGABRT);
    } else if (WIFEXITED(status)) {
        CHECK(WEXITSTATUS(status) != 0);
        CHECK(!(WEXITSTATUS(status) == 2) &&
              "coroutine returned instead of faulting - guard page broken");
    } else {
        CHECK(0 && "child neither signalled nor exited");
    }
}

#endif /* !_WIN32 */

TEST_MAIN()
