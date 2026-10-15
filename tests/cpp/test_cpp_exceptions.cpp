/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * tests/cpp/test_cpp_exceptions.cpp -- the exception boundary: the capture-
 * and-deliver contract at every wrapper entry (L1, L2 task, detached
 * handler, sched_task, L3 scope), the authoritative exception channel
 * (first_exception / wait_or_throw, including the timeout scene), and the
 * death tests for the documented red lines (detached default terminate,
 * self-destroy of a running coroutine, cell::get before readiness).
 */
#include "../harness.h"

#include <corocc/coro.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

struct my_problem {
    int code;
};

std::atomic<int> g_handler_calls(0);
std::exception_ptr g_handler_eptr;

void capture_handler(std::exception_ptr e) {
    g_handler_calls.fetch_add(1);
    g_handler_eptr = e;
}

} /* namespace */

/* ---- the death-test child bodies (the process must die, never return) ---- */

static void run_death_scenario(int which) {
    switch (which) {
    case 1: {
        /* A detached coroutine escapes with the default handler: the
         * documented red line is std::terminate on the worker thread. */
        corocc::runtime rt(2);
        (void)corocc::set_detached_exception_handler(NULL); /* default */
        (void)rt.spawn([]() { throw my_problem{1}; });
        (void)rt.wait_all();
        break;
    }
    case 2: {
        /* A running coroutine's destructor: EBUSY on the C side is a
         * contract violation for the RAII path -> terminate. */
        corocc::coroutine* c = new corocc::coroutine();
        (void)corocc::coroutine::create(*c, [c]() {
            c->~coroutine(); /* running: must terminate, never return */
        });
        (void)c->start();
        break;
    }
    case 3: {
        /* cell::get() before the child finished: a documented red line. */
        corocc::runtime rt(2);
        corocc::scope s(rt);
        corocc::cell<int> out;
        (void)s.spawn(out, [&rt]() -> int {
            (void)rt.sleep_for(std::chrono::seconds(30));
            return 1;
        });
        (void)out.get();
        break;
    }
    default:
        break;
    }
}

/* Literal CHECK(0 && ...) forms trip MSVC /W4 C4127; this helper keeps the
 * same tally without a constant condition. */
static void fail_msg(const char* msg) {
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);
    ++g_failures;
}

#if defined(_WIN32)
/* Windows has no fork: re-exec this binary with an environment marker and
 * assert the child died with a failure shape. */
#define COROCC_CPP_DEATH_CHILD_ENV "COROCC_CPP_DEATH_CHILD"

static void death_child_dispatch(void) {
    char buf[16];
    if (GetEnvironmentVariableA(COROCC_CPP_DEATH_CHILD_ENV, buf, sizeof buf) == 0) {
        return;
    }
    run_death_scenario(atoi(buf));
    /* The scenario returned: the red line failed to fire. Exit 0 so the
     * parent marks the failure. */
}
#endif

/* ---- the six-entry delivery matrix ---- */

static void test_l1_exception(void) {
    corocc::coroutine c;
    CHECK(corocc::coroutine::create(c, []() -> coro_status {
        throw my_problem{11};
    }).ok());
    CHECK(c.start().ok());
    CHECK(c.done());
    bool caught = false;
    try {
        std::rethrow_exception(c.exception());
    } catch (const my_problem& e) {
        caught = (e.code == 11);
    }
    CHECK(caught);
    CHECK(c.destroy().ok());
}

static void test_task_exception(void) {
    corocc::runtime rt(2);
    corocc::task<int> t;
    CHECK(rt.spawn_h(t, []() -> int { throw my_problem{12}; }).ok());
    CHECK(t.wait().ok()); /* the closeout never throws */
    bool caught = false;
    try {
        (void)t.get();
    } catch (const my_problem& e) {
        caught = (e.code == 12);
    }
    CHECK(caught);
}

static void test_detached_handler_replacement(void) {
    corocc::runtime rt(2);
    g_handler_calls.store(0);
    g_handler_eptr = std::exception_ptr();
    corocc::detached_exception_handler old =
        corocc::set_detached_exception_handler(&capture_handler);
    CHECK(old != NULL); /* the previous handler comes back */
    /* Installing again returns the CURRENT (capture) handler. */
    CHECK(corocc::set_detached_exception_handler(&capture_handler) ==
          &capture_handler);
    CHECK(rt.spawn([]() { throw my_problem{13}; }).ok());
    CHECK(rt.wait_all().ok());
    corocc::set_detached_exception_handler(old);
    CHECK(g_handler_calls.load() == 1);
    bool caught = false;
    try {
        std::rethrow_exception(g_handler_eptr);
    } catch (const my_problem& e) {
        caught = (e.code == 13);
    }
    CHECK(caught);
}

static void test_sched_task_exception(void) {
    corocc::scheduler s;
    CHECK(corocc::scheduler::create(s).ok());
    corocc::sched_task<int> t;
    CHECK(s.spawn_h(t, []() -> int { throw my_problem{14}; }).ok());
    CHECK(s.run().ok());
    bool caught = false;
    try {
        (void)t.get();
    } catch (const my_problem& e) {
        caught = (e.code == 14);
    }
    CHECK(caught);
}

static void test_scope_first_exception_authoritative(void) {
    corocc::runtime rt(2);
    corocc::scope s(rt);
    /* One abnormal child (ECANCELED code + exception) and one clean child:
     * first_exception() is the authority over the code channel. */
    CHECK(s.spawn([]() -> coro_status { throw my_problem{15}; }).ok());
    CHECK(s.spawn([]() -> coro_status { return CORO_OK; }).ok());
    const corocc::status agg = s.wait();
    CHECK(agg.code() == CORO_ECANCELED);
    std::exception_ptr e = s.first_exception();
    CHECK(e != std::exception_ptr());
    bool caught = false;
    try {
        s.wait_or_throw();
    } catch (const my_problem& ex) {
        caught = (ex.code == 15);
    }
    CHECK(caught);
    CHECK(s.destroy().ok());
}

static void test_wait_or_throw_exception_priority_on_timeout(void) {
    corocc::runtime rt(2);
    corocc::scope s(rt);
    /* The abnormal child ends immediately; the sleeper holds the scope open
     * past the deadline, so wait_for returns ECANCELED (meaning 3) -- and
     * wait_or_throw must still rethrow the body's exception, not the code. */
    CHECK(s.spawn([]() { throw my_problem{16}; }).ok());
    CHECK(s.spawn([&rt]() {
        (void)rt.sleep_for(std::chrono::seconds(30));
    }).ok());
    const corocc::status st = s.wait_for(std::chrono::milliseconds(50));
    CHECK(st.code() == CORO_ECANCELED);
    CHECK(s.first_exception() != std::exception_ptr());
    bool caught = false;
    try {
        s.wait_or_throw();
    } catch (const my_problem& ex) {
        caught = (ex.code == 16);
    } catch (const corocc::error&) {
        caught = false; /* the code channel must NOT win */
    }
    CHECK(caught);
    CHECK(s.destroy().ok());
}

static void test_wait_or_throw_code_when_no_exception(void) {
    corocc::runtime rt(2);
    corocc::scope s(rt);
    CHECK(s.spawn([]() -> coro_status { return CORO_ESTALE; }).ok());
    bool got_error = false;
    try {
        s.wait_or_throw();
    } catch (const corocc::error& e) {
        got_error = (e.code() == CORO_ESTALE);
        CHECK(std::string(e.what()) == "CORO_ESTALE");
    }
    CHECK(got_error);
    CHECK(s.destroy().ok());
}

static void test_task_get_throws_error_on_status_failure(void) {
    corocc::runtime rt(2);
    corocc::task<int> t;
    CHECK(rt.spawn_h(t, [&rt]() -> int {
        const corocc::status st = rt.sleep_for(std::chrono::seconds(30));
        return st.ok() ? 1 : 0;
    }).ok());
    CHECK(t.cancel().ok());
    CHECK(t.wait().ok());
    /* The body returned normally, so get() yields the value, not a throw. */
    CHECK(t.get() == 0);

    /* A task killed by its own caller cancellation before it ever ran still
     * has a defined closeout: cancel + wait + get reads the snapshot. */
    corocc::task<void> tv;
    CHECK(rt.spawn_h(tv, []() {}).ok());
    CHECK(tv.wait().ok());
    tv.get();
}

#if defined(_WIN32)

static void test_death_shapes(void) {
    char exe[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe) == 0) {
        fail_msg("GetModuleFileName failed");
        return;
    }
    for (int which = 1; which <= 3; ++which) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", which);
        SetEnvironmentVariableA(COROCC_CPP_DEATH_CHILD_ENV, buf);
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        memset(&pi, 0, sizeof pi);
        DWORD code = 0;
        if (!CreateProcessA(exe, NULL, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            SetEnvironmentVariableA(COROCC_CPP_DEATH_CHILD_ENV, NULL);
            fail_msg("CreateProcess failed");
            continue;
        }
        SetEnvironmentVariableA(COROCC_CPP_DEATH_CHILD_ENV, NULL);
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        /* Accepted death shapes: abort (3), fast-fail (0xC0000409) or an
         * access violation under a sanitizer; exit 0 means the red line
         * failed to fire. */
        const int accepted = (code != 0);
        if (!accepted) {
            fail_msg("death scenario returned instead of dying");
        }
    }
}

#else /* !_WIN32 */

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

static void test_death_shapes(void) {
    for (int which = 1; which <= 3; ++which) {
        const pid_t child = fork();
        if (child == 0) {
            /* The child's abort diagnostics are expected noise; keep them
             * out of this process's captured output. */
            const int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                (void)dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            run_death_scenario(which);
            _exit(0); /* the red line failed to fire */
        }
        int status = 0;
        if (waitpid(child, &status, 0) != child) {
            fail_msg("waitpid failed");
            continue;
        }
        /* std::terminate aborts: SIGABRT (or SIGSEGV under some sanitizer
         * shapes); a nonzero exit is accepted for sanitizer builds. Exit 0
         * means the scenario returned. */
        if (WIFSIGNALED(status)) {
            const int sig = WTERMSIG(status);
            CHECK(sig == SIGABRT || sig == SIGSEGV);
        } else if (WIFEXITED(status)) {
            CHECK(WEXITSTATUS(status) != 0);
        } else {
            fail_msg("child neither signalled nor exited");
        }
    }
}

#endif /* !_WIN32 */

static void run_tests(void) {
#if defined(_WIN32)
    death_child_dispatch();
#endif
    test_l1_exception();
    test_task_exception();
    test_detached_handler_replacement();
    test_sched_task_exception();
    test_scope_first_exception_authoritative();
    test_wait_or_throw_exception_priority_on_timeout();
    test_wait_or_throw_code_when_no_exception();
    test_task_get_throws_error_on_status_failure();
    test_death_shapes();
}
TEST_MAIN()
