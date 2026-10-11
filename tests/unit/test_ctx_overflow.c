#include "../harness.h"
#include <corocc/coro_ctx.h>
#include <string.h>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void deep(void* a) {
    volatile char buf[128 * 1024]; /* far larger than the 64 KiB stack */
    memset((void*)buf, 1, sizeof buf);
    (void)buf;
    (void)a;
}

/* Runs the overflowing coroutine; never returns if the guard page works. */
static void run_overflowing_coroutine(void) {
    coro_stack stk;
    coro_ctx c, m;
    coro_stack_alloc(&stk, 64 * 1024); /* far smaller than buf */
    coro_ctx_init(&c, &stk, deep, NULL);
    coro_ctx_swap(&m, &c); /* expected to fault on the guard page (SIGSEGV) */
    _exit(2);              /* reached only if the guard did NOT catch it */
}

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
         - a nonzero exit status: under an ASan-instrumented build the
           sanitizer intercepts the same fault, reports "stack-overflow" and
           terminates with its own exit code.
       Only a normal exit means the guard failed. */
    if (WIFSIGNALED(status)) {
        CHECK(WTERMSIG(status) == SIGSEGV);
    } else if (WIFEXITED(status)) {
        CHECK(WEXITSTATUS(status) != 0);
        CHECK(!(WEXITSTATUS(status) == 2) &&
              "coroutine returned instead of faulting - guard page broken");
    } else {
        CHECK(0 && "child neither signalled nor exited");
    }
}

TEST_MAIN()