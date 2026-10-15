/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * coro.hpp -- the C++11 wrapper over the corocc C API (L1 coroutine object,
 * L2 M:N runtime and single-thread scheduler, L3 structured-concurrency
 * scope).
 *
 * The wrapper is header-only and parallel to the C API: every class is a
 * thin RAII/typed face over the exact C entry points, with a status-first
 * error contract, chrono-based durations and a capture-and-deliver exception
 * boundary on the coroutine stacks. No exception ever crosses a coroutine
 * context switch: each trampoline catches inside the coroutine and stores
 * the exception in the wrapper's own control block, where the closeout
 * points (get(), wait_or_throw(), first_exception()) hand it back.
 *
 * This header is conservative C++11: it includes no third-party header and
 * no platform header (no <windows.h>), uses no C++14 facility and defines
 * exactly one public macro besides its include guard:
 *
 *   COROCC_CPP_NO_EXCEPTIONS -- force the no-exception degraded mode.
 *   It must be consistent across the whole binary (it changes class
 *   definitions, so mixing is an ODR violation), and mixing EH and
 *   -fno-exceptions translation units in one binary is forbidden for the
 *   same reason.
 *
 * The guiding contract is the C API documentation in the sibling C headers;
 * this wrapper adds ownership, typing and timing but invents no second
 * scheduling or cancellation mechanism.
 */
#ifndef COROCC_CORO_HPP
#define COROCC_CORO_HPP

#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_sched.h>
#include <corocc/coro_scope.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

/*
 * COROCC_CPP_HAS_EXCEPTIONS -- 1 when throw/catch/exception_ptr may be used.
 *
 * Detected from the compiler's own macro on GCC/Clang (__cpp_exceptions or
 * __EXCEPTIONS) and MSVC (_CPPUNWIND). The user macro COROCC_CPP_NO_EXCEPTIONS
 * forces the degraded mode. The value must be identical across every TU of a
 * binary: get() and wait_or_throw() vanish in the degraded mode, so a mixed
 * binary is an ODR violation by construction.
 */
#if !defined(COROCC_CPP_HAS_EXCEPTIONS)
#  if defined(COROCC_CPP_NO_EXCEPTIONS)
#    define COROCC_CPP_HAS_EXCEPTIONS 0
#  elif defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#    define COROCC_CPP_HAS_EXCEPTIONS 1
#  else
#    define COROCC_CPP_HAS_EXCEPTIONS 0
#  endif
#endif

namespace corocc {

/*
 * detached_exception_handler -- reaction to an exception escaping a detached
 * coroutine (runtime::spawn without a task handle). See
 * set_detached_exception_handler below for the full contract.
 */
typedef void (*detached_exception_handler)(std::exception_ptr);

/* ======================================================================= */
/* Value types: status, error, attrs, monotonic_clock                      */
/* ======================================================================= */

/*
 * status -- zero-overhead wrapper of coro_status.
 *
 * Converts implicitly in both directions and is layout-compatible with the C
 * enumeration (a static_assert pins the size), so it can be used anywhere the
 * C code returns or accepts a status while still offering ok()/failed() and
 * the human-readable message(). Every query is noexcept: this is a value, not
 * a fallible operation.
 */
class status {
public:
    status() noexcept : c_(CORO_OK) {}
    status(coro_status c) noexcept : c_(c) {}            /* NOLINT: implicit by design */

    status& operator=(coro_status c) noexcept { c_ = c; return *this; }

    operator coro_status() const noexcept { return c_; } /* NOLINT: implicit by design */

    bool ok() const noexcept { return c_ == CORO_OK; }
    bool failed() const noexcept { return c_ != CORO_OK; }
    coro_status code() const noexcept { return c_; }
    const char* message() const noexcept { return coro_status_str(c_); }

private:
    coro_status c_;
};

static_assert(sizeof(status) == sizeof(coro_status),
              "corocc::status must stay layout-compatible with coro_status");

/*
 * error -- the exception a closeout point throws to report a status failure.
 *
 * Thrown only by get()/wait_or_throw() (the explicitly named exception
 * delivery points); the default API path reports failures as status values.
 * Carries the coro_status code and uses coro_status_str as its message.
 */
class error : public std::exception {
public:
    explicit error(coro_status c) noexcept : c_(c) {}
    coro_status code() const noexcept { return c_; }
    const char* what() const noexcept override { return coro_status_str(c_); }

private:
    coro_status c_;
};

/*
 * attrs -- creation attributes builder (parallel to coro_attr_t).
 *
 * Holds an owning copy of the diagnostic name. The C attribute struct
 * borrows that pointer, so every spawn path copies the name into its own
 * control block and keeps the pointer valid for the whole coroutine
 * lifetime; handing a temporary attrs{} to a spawn call is safe.
 *
 * flags is deliberately not exposed: the C API accepts only CORO_F_NONE
 * today.
 */
class attrs {
public:
    attrs() noexcept : stack_size_(0), allocator_(nullptr) {}

    attrs& stack_size(std::size_t n) noexcept { stack_size_ = n; return *this; }
    attrs& name(const std::string& s) { name_ = s; return *this; }
    attrs& name(const char* s) { name_ = s ? s : ""; return *this; }
    attrs& allocator(const coro_allocator_t* p) noexcept {
        allocator_ = p; return *this;
    }

    std::size_t stack_size_value() const noexcept { return stack_size_; }
    const std::string& name_value() const noexcept { return name_; }
    const coro_allocator_t* allocator_value() const noexcept { return allocator_; }

    /* The C face of these attributes. `name_ptr` must stay valid for the
     * life of the created coroutine (borrowed by the C struct). */
    coro_attr_t c_attr(const char* name_ptr) const noexcept {
        coro_attr_t a = coro_attr_default();
        a.stack_size = stack_size_;
        a.name = name_ptr;
        a.flags = CORO_F_NONE;
        a.allocator = allocator_;
        return a;
    }

private:
    std::size_t stack_size_;
    std::string name_;
    const coro_allocator_t* allocator_;
};

/*
 * monotonic_clock -- the C++11 Clock face of coro_monotonic_ns().
 *
 * Its duration counts nanoseconds from the same unspecified monotone epoch
 * the C layer uses, so now() and the C clock are interchangeable and
 * std::chrono arithmetic (now() + std::chrono::milliseconds(n)) stays in
 * this clock. Only differences are meaningful.
 */
struct monotonic_clock {
    typedef std::uint64_t rep;
    typedef std::nano period;
    typedef std::chrono::duration<rep, period> duration;
    typedef std::chrono::time_point<monotonic_clock> time_point;

    static const bool is_steady = true;

    static time_point now() noexcept {
        return time_point(duration(coro_monotonic_ns()));
    }
};

/* ======================================================================= */
/* Internal machinery                                                      */
/* ======================================================================= */

namespace detail {

/* A contract violation is a caller bug on a documented red line: the wrapper
 * reports it on stderr and terminates, it never turns it into a silent
 * failure. Ordinary errors travel as status values. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
inline void contract_violation(const char* msg) noexcept {
    std::fprintf(stderr, "corocc: contract violation: %s\n", msg);
    std::fflush(stderr);
    std::terminate();
}

inline void default_detached_exception_handler(std::exception_ptr) noexcept {
    std::terminate();
}

/* The one piece of shared wrapper state: the detached-exception handler.
 * A function-local static in an inline function is the C++11 way to have a
 * single instance per binary (module-scope one per DLL, documented); no
 * anonymous-namespace or global static may be used instead. */
inline std::atomic<detached_exception_handler>& detached_handler_slot() noexcept {
    static std::atomic<detached_exception_handler> slot(
        &default_detached_exception_handler);
    return slot;
}

#if COROCC_CPP_HAS_EXCEPTIONS
/* Deliver an exception that escaped a detached (handler-less) coroutine. */
inline void deliver_detached_exception(std::exception_ptr e) noexcept {
    detached_exception_handler h =
        detached_handler_slot().load(std::memory_order_acquire);
    if (!h) h = &default_detached_exception_handler;
    h(e);
}
#endif

/* The bounded-retry cadence: 1 ms, the same quantity L3's thread-side
 * collect retry uses. Never a busy spin. */
inline void sleep_1ms() noexcept {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

/* C++11 index sequence (no std::index_sequence before C++14). */
template <std::size_t... I> struct index_seq {};

template <std::size_t N, std::size_t... I>
struct make_index_seq : make_index_seq<N - 1, N - 1, I...> {};

template <std::size_t... I>
struct make_index_seq<0, I...> { typedef index_seq<I...> type; };

/* std::reference_wrapper (std::ref/cRef) unwrapping for stored arguments. */
template <class T> inline T& unwrap_arg(T& v) noexcept { return v; }
template <class T>
inline T& unwrap_arg(std::reference_wrapper<T>& v) noexcept { return v.get(); }
template <class T>
inline const T& unwrap_arg(const std::reference_wrapper<T>& v) noexcept {
    return v.get();
}

/* Call a stored (callable, tuple-of-decayed-args) pair. */
template <class F, class Tuple, std::size_t... I>
inline auto call_tuple(F& f, Tuple& t, index_seq<I...>)
    -> decltype(f(unwrap_arg(std::get<I>(t))...)) {
    return f(unwrap_arg(std::get<I>(t))...);
}

/* The callable adapters: one shape for "no extra arguments", one for the
 * decay-copied argument pack, both with a nullary operator(). Boxing stores
 * one adapter plus the owning name, so the box is the single control block
 * the design budgets per spawn. */
template <class F>
struct callable0 {
    F f;
    explicit callable0(F&& fn) : f(std::move(fn)) {}
    auto operator()() -> decltype(f()) { return f(); }
};

template <class F, class... A>
struct callable_args {
    F f;
    std::tuple<A...> args;
    explicit callable_args(F&& fn, A... a)
        : f(std::move(fn)), args(std::move(a)...) {}

    typedef decltype(call_tuple(std::declval<F&>(),
                                std::declval<std::tuple<A...>&>(),
                                typename make_index_seq<sizeof...(A)>::type()))
        result_type;

    result_type operator()() {
        return call_tuple(f, args, typename make_index_seq<sizeof...(A)>::type());
    }
};

/* The code channel of a non-typed callable: void means OK, coro_status is
 * its own code. Anything else is rejected at compile time (the C entry
 * points of this shape have exactly one result channel). */
/*
 * MSVC-only scoped silencing of C4702 around the runners below (and
 * runtime::spawn further down; same reason). In a Release /GL + /LTCG build
 * the code-generation pass runs a whole-program unreachability check and
 * flags the statements after a call it proved non-returning for a given
 * instantiation: with a callable whose operator() always throws (a
 * legitimate test shape -- the exception channel is exactly how such a
 * body reports), the trailing statement is unreachable *for that
 * instantiation only*, while being semantically required for every other
 * one. The warning is per-instantiation and cannot be "fixed" without
 * changing the semantics, so the check is disabled for exactly these small
 * runners (the same practice the MSVC STL uses).
 */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
template <class C>
inline auto run_code(C& c) ->
    typename std::enable_if<std::is_void<decltype(c())>::value, coro_status>::type {
    c();
    return CORO_OK;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

template <class C>
inline auto run_code(C& c) ->
    typename std::enable_if<
        std::is_same<decltype(c()), coro_status>::value, coro_status>::type {
    return c();
}

/* Detached spawns have no result channel: whatever the callable returns is
 * discarded (the exception channel is the handler). */
template <class C>
inline void run_discard(C& c) { (void)c(); }

} /* namespace detail */

/* ======================================================================= */
/* Detached exception policy                                               */
/* ======================================================================= */

/*
 * detached_exception_handler -- reaction to an exception escaping a detached
 * coroutine (runtime::spawn without a task handle).
 *
 * The default is std::terminate: a detached failure has no observer and
 * corocc's contract is that failures are observable. The handler runs on the
 * coroutine's stack, on the worker thread, inside the trampoline's catch; it
 * must not throw (a throwing handler terminates) and must not return an
 * exception to the caller. Passing a null handler restores the default.
 */
inline detached_exception_handler
set_detached_exception_handler(detached_exception_handler h) noexcept {
    if (!h) h = &detail::default_detached_exception_handler;
    return detail::detached_handler_slot().exchange(h, std::memory_order_acq_rel);
}

/* ======================================================================= */
/* Forward declarations of the RAII classes                                */
/* ======================================================================= */

class coroutine;
class runtime;
class scheduler;
class scope;
template <class R> class task;
template <class R> class sched_task;
template <class R> class cell;
template <class Owner> class cancel_guard;

/* ======================================================================= */
/* Internal machinery: state, boxes, trampolines                           */
/* ======================================================================= */

namespace detail {

/* ---- the shared result/status snapshot ---- */

/*
 * state_base -- what every wrapper-managed coroutine publishes when its body
 * finishes: the code channel (which is what the C cell would carry), the
 * exception channel, and a completion sequence number the scope uses to
 * decide the first-completed exception.
 *
 * The trampoline writes code/eptr/value, then publishes `ready` with a
 * release store; readers acquire it. The wrapper never reads the C cell
 * alias: this snapshot is the only source of truth (an exception child's C
 * cell carries CORO_ECANCELED as the "abnormal end" marker).
 */
struct state_base {
    std::atomic<bool> ready;
    coro_status code;
    std::exception_ptr eptr;
    std::uint64_t completion_seq;

    state_base() noexcept : ready(false), code(CORO_OK), completion_seq(0) {}
    virtual ~state_base() {}

private:
    state_base(const state_base&);
    state_base& operator=(const state_base&);
};

/*
 * slot<R> -- placement-constructed result storage.
 *
 * `constructed` flips only after a successful construction, so a failed
 * construction leaves nothing to destroy (a throwing move constructor is
 * user error delivered through the exception channel, not UB).
 */
template <class R>
class slot {
public:
    slot() noexcept : constructed_(false) {}
    ~slot() {
        if (constructed_) ptr()->~R();
    }

    template <class U>
    void construct(U&& u) {
        new (static_cast<void*>(&storage_)) R(std::forward<U>(u));
        constructed_ = true;
    }

    R* ptr() noexcept { return reinterpret_cast<R*>(&storage_); }
    const R* ptr() const noexcept { return reinterpret_cast<const R*>(&storage_); }
    bool constructed() const noexcept { return constructed_; }

private:
    slot(const slot&);
    slot& operator=(const slot&);

    typename std::aligned_storage<sizeof(R), alignof(R)>::type storage_;
    bool constructed_;
};

/* state<R> -- the snapshot plus the typed value. void has no value;
 * coro_status keeps the code as its value (single storage). */
template <class R> struct state : state_base { slot<R> value; };
template <> struct state<void> : state_base {};
template <> struct state<coro_status> : state_base {};

template <class R>
struct result_access {
    static const R* peek(const state<R>& st) noexcept {
        return st.value.constructed() ? st.value.ptr() : NULL;
    }
    static R take(state<R>& st) { return std::move(*st.value.ptr()); }
};

template <>
struct result_access<coro_status> {
    static const coro_status* peek(const state<coro_status>& st) noexcept {
        return &st.code;
    }
    static coro_status take(state<coro_status>& st) noexcept { return st.code; }
};

template <class R>
inline std::shared_ptr<state<R> > make_state() {
#if COROCC_CPP_HAS_EXCEPTIONS
    try {
        return std::make_shared<state<R> >();
    } catch (const std::bad_alloc&) {
        return std::shared_ptr<state<R> >();
    }
#else
    return std::make_shared<state<R> >();
#endif
}

/* ---- the typed runners (the code channel of each callable shape) ---- */

/* C4702 silencing: see the note above detail::run_code. */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif

/* Generic R: whatever the callable returns is constructed into the slot and
 * the C-facing code is OK. */
template <class R>
struct typed_runner {
    template <class C>
    static coro_status run(C& c, state<R>& st) {
        st.value.construct(c());
        return CORO_OK;
    }
};

/* void: no value; the code is OK unless the body throws. */
template <>
struct typed_runner<void> {
    template <class C>
    static coro_status run(C& c, state<void>&) {
        c();
        return CORO_OK;
    }
};

/* coro_status: the returned code is the value and the code channel. */
template <>
struct typed_runner<coro_status> {
    template <class C>
    static coro_status run(C& c, state<coro_status>& st) {
        st.code = c();
        return st.code;
    }
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/* ---- the boxes (one control block per spawn) ---- */

struct box_base {
    virtual ~box_base() {}
};

/* Every box carries the owned copy of the diagnostic name, so the C layer's
 * borrowed attr.name always points into memory that lives exactly as long as
 * the coroutine using it. */
struct named_box : box_base {
    std::string name;
    explicit named_box(const attrs& a) : name(a.name_value()) {}
    const char* name_ptr() const noexcept {
        return name.empty() ? NULL : name.c_str();
    }
};

/* ---- L1 box (the wrapper owns it; the trampoline only runs the body) ---- */

struct l1_box_base : named_box {
    std::atomic<bool> ready;
    std::exception_ptr eptr;

    explicit l1_box_base(const attrs& a) : named_box(a), ready(false) {}
    virtual void invoke() noexcept = 0;
};

template <class C>
class l1_box : public l1_box_base {
public:
    l1_box(C&& c, const attrs& a) : l1_box_base(a), c_(std::move(c)) {}

    void invoke() noexcept override {
#if COROCC_CPP_HAS_EXCEPTIONS
        try {
            (void)run_code(c_);
        } catch (...) {
            eptr = std::current_exception();
        }
#else
        (void)run_code(c_);
#endif
        ready.store(true, std::memory_order_release);
    }

private:
    C c_;
};

inline void l1_trampoline(void* pv) noexcept {
    static_cast<l1_box_base*>(pv)->invoke();
}

/* ---- L2/L2' boxes (self-releasing: no wrapper handle keeps them) ---- */

struct rt_box_base : named_box {
    explicit rt_box_base(const attrs& a) : named_box(a) {}
    virtual void invoke() noexcept = 0;
};

/* Detached spawn: no result channel; an exception goes to the handler. */
template <class C>
class detached_box : public rt_box_base {
public:
    detached_box(C&& c, const attrs& a) : rt_box_base(a), c_(std::move(c)) {}

    void invoke() noexcept override {
#if COROCC_CPP_HAS_EXCEPTIONS
        try {
            run_discard(c_);
        } catch (...) {
            deliver_detached_exception(std::current_exception());
        }
#else
        run_discard(c_);
#endif
    }

private:
    C c_;
};

/* Managed task / sched_task: the typed result lands in the shared state. */
template <class C, class R>
class result_box : public rt_box_base {
public:
    result_box(C&& c, std::shared_ptr<state<R> > st, const attrs& a)
        : rt_box_base(a), c_(std::move(c)), st_(std::move(st)) {}

    void invoke() noexcept override {
        coro_status code;
#if COROCC_CPP_HAS_EXCEPTIONS
        try {
            code = typed_runner<R>::run(c_, *st_);
        } catch (...) {
            st_->eptr = std::current_exception();
            code = CORO_ECANCELED;
        }
#else
        code = typed_runner<R>::run(c_, *st_);
#endif
        st_->code = code;
        st_->ready.store(true, std::memory_order_release);
    }

private:
    C c_;
    std::shared_ptr<state<R> > st_;
};

/* The L2 and L2' trampolines share this body: run, then self-release (the
 * runtime/scheduler frees the coroutine; the box is the wrapper's alone). */
inline void rt_trampoline(void* pv) noexcept {
    rt_box_base* b = static_cast<rt_box_base*>(pv);
    b->invoke();
    delete b;
}

/* ---- L3 scope boxes (self-releasing; the scope keeps the shared state) ---- */

struct scope_box_base : box_base {
    virtual coro_status invoke() noexcept = 0;
};

template <class C>
class scope_void_box : public scope_box_base {
public:
    scope_void_box(C&& c, std::shared_ptr<state<void> > st,
                   std::atomic<std::uint64_t>* seq, const attrs& a)
        : name_(a.name_value()), c_(std::move(c)), st_(std::move(st)), seq_(seq) {}

    coro_status invoke() noexcept override {
        coro_status code;
#if COROCC_CPP_HAS_EXCEPTIONS
        try {
            code = run_code(c_);
        } catch (...) {
            st_->eptr = std::current_exception();
            code = CORO_ECANCELED;
        }
#else
        code = run_code(c_);
#endif
        st_->completion_seq = seq_->fetch_add(1, std::memory_order_relaxed);
        st_->code = code;
        st_->ready.store(true, std::memory_order_release);
        return code;
    }

    const char* name_ptr() const noexcept {
        return name_.empty() ? NULL : name_.c_str();
    }

private:
    std::string name_;
    C c_;
    std::shared_ptr<state<void> > st_;
    std::atomic<std::uint64_t>* seq_;
};

template <class C, class R>
class scope_result_box : public scope_box_base {
public:
    scope_result_box(C&& c, std::shared_ptr<state<R> > st,
                     std::atomic<std::uint64_t>* seq, const attrs& a)
        : name_(a.name_value()), c_(std::move(c)), st_(std::move(st)), seq_(seq) {}

    coro_status invoke() noexcept override {
        coro_status code;
#if COROCC_CPP_HAS_EXCEPTIONS
        try {
            code = typed_runner<R>::run(c_, *st_);
        } catch (...) {
            st_->eptr = std::current_exception();
            code = CORO_ECANCELED;
        }
#else
        code = typed_runner<R>::run(c_, *st_);
#endif
        st_->completion_seq = seq_->fetch_add(1, std::memory_order_relaxed);
        st_->code = code;
        st_->ready.store(true, std::memory_order_release);
        return code;
    }

    const char* name_ptr() const noexcept {
        return name_.empty() ? NULL : name_.c_str();
    }

private:
    std::string name_;
    C c_;
    std::shared_ptr<state<R> > st_;
    std::atomic<std::uint64_t>* seq_;
};

/*
 * The scope trampoline: the C scope calls this as the child's fn and then
 * publishes the returned code into its own cell. The box is deleted here --
 * the C record's `arg` is never read again after this call (verified in
 * rt_scope.c: the internal trampoline only forwards rec->arg into fn).
 */
inline coro_status scope_trampoline(void* pv) noexcept {
    scope_box_base* b = static_cast<scope_box_base*>(pv);
    const coro_status rc = b->invoke();
    delete b;
    return rc;
}

/* ---- allocation helpers (metadata OOM -> null; user ctor exceptions
 *      propagate; the caller maps null to CORO_ENOMEM) ---- */

template <class Box, class... A>
inline Box* box_new(A&&... a) {
#if COROCC_CPP_HAS_EXCEPTIONS
    try {
        return new (std::nothrow) Box(std::forward<A>(a)...);
    } catch (const std::bad_alloc&) {
        return NULL;
    }
#else
    return new (std::nothrow) Box(std::forward<A>(a)...);
#endif
}

/*
 * normalize_entry -- normalise a std::bad_alloc raised while the entry
 * stages the user callable/arguments: the first hop of moving them into
 * the box, i.e. into detail::callable0/callable_args. Non-bad_alloc user
 * exceptions keep propagating; the call-site transfer into the by-value
 * parameter happens in the caller and cannot be caught here (documented
 * propagation).
 */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
#if COROCC_CPP_HAS_EXCEPTIONS
template <class Fn>
inline status normalize_entry(Fn fn) {
    try {
        return fn();
    } catch (const std::bad_alloc&) {
        return status(CORO_ENOMEM);
    }
}
#else
template <class Fn>
inline status normalize_entry(Fn fn) {
    return fn();
}
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/* ---- closeout cadence: target-directed collect retry ---- */

/*
 * collect_retry -- retry coro_rt_collect's EBUSY window (finished but not
 * retired yet) without ever busy-spinning: inside a runtime coroutine the
 * retry yields (bounded by max_yields, the L3 coroutine bound); on a plain
 * thread it sleeps 1 ms (bounded by max_sleeps, the L3 thread bound).
 * Returns the final status, EBUSY when a bound is exhausted.
 */
inline status collect_retry(coro_rt_t* rt, coro_t* c,
                            unsigned long max_yields,
                            unsigned long max_sleeps) noexcept {
    unsigned long yields = 0;
    unsigned long sleeps = 0;
    for (;;) {
        const coro_status cs = coro_rt_collect(rt, c);
        if (cs != CORO_EBUSY) return status(cs);
        const coro_status y = coro_rt_yield();
        if (y == CORO_ESTALE) {
            if (sleeps >= max_sleeps) return status(CORO_EBUSY);
            ++sleeps;
            sleep_1ms();
        } else {
            if (yields >= max_yields) return status(CORO_EBUSY);
            ++yields;
        }
    }
}

/*
 * closeout_collect -- the dtor drain (unbounded by design, like every
 * scope-level drain): cancel has already been requested; park on the target
 * (join) when inside a coroutine, ride the EBUSY window with 1 ms sleeps on
 * a plain thread, until the handle is collected. A target that can never
 * finish blocks here -- the documented caller responsibility.
 */
inline coro_status closeout_collect(coro_rt_t* rt, coro_t* c) noexcept {
    for (;;) {
        const coro_status cs = coro_rt_collect(rt, c);
        if (cs != CORO_EBUSY) return cs;
        const coro_status j = coro_rt_join(rt, c);
        if (j == CORO_EAGAIN) {
            sleep_1ms();
        } else if (coro_rt_yield() == CORO_ESTALE) {
            sleep_1ms();
        }
    }
}

/* ---- chrono conversions: ceil to the C unit, saturating, <=0 -> 0 ---- */

/* duration -> milliseconds, ceil, saturated at a far-future value the C
 * layer clamps again. Negative and zero clamp to 0 (the C "check once"
 * deadline semantics of wait_for(0)). */
template <class Rep, class Period>
inline std::uint64_t duration_to_ms_ceil(
    const std::chrono::duration<Rep, Period>& d) noexcept {
    typedef std::chrono::duration<Rep, Period> d_t;
    if (d <= d_t::zero()) return 0;
    /* A double-second view decides saturation before the integer cast, so
     * the cast itself can never overflow. ~126 million years stays below
     * INT64_MAX milliseconds. */
    const double secs = std::chrono::duration_cast<std::chrono::duration<double> >(d).count();
    const double kCapSecs = 4.0e15;
    if (!(secs < kCapSecs)) return static_cast<std::uint64_t>(0x7fffffffffffffffLL);
    std::chrono::milliseconds m = std::chrono::duration_cast<std::chrono::milliseconds>(d);
    if (m < d) m += std::chrono::milliseconds(1);
    return static_cast<std::uint64_t>(m.count());
}

/* duration -> nanoseconds, ceil, saturated (~292 years). */
template <class Rep, class Period>
inline std::uint64_t duration_to_ns_ceil(
    const std::chrono::duration<Rep, Period>& d) noexcept {
    typedef std::chrono::duration<Rep, Period> d_t;
    if (d <= d_t::zero()) return 0;
    const double secs = std::chrono::duration_cast<std::chrono::duration<double> >(d).count();
    const double kCapSecs = 9.0e9;
    if (!(secs < kCapSecs)) return static_cast<std::uint64_t>(0x7fffffffffffffffLL);
    std::chrono::nanoseconds ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d);
    if (ns < d) ns += std::chrono::nanoseconds(1);
    return static_cast<std::uint64_t>(ns.count());
}

/* now + delta, saturating at UINT64_MAX (the C deadline semantics). */
inline std::uint64_t deadline_from_now_ns(std::uint64_t delta) noexcept {
    const std::uint64_t now = coro_monotonic_ns();
    if (delta > static_cast<std::uint64_t>(~0ull) - now) {
        return static_cast<std::uint64_t>(~0ull);
    }
    return now + delta;
}

/* ---- debug diagnostics (never terminate on the L1 abandon path) ---- */

inline std::atomic<unsigned long>& abandoned_count_slot() noexcept {
    static std::atomic<unsigned long> count(0);
    return count;
}

/* Debug-build count of coroutines destroyed (or destructed) before they
 * reached DEAD -- the silent frame drop of a SUSPENDED handle. Release
 * builds compile it out; the count itself never affects semantics. */
inline unsigned long abandoned_coroutines() noexcept {
    return abandoned_count_slot().load(std::memory_order_relaxed);
}

/* The scheduler's thread affinity check (debug builds abort misuse; release
 * builds keep the documented unsynchronised-use contract). */
inline void debug_assert_bound_thread(const std::thread::id& bound) noexcept {
#if !defined(NDEBUG)
    if (bound != std::this_thread::get_id()) {
        contract_violation(
            "corocc::scheduler used from a thread other than the bound one");
    }
#else
    (void)bound;
#endif
}

/* ---- shared task bodies (task<R> and task<void> differ only in get/peek) ---- */

template <class R>
class task_core {
public:
    task_core() noexcept : rt_(NULL), c_(NULL) {}

    task_core(task_core&& other) noexcept
        : rt_(other.rt_), c_(other.c_), st_(std::move(other.st_)) {
        other.rt_ = NULL;
        other.c_ = NULL;
    }

    task_core& operator=(task_core&& other) noexcept {
        if (this != &other) {
            close_out();
            rt_ = other.rt_;
            c_ = other.c_;
            st_ = std::move(other.st_);
            other.rt_ = NULL;
            other.c_ = NULL;
        }
        return *this;
    }

    task_core(const task_core&) = delete;
    task_core& operator=(const task_core&) = delete;
    ~task_core() { close_out(); }

    bool valid() const noexcept { return c_ != NULL; }

    bool done() const noexcept {
        return static_cast<bool>(st_) &&
               st_->ready.load(std::memory_order_acquire);
    }

    coro_t* native() noexcept { return c_; }

    status cancel() noexcept {
        if (!c_) return CORO_EINVAL;
        return status(coro_rt_cancel(rt_, c_));
    }

    status join() noexcept {
        if (!c_) return CORO_EINVAL;
        return status(coro_rt_join(rt_, c_));
    }

    /*
     * wait() -- the target-directed closeout. Never touches
     * coro_rt_wait_all: unrelated parked coroutines cannot hold this call
     * up. Inside a coroutine it joins (parks until the target ends); on a
     * plain thread it rides the retirement window out with the bounded
     * 1 ms collect retry, reporting EBUSY when the budget is spent (the
     * caller may retry later). A target that already finished is collected
     * through the same retry sequence, so wait() normalises "done but not
     * retired yet" to OK.
     */
    status wait() noexcept {
        if (!c_) {
            return st_ ? status(CORO_OK) : status(CORO_EINVAL);
        }
        status st;
        if (done()) {
            st = collect_retry(rt_, c_, 1000000u, 5000u);
        } else {
            const status j(coro_rt_join(rt_, c_));
            if (j.code() == CORO_EAGAIN) {
                /* EAGAIN from join has two sources: the plain-thread
                 * no-park rule (join never parks outside a coroutine), and
                 * a self-join inside a runtime coroutine (the target is
                 * the calling coroutine itself). Self-join must surface as
                 * EAGAIN unchanged (the design's return-set matrix); only
                 * the thread side rides the bounded sleep retry. The yield
                 * probe is the same inside-detection collect_retry uses: a
                 * plain thread sees ESTALE and the call has no effect. */
                if (coro_rt_yield() != CORO_ESTALE) {
                    return j;
                }
                st = collect_retry(rt_, c_, 0u, 5000u);
            } else if (j.code() == CORO_OK || j.code() == CORO_EDESTROYED) {
                /* Done (EDESTROYED normalises: the target already finished). */
                st = collect_retry(rt_, c_, 1000000u, 5000u);
            } else {
                /* ECANCELED (our own cancellation consumed at the safe point
                 * while the target was still live) or EINVAL. */
                return j;
            }
        }
        if (st.ok()) {
            c_ = NULL; /* collected: the raw handle is gone for good */
        }
        return st;
    }

    std::exception_ptr exception() const noexcept {
        if (!done()) return std::exception_ptr();
        return st_->eptr;
    }

protected:
    void adopt(coro_rt_t* rt, coro_t* h, std::shared_ptr<state<R> > st) noexcept {
        close_out();
        rt_ = rt;
        c_ = h;
        st_ = std::move(st);
    }

    /* The dtor closeout: cancel the unfinished target, then drain to the
     * collect (unbounded by design; the target must be reachable -- that is
     * the caller contract every drain in this library states). */
    void close_out() noexcept {
        if (!c_) {
            st_.reset();
            return;
        }
        if (!st_ || !st_->ready.load(std::memory_order_acquire)) {
            (void)coro_rt_cancel(rt_, c_);
        }
        (void)closeout_collect(rt_, c_);
        rt_ = NULL;
        c_ = NULL;
        st_.reset();
    }

    coro_rt_t* rt_;
    coro_t* c_;
    std::shared_ptr<state<R> > st_;

    friend class ::corocc::runtime;
};

} /* namespace detail */

/* ======================================================================= */
/* L2: task<R> -- the managed handle of runtime::spawn_h                   */
/* ======================================================================= */

/*
 * task<R> -- a retained runtime spawn: wait()/get()/exception() deliver the
 * typed result; the C handle is collected exactly once (by wait() or the
 * dtor), so the wrapper leaks nothing. Lifecycle rule: a task must be
 * destroyed before its runtime (only collect is safe after
 * coro_rt_destroy).
 */
template <class R>
class task : public detail::task_core<R> {
public:
    static_assert(!std::is_reference<R>::value,
                  "corocc::task<R> does not support reference results");

    task() noexcept {}
    task(task&&) noexcept = default;
    task& operator=(task&&) noexcept = default;
    task(const task&) = delete;
    task& operator=(const task&) = delete;

#if COROCC_CPP_HAS_EXCEPTIONS
    /*
     * get() -- the value or a rethrow. Done -> the stored value (or the
     * body's exception); otherwise wait() first, and a failed wait throws
     * corocc::error carrying its status. The exception channel is
     * authoritative: a body exception is rethrown before the code channel
     * is consulted.
     */
    R get() {
        if (!this->st_) throw error(CORO_EINVAL);
        if (!this->done()) {
            const status w = this->wait();
            if (w.failed()) {
                if (this->st_->eptr) std::rethrow_exception(this->st_->eptr);
                throw error(w.code());
            }
        }
        if (this->st_->eptr) std::rethrow_exception(this->st_->eptr);
        return detail::result_access<R>::take(*this->st_);
    }
#else
    /* The degraded mode has no exception delivery: the closeout point is
     * deleted, not hidden (peek()/status stay available). */
    R get() = delete;
#endif

    const R* peek() const noexcept {
        if (!this->done()) return NULL;
        return detail::result_access<R>::peek(*this->st_);
    }
};

/* task<void> -- the status/exception-only specialisation. */
template <>
class task<void> : public detail::task_core<void> {
public:
    task() noexcept {}
    task(task&&) noexcept = default;
    task& operator=(task&&) noexcept = default;
    task(const task&) = delete;
    task& operator=(const task&) = delete;

#if COROCC_CPP_HAS_EXCEPTIONS
    void get() {
        if (!this->st_) throw error(CORO_EINVAL);
        if (!this->done()) {
            const status w = this->wait();
            if (w.failed()) {
                if (this->st_->eptr) std::rethrow_exception(this->st_->eptr);
                throw error(w.code());
            }
        }
        if (this->st_->eptr) std::rethrow_exception(this->st_->eptr);
    }
#else
    void get() = delete;
#endif
};

/* ======================================================================= */
/* L3: cell<R> -- the typed child-result handle                            */
/* ======================================================================= */

/*
 * cell<R> -- a shared handle over one scope child's result. Reads only the
 * wrapper snapshot (ready-gated), so a cell stays valid after the scope is
 * destroyed and the C record -- and its cell -- are gone. get()/peek()
 * require ready; try_get() is the non-throwing probe.
 */
template <class R>
class cell {
public:
    static_assert(!std::is_reference<R>::value,
                  "corocc::cell<R> does not support reference results");

    cell() noexcept {}
    cell(const cell&) noexcept = default;
    cell(cell&&) noexcept = default;
    cell& operator=(const cell&) noexcept = default;
    cell& operator=(cell&&) noexcept = default;

    bool valid() const noexcept { return static_cast<bool>(st_); }

    bool ready() const noexcept {
        return static_cast<bool>(st_) &&
               st_->ready.load(std::memory_order_acquire);
    }

    /* Before readiness the code channel reads OK (no error observed yet);
     * after readiness it is the child's final code. ECANCELED here is
     * ambiguous by design -- consult exception()/first_exception(). */
    corocc::status status() const noexcept {
        return ready() ? corocc::status(st_->code) : corocc::status(CORO_OK);
    }

    std::exception_ptr exception() const noexcept {
        return ready() ? st_->eptr : std::exception_ptr();
    }

    const R* peek() const noexcept {
        if (!ready()) return NULL;
        return detail::result_access<R>::peek(*st_);
    }

#if COROCC_CPP_HAS_EXCEPTIONS
    R get() {
        if (!ready()) {
            detail::contract_violation(
                "corocc::cell::get() before the child finished");
        }
        if (st_->eptr) std::rethrow_exception(st_->eptr);
        return detail::result_access<R>::take(*st_);
    }
#else
    R get() = delete;
#endif

    bool try_get(R& out) noexcept {
        if (!ready() || st_->eptr) return false;
        out = detail::result_access<R>::take(*st_);
        return true;
    }

private:
    explicit cell(std::shared_ptr<detail::state<R> > st) noexcept
        : st_(std::move(st)) {}

    std::shared_ptr<detail::state<R> > st_;

    friend class scope;
};

/* cell<void> -- the status/exception-only specialisation. */
template <>
class cell<void> {
public:
    cell() noexcept {}
    cell(const cell&) noexcept = default;
    cell(cell&&) noexcept = default;
    cell& operator=(const cell&) noexcept = default;
    cell& operator=(cell&&) noexcept = default;

    bool valid() const noexcept { return static_cast<bool>(st_); }

    bool ready() const noexcept {
        return static_cast<bool>(st_) &&
               st_->ready.load(std::memory_order_acquire);
    }

    corocc::status status() const noexcept {
        return ready() ? corocc::status(st_->code) : corocc::status(CORO_OK);
    }

    std::exception_ptr exception() const noexcept {
        return ready() ? st_->eptr : std::exception_ptr();
    }

#if COROCC_CPP_HAS_EXCEPTIONS
    void get() {
        if (!ready()) {
            detail::contract_violation(
                "corocc::cell::get() before the child finished");
        }
        if (st_->eptr) std::rethrow_exception(st_->eptr);
    }
#else
    void get() = delete;
#endif

private:
    explicit cell(std::shared_ptr<detail::state<void> > st) noexcept
        : st_(std::move(st)) {}

    std::shared_ptr<detail::state<void> > st_;

    friend class scope;
};

/* ======================================================================= */
/* L2: runtime -- the M:N work-stealing runtime                            */
/* ======================================================================= */

/*
 * runtime -- unique ownership of coro_rt_t.
 *
 * status-first: the default constructor cannot fail (invalid() reports a
 * failed create), create() is the explicit form. The destructor drains:
 * wait_all() then destroy(). wait_all may block forever on a coroutine that
 * parks waiting for an event that never comes -- the same documented caller
 * responsibility every drain in this library carries.
 *
 * Lifetime rule: every wrapper object that touches the runtime's internals
 * (task cancel/join/wait/get, scope operations) must die before the runtime
 * does.
 */
class runtime {
public:
    runtime() noexcept : rt_(NULL) {}
    /* Note: the nthreads parameter carries no default here -- with the
     * default it would be ambiguous against runtime() for a default
     * construction. runtime(0) selects the C default worker count. */
    explicit runtime(std::size_t nthreads) noexcept
        : rt_(coro_rt_create(nthreads)) {}

    runtime(runtime&& other) noexcept : rt_(other.rt_) { other.rt_ = NULL; }

    runtime& operator=(runtime&& other) noexcept {
        if (this != &other) {
            close();
            rt_ = other.rt_;
            other.rt_ = NULL;
        }
        return *this;
    }

    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;
    ~runtime() { close(); }

    static status create(runtime& out, std::size_t nthreads = 0) {
        if (out.rt_) return CORO_ESTATE;
        out.rt_ = coro_rt_create(nthreads);
        return out.rt_ ? status(CORO_OK) : status(CORO_ENOMEM);
    }

    bool valid() const noexcept { return rt_ != NULL; }
    coro_rt_t* native() noexcept { return rt_; }
    std::size_t pending() const noexcept { return coro_rt_pending(rt_); }
    status wait_all() noexcept { return status(coro_rt_wait_all(rt_)); }

    status destroy() noexcept {
        if (!rt_) return CORO_OK;
        const coro_status cs = coro_rt_destroy(rt_);
        if (cs == CORO_OK) rt_ = NULL;
        return status(cs);
    }

    status cancel(coro_t* c) noexcept { return status(coro_rt_cancel(rt_, c)); }

    /*
     * spawn -- a detached coroutine: no handle, the runtime reclaims every
     * resource. An exception escaping the body goes to the detached handler.
     *
     * C4702 silencing: see the note above detail::run_code (a user
     * callable whose move constructor always throws makes the trailing
     * return unreachable for that instantiation).
     */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
    template <class F>
    status spawn(F f, const attrs& a = attrs()) {
        return detail::normalize_entry([&]() -> status {
            return spawn_detached_impl(detail::callable0<F>(std::move(f)), a);
        });
    }

    template <class F, class... A>
    typename std::enable_if<(sizeof...(A) > 0), status>::type
    spawn(F f, A... args) {
        return detail::normalize_entry([&]() -> status {
            return spawn_detached_impl(
                detail::callable_args<F, A...>(std::move(f), std::move(args)...),
                attrs());
        });
    }

    /*
     * spawn_h -- a managed task. On failure out is untouched; a live out is
     * closed out (dtor semantics) before the new handle binds.
     *
     * C4702 silencing covers this pair too (a move-always-throws
     * instantiation re-emits the warning in the simple form; the variadic
     * twin sits in the same region for uniformity).
     */
    template <class R, class F>
    status spawn_h(task<R>& out, F f, const attrs& a = attrs()) {
        return detail::normalize_entry([&]() -> status {
            return spawn_task_impl<R>(
                out, detail::callable0<F>(std::move(f)), a);
        });
    }

    template <class R, class F, class... A>
    typename std::enable_if<(sizeof...(A) > 0), status>::type
    spawn_h(task<R>& out, F f, A... args) {
        return detail::normalize_entry([&]() -> status {
            return spawn_task_impl<R>(
                out, detail::callable_args<F, A...>(std::move(f), std::move(args)...),
                attrs());
        });
    }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    /* sleep_for/until -- park the running runtime coroutine. The duration is
     * ceil-converted to nanoseconds and saturated; a negative or already
     * expired deadline parks the coroutine zero time (the C absolute-
     * deadline semantics). */
    template <class Rep, class Period>
    status sleep_for(const std::chrono::duration<Rep, Period>& d) noexcept {
        if (!rt_) return CORO_EINVAL;
        return status(coro_rt_sleep(
            rt_, detail::deadline_from_now_ns(detail::duration_to_ns_ceil(d))));
    }

    status sleep_until(monotonic_clock::time_point tp) noexcept {
        if (!rt_) return CORO_EINVAL;
        std::uint64_t deadline =
            static_cast<std::uint64_t>(tp.time_since_epoch().count());
        const std::uint64_t now = coro_monotonic_ns();
        if (deadline < now) deadline = now;
        return status(coro_rt_sleep(rt_, deadline));
    }

private:
    template <class C>
    status spawn_detached_impl(C&& c, const attrs& a) {
        if (!rt_) return CORO_EINVAL;
        typedef typename std::decay<C>::type caller_t;
        typedef detail::detached_box<caller_t> box_t;
        box_t* box = detail::box_new<box_t>(std::forward<C>(c), a);
        if (!box) return CORO_ENOMEM;
        coro_attr_t ca = a.c_attr(box->name_ptr());
        const coro_status cs =
            coro_rt_spawn(rt_, &ca, &detail::rt_trampoline, box);
        if (cs != CORO_OK) {
            delete box;
            return status(cs);
        }
        return CORO_OK;
    }

    template <class R, class C>
    status spawn_task_impl(task<R>& out, C&& c, const attrs& a) {
        if (!rt_) return CORO_EINVAL;
        std::shared_ptr<detail::state<R> > st = detail::make_state<R>();
        if (!st) return CORO_ENOMEM;
        typedef typename std::decay<C>::type caller_t;
        typedef detail::result_box<caller_t, R> box_t;
        box_t* box = detail::box_new<box_t>(std::forward<C>(c), st, a);
        if (!box) return CORO_ENOMEM;
        coro_attr_t ca = a.c_attr(box->name_ptr());
        coro_t* h = NULL;
        const coro_status cs =
            coro_rt_spawn_h(rt_, &ca, &detail::rt_trampoline, box, &h);
        if (cs != CORO_OK) {
            delete box;
            return status(cs);
        }
        out.adopt(rt_, h, st);
        return CORO_OK;
    }

    void close() noexcept {
        if (!rt_) return;
        (void)coro_rt_wait_all(rt_);
        const coro_status cs = coro_rt_destroy(rt_);
        if (cs != CORO_OK) {
            detail::contract_violation(
                "corocc::runtime destroyed while coroutines are still live");
        }
        rt_ = NULL;
    }

    coro_rt_t* rt_;
};

/* ======================================================================= */
/* L3: scope -- structured concurrency                                     */
/* ======================================================================= */

/*
 * scope -- a structured-concurrency scope bound to one runtime.
 *
 * Not movable (the C record holds a back-pointer to this object) and not
 * copyable. The destructor drains: cancel(), wait() (unbounded by design --
 * a child that can never finish blocks it, the documented caller
 * responsibility inherited from L3), destroy().
 *
 * Every child -- typed or not -- is registered with a non-NULL cell_out:
 * a child that ends abnormally (an exception) writes ECANCELED into
 * its C cell, which is what drives coro_rt_group_wait_first_error's early
 * exit. The wrapper never reads that C alias: the code and exception the C++
 * side sees come from the wrapper's own snapshot (the child's shared state),
 * which is why cells stay readable after the scope is destroyed.
 *
 * ECANCELED from the wait family has three meanings: a cancel request was
 * observed; a child ended abnormally (consult first_exception()); or this
 * waiter's own deadline fired (wait_for/wait_until). first_exception() is
 * the authoritative error channel and takes precedence over the code
 * channel.
 */
class scope {
public:
    explicit scope(runtime& rt) noexcept : rt_(rt.native()), frozen_(false), cached_code_(0), destroyed_(false) {
        (void)coro_rt_scope_init(&c_);
    }
    explicit scope(coro_rt_t* rt) noexcept : rt_(rt), frozen_(false), cached_code_(0), destroyed_(false) {
        (void)coro_rt_scope_init(&c_);
    }
    ~scope() {
        (void)cancel();
        (void)wait();
        const coro_status cs = destroy_impl();
        if (cs != CORO_OK) {
            detail::contract_violation(
                "corocc::scope dtor failed to destroy the scope (live children?)");
        }
    }

    scope(const scope&) = delete;
    scope& operator=(const scope&) = delete;
    scope(scope&&) = delete;
    scope& operator=(scope&&) = delete;

    coro_rt_scope_t* native() noexcept { return &c_; }

    bool cancel_requested() const noexcept {
        return coro_rt_scope_cancel_requested(&c_) != 0;
    }

    /* Before the drain: the C partial aggregate; after the drain (or after
     * destroy): the cached final value. Never dereferences C records after
     * destroy. */
    corocc::status status() const noexcept {
        if (frozen_.load(std::memory_order_acquire)) {
            return corocc::status(static_cast<coro_status>(
                cached_code_.load(std::memory_order_relaxed)));
        }
        if (!rt_) return corocc::status(CORO_OK);
        return corocc::status(coro_rt_scope_status(&c_));
    }

    corocc::status cancel() noexcept {
        return corocc::status(coro_rt_scope_cancel(rt_, &c_));
    }

    /* wait/wait_all: drain and return the aggregate. */
    corocc::status wait() noexcept {
        const corocc::status r(coro_rt_scope_wait(rt_, &c_));
        if (r.code() != CORO_EINVAL && r.code() != CORO_ESTALE) freeze_after_drain();
        return r;
    }

    corocc::status wait_all() noexcept { return wait(); }

    /* The first non-OK child result; ECANCELED here may be a cancel or an
     * abnormal end (check first_exception()) -- the C-level early exit. */
    corocc::status wait_first_error() noexcept {
        const corocc::status r(coro_rt_group_wait_first_error(rt_, &c_));
        if (r.code() != CORO_EINVAL && r.code() != CORO_ESTALE) freeze_after_drain();
        return r;
    }

    /* The first completed child's result; the scope stays alive and the
     * caller MUST follow up with wait() or cancel() (destroy alone is a
     * detectable EBUSY). */
    corocc::status wait_first_done(corocc::status& first) noexcept {
        coro_status f = CORO_OK;
        const coro_status cs = coro_rt_group_wait_first_done(rt_, &c_, &f);
        if (cs != CORO_OK) return corocc::status(cs);
        first = corocc::status(f);
        return corocc::status(CORO_OK);
    }

    template <class Rep, class Period>
    corocc::status wait_for(const std::chrono::duration<Rep, Period>& d) noexcept {
        const corocc::status r(coro_rt_scope_wait_for(rt_, &c_, detail::duration_to_ms_ceil(d)));
        if (r.code() != CORO_EINVAL && r.code() != CORO_ESTALE &&
            r.code() != CORO_ENOMEM) {
            freeze_after_drain();
        }
        return r;
    }

    corocc::status wait_until(monotonic_clock::time_point tp) noexcept {
        const std::uint64_t now = coro_monotonic_ns();
        const std::uint64_t at =
            static_cast<std::uint64_t>(tp.time_since_epoch().count());
        std::uint64_t ms = 0;
        if (at > now) {
            const std::uint64_t delta = at - now;
            ms = delta / 1000000ull + (delta % 1000000ull ? 1ull : 0ull);
        }
        const corocc::status r(coro_rt_scope_wait_for(rt_, &c_, ms));
        if (r.code() != CORO_EINVAL && r.code() != CORO_ESTALE &&
            r.code() != CORO_ENOMEM) {
            freeze_after_drain();
        }
        return r;
    }

    /* Drain, then deliver: the exception channel first (authoritative),
     * then the code channel. */
#if COROCC_CPP_HAS_EXCEPTIONS
    void wait_or_throw() {
        const corocc::status r = wait();
        const std::exception_ptr e = first_exception();
        if (e) std::rethrow_exception(e);
        if (r.failed()) throw error(r.code());
    }
#else
    void wait_or_throw() = delete;
#endif

    /* The exception of the first completed child that ended abnormally
     * (completion order); the authoritative error channel. */
    std::exception_ptr first_exception() const noexcept {
        std::lock_guard<std::mutex> g(table_mu_);
        std::exception_ptr best;
        std::uint64_t best_seq = 0;
        bool found = false;
        for (std::size_t i = 0; i < children_.size(); ++i) {
            const detail::state_base& st = *children_[i];
            if (!st.ready.load(std::memory_order_acquire)) continue;
            if (!st.eptr) continue;
            if (!found || st.completion_seq < best_seq) {
                found = true;
                best_seq = st.completion_seq;
                best = st.eptr;
            }
        }
        return best;
    }

    /* ---- spawns ---- */

    /* Non-typed spawn: the callable returns void (code OK) or coro_status
     * (its value is the code channel). */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
    template <class F>
    corocc::status spawn(F f, const attrs& a = attrs()) {
        return detail::normalize_entry([&]() -> corocc::status {
            return spawn_void_impl(detail::callable0<F>(std::move(f)), a);
        });
    }

    template <class F, class... A>
    typename std::enable_if<(sizeof...(A) > 0), corocc::status>::type
    spawn(F f, A... args) {
        return detail::normalize_entry([&]() -> corocc::status {
            return spawn_void_impl(
                detail::callable_args<F, A...>(std::move(f), std::move(args)...),
                attrs());
        });
    }

    /* Typed spawn: the result lands in out's shared state; out is bound only
     * on success (a previous cell binding is released first). */
    template <class R, class F>
    corocc::status spawn(cell<R>& out, F f, const attrs& a = attrs()) {
        return detail::normalize_entry([&]() -> corocc::status {
            return spawn_typed_impl<R>(out, detail::callable0<F>(std::move(f)), a);
        });
    }

    template <class R, class F, class... A>
    typename std::enable_if<(sizeof...(A) > 0), corocc::status>::type
    spawn(cell<R>& out, F f, A... args) {
        return detail::normalize_entry([&]() -> corocc::status {
            return spawn_typed_impl<R>(
                out, detail::callable_args<F, A...>(std::move(f), std::move(args)...),
                attrs());
        });
    }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    /* Explicit destroy: OK (idempotent) / EBUSY (children live) / EINVAL. */
    corocc::status destroy() noexcept { return corocc::status(destroy_impl()); }

private:
    template <class C>
    corocc::status spawn_void_impl(C&& c, const attrs& a) {
        if (!rt_) return CORO_EINVAL;
        std::shared_ptr<detail::state<void> > st = detail::make_state<void>();
        if (!st) return CORO_ENOMEM;
        typedef typename std::decay<C>::type caller_t;
        typedef detail::scope_void_box<caller_t> box_t;
        box_t* box = detail::box_new<box_t>(std::forward<C>(c), st,
                                            &completion_seq_, a);
        if (!box) return CORO_ENOMEM;
        /* The table growth is paid before the C registration, so a metadata
         * failure leaves no half state anywhere. */
        if (!register_child(st)) {
            delete box;
            return CORO_ENOMEM;
        }
        coro_attr_t ca = a.c_attr(box->name_ptr());
        /* A non-NULL cell_out for EVERY child (the received alias is
         * deliberately never read). A stack local is required: the child may
         * finish -- and the box goes -- before the C call returns, while the
         * alias is written at the very end of that call. */
        coro_status* alias = NULL;
        const coro_status cs = coro_rt_scope_spawn(
            rt_, &c_, &ca, &detail::scope_trampoline, box, &alias);
        if (cs != CORO_OK) {
            unregister_child(st);
            delete box;
            return corocc::status(cs);
        }
        return CORO_OK;
    }

    template <class R, class C>
    corocc::status spawn_typed_impl(cell<R>& out, C&& c, const attrs& a) {
        if (!rt_) return CORO_EINVAL;
        std::shared_ptr<detail::state<R> > st = detail::make_state<R>();
        if (!st) return CORO_ENOMEM;
        typedef typename std::decay<C>::type caller_t;
        typedef detail::scope_result_box<caller_t, R> box_t;
        box_t* box = detail::box_new<box_t>(std::forward<C>(c), st,
                                            &completion_seq_, a);
        if (!box) return CORO_ENOMEM;
        if (!register_child(st)) {
            delete box;
            return CORO_ENOMEM;
        }
        coro_attr_t ca = a.c_attr(box->name_ptr());
        coro_status* alias = NULL;
        const coro_status cs = coro_rt_scope_spawn(
            rt_, &c_, &ca, &detail::scope_trampoline, box, &alias);
        if (cs != CORO_OK) {
            unregister_child(st);
            delete box;
            return corocc::status(cs);
        }
        out = cell<R>(st); /* bind only on success; releases the old ref */
        return CORO_OK;
    }

    bool register_child(const std::shared_ptr<detail::state_base>& st) {
        std::lock_guard<std::mutex> g(table_mu_);
#if COROCC_CPP_HAS_EXCEPTIONS
        try {
            children_.push_back(st);
        } catch (const std::bad_alloc&) {
            return false;
        }
#else
        children_.push_back(st);
#endif
        return true;
    }

    void unregister_child(const std::shared_ptr<detail::state_base>& st) noexcept {
        std::lock_guard<std::mutex> g(table_mu_);
        for (std::size_t i = 0; i < children_.size(); ++i) {
            if (children_[i].get() == st.get()) {
                children_.erase(children_.begin() +
                                static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    /* Snapshot the scope's aggregate into the cache. Called after a drain
     * (the C records are still alive then; destroy() freezes before
     * releasing them), so status() keeps answering after the records are
     * gone. */
    void freeze_after_drain() const noexcept {
        const coro_status code = rt_ ? coro_rt_scope_status(&c_) : CORO_OK;
        cached_code_.store(static_cast<int>(code), std::memory_order_relaxed);
        frozen_.store(true, std::memory_order_release);
    }

    coro_status destroy_impl() noexcept {
        if (destroyed_) return CORO_OK;
        if (!frozen_.load(std::memory_order_acquire)) {
            freeze_after_drain();
        }
        const coro_status cs = coro_rt_scope_destroy(&c_);
        if (cs == CORO_OK) destroyed_ = true;
        return cs;
    }

    coro_rt_scope_t c_;
    coro_rt_t* rt_;
    std::atomic<std::uint64_t> completion_seq_;
    mutable std::mutex table_mu_;
    mutable std::vector<std::shared_ptr<detail::state_base> > children_;
    mutable std::atomic<bool> frozen_;
    mutable std::atomic<int> cached_code_;
    bool destroyed_;
};

/* ======================================================================= */
/* L1: coroutine -- the manually driven coroutine                          */
/* ======================================================================= */

/*
 * coroutine -- unique ownership of a manually driven coro_t.
 *
 * The suspension credential is internal: start()/resume() advance the
 * wrapper's token, which assumes the documented single-driver discipline.
 * Advanced token protocols use native() and the C API.
 *
 * Destroying a SUSPENDED coroutine is legal and silently abandons the
 * coroutine frame: the destructors of C++ objects on that stack do NOT run
 * (callbacks pre-armed on the frame will not fire). Cooperate when it
 * matters: cancel() then drive resume() until done().
 */
class coroutine {
public:
    coroutine() noexcept : c_(NULL), box_(NULL), tok_(CORO_TOKEN_NONE) {}

    coroutine(coroutine&& other) noexcept
        : c_(other.c_), box_(other.box_), tok_(other.tok_) {
        other.c_ = NULL;
        other.box_ = NULL;
        other.tok_ = CORO_TOKEN_NONE;
    }

    coroutine& operator=(coroutine&& other) noexcept {
        if (this != &other) {
            close();
            c_ = other.c_;
            box_ = other.box_;
            tok_ = other.tok_;
            other.c_ = NULL;
            other.box_ = NULL;
            other.tok_ = CORO_TOKEN_NONE;
        }
        return *this;
    }

    coroutine(const coroutine&) = delete;
    coroutine& operator=(const coroutine&) = delete;
    ~coroutine() { close(); }

    /*
     * create -- build the coroutine in out. out must be empty (CORO_ESTATE
     * otherwise); on failure out is untouched. The callable runs on the
     * coroutine's own stack; its exceptions are captured, never propagated
     * across the context switch. Metadata OOM is CORO_ENOMEM; an exception
     * thrown while storing the callable propagates to the caller.
     */
    template <class F>
    static status create(coroutine& out, F f, const attrs& a = attrs()) {
        if (out.valid()) return CORO_ESTATE;
        return detail::normalize_entry([&]() -> status {
            typedef detail::callable0<F> caller_t;
            typedef detail::l1_box<caller_t> box_t;
            box_t* box = detail::box_new<box_t>(caller_t(std::move(f)), a);
            if (!box) return status(CORO_ENOMEM);
            coro_attr_t ca = a.c_attr(box->name_ptr());
            coro_t* h = NULL;
            const coro_status cs =
                coro_create(&h, &ca, &detail::l1_trampoline, box);
            if (cs != CORO_OK) {
                delete box;
                return status(cs);
            }
            out.c_ = h;
            out.box_ = box;
            out.tok_ = CORO_TOKEN_NONE;
            return CORO_OK;
        });
    }

    bool valid() const noexcept { return c_ != NULL; }
    bool done() const noexcept { return coro_get_state(c_) == CORO_DEAD; }
    coro_state state() const noexcept { return coro_get_state(c_); }
    coro_t* native() noexcept { return c_; }

    status start() noexcept {
        if (!c_) return CORO_EINVAL;
        coro_token next = CORO_TOKEN_NONE;
        const status st(coro_start(c_, &next));
        if (st.ok()) tok_ = next;
        return st;
    }

    status resume() noexcept {
        if (!c_) return CORO_EINVAL;
        coro_token next = CORO_TOKEN_NONE;
        const status st(coro_resume(c_, tok_, &next));
        if (st.ok()) tok_ = next;
        return st;
    }

    status cancel() noexcept { return status(coro_cancel(c_)); }
    status join() noexcept { return status(coro_join(c_)); }

    /* The captured body exception (null unless the body threw and the
     * coroutine has finished). */
    std::exception_ptr exception() const noexcept {
        if (!box_) return std::exception_ptr();
        detail::l1_box_base* b = static_cast<detail::l1_box_base*>(box_);
        if (!b->ready.load(std::memory_order_acquire)) {
            return std::exception_ptr();
        }
        return b->eptr;
    }

    /* Explicit release: OK and the handle empties; EBUSY (running) is
     * reported to the caller; a second destroy is the empty-handle EINVAL. */
    status destroy() noexcept {
        if (!c_) return CORO_EINVAL;
        note_abandon();
        const coro_status cs = coro_destroy(c_);
        if (cs == CORO_OK) {
            this->clear_handle();
        }
        return status(cs);
    }

private:
    void note_abandon() noexcept {
#ifndef NDEBUG
        if (coro_get_state(c_) != CORO_DEAD) {
            detail::abandoned_count_slot().fetch_add(1, std::memory_order_relaxed);
        }
#endif
    }

    void clear_handle() noexcept {
        c_ = NULL;
        delete static_cast<detail::box_base*>(box_);
        box_ = NULL;
        tok_ = CORO_TOKEN_NONE;
    }

    /* The RAII path: destroying a running coroutine is a contract
     * violation (the frame is executing elsewhere); a SUSPENDED one is
     * abandoned silently, which is legal -- see the class comment. */
    void close() noexcept {
        if (!c_) return;
        note_abandon();
        const coro_status cs = coro_destroy(c_);
        if (cs != CORO_OK) {
            if (cs == CORO_EBUSY) {
                detail::contract_violation(
                    "corocc::coroutine destroyed while running");
            }
            detail::contract_violation("corocc::coroutine destroy failed");
        }
        clear_handle();
    }

    coro_t* c_;
    void* box_;
    coro_token tok_;
};

/* ======================================================================= */
/* L2': sched_task<R> and scheduler -- the single-thread run loop          */
/* ======================================================================= */

namespace detail {

/*
 * sched_task_core -- shared body of sched_task<R>/sched_task<void>.
 *
 * Thread affinity: every operation that could touch the C handle runs only
 * on the thread the scheduler was created on, and only while the wrapper's
 * done flag is clear. The scheduler retires (and frees) a finished handle,
 * so the done flag -- set by the trampoline before it returns -- is the
 * guard that makes a retired handle unobservable.
 */
template <class R>
class sched_task_core {
public:
    sched_task_core() noexcept : s_(NULL), c_(NULL), tid_() {}

    sched_task_core(sched_task_core&& other) noexcept
        : s_(other.s_), c_(other.c_), st_(std::move(other.st_)), tid_(other.tid_) {
        other.s_ = NULL;
        other.c_ = NULL;
    }

    sched_task_core& operator=(sched_task_core&& other) noexcept {
        if (this != &other) {
            close_out();
            s_ = other.s_;
            c_ = other.c_;
            st_ = std::move(other.st_);
            tid_ = other.tid_;
            other.s_ = NULL;
            other.c_ = NULL;
        }
        return *this;
    }

    sched_task_core(const sched_task_core&) = delete;
    sched_task_core& operator=(const sched_task_core&) = delete;
    ~sched_task_core() { close_out(); }

    bool valid() const noexcept { return c_ != NULL; }

    bool done() const noexcept {
        return static_cast<bool>(st_) &&
               st_->ready.load(std::memory_order_acquire);
    }

    /* Cancel: done -> OK without touching C (the handle may be retired
     * already); another thread -> ESTALE without touching C; otherwise the C
     * request. */
    status cancel() noexcept {
        if (!c_) return CORO_EINVAL;
        if (done()) return CORO_OK;
        if (tid_ != std::this_thread::get_id()) return CORO_ESTALE;
        return status(coro_sched_cancel(s_, c_));
    }

    /* Wait: done -> OK (the completed-normalisation); otherwise park on the
     * target's chain from this scheduler's coroutine (ESTALE outside one).
     * EDESTROYED from the C call is normalised to OK. */
    status wait() noexcept {
        if (!c_) return CORO_EINVAL;
        if (done()) return CORO_OK;
        if (tid_ != std::this_thread::get_id()) return CORO_ESTALE;
        const coro_status cs = coro_sched_join(s_, c_);
        if (cs == CORO_EDESTROYED) return CORO_OK;
        return status(cs);
    }

    std::exception_ptr exception() const noexcept {
        return done() ? st_->eptr : std::exception_ptr();
    }

protected:
    void adopt(coro_sched_t* s, coro_t* h, std::shared_ptr<state<R> > st) noexcept {
        close_out();
        s_ = s;
        c_ = h;
        st_ = std::move(st);
        tid_ = std::this_thread::get_id();
    }

    /* The handle belongs to the scheduler: an unfinished task gets one
     * cancel request (bound thread only, never touching C from elsewhere);
     * the scheduler's retire releases the handle, so only the wrapper state
     * is dropped here. */
    void close_out() noexcept {
        if (!c_) {
            st_.reset();
            return;
        }
        if (!done() && tid_ == std::this_thread::get_id()) {
            (void)coro_sched_cancel(s_, c_);
        }
        s_ = NULL;
        c_ = NULL;
        st_.reset();
    }

    coro_sched_t* s_;
    coro_t* c_;
    std::shared_ptr<state<R> > st_;
    std::thread::id tid_;

    friend class ::corocc::scheduler;
};

} /* namespace detail */

template <class R>
class sched_task : public detail::sched_task_core<R> {
public:
    static_assert(!std::is_reference<R>::value,
                  "corocc::sched_task<R> does not support reference results");

    sched_task() noexcept {}
    sched_task(sched_task&&) noexcept = default;
    sched_task& operator=(sched_task&&) noexcept = default;
    sched_task(const sched_task&) = delete;
    sched_task& operator=(const sched_task&) = delete;

#if COROCC_CPP_HAS_EXCEPTIONS
    R get() {
        if (!this->st_) throw error(CORO_EINVAL);
        if (!this->done()) {
            const status w = this->wait();
            if (w.failed()) {
                if (this->st_->eptr) std::rethrow_exception(this->st_->eptr);
                throw error(w.code());
            }
        }
        if (this->st_->eptr) std::rethrow_exception(this->st_->eptr);
        return detail::result_access<R>::take(*this->st_);
    }
#else
    R get() = delete;
#endif

    const R* peek() const noexcept {
        if (!this->done()) return NULL;
        return detail::result_access<R>::peek(*this->st_);
    }
};

template <>
class sched_task<void> : public detail::sched_task_core<void> {
public:
    sched_task() noexcept {}
    sched_task(sched_task&&) noexcept = default;
    sched_task& operator=(sched_task&&) noexcept = default;
    sched_task(const sched_task&) = delete;
    sched_task& operator=(const sched_task&) = delete;

#if COROCC_CPP_HAS_EXCEPTIONS
    void get() {
        if (!this->st_) throw error(CORO_EINVAL);
        if (!this->done()) {
            const status w = this->wait();
            if (w.failed()) {
                if (this->st_->eptr) std::rethrow_exception(this->st_->eptr);
                throw error(w.code());
            }
        }
        if (this->st_->eptr) std::rethrow_exception(this->st_->eptr);
    }
#else
    void get() = delete;
#endif
};

/*
 * scheduler -- unique ownership of coro_sched_t, bound to the creating
 * thread. Every run-loop call (and the dtor) asserts the bound thread in
 * debug builds; the released contract is documented unsynchronised use.
 *
 * The dtor drains with wait_all() then destroys. wait_all may return while
 * a parked coroutine is still live (it has nothing runnable and no timer),
 * and destroy then reports EBUSY: that is a caller red line (a child must
 * be reachable to completion), and the dtor aborts on it.
 */
class scheduler {
public:
    scheduler() noexcept : s_(NULL), tid_() {}

    scheduler(scheduler&& other) noexcept : s_(other.s_), tid_(other.tid_) {
        other.s_ = NULL;
    }

    scheduler& operator=(scheduler&& other) noexcept {
        if (this != &other) {
            close();
            s_ = other.s_;
            tid_ = other.tid_;
            other.s_ = NULL;
        }
        return *this;
    }

    scheduler(const scheduler&) = delete;
    scheduler& operator=(const scheduler&) = delete;
    ~scheduler() { close(); }

    static status create(scheduler& out) {
        if (out.s_) return CORO_ESTATE;
        out.s_ = coro_sched_create();
        out.tid_ = std::this_thread::get_id();
        return out.s_ ? status(CORO_OK) : status(CORO_ENOMEM);
    }

    bool valid() const noexcept { return s_ != NULL; }
    coro_sched_t* native() noexcept { return s_; }
    std::size_t pending() const noexcept { return coro_sched_pending(s_); }

    status run() noexcept {
        if (!s_) return CORO_EINVAL;
        detail::debug_assert_bound_thread(tid_);
        return status(coro_sched_run(s_));
    }

    status wait_all() noexcept {
        if (!s_) return CORO_EINVAL;
        detail::debug_assert_bound_thread(tid_);
        return status(coro_sched_wait_all(s_));
    }

    status destroy() noexcept {
        if (!s_) return CORO_OK;
        detail::debug_assert_bound_thread(tid_);
        const coro_status cs = coro_sched_destroy(s_);
        if (cs == CORO_OK) s_ = NULL;
        return status(cs);
    }

    status cancel(coro_t* c) noexcept {
        if (!s_) return CORO_EINVAL;
        detail::debug_assert_bound_thread(tid_);
        return status(coro_sched_cancel(s_, c));
    }

    /* ---- spawns (C4702 silencing: a move-always-throws instantiation;
     * the variadic twins ride the same region) ---- */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
    template <class F>
    status spawn(F f, const attrs& a = attrs()) {
        return detail::normalize_entry([&]() -> status {
            return spawn_detached_impl(detail::callable0<F>(std::move(f)), a);
        });
    }

    template <class F, class... A>
    typename std::enable_if<(sizeof...(A) > 0), status>::type
    spawn(F f, A... args) {
        return detail::normalize_entry([&]() -> status {
            return spawn_detached_impl(
                detail::callable_args<F, A...>(std::move(f), std::move(args)...),
                attrs());
        });
    }

    template <class R, class F>
    status spawn_h(sched_task<R>& out, F f, const attrs& a = attrs()) {
        return detail::normalize_entry([&]() -> status {
            return spawn_task_impl<R>(
                out, detail::callable0<F>(std::move(f)), a);
        });
    }

    template <class R, class F, class... A>
    typename std::enable_if<(sizeof...(A) > 0), status>::type
    spawn_h(sched_task<R>& out, F f, A... args) {
        return detail::normalize_entry([&]() -> status {
            return spawn_task_impl<R>(
                out, detail::callable_args<F, A...>(std::move(f), std::move(args)...),
                attrs());
        });
    }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    template <class Rep, class Period>
    status sleep_for(const std::chrono::duration<Rep, Period>& d) noexcept {
        if (!s_) return CORO_EINVAL;
        return status(coro_sched_sleep(
            s_, detail::deadline_from_now_ns(detail::duration_to_ns_ceil(d))));
    }

    status sleep_until(monotonic_clock::time_point tp) noexcept {
        if (!s_) return CORO_EINVAL;
        std::uint64_t deadline =
            static_cast<std::uint64_t>(tp.time_since_epoch().count());
        const std::uint64_t now = coro_monotonic_ns();
        if (deadline < now) deadline = now;
        return status(coro_sched_sleep(s_, deadline));
    }

private:
    template <class C>
    status spawn_detached_impl(C&& c, const attrs& a) {
        if (!s_) return CORO_EINVAL;
        detail::debug_assert_bound_thread(tid_);
        typedef typename std::decay<C>::type caller_t;
        typedef detail::detached_box<caller_t> box_t;
        box_t* box = detail::box_new<box_t>(std::forward<C>(c), a);
        if (!box) return CORO_ENOMEM;
        coro_attr_t ca = a.c_attr(box->name_ptr());
        const coro_status cs =
            coro_sched_spawn(s_, &ca, &detail::rt_trampoline, box);
        if (cs != CORO_OK) {
            delete box;
            return status(cs);
        }
        return CORO_OK;
    }

    template <class R, class C>
    status spawn_task_impl(sched_task<R>& out, C&& c, const attrs& a) {
        if (!s_) return CORO_EINVAL;
        detail::debug_assert_bound_thread(tid_);
        std::shared_ptr<detail::state<R> > st = detail::make_state<R>();
        if (!st) return CORO_ENOMEM;
        typedef typename std::decay<C>::type caller_t;
        typedef detail::result_box<caller_t, R> box_t;
        box_t* box = detail::box_new<box_t>(std::forward<C>(c), st, a);
        if (!box) return CORO_ENOMEM;
        coro_attr_t ca = a.c_attr(box->name_ptr());
        coro_t* h = NULL;
        const coro_status cs =
            coro_sched_spawn_h(s_, &ca, &detail::rt_trampoline, box, &h);
        if (cs != CORO_OK) {
            delete box;
            return status(cs);
        }
        out.adopt(s_, h, st);
        return CORO_OK;
    }

    /* Drain + destroy, with the caller red line on a parked child. */
    void close() noexcept {
        if (!s_) return;
        detail::debug_assert_bound_thread(tid_);
        (void)coro_sched_wait_all(s_);
        const coro_status cs = coro_sched_destroy(s_);
        if (cs != CORO_OK) {
            detail::contract_violation(
                "corocc::scheduler destroyed with live coroutines: drive every child to completion first (a child parked forever is a caller responsibility)");
        }
        s_ = NULL;
    }

    coro_sched_t* s_;
    std::thread::id tid_;
};

/* ======================================================================= */
/* L2: synchronization primitives                                          */
/* ======================================================================= */

/*
 * The primitives are value types bound to one runtime at construction; a
 * zero-initialized C object is valid, so construction cannot fail. All the
 * waiting entries are coroutine-only: a plain thread gets CORO_ESTALE, the
 * C answer (never a block). unlock/rdunlock/wrunlock outside the owning
 * state, and trylock on a held object, report the C codes (ESTATE, EBUSY).
 *
 * Destroying a primitive while a coroutine holds or waits on it is a caller
 * bug (the C documentation says the same); the wrapper performs no C call in
 * these destructors.
 */

class mutex {
public:
    explicit mutex(runtime& rt) noexcept : rt_(rt.native()), m_() {
        coro_rt_mutex_init(&m_);
    }
    explicit mutex(coro_rt_t* rt) noexcept : rt_(rt), m_() {
        coro_rt_mutex_init(&m_);
    }
    ~mutex() {}

    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;

    coro_rt_mutex_t* native() noexcept { return &m_; }

    status lock() noexcept { return status(coro_rt_mutex_lock(rt_, &m_)); }
    status trylock() noexcept { return status(coro_rt_mutex_trylock(rt_, &m_)); }
    bool try_lock() noexcept { return trylock().ok(); } /* std Lockable face */
    status unlock() noexcept { return status(coro_rt_mutex_unlock(rt_, &m_)); }

private:
    coro_rt_t* rt_;
    coro_rt_mutex_t m_;
};

class condition_variable {
public:
    explicit condition_variable(runtime& rt) noexcept : rt_(rt.native()), c_() {
        coro_rt_cond_init(&c_);
    }
    explicit condition_variable(coro_rt_t* rt) noexcept : rt_(rt), c_() {
        coro_rt_cond_init(&c_);
    }
    ~condition_variable() {}

    condition_variable(const condition_variable&) = delete;
    condition_variable& operator=(const condition_variable&) = delete;

    /* Atomically release m, park, re-acquire m before returning. */
    status wait(mutex& m) noexcept {
        return status(coro_rt_cond_wait(rt_, &c_, m.native()));
    }
    status notify_one() noexcept { return status(coro_rt_cond_notify(rt_, &c_)); }
    status notify_all() noexcept { return status(coro_rt_cond_broadcast(rt_, &c_)); }

private:
    coro_rt_t* rt_;
    coro_rt_cond_t c_;
};

class semaphore {
public:
    explicit semaphore(runtime& rt, std::size_t count = 0) noexcept
        : rt_(rt.native()), s_() {
        coro_rt_sem_init(&s_, count);
    }
    explicit semaphore(coro_rt_t* rt, std::size_t count = 0) noexcept
        : rt_(rt), s_() {
        coro_rt_sem_init(&s_, count);
    }
    ~semaphore() {}

    semaphore(const semaphore&) = delete;
    semaphore& operator=(const semaphore&) = delete;

    status wait() noexcept { return status(coro_rt_sem_wait(rt_, &s_)); }
    status trywait() noexcept { return status(coro_rt_sem_trywait(rt_, &s_)); }
    bool try_wait() noexcept { return trywait().ok(); }
    status post() noexcept { return status(coro_rt_sem_post(rt_, &s_)); }
    std::size_t count() const noexcept { return coro_rt_sem_count(&s_); }

private:
    coro_rt_t* rt_;
    coro_rt_sem_t s_;
};

class rwlock {
public:
    explicit rwlock(runtime& rt) noexcept : rt_(rt.native()), l_() {
        coro_rt_rwlock_init(&l_);
    }
    explicit rwlock(coro_rt_t* rt) noexcept : rt_(rt), l_() {
        coro_rt_rwlock_init(&l_);
    }
    ~rwlock() {}

    rwlock(const rwlock&) = delete;
    rwlock& operator=(const rwlock&) = delete;

    status rdlock() noexcept { return status(coro_rt_rwlock_rdlock(rt_, &l_)); }
    status tryrdlock() noexcept { return status(coro_rt_rwlock_tryrdlock(rt_, &l_)); }
    status rdunlock() noexcept { return status(coro_rt_rwlock_rdunlock(rt_, &l_)); }
    status wrlock() noexcept { return status(coro_rt_rwlock_wrlock(rt_, &l_)); }
    status trywrlock() noexcept { return status(coro_rt_rwlock_trywrlock(rt_, &l_)); }
    status wrunlock() noexcept { return status(coro_rt_rwlock_wrunlock(rt_, &l_)); }

private:
    coro_rt_t* rt_;
    coro_rt_rwlock_t l_;
};

/*
 * channel<T> -- a FIFO message channel between runtime coroutines.
 *
 * channel<> (that is, channel<void*>) is the zero-allocation C-parallel
 * face: the payload is an opaque pointer whose lifetime the caller owns.
 * channel<T> stores one heap node per message; the copy/move that stores the
 * payload follows the callable-argument rule (a std::bad_alloc normalises to
 * CORO_ENOMEM, any other exception propagates to the caller), and the
 * destructor closes, drains every queued node and destroys.
 *
 * send/recv are coroutine-only (ESTALE on a plain thread); a closed channel
 * reports CORO_ECLOSED after handing out what was already queued.
 */
template <class T = void*>
class channel;

template <>
class channel<void*> {
public:
    /* c_ is value-initialized: coro_rt_channel_init resets the ring by
     * freeing ch->slots first, so an indeterminate object would be freed as
     * a garbage pointer (the C contract is "zero-initialized is valid"). */
    explicit channel(runtime& rt, std::size_t capacity = 0) noexcept
        : rt_(rt.native()), c_(), destroyed_(false) {
        init_ = coro_rt_channel_init(&c_, capacity);
    }
    explicit channel(coro_rt_t* rt, std::size_t capacity = 0) noexcept
        : rt_(rt), c_(), destroyed_(false) {
        init_ = coro_rt_channel_init(&c_, capacity);
    }
    ~channel() {
        if (destroyed_ || !init_.ok()) return;
        const coro_status cs = teardown();
        if (cs != CORO_OK) {
            detail::contract_violation(
                "corocc::channel destroyed with waiters or pending items");
        }
    }

    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;

    bool valid() const noexcept { return init_.ok(); }

    status send(void* v) noexcept { return guarded(coro_rt_channel_send(rt_, &c_, v)); }
    status trysend(void* v) noexcept { return guarded(coro_rt_channel_trysend(rt_, &c_, v)); }
    status recv(void** out) noexcept { return guarded(coro_rt_channel_recv(rt_, &c_, out)); }
    status tryrecv(void** out) noexcept { return guarded(coro_rt_channel_tryrecv(rt_, &c_, out)); }
    status close() noexcept { return guarded(coro_rt_channel_close(rt_, &c_)); }
    std::size_t size() const noexcept { return coro_rt_channel_size(&c_); }
    bool closed() const noexcept { return coro_rt_channel_closed(&c_) != 0; }

    /* Idempotent: close, drain the queued items (they are caller-owned
     * pointers, so the wrapper deliberately does not free them), destroy. */
    status destroy() noexcept {
        if (destroyed_) return CORO_OK;
        if (!init_.ok()) return init_;
        return status(teardown());
    }

private:
    status guarded(coro_status cs) const noexcept {
        return init_.ok() ? status(cs) : init_;
    }

    coro_status teardown() noexcept {
        (void)coro_rt_channel_close(rt_, &c_);
        void* p = NULL;
        while (coro_rt_channel_tryrecv(rt_, &c_, &p) == CORO_OK) {
            /* discarded: the caller owns the pointee */
        }
        const coro_status cs = coro_rt_channel_destroy(&c_);
        if (cs == CORO_OK) destroyed_ = true;
        return cs;
    }

    coro_rt_t* rt_;
    coro_rt_channel_t c_;
    status init_;
    bool destroyed_;
};

template <class T>
class channel {
public:
    struct node {
        T v;
        template <class U>
        explicit node(U&& u) : v(std::forward<U>(u)) {}
    };

    explicit channel(runtime& rt, std::size_t capacity = 0) noexcept
        : rt_(rt.native()), c_(), destroyed_(false) {
        init_ = coro_rt_channel_init(&c_, capacity);
    }
    explicit channel(coro_rt_t* rt, std::size_t capacity = 0) noexcept
        : rt_(rt), c_(), destroyed_(false) {
        init_ = coro_rt_channel_init(&c_, capacity);
    }
    ~channel() {
        if (destroyed_ || !init_.ok()) return;
        const coro_status cs = teardown();
        if (cs != CORO_OK) {
            detail::contract_violation(
                "corocc::channel<T> destroyed with waiters or pending items");
        }
    }

    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;

    bool valid() const noexcept { return init_.ok(); }

    /* One node allocation per message. The payload store may throw (user
     * type); std::bad_alloc becomes ENOMEM, other exceptions propagate. */
    status send(const T& v) { return send_impl(v, false); }
    status send(T&& v) { return send_impl(std::move(v), false); }
    status trysend(const T& v) { return send_impl(v, true); }
    status trysend(T&& v) { return send_impl(std::move(v), true); }

    status recv(T& out) { return recv_impl(out, false); }
    status tryrecv(T& out) { return recv_impl(out, true); }

    status close() noexcept {
        if (!init_.ok()) return init_;
        return status(coro_rt_channel_close(rt_, &c_));
    }
    std::size_t size() const noexcept { return coro_rt_channel_size(&c_); }
    bool closed() const noexcept { return coro_rt_channel_closed(&c_) != 0; }

    status destroy() noexcept {
        if (destroyed_) return CORO_OK;
        if (!init_.ok()) return init_;
        return status(teardown());
    }

private:
    template <class U>
    status send_impl(U&& u, bool try_only) {
        if (!init_.ok()) return init_;
        node* n = alloc_node(std::forward<U>(u));
        if (!n) return CORO_ENOMEM;
        const coro_status cs = try_only ? coro_rt_channel_trysend(rt_, &c_, n)
                                        : coro_rt_channel_send(rt_, &c_, n);
        if (cs != CORO_OK) {
            delete n;
            return status(cs);
        }
        return CORO_OK;
    }

    status recv_impl(T& out, bool try_only) {
        if (!init_.ok()) return init_;
        void* p = NULL;
        const coro_status cs = try_only ? coro_rt_channel_tryrecv(rt_, &c_, &p)
                                        : coro_rt_channel_recv(rt_, &c_, &p);
        if (cs != CORO_OK) return status(cs);
        node* n = static_cast<node*>(p);
#if COROCC_CPP_HAS_EXCEPTIONS
        try {
            out = std::move(n->v);
        } catch (...) {
            delete n;
            throw;
        }
#else
        out = std::move(n->v);
#endif
        delete n;
        return CORO_OK;
    }

    template <class U>
    static node* alloc_node(U&& u) {
#if COROCC_CPP_HAS_EXCEPTIONS
        try {
            return new (std::nothrow) node(std::forward<U>(u));
        } catch (const std::bad_alloc&) {
            return NULL;
        }
#else
        return new (std::nothrow) node(std::forward<U>(u));
#endif
    }

    coro_status teardown() noexcept {
        (void)coro_rt_channel_close(rt_, &c_);
        void* p = NULL;
        while (coro_rt_channel_tryrecv(rt_, &c_, &p) == CORO_OK) {
            delete static_cast<node*>(p);
        }
        const coro_status cs = coro_rt_channel_destroy(&c_);
        if (cs == CORO_OK) destroyed_ = true;
        return cs;
    }

    coro_rt_t* rt_;
    coro_rt_channel_t c_;
    status init_;
    bool destroyed_;
};

/* ======================================================================= */
/* Free functions                                                          */
/* ======================================================================= */

/* yield -- park the running coroutine and return control to its resumer
 * (the C face; the plain-thread answer is CORO_ESTALE). */
inline status yield() noexcept { return status(coro_yield()); }

/* pump/pump_wait -- serve this thread's queued wakeup requests. */
inline std::size_t pump() noexcept { return coro_pump(); }
inline std::size_t pump_wait() noexcept { return coro_pump_wait(); }

} /* namespace corocc */

#endif /* COROCC_CORO_HPP */
