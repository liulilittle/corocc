/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * tests/cpp/test_cpp_coroutine.cpp -- the L1 manual coroutine face
 * (corocc::coroutine): create/start/resume/cancel/join/state, move
 * semantics, explicit destroy and the RAII dtor, exception capture, empty
 * handles, the token-less driving contract, and the repeated round trips
 * that guard the saved-frame path.
 */
#include "../harness.h"

#include <corocc/coro.hpp>

#include <exception>

namespace {

struct my_problem {};

int g_steps = 0;

/* Namespace scope: the lambdas use it without a capture (MSVC requires a
 * local const captured, clang flags the unnecessary capture). */
const int kTrips = 10000;

} /* namespace */

static void test_empty_handle(void) {
    corocc::coroutine c;
    CHECK(!c.valid());
    CHECK(c.native() == NULL);
    CHECK(c.state() == CORO_NEW);         /* the C NULL answer, not a crash */
    CHECK(!c.done());
    CHECK(c.start().code() == CORO_EINVAL);
    CHECK(c.resume().code() == CORO_EINVAL);
    CHECK(c.cancel().code() == CORO_EINVAL);
    CHECK(c.join().code() == CORO_EINVAL);
    CHECK(c.destroy().code() == CORO_EINVAL);
    CHECK(c.exception() == std::exception_ptr());
}

static void test_run_to_completion(void) {
    g_steps = 0;
    corocc::coroutine c;
    corocc::status st = corocc::coroutine::create(c, []() -> coro_status {
        ++g_steps;
        if (corocc::yield().failed()) return CORO_ECANCELED;
        ++g_steps;
        if (corocc::yield().failed()) return CORO_ECANCELED;
        ++g_steps;
        return CORO_OK;
    });
    CHECK(st.ok());
    CHECK(c.valid());
    CHECK(c.state() == CORO_NEW);          /* created, never started */
    CHECK(c.join().code() == CORO_EAGAIN); /* live: manual join refuses */

    CHECK(c.start().ok());
    CHECK(g_steps == 1);
    CHECK(c.state() == CORO_SUSPENDED);
    CHECK(!c.done());

    CHECK(c.resume().ok());
    CHECK(g_steps == 2);
    CHECK(c.resume().ok());
    CHECK(g_steps == 3);
    CHECK(c.done());
    CHECK(c.state() == CORO_DEAD);
    CHECK(c.join().ok());                  /* dead: join reports OK */
    CHECK(c.destroy().ok());
    CHECK(!c.valid());
    /* A second destroy is the empty-handle refusal, never a double free. */
    CHECK(c.destroy().code() == CORO_EINVAL);
    CHECK(c.start().code() == CORO_EINVAL);
}

static void test_create_contract(void) {
    corocc::coroutine c;
    CHECK(corocc::coroutine::create(c, []() {}).ok());
    /* out already holds a live coroutine: refused, out untouched. */
    CHECK(corocc::coroutine::create(c, []() {}).code() == CORO_ESTATE);
    CHECK(c.valid());
    CHECK(c.destroy().ok());
}

static void test_cancel(void) {
    int saw_cancel = 0;
    corocc::coroutine c;
    CHECK(corocc::coroutine::create(c, [&saw_cancel]() -> coro_status {
        corocc::status y = corocc::yield();
        saw_cancel = (y.code() == CORO_ECANCELED) ? 1 : 0;
        return CORO_OK;
    }).ok());
    CHECK(c.start().ok());
    CHECK(c.cancel().ok());
    CHECK(c.resume().ok());            /* the safe point consumes the request */
    CHECK(c.done());
    CHECK(saw_cancel == 1);
    CHECK(c.destroy().ok());
}

static void test_cancel_idempotent_on_dead(void) {
    corocc::coroutine c;
    CHECK(corocc::coroutine::create(c, []() {}).ok());
    CHECK(c.start().ok());
    CHECK(c.done());
    CHECK(c.cancel().ok());            /* a request on a dead coroutine is OK */
    CHECK(c.destroy().ok());
}

static void test_round_trips(void) {
    /* The saved-frame guard: tens of thousands of yield/resume round trips
     * through one coroutine must not corrupt the frame or the credentials. */
    int count = 0;
    corocc::coroutine c;
    CHECK(corocc::coroutine::create(c, [&count]() -> coro_status {
        for (int i = 0; i < kTrips; ++i) {
            if (corocc::yield().failed()) return CORO_ECANCELED;
            ++count;
        }
        return CORO_OK;
    }).ok());
    CHECK(c.start().ok());
    for (int i = 0; i < kTrips; ++i) {
        corocc::status st = c.resume();
        if (st.failed()) {
            CHECK(st.ok());
            break;
        }
    }
    CHECK(c.done());
    CHECK(count == kTrips);
    CHECK(c.destroy().ok());
}

static void test_exception_capture(void) {
    corocc::coroutine c;
    CHECK(corocc::coroutine::create(c, []() -> coro_status {
        throw my_problem();
    }).ok());
    CHECK(c.start().ok());
    CHECK(c.done());                    /* the throw ended the coroutine */
    std::exception_ptr e = c.exception();
    CHECK(e != std::exception_ptr());
    bool caught = false;
    try {
        std::rethrow_exception(e);
    } catch (const my_problem&) {
        caught = true;
    }
    CHECK(caught);
    CHECK(c.destroy().ok());
}

static void test_no_exception_when_clean(void) {
    corocc::coroutine c;
    CHECK(corocc::coroutine::create(c, []() {}).ok());
    CHECK(c.start().ok());
    CHECK(c.done());
    CHECK(c.exception() == std::exception_ptr());
    CHECK(c.destroy().ok());
}

static void test_move_semantics(void) {
    corocc::coroutine a;
    CHECK(corocc::coroutine::create(a, []() {}).ok());
    CHECK(a.valid());
    coro_t* raw = a.native();

    corocc::coroutine b(std::move(a));
    CHECK(!a.valid());
    CHECK(b.valid());
    CHECK(b.native() == raw);

    corocc::coroutine c;
    c = std::move(b);
    CHECK(!b.valid());
    CHECK(c.valid());
    CHECK(c.native() == raw);

    /* The moved-from handles are empty: every operation refuses. */
    CHECK(a.destroy().code() == CORO_EINVAL);
    CHECK(b.start().code() == CORO_EINVAL);

    CHECK(c.start().ok());
    CHECK(c.done());
    CHECK(c.destroy().ok());
}

static void test_native_interop(void) {
    corocc::coroutine c;
    CHECK(corocc::coroutine::create(c, []() -> coro_status {
        (void)corocc::yield();
        return CORO_OK;
    }).ok());
    coro_t* h = c.native();
    CHECK(h != NULL);
    CHECK(coro_get_state(h) == CORO_NEW);
    CHECK(c.start().ok());
    CHECK(coro_get_state(h) == CORO_SUSPENDED);
    /* The C face drives the same object: the wrapper's token advanced with
     * the C call, so a native resume through the wrapper's credential works. */
    CHECK(c.resume().ok());
    CHECK(coro_get_state(h) == CORO_DEAD);
    CHECK(c.destroy().ok());
}

static void test_attrs_name_is_copied(void) {
    /* A temporary attrs with a name: the wrapper copies the name into its
     * control block, so the borrowed C pointer never dangles. */
    corocc::coroutine c;
    CHECK(corocc::coroutine::create(c, []() {}, corocc::attrs().name("short-lived"))
              .ok());
    CHECK(c.start().ok());
    CHECK(c.done());
    CHECK(c.destroy().ok());
}

#ifndef NDEBUG
static void test_suspended_abandon_counted(void) {
    /* Destroying a SUSPENDED coroutine is legal but silently drops the
     * frame; the debug build counts it (it never aborts). */
    const unsigned long before = corocc::detail::abandoned_coroutines();
    {
        corocc::coroutine c;
        CHECK(corocc::coroutine::create(c, []() -> coro_status {
            (void)corocc::yield();
            return CORO_OK;
        }).ok());
        CHECK(c.start().ok());
        /* ~coroutine() destroys the SUSPENDED coroutine: no terminate. */
    }
    CHECK(corocc::detail::abandoned_coroutines() == before + 1);
}
#endif

static void run_tests(void) {
    test_empty_handle();
    test_run_to_completion();
    test_create_contract();
    test_cancel();
    test_cancel_idempotent_on_dead();
    test_round_trips();
    test_exception_capture();
    test_no_exception_when_clean();
    test_move_semantics();
    test_native_interop();
    test_attrs_name_is_copied();
#ifndef NDEBUG
    test_suspended_abandon_counted();
#endif
}
TEST_MAIN()
