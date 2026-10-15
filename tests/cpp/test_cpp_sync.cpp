/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * tests/cpp/test_cpp_sync.cpp -- the L2 synchronization primitives:
 * mutex (with the std::lock_guard face), condition_variable, semaphore,
 * rwlock, the zero-allocation void* channel and the typed node channel.
 * Covers the C-parallel return codes (ESTALE outside a coroutine, EBUSY on
 * a failed try, ESTATE on an unlock mismatch, ECLOSED on a closed channel),
 * close-drain destruction and multi-worker contention.
 */
#include "../harness.h"

#include <corocc/coro.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <new>
#include <thread>

namespace {
long g_new_countdown = -1; /* -1: never; 0: fail now; >0: pass N then fail */
}

/* GCC's inlining-dependent -Wmismatched-new-delete false positive: a box
 * allocated through the nothrow shim and later plain-deleted trips the check
 * at -O1/-O2/-O3 even though this shim's new/delete pair is a matched
 * malloc/free pair (the check is syntactic on the allocation function's
 * identity and cannot see the delegation). The silencing is scoped to the
 * shim itself. */
#if defined(__GNUC__) && !defined(_MSC_VER)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t n) {
    if (g_new_countdown == 0) {
        g_new_countdown = -1;
        throw std::bad_alloc();
    }
    if (g_new_countdown > 0) {
        --g_new_countdown;
    }
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void* p) noexcept {
    std::free(p);
}

/* The nothrow pair must be replaced too: with a sanitizer interposing its
 * own nothrow new, the default pairing would report an alloc-dealloc
 * mismatch (sanitizer operator new vs our free). */
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(n);
    } catch (...) {
        return NULL;
    }
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    ::operator delete(p);
}

void* operator new[](std::size_t n) {
    return ::operator new(n);
}

void operator delete[](void* p) noexcept {
    std::free(p);
}

/* The C++14 sized forms: without them -Werror=sized-deallocation fires in
 * forward-standard builds. */
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

#if defined(__GNUC__) && !defined(_MSC_VER)
#pragma GCC diagnostic pop
#endif

namespace {

/* Namespace scope: the lambdas use them without a capture (MSVC requires a
 * local const captured, clang flags the unnecessary capture). */
const int kCoros = 8;
const int kIters = 2000;

} /* namespace */

static void test_mutex_mutual_exclusion(void) {
    corocc::runtime rt(4);
    corocc::mutex m(rt);
    long shared = 0;

    for (int i = 0; i < kCoros; ++i) {
        CHECK(rt.spawn([&m, &shared]() {
            for (int k = 0; k < kIters; ++k) {
                const corocc::status st = m.lock();
                if (!st.ok()) return;
                const long before = shared;
                shared = before + 1;
                (void)m.unlock();
            }
        }).ok());
    }
    CHECK(rt.wait_all().ok());
    CHECK(shared == static_cast<long>(kCoros) * kIters);
}

static void test_mutex_lock_guard(void) {
    /* The std::lock_guard face: any Lockable with lock()/unlock(). */
    corocc::runtime rt(2);
    corocc::mutex m(rt);
    long guarded = 0;
    CHECK(rt.spawn([&m, &guarded]() {
        std::lock_guard<corocc::mutex> g(m);
        guarded += 1;
    }).ok());
    CHECK(rt.wait_all().ok());
    CHECK(guarded == 1);
}

static void test_mutex_thread_and_mismatch(void) {
    corocc::runtime rt(2);
    corocc::mutex m(rt);
    /* The waitable acquisition from a plain thread reports ESTALE (nothing
     * can park the thread); trylock never parks, so the free lock is taken
     * even from a thread (the C contract as implemented). */
    CHECK(m.lock().code() == CORO_ESTALE);
    CHECK(m.trylock().ok());
    CHECK(!m.try_lock()); /* held now: the EBUSY face */
    CHECK(m.unlock().ok());

    long bad = 0;
    CHECK(rt.spawn([&m, &bad]() {
        /* Unlocking what is not held: ESTATE, no state change. */
        bad = (m.unlock().code() == CORO_ESTATE) ? 1 : 0;
    }).ok());
    CHECK(rt.wait_all().ok());
    CHECK(bad == 1);
}

static void test_condition_variable(void) {
    corocc::runtime rt(2);
    corocc::mutex m(rt);
    corocc::condition_variable cv(rt);
    bool ready = false;
    long consumed = 0;

    CHECK(rt.spawn([&m, &cv, &ready, &consumed]() {
        const corocc::status lk = m.lock();
        if (!lk.ok()) return;
        while (!ready) {
            if (cv.wait(m).code() == CORO_ESTALE) break;
        }
        consumed = 42;
        (void)m.unlock();
    }).ok());
    CHECK(rt.spawn([&m, &cv, &ready, &rt]() {
        (void)rt.sleep_for(std::chrono::milliseconds(20));
        const corocc::status lk = m.lock();
        if (!lk.ok()) return;
        ready = true;
        (void)cv.notify_one();
        (void)m.unlock();
    }).ok());
    CHECK(rt.wait_all().ok());
    CHECK(consumed == 42);
}

static void test_semaphore(void) {
    corocc::runtime rt(4);
    corocc::semaphore sem(rt, 0);
    CHECK(sem.count() == 0u);
    CHECK(sem.trywait().code() == CORO_EBUSY); /* thread side: C trywait is
                                                  thread-safe; empty -> EBUSY */
    std::atomic<long> woken(0);
    for (int i = 0; i < 4; ++i) {
        CHECK(rt.spawn([&sem, &woken]() {
            if (sem.wait().ok()) woken.fetch_add(1);
        }).ok());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(sem.count() == 0u);
    for (int i = 0; i < 4; ++i) {
        CHECK(sem.post().ok());
    }
    CHECK(rt.wait_all().ok());
    CHECK(woken.load() == 4);
}

static void test_rwlock(void) {
    corocc::runtime rt(4);
    corocc::rwlock lk(rt);
    std::atomic<int> active_readers(0);
    std::atomic<int> writer_active(0);
    std::atomic<int> violations(0);
    std::atomic<int> done(0);

    for (int i = 0; i < 4; ++i) {
        CHECK(rt.spawn([&]() {
            for (int k = 0; k < 50; ++k) {
                if (!lk.rdlock().ok()) return;
                if (writer_active.load() != 0) violations.fetch_add(1);
                active_readers.fetch_add(1);
                active_readers.fetch_sub(1);
                (void)lk.rdunlock();
            }
            done.fetch_add(1);
        }).ok());
    }
    for (int i = 0; i < 2; ++i) {
        CHECK(rt.spawn([&]() {
            for (int k = 0; k < 25; ++k) {
                if (!lk.wrlock().ok()) return;
                writer_active.store(1);
                if (active_readers.load() != 0) violations.fetch_add(1);
                writer_active.store(0);
                (void)lk.wrunlock();
            }
            done.fetch_add(1);
        }).ok());
    }
    CHECK(rt.wait_all().ok());
    CHECK(violations.load() == 0);
    CHECK(done.load() == 6);

    /* Mismatched releases report ESTATE (the C contract). */
    corocc::rwlock lk2(rt);
    long mismatch = 0;
    CHECK(rt.spawn([&lk2, &mismatch]() {
        mismatch = (lk2.rdunlock().code() == CORO_ESTATE &&
                    lk2.wrunlock().code() == CORO_ESTATE)
                       ? 1
                       : 0;
    }).ok());
    CHECK(rt.wait_all().ok());
    CHECK(mismatch == 1);
}

static void test_channel_void_face(void) {
    corocc::runtime rt(2);
    corocc::channel<> ch(rt, 0); /* unbounded */
    CHECK(ch.valid());
    CHECK(ch.size() == 0u);
    CHECK(!ch.closed());

    static int payload[3] = {1, 2, 3};
    std::atomic<long> sum(0);
    CHECK(rt.spawn([&ch, &sum]() {
        void* p = NULL;
        for (;;) {
            const corocc::status st = ch.recv(&p);
            if (st.code() == CORO_OK) {
                sum.fetch_add(*static_cast<int*>(p));
                continue;
            }
            if (st.code() == CORO_ECLOSED) break;
            return; /* ECANCELED and friends end the consumer */
        }
    }).ok());

    /* send/recv are coroutine-only; the plain thread gets ESTALE. */
    CHECK(ch.send(NULL).code() == CORO_ESTALE);
    void* drained = NULL;
    CHECK(ch.recv(&drained).code() == CORO_ESTALE);

    CHECK(rt.spawn([&ch]() {
        for (int i = 0; i < 3; ++i) {
            const corocc::status st = ch.send(&payload[i]);
            if (!st.ok()) return;
        }
        (void)ch.close();
    }).ok());
    CHECK(rt.wait_all().ok());
    CHECK(sum.load() == 6);
    CHECK(ch.closed());
    CHECK(ch.size() == 0u); /* the consumer drained everything */

    /* After close: send is ECLOSED, recv on the empty closed channel is
     * ECLOSED, tryrecv the same. */
    std::atomic<long> closed_flags(0);
    void* p0 = &payload[0];
    CHECK(rt.spawn([&ch, &closed_flags, p0]() {
        long flags = 0;
        if (ch.send(p0).code() == CORO_ECLOSED) flags |= 1;
        void* out = NULL;
        if (ch.tryrecv(&out).code() == CORO_ECLOSED) flags |= 2;
        if (ch.trysend(p0).code() == CORO_ECLOSED) flags |= 4;
        closed_flags.store(flags);
    }).ok());
    CHECK(rt.wait_all().ok());
    CHECK(closed_flags.load() == 7);
    CHECK(ch.destroy().ok());
    CHECK(ch.destroy().ok()); /* idempotent */
}

static void test_channel_typed(void) {
    corocc::runtime rt(2);
    corocc::channel<int> ch(rt, 4); /* bounded */
    CHECK(ch.valid());

    std::atomic<long> sum(0);
    CHECK(rt.spawn([&ch, &sum]() {
        int v = 0;
        for (;;) {
            const corocc::status st = ch.recv(v);
            if (st.code() == CORO_OK) {
                sum.fetch_add(v);
                continue;
            }
            if (st.code() == CORO_ECLOSED) break;
            return;
        }
    }).ok());

    CHECK(rt.spawn([&ch]() {
        for (int i = 1; i <= 4; ++i) {
            if (!ch.send(i).ok()) return;
        }
        (void)ch.close();
    }).ok());
    CHECK(rt.wait_all().ok());
    CHECK(sum.load() == 10);

    /* Node allocation failure -> ENOMEM, and the channel stays usable. */
    corocc::channel<int> ch2(rt, 0);
    int got = 0;
    corocc::status st(CORO_OK);
    CHECK(rt.spawn([&ch2, &got, &st]() {
        g_new_countdown = 0;
        st = ch2.send(7);
        g_new_countdown = -1;
        /* nothing was enqueued by the failed send */
        int v = 0;
        got = ch2.tryrecv(v).ok() ? v : -1;
    }).ok());
    CHECK(rt.wait_all().ok());
    CHECK(st.code() == CORO_ENOMEM);
    CHECK(got == -1);

    CHECK(rt.spawn([&ch2]() {
        (void)ch2.send(9);
        (void)ch2.close();
    }).ok());
    CHECK(rt.wait_all().ok());
    int v = 0;
    CHECK(ch2.tryrecv(v).ok());
    CHECK(v == 9);
    CHECK(ch2.destroy().ok());

    /* The dtor drains queued nodes (no leak, no EBUSY surprise). */
    {
        corocc::channel<int> ch3(rt, 0);
        CHECK(rt.spawn([&ch3]() {
            for (int i = 0; i < 5; ++i) (void)ch3.send(i);
        }).ok());
        CHECK(rt.wait_all().ok());
        /* ~channel<int>() closes, drains the five queued nodes and
         * destroys. ASan/LSan would flag a missed delete. */
    }
}

static void run_tests(void) {
    test_mutex_mutual_exclusion();
    test_mutex_lock_guard();
    test_mutex_thread_and_mismatch();
    test_condition_variable();
    test_semaphore();
    test_rwlock();
    test_channel_void_face();
    test_channel_typed();
}
TEST_MAIN()
