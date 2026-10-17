/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * corocc/boost/asio_host.hpp -- Boost.Asio calls with blocking shape inside a
 * stackful coroutine driven directly by an io_context (host-loop mode).
 *
 * ---------------------------------------------------------------------------
 * This header is an INTEGRATION EXAMPLE, not part of the corocc library: the
 * library ships only its own core plus its built-in EDSM backend. It lives in
 * examples/backends/asio-host/ to demonstrate how the library is used with an
 * external backend, and how the backend choice is made: no corocc runtime is
 * involved, the coroutine is driven by Boost.Asio itself.
 * ---------------------------------------------------------------------------
 *
 * Runtime mode (corocc/boost/asio.hpp) parks the coroutine in the corocc
 * runtime and wakes it on a runtime worker. Host-loop mode removes the
 * runtime entirely:
 *
 *     spawn(strand, fn)
 *         |
 *         v  (posted to the strand; runs on an io_context thread)
 *     fn(yield_context)                     <- the entry starts on the io thread
 *         |
 *         v  async_read/write: arm the completion, then coro_yield()
 *     ... return to the resumer ...          <- the io thread goes back to run()
 *         |
 *         v  the completion handler runs on the strand (inline resume)
 *     coro_resume()  -> the coroutine continues exactly where it yielded
 *         |
 *         v  the entry returns (or throws)
 *     reclaim: coro_destroy() from the resumer, outside the coroutine's stack
 *
 * The semantics are "manual switch": completion handlers resume the coroutine
 * INLINE on the io thread (the openppp2 YieldContext idea: y.R()); a suspend
 * immediately returns the io thread to its run loop. The io thread is never
 * blocked, and there is no worker thread anywhere.
 *
 * Why inline resume is safe -- the strand inheritance rule:
 *
 *   - spawn posts the start to the strand it was given; if it was given a
 *     plain executor, spawn creates and owns a private strand for the
 *     coroutine. Either way the coroutine's whole life runs inside strand
 *     handlers.
 *   - Every operation binds its completion to that same strand -- on both
 *     executor associations a completion can use: the ordinary associated
 *     executor AND the immediate executor, the association that governs a
 *     completion delivered inside its initiating call (see the
 *     bound_completion comment below for how the two associations differ).
 *     Completions therefore always inherit the coroutine's strand.
 *   - Asio strands never run two handlers concurrently. While the coroutine
 *     is running (inside a strand handler) no completion bound to the same
 *     strand can run; when it yields, the handler returns, the strand frees,
 *     and the queued completion runs -- the only two orderings a completion
 *     can see are "after the coroutine parked" (resume inline) or "inline
 *     during the initiating call" (record the result, do not resume; the
 *     coroutine observes it before yielding). Both are handled below.
 *   - Exactly one operation may be outstanding per coroutine (contract), so
 *     at most one completion owns the right to resume at any time.
 *
 * Why every asio call made from coroutine context goes through a call fence:
 *
 *   A stackful coroutine's frames migrate between io threads, while the code
 *   compiled into those frames was compiled under the ordinary assumption
 *   that an execution stays on one thread. Boost.Asio keeps per-thread state
 *   on the running thread's stack (the scheduler's thread_info allocation
 *   cache, reached through thread-local bookkeeping) and, with clang >= -O1,
 *   asio code inlined into a coroutine frame can leave values derived from
 *   that state in registers or stack slots across coro_yield(). After the
 *   coroutine migrates, those values name the previous thread's stack-resident
 *   state, which that thread is concurrently using without synchronization;
 *   the result is heap corruption (reproduced: heap-use-after-free in strand
 *   invoker scheduling on a 4-thread io_context with a stream that posts its
 *   completion from inside the coroutine).
 *
 *   This header therefore performs every asio call reachable from coroutine
 *   context through call_fenced(): an indirect call through a volatile
 *   function pointer, i.e. an opaque boundary the optimizer cannot inline
 *   through. The asio machinery runs, allocates and returns entirely inside
 *   that call frame, which begins and ends between two suspensions, so no
 *   thread-derived value can survive into the coroutine's own frames. The
 *   completion handlers and the asio-internal machinery they trigger run on
 *   the io threads themselves and never migrate, so they need no fence.
 *
 *   The integration additionally associates a plain heap allocator
 *   (asio_host_allocator) with all of its handlers: Boost.Asio would
 *   otherwise silently replace std::allocator with its per-thread
 *   recycling_allocator. That is defense in depth on top of the structural
 *   fence, and it keeps the ownership story of this integration's allocations
 *   explicit.
 *
 * Lifetime: spawn is detached by design. There is no runtime to park a joiner
 * in; a joinable handle would need a cross-thread completion channel -- i.e.
 * exactly the runtime host-loop mode removes. The io_context itself is the
 * completion domain: run() returns once the last coroutine, completion and
 * peer handler have drained. Pre-start failures are still reported: spawn
 * returns a coro_status (creation/posting), and all I/O outcomes travel
 * through ec. The coroutine state is reclaimed by whichever handler observes
 * its completion, always OUTSIDE the coroutine's own stack (the prohibited
 * "destroy the stack you run on" case cannot happen).
 *
 * Contracts (violating them is a programming error, never std::terminate):
 *   - spawn accepts any Asio executor or strand; pass a per-connection strand
 *     when several coroutines share an io_context, so each coroutine gets its
 *     own serialization domain.
 *   - The io_context behind the executor must be running on at least one
 *     thread, or nothing ever starts or completes (standard Asio contract).
 *   - One operation per coroutine at a time. A second call while one is in
 *     flight is rejected with an error through ec
 *     (device_or_resource_busy); it never touches the pending operation. The
 *     guard is a plain flag: it covers the normal strand-serialized pattern
 *     (a second call issued from the same strand/thread). Calling an
 *     operation for the same coroutine from another thread is a data race
 *     and unsupported.
 *   - Do not call Boost.Asio directly from a coroutine body (e.g. asio::post):
 *     asio machinery belongs in a frame that cannot span a suspension -- see
 *     the fence note above. Perform I/O through the operations below; they
 *     are the fenced entry points. This is the same rule the corocc core
 *     documents for thread_local state in general.
 *   - stream/buffers must stay alive until the call returns, exactly as for
 *     boost::asio::async_read/write.
 *   - An exception escaping the coroutine body ends the coroutine: it is
 *     caught at the coroutine boundary and dropped (there is no runtime to
 *     propagate it through); the stack is still reclaimed correctly. Use the
 *     ec channel for reporting.
 *   - Like the runtime integration, a cancellation requested through
 *     coro_cancel while parked does not cut a wait short (the pending handler
 *     must never outlive the frame): end the I/O (close/cancel the stream) to
 *     complete the wait; an observed cancellation is then reported as
 *     operation_aborted with zero bytes.
 *   - Lifecycle: destroy the io_context only after run() has drained (the
 *     standard Asio contract). A live coroutine keeps the io_context busy; a
 *     parked coroutine whose I/O never completes keeps it parked (end the
 *     I/O to finish it). As a guard, the integration registers a service
 *     with the owning execution_context: should the io_context still be
 *     destroyed while coroutines are pending or parked, their stacks and
 *     state are reclaimed during service shutdown instead of leaking.
 *
 * The await_* family is the bridge for plain OS threads: same ec-first shape,
 * but the calling thread blocks on a condition variable instead of parking a
 * coroutine. Never call it from a thread that is running the stream's
 * io_context (it would block the only thread able to complete the operation).
 *
 * The header only needs the Boost.Asio headers plus <corocc/coro.h>, and a
 * compiler in C++11 mode or later. It coexists with corocc/boost/asio.hpp in
 * one binary: the namespaces are disjoint and nothing here references the
 * runtime. Nothing is installed or linked by the corocc library itself.
 */
#ifndef COROCC_BOOST_ASIO_HOST_HPP
#define COROCC_BOOST_ASIO_HOST_HPP

#if !defined(__cplusplus)
#error "corocc/boost/asio_host.hpp is a C++ header; compile as C++11 or later"
#endif

#if !defined(_MSC_VER) && __cplusplus < 201103L
#error "corocc/boost/asio_host.hpp requires C++11 or later"
#endif

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <new>
#include <set>
#include <type_traits>

#include <boost/version.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/execution_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/errc.hpp>

#if BOOST_VERSION >= 107400
/* The execution-context query (used by the io_context lifecycle guard) and
 * the execution::context property exist since Boost 1.74. */
#  include <boost/asio/execution/context.hpp>
#  include <boost/asio/query.hpp>
#  define COROCC_BOOST_ASIO_HOST_HAS_CONTEXT_QUERY 1
#else
#  define COROCC_BOOST_ASIO_HOST_HAS_CONTEXT_QUERY 0
#endif

#include <corocc/coro.h>

#if !defined(COROCC_BOOST_ASIO_HOST_EXCEPTIONS)
#  if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#    define COROCC_BOOST_ASIO_HOST_EXCEPTIONS 1
#  else
#    define COROCC_BOOST_ASIO_HOST_EXCEPTIONS 0
#  endif
#endif

/* noinline on the call trampolines below (arm_* and post_start): pure
 * defense in depth. The volatile call fence already denies the optimizer the
 * callee identity, so their bodies cannot be inlined into coroutine frames
 * (the migration fix); the attribute pins the boundary even if a future
 * compiler learns to see through the indirect call. */
#if !defined(COROCC_BOOST_ASIO_HOST_NOINLINE)
#  if defined(_MSC_VER)
#    define COROCC_BOOST_ASIO_HOST_NOINLINE __declspec(noinline)
#  elif defined(__GNUC__)
#    define COROCC_BOOST_ASIO_HOST_NOINLINE __attribute__((noinline))
#  else
#    define COROCC_BOOST_ASIO_HOST_NOINLINE
#  endif
#endif

namespace corocc {
namespace asio_host {

namespace detail {

/* Normalize the spawn executor: a strand is used as-is; anything else is
 * wrapped in a private strand so the serialization rule holds by
 * construction. */
template <class Executor>
struct strand_type {
    typedef boost::asio::strand<Executor> type;
};

template <class Inner>
struct strand_type<boost::asio::strand<Inner> > {
    typedef boost::asio::strand<Inner> type;
};

/* asio_host_allocator -- stateless heap allocator with a distinct type.
 *
 * Boost.Asio replaces std::allocator with its internal per-thread
 * recycling_allocator (get_recycling_allocator) whose cache lives in the
 * stack-resident scheduler thread_info of whichever thread happens to run the
 * code. The fenced calls below already keep that cache out of suspension-
 * spanning coroutine frames; associating this plain heap allocator with all
 * handlers of this integration additionally keeps allocations made on its
 * behalf off that cache entirely (defense in depth), and makes the
 * integration's allocation ownership explicit. */
template <class T>
class asio_host_allocator {
public:
    typedef T value_type;
    template <class U>
    struct rebind { typedef asio_host_allocator<U> other; };

    asio_host_allocator() noexcept {}
    template <class U>
    asio_host_allocator(const asio_host_allocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }
    void deallocate(T* p, std::size_t) noexcept {
        ::operator delete(p);
    }
};

template <>
class asio_host_allocator<void> {
public:
    typedef void value_type;
    template <class U>
    struct rebind { typedef asio_host_allocator<U> other; };
};

/* Equality: the allocator is stateless, so every instance of every
 * specialization allocates interchangeable memory and therefore compares
 * equal. Boost.Asio does not need these on the paths used here, but they
 * complete the minimal allocator interface. */
template <class T, class U>
inline bool operator==(const asio_host_allocator<T>&,
                       const asio_host_allocator<U>&) noexcept {
    return true;
}

template <class T, class U>
inline bool operator!=(const asio_host_allocator<T>&,
                       const asio_host_allocator<U>&) noexcept {
    return false;
}

/* call_fenced -- the structural boundary for asio calls made from coroutine
 * context (see the header comment). The call goes through a volatile
 * function pointer: the optimizer must load the pointer and cannot know the
 * target, so the callee is a real call frame that cannot be inlined through
 * (including under LTO, because volatile accesses may not be elided or
 * constant-folded). */
typedef void (*fenced_fn)(void*);

inline void call_fenced(fenced_fn fn, void* ctx) {
    fenced_fn volatile fp = fn;
    fp(ctx);
}

/* coro_state_base -- the type-erased part of a coroutine's control block.
 * The io_context lifecycle guard tracks these; the strand-typed part lives in
 * coro_state below. */
struct coro_state_base {
    coro_t*    coro;     /* handle, or 0 before coro_create succeeded */
    coro_token token;    /* live credential of the current park point */
    bool       in_flight;/* one operation at a time (busy guard) */
    void     (*destroy)(coro_state_base*);
    void*      registry; /* spawn_registry* while tracked, else 0 */

    coro_state_base()
        : coro(0), token(CORO_TOKEN_NONE), in_flight(false), destroy(0),
          registry(0) {}
};

/* coro_state -- the per-coroutine control block.
 *
 * Lives on the heap for the whole life of the coroutine and is owned by
 * "whoever observes its completion": the start handler, or a completion
 * handler. Those handlers are the only code that ever drives the coroutine,
 * so raw pointers are sufficient -- no reference counting exists because no
 * handler can outlive completion (one operation in flight, and the completion
 * that resumes the coroutine is always the last one armed).
 *
 * destroy() is installed by spawn (it knows the full state type) and runs
 * OUTSIDE the coroutine's stack: destroying a handle from inside its own
 * stack would free the memory under the running frame and is refused by
 * coro_destroy (CORO_EBUSY) by design.
 */
template <class Strand>
struct coro_state : coro_state_base {
    Strand strand; /* the coroutine's serialization domain */

    coro_state(coro_t* c, const Strand& s) : coro_state_base(), strand(s) {
        coro = c;
    }
};

/* spawn_registry -- the io_context lifecycle guard.
 *
 * Registered lazily with the execution_context behind the spawn executor (when
 * that context can be queried; Boost >= 1.74). Security net for the detached
 * design: if the io_context is destroyed while coroutines are still pending
 * (start never ran) or parked (I/O never completed), their stacks and states
 * are reclaimed during service shutdown instead of leaking. The contract
 * stays "drain run() before destroying the io_context"; this only guarantees
 * that a violation leaks nothing. Services shut down in reverse registration
 * order, so this runs before the scheduler tears its queues down; the queued
 * handlers that still reference a state are destroyed, never invoked, so
 * reclaiming first is safe. A coroutine that is actually running cannot be
 * reclaimed (coro_destroy refuses) and is left alone -- leak over corruption.
 */
class spawn_registry
    : public boost::asio::detail::execution_context_service_base<
          spawn_registry> {
public:
    explicit spawn_registry(boost::asio::execution_context& ctx)
        : boost::asio::detail::execution_context_service_base<spawn_registry>(
              ctx) {}

    void track(coro_state_base* st) {
        std::lock_guard<std::mutex> lock(mutex_);
        live_.insert(st);
    }

    void untrack(coro_state_base* st) {
        std::lock_guard<std::mutex> lock(mutex_);
        live_.erase(st);
    }

    void shutdown() {
        std::set<coro_state_base*> live;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            live.swap(live_);
        }
        for (std::set<coro_state_base*>::iterator it = live.begin();
             it != live.end(); ++it) {
            coro_state_base* st = *it;
            st->registry = 0; /* already untracked */
            if (st->destroy) {
                st->destroy(st);
            }
        }
    }

private:
    std::mutex mutex_;
    std::set<coro_state_base*> live_;
};

inline void untrack_state(coro_state_base* st) noexcept {
    spawn_registry* reg = static_cast<spawn_registry*>(st->registry);
    if (reg) {
        st->registry = 0;
        reg->untrack(st);
    }
}

#if COROCC_BOOST_ASIO_HOST_HAS_CONTEXT_QUERY
template <class Strand>
inline typename std::enable_if<
    boost::asio::can_query<const Strand&,
                           boost::asio::execution::context_t>::value>::type
track_state(const Strand& strand, coro_state_base* st) {
    boost::asio::execution_context& ctx =
        boost::asio::query(strand, boost::asio::execution::context);
    spawn_registry& reg = boost::asio::use_service<spawn_registry>(ctx);
    reg.track(st);
    st->registry = &reg;
}

template <class Strand>
inline typename std::enable_if<
    !boost::asio::can_query<const Strand&,
                            boost::asio::execution::context_t>::value>::type
track_state(const Strand&, coro_state_base*) {}
#else
template <class Strand>
inline void track_state(const Strand&, coro_state_base*) {}
#endif

} /* namespace detail */

template <class Executor>
class yield_context;

namespace detail {

struct access;
template <class Strand, class Fn>
struct spawn_state;

} /* namespace detail */

/**
 * yield_context -- the coroutine-side handle passed to the entry function.
 *
 * It carries the three things an operation needs at a suspension point: the
 * raw coro_t* handle, the live resume credential, and the strand every
 * completion must run on. Operations take it as their first parameter;
 * native() exposes the raw handle for observability (thread id, cancellation
 * through coro_cancel, ...).
 */
template <class Executor>
class yield_context {
public:
    typedef Executor executor_type;

    /** The raw handle (CORO-level). Never NULL for a live context. */
    coro_t* native() const noexcept { return state_ ? state_->coro : 0; }

    /** The strand this coroutine's completions are serialized on. */
    const executor_type& executor() const noexcept { return state_->strand; }

private:
    friend struct detail::access;
    template <class Strand, class Fn>
    friend struct detail::spawn_state;

    explicit yield_context(detail::coro_state<Executor>* st) noexcept
        : state_(st) {}

    detail::coro_state<Executor>* state_;
};

namespace detail {

struct access {
    template <class E>
    static coro_state<E>* state(yield_context<E>& y) noexcept {
        return y.state_;
    }
};

/* Inline resume from a completion handler (the y.R() semantics).
 *
 * The handler runs on the coroutine's strand, after the coroutine parked, so
 * the stored token is the live credential. If the resume drives the entry to
 * completion, the coroutine stack and this state are reclaimed here -- the
 * handler's own frame, not the coroutine's, is the execution stack at this
 * point.
 *
 * A failed claim is impossible under the strand discipline and has no report
 * path; the coroutine would simply stay parked.
 */
template <class Strand>
inline void resume_inline(coro_state<Strand>* st) noexcept {
    coro_token next = CORO_TOKEN_NONE;
    const coro_status s = coro_resume(st->coro, st->token, &next);
    if (s != CORO_OK) {
        return;
    }
    if (next == CORO_TOKEN_DEAD) {
        st->destroy(st);
    } else {
        st->token = next;
    }
}

/* First drive of the coroutine; runs as the posted start handler. */
template <class Strand>
inline void start_inline(coro_state<Strand>* st) noexcept {
    coro_token next = CORO_TOKEN_NONE;
    const coro_status s = coro_start(st->coro, &next);
    if (s == CORO_OK && next != CORO_TOKEN_DEAD) {
        st->token = next;
        return;
    }
    /* Finished without suspending, or (defensively) could not start. */
    st->destroy(st);
}

template <class Strand, class Fn>
struct spawn_state;
}

template <class Executor, class Fn>
coro_status spawn(const Executor& ex, Fn fn);

namespace detail {

/* The full state: the entry function object plus the yield_context the entry
 * receives. The base pointer is what handlers and operations see; the
 * trampoline (which is instantiated with the full type) goes back to the
 * derived object. */
template <class Strand, class Fn>
struct spawn_state : coro_state<Strand> {
    Fn                   fn;
    yield_context<Strand> yield;

    spawn_state(const Strand& s, const Fn& f)
        : coro_state<Strand>(0, s), fn(f), yield(this) {}
};

template <class Strand, class Fn>
inline void destroy_spawn_state(coro_state_base* core) noexcept {
    spawn_state<Strand, Fn>* st = static_cast<spawn_state<Strand, Fn>*>(core);
    untrack_state(core);
    coro_t* c = st->coro;
    if (c) {
        const coro_status s = coro_destroy(c);
        if (s != CORO_OK) {
            /* Only reachable when the lifecycle contract is violated (the
             * io_context is destroyed while the coroutine is running):
             * leaving the state alive is the safe outcome -- leak over
             * corruption. */
            return;
        }
    }
    delete st;
}

/* Coroutine entry. Any exception escaping fn is caught here so it can never
 * unwind through the context switch (which would be undefined behavior); the
 * coroutine then ends normally from the driver's perspective. */
template <class Strand, class Fn>
void trampoline_entry(void* arg) {
    spawn_state<Strand, Fn>* st = static_cast<spawn_state<Strand, Fn>*>(arg);
#if COROCC_BOOST_ASIO_HOST_EXCEPTIONS
    try {
        st->fn(st->yield);
    } catch (...) {
        /* Ends the coroutine; nothing to propagate to. */
    }
#else
    st->fn(st->yield);
#endif
}

/* The posted start handler. */
template <class Strand>
struct start_handler {
    typedef asio_host_allocator<void> allocator_type;

    coro_state<Strand>* st;
    explicit start_handler(coro_state<Strand>* s) : st(s) {}

    allocator_type get_allocator() const noexcept { return allocator_type(); }
    void operator()() const { start_inline(st); }
};

/* One-shot outcome cell for a suspended operation. Lives on the coroutine's
 * stack: while parked, that frame is stable and the completion handler (the
 * only other user) writes into it before resuming the coroutine on the same
 * thread.
 *
 * completed: the handler has stored the outcome.
 * waiting:   the coroutine is parked in the wait loop. The handler resumes
 *            only when this is set; otherwise the completion ran inline
 *            during initiation and the loop observes completed directly.
 */
struct op_latch {
    boost::system::error_code ec;
    std::size_t              bytes;
    std::atomic<bool>        completed;
    std::atomic<bool>        waiting;

    op_latch() : ec(), bytes(0), completed(false), waiting(false) {}
};

/* Completion handler: record the outcome, then resume inline if -- and only
 * if -- the coroutine is parked. Bound to the coroutine's strand by every
 * operation, so it can never run concurrently with the coroutine's frame. */
template <class Strand>
struct completion {
    typedef asio_host_allocator<void> allocator_type;

    coro_state<Strand>* st;
    op_latch*           latch;

    completion(coro_state<Strand>* s, op_latch* l) : st(s), latch(l) {}

    allocator_type get_allocator() const noexcept { return allocator_type(); }

    void operator()(const boost::system::error_code& e, std::size_t n) const
        noexcept {
        latch->ec = e;
        latch->bytes = n;
        latch->completed.store(true, std::memory_order_release);
        if (latch->waiting.load(std::memory_order_acquire)) {
            resume_inline(st);
        }
    }
};

/* bound_completion -- the handler wrapper that carries the coroutine's strand
 * on BOTH executor associations a completion can be delivered through:
 *
 *   - executor_type:           the ordinary completion path (reactor ->
 *                              associated executor).
 *   - immediate_executor_type: the immediate-completion path, used when an
 *                              operation completes inside its initiating
 *                              call. Associating the strand here runs such a
 *                              completion inline, while the coroutine's own
 *                              frame still holds the strand: the result is
 *                              recorded and the coroutine observes it
 *                              without ever suspending.
 *
 * On Boost 1.83 (the tested baseline) such a completion is serialized under
 * the strand even without this association: with only the ordinary
 * association -- e.g. plain bind_executor(strand, h) -- an immediate
 * completion is still delivered in-strand, but as a posted operation, not
 * inline (the reactor's call_post_immediate_completion hook posts it, and
 * the handler_work layer delivers it through the handler's associated
 * executor, i.e. the strand). Only a bare handler, whose associated
 * executor falls back to the raw I/O executor, would run an immediate
 * completion outside the strand. Declaring the immediate association
 * explicitly is what keeps the inline path -- and not just the
 * serialization -- under the strand's control, independently of a
 * particular Asio version's fallback behavior.
 */
template <class Handler, class Executor>
class bound_completion {
public:
    typedef Executor executor_type;
    typedef Executor immediate_executor_type;
    typedef typename Handler::allocator_type allocator_type;

    bound_completion(const Executor& ex, const Handler& h)
        : executor_(ex), handler_(h) {}

    executor_type get_executor() const noexcept { return executor_; }
    immediate_executor_type get_immediate_executor() const noexcept {
        return executor_;
    }
    allocator_type get_allocator() const noexcept {
        return handler_.get_allocator();
    }

    void operator()(const boost::system::error_code& e, std::size_t n) {
        handler_(e, n);
    }

private:
    Executor executor_;
    Handler  handler_;
};

template <class Strand>
inline bound_completion<completion<Strand>, Strand>
bind_completion(const Strand& strand, coro_state<Strand>* st, op_latch* latch) {
    return bound_completion<completion<Strand>, Strand>(
        strand, completion<Strand>(st, latch));
}

/* Armed-call contexts and trampolines.
 *
 * Every asio call made from coroutine context runs inside one of these
 * trampolines, reached only through call_fenced(). The context is plain,
 * trivially-destructible data (pointers only) living in the coroutine frame;
 * the asio machinery (the initiation, any immediately-delivered completion,
 * and all allocations either performs) runs entirely inside the trampoline
 * frame, which begins and returns before the operation suspends. The strand
 * is reached through the (heap-resident) state, never copied into the
 * context: an abandoned coroutine's stack is released without unwinding, so
 * non-trivial locals there would leak. The handler temporary built in the
 * trampoline is destroyed before the trampoline returns; the op object asio
 * keeps holds the strand copy asio itself releases. */
template <class Stream, class Buffers, class Executor>
struct arm_read_some_ctx {
    Stream*               stream;
    const Buffers*        buffers;
    coro_state<Executor>* st;
    op_latch*             latch;

    arm_read_some_ctx(Stream* s, const Buffers* b, coro_state<Executor>* c,
                      op_latch* l)
        : stream(s), buffers(b), st(c), latch(l) {}
};

template <class Stream, class Buffers, class Executor>
COROCC_BOOST_ASIO_HOST_NOINLINE void arm_read_some(void* p) {
    arm_read_some_ctx<Stream, Buffers, Executor>* c =
        static_cast<arm_read_some_ctx<Stream, Buffers, Executor>*>(p);
    c->stream->async_read_some(
        *c->buffers, bind_completion(c->st->strand, c->st, c->latch));
}

template <class Stream, class Buffers, class Executor>
struct arm_write_some_ctx {
    Stream*               stream;
    const Buffers*        buffers;
    coro_state<Executor>* st;
    op_latch*             latch;

    arm_write_some_ctx(Stream* s, const Buffers* b, coro_state<Executor>* c,
                       op_latch* l)
        : stream(s), buffers(b), st(c), latch(l) {}
};

template <class Stream, class Buffers, class Executor>
COROCC_BOOST_ASIO_HOST_NOINLINE void arm_write_some(void* p) {
    arm_write_some_ctx<Stream, Buffers, Executor>* c =
        static_cast<arm_write_some_ctx<Stream, Buffers, Executor>*>(p);
    c->stream->async_write_some(
        *c->buffers, bind_completion(c->st->strand, c->st, c->latch));
}

template <class Stream, class Buffers, class Executor>
struct arm_read_ctx {
    Stream*               stream;
    const Buffers*        buffers;
    coro_state<Executor>* st;
    op_latch*             latch;

    arm_read_ctx(Stream* s, const Buffers* b, coro_state<Executor>* c,
                 op_latch* l)
        : stream(s), buffers(b), st(c), latch(l) {}
};

template <class Stream, class Buffers, class Executor>
COROCC_BOOST_ASIO_HOST_NOINLINE void arm_read(void* p) {
    arm_read_ctx<Stream, Buffers, Executor>* c =
        static_cast<arm_read_ctx<Stream, Buffers, Executor>*>(p);
    ::boost::asio::async_read(
        *c->stream, *c->buffers,
        bind_completion(c->st->strand, c->st, c->latch));
}

template <class Stream, class Buffers, class Executor>
struct arm_write_ctx {
    Stream*               stream;
    const Buffers*        buffers;
    coro_state<Executor>* st;
    op_latch*             latch;

    arm_write_ctx(Stream* s, const Buffers* b, coro_state<Executor>* c,
                  op_latch* l)
        : stream(s), buffers(b), st(c), latch(l) {}
};

template <class Stream, class Buffers, class Executor>
COROCC_BOOST_ASIO_HOST_NOINLINE void arm_write(void* p) {
    arm_write_ctx<Stream, Buffers, Executor>* c =
        static_cast<arm_write_ctx<Stream, Buffers, Executor>*>(p);
    ::boost::asio::async_write(
        *c->stream, *c->buffers,
        bind_completion(c->st->strand, c->st, c->latch));
}

/* The posted start, executed through the fence as well: spawn may be called
 * from inside a coroutine. Pointer-only context, same reasoning as above. */
template <class Strand>
struct post_start_ctx {
    coro_state<Strand>* st;

    explicit post_start_ctx(coro_state<Strand>* c) : st(c) {}
};

template <class Strand>
COROCC_BOOST_ASIO_HOST_NOINLINE void post_start(void* p) {
    post_start_ctx<Strand>* c = static_cast<post_start_ctx<Strand>*>(p);
    ::boost::asio::post(c->st->strand, start_handler<Strand>(c->st));
}

/* The suspension loop shared by all four operations.
 *
 * The handshake with the completion handler:
 *   - completion already recorded (it ran inline during initiation): return
 *     its outcome without ever yielding;
 *   - otherwise mark "waiting" and yield. The handler sets completed and
 *     resumes; a cancellation observed at the resume point keeps the loop
 *     waiting for the completion (the pending handler owns the frame), and
 *     the outcome is reported as operation_aborted afterwards.
 */
template <class Strand>
inline std::size_t op_wait(coro_state<Strand>* st, op_latch& latch,
                           boost::system::error_code& ec) {
    bool cancelled = false;
    for (;;) {
        if (latch.completed.load(std::memory_order_acquire)) {
            break;
        }
        latch.waiting.store(true, std::memory_order_release);
        const coro_status s = coro_yield();
        latch.waiting.store(false, std::memory_order_release);
        if (s == CORO_ECANCELED) {
            cancelled = true;
        } else if (s != CORO_OK) {
            /* Impossible under the contract (a lost credential); report
             * instead of silently parking forever. */
            st->in_flight = false;
            ec = boost::system::errc::make_error_code(
                boost::system::errc::state_not_recoverable);
            return 0;
        }
        if (latch.completed.load(std::memory_order_acquire)) {
            break;
        }
    }
    st->in_flight = false;
    if (cancelled) {
        ec = boost::asio::error::operation_aborted;
        return 0;
    }
    ec = latch.ec;
    return latch.bytes;
}

/* Busy guard: one operation per coroutine. */
template <class Strand>
inline bool enter_op(coro_state<Strand>* st, boost::system::error_code& ec) {
    if (st->in_flight) {
        ec = boost::system::errc::make_error_code(
            boost::system::errc::device_or_resource_busy);
        return false;
    }
    st->in_flight = true;
    return true;
}

} /* namespace detail */

/**
 * spawn -- start a stackful coroutine on @p ex (executor or strand).
 *
 * The start is posted to the (normalized) strand, so the entry function runs
 * on an io_context thread; the coroutine is detached: it is reclaimed
 * automatically when it returns. Pre-start validation failures (allocation,
 * posting) are reported through the return value; I/O outcomes travel through
 * the operations' ec.
 *
 * @param ex  The serialization domain: pass a strand for per-connection
 *            serialization, or any executor (spawn then owns a private
 *            strand). The io_context behind it must be running.
 * @param fn  Callable as fn(yield_context<strand_type>&); copied into the
 *            coroutine state. An exception escaping fn ends the coroutine.
 * @return CORO_OK once the start was posted; CORO_ENOMEM if the state or the
 *         post could not be allocated; the coro_create status (CORO_EINVAL/
 *         CORO_ENOMEM) if the coroutine itself could not be created.
 */
template <class Executor, class Fn>
coro_status spawn(const Executor& ex, Fn fn) {
    typedef typename detail::strand_type<Executor>::type strand_t;
    typedef detail::spawn_state<strand_t, Fn> state_t;

    /* Copy-construct: a strand is passed through, a plain executor is wrapped
     * in a private strand (same call shape for both). */
    strand_t strand(ex);

    state_t* st = 0;
#if COROCC_BOOST_ASIO_HOST_EXCEPTIONS
    try {
        st = new state_t(strand, fn);
    } catch (...) {
        return CORO_ENOMEM;
    }
#else
    st = new state_t(strand, fn);
#endif

    const coro_status cs = coro_create(
        &st->coro, 0, &detail::trampoline_entry<strand_t, Fn>, st);
    if (cs != CORO_OK) {
        delete st;
        return cs;
    }
    st->destroy = &detail::destroy_spawn_state<strand_t, Fn>;

    detail::post_start_ctx<strand_t> post_ctx(st);
#if COROCC_BOOST_ASIO_HOST_EXCEPTIONS
    try {
        detail::track_state(strand, st);
        detail::call_fenced(&detail::post_start<strand_t>,
                            static_cast<void*>(&post_ctx));
    } catch (...) {
        detail::untrack_state(st);
        (void)coro_destroy(st->coro);
        st->coro = 0;
        delete st;
        return CORO_ENOMEM;
    }
#else
    detail::track_state(strand, st);
    detail::call_fenced(&detail::post_start<strand_t>,
                        static_cast<void*>(&post_ctx));
#endif
    return CORO_OK;
}

/**
 * async_read -- read until the buffer sequence is full; suspends the calling
 * coroutine. Completion condition identical to boost::asio::async_read.
 *
 * @param y       The coroutine's yield context (first parameter of the entry).
 * @param stream  AsyncReadStream (socket, ssl stream, any Asio stream).
 * @param buffers MutableBufferSequence; valid until return.
 * @param ec      Asio result; operation_aborted when a cancellation was
 *                observed; device_or_resource_busy on the one-op violation.
 * @return Bytes read. On success, the full buffer size. When @p ec reports an
 *         error, the count delivered before the failure (0 for this composed
 *         operation); after an observed cancellation, 0.
 */
template <class Executor, class AsyncReadStream, class MutableBufferSequence>
std::size_t async_read(yield_context<Executor>& y, AsyncReadStream& stream,
                       const MutableBufferSequence& buffers,
                       boost::system::error_code& ec) {
    detail::coro_state<Executor>* st = detail::access::state(y);
    if (!detail::enter_op(st, ec)) {
        return 0;
    }
    detail::op_latch latch;
    detail::arm_read_ctx<AsyncReadStream, MutableBufferSequence, Executor> ctx(
        &stream, &buffers, st, &latch);
    detail::call_fenced(
        &detail::arm_read<AsyncReadStream, MutableBufferSequence, Executor>,
        static_cast<void*>(&ctx));
    return detail::op_wait(st, latch, ec);
}

/**
 * async_read_some -- one stream read; suspends the calling coroutine.
 * Completion condition identical to boost::asio::async_read_some.
 *
 * @return Bytes read (> 0 on success); 0 after a cancellation; on error, the
 *         count delivered before the failure (normally 0 -- this is Asio's
 *         own contract, so the value is passed through unchanged).
 * @see async_read for the parameter and contract description.
 */
template <class Executor, class AsyncReadStream, class MutableBufferSequence>
std::size_t async_read_some(yield_context<Executor>& y,
                            AsyncReadStream& stream,
                            const MutableBufferSequence& buffers,
                            boost::system::error_code& ec) {
    detail::coro_state<Executor>* st = detail::access::state(y);
    if (!detail::enter_op(st, ec)) {
        return 0;
    }
    detail::op_latch latch;
    detail::arm_read_some_ctx<AsyncReadStream, MutableBufferSequence, Executor>
        ctx(&stream, &buffers, st, &latch);
    detail::call_fenced(
        &detail::arm_read_some<AsyncReadStream, MutableBufferSequence, Executor>,
        static_cast<void*>(&ctx));
    return detail::op_wait(st, latch, ec);
}

/**
 * async_write -- write the whole buffer sequence; suspends the calling
 * coroutine. Completion condition identical to boost::asio::async_write.
 *
 * @return Bytes written. On success, the full buffer size. When @p ec reports
 *         an error, the count delivered before the failure (0 for this
 *         composed operation); after an observed cancellation, 0.
 * @see async_read for the parameter and contract description.
 */
template <class Executor, class AsyncWriteStream, class ConstBufferSequence>
std::size_t async_write(yield_context<Executor>& y, AsyncWriteStream& stream,
                        const ConstBufferSequence& buffers,
                        boost::system::error_code& ec) {
    detail::coro_state<Executor>* st = detail::access::state(y);
    if (!detail::enter_op(st, ec)) {
        return 0;
    }
    detail::op_latch latch;
    detail::arm_write_ctx<AsyncWriteStream, ConstBufferSequence, Executor> ctx(
        &stream, &buffers, st, &latch);
    detail::call_fenced(
        &detail::arm_write<AsyncWriteStream, ConstBufferSequence, Executor>,
        static_cast<void*>(&ctx));
    return detail::op_wait(st, latch, ec);
}

/**
 * async_write_some -- one stream write; suspends the calling coroutine.
 * Completion condition identical to boost::asio::async_write_some.
 *
 * @return Bytes written (> 0 on success); 0 after a cancellation; on error,
 *         the count delivered before the failure (normally 0).
 * @see async_read for the parameter and contract description.
 */
template <class Executor, class AsyncWriteStream, class ConstBufferSequence>
std::size_t async_write_some(yield_context<Executor>& y,
                             AsyncWriteStream& stream,
                             const ConstBufferSequence& buffers,
                             boost::system::error_code& ec) {
    detail::coro_state<Executor>* st = detail::access::state(y);
    if (!detail::enter_op(st, ec)) {
        return 0;
    }
    detail::op_latch latch;
    detail::arm_write_some_ctx<AsyncWriteStream, ConstBufferSequence, Executor>
        ctx(&stream, &buffers, st, &latch);
    detail::call_fenced(
        &detail::arm_write_some<AsyncWriteStream, ConstBufferSequence, Executor>,
        static_cast<void*>(&ctx));
    return detail::op_wait(st, latch, ec);
}

namespace detail {

struct blocking_latch {
    boost::system::error_code ec;
    std::size_t              bytes;
    bool                     done;
    std::mutex               mutex;
    std::condition_variable  ready;

    blocking_latch() : ec(), bytes(0), done(false) {}
};

struct blocking_waiter {
    blocking_latch* latch;
    explicit blocking_waiter(blocking_latch* l) : latch(l) {}

    void operator()(const boost::system::error_code& e, std::size_t n) const {
        std::lock_guard<std::mutex> lock(latch->mutex);
        latch->ec = e;
        latch->bytes = n;
        latch->done = true;
        latch->ready.notify_one();
    }
};

inline std::size_t blocking_wait(blocking_latch& latch,
                                 boost::system::error_code& ec) {
    std::unique_lock<std::mutex> lock(latch.mutex);
    while (!latch.done) {
        latch.ready.wait(lock);
    }
    ec = latch.ec;
    return latch.bytes;
}

} /* namespace detail */

/**
 * await_read -- the awaitable bridge for plain OS threads: initiate the
 * operation, block the calling thread on a condition variable until it
 * completes, report the outcome exactly like async_read.
 *
 * NEVER call from a thread that runs the stream's io_context: it would block
 * the only thread able to complete the operation. Call it from a plain OS
 * thread while the io_context runs elsewhere.
 *
 * @see async_read for the completion condition, parameters and error channel.
 */
template <class AsyncReadStream, class MutableBufferSequence>
std::size_t await_read(AsyncReadStream& stream,
                       const MutableBufferSequence& buffers,
                       boost::system::error_code& ec) {
    detail::blocking_latch latch;
    ::boost::asio::async_read(stream, buffers, detail::blocking_waiter(&latch));
    return detail::blocking_wait(latch, ec);
}

/**
 * await_read_some -- one read on an OS thread; see await_read for the
 * blocking contract and async_read_some for the completion condition.
 */
template <class AsyncReadStream, class MutableBufferSequence>
std::size_t await_read_some(AsyncReadStream& stream,
                            const MutableBufferSequence& buffers,
                            boost::system::error_code& ec) {
    detail::blocking_latch latch;
    stream.async_read_some(buffers, detail::blocking_waiter(&latch));
    return detail::blocking_wait(latch, ec);
}

/**
 * await_write -- write the whole buffer sequence from an OS thread; see
 * await_read for the blocking contract and async_write for the completion
 * condition.
 */
template <class AsyncWriteStream, class ConstBufferSequence>
std::size_t await_write(AsyncWriteStream& stream,
                        const ConstBufferSequence& buffers,
                        boost::system::error_code& ec) {
    detail::blocking_latch latch;
    ::boost::asio::async_write(stream, buffers, detail::blocking_waiter(&latch));
    return detail::blocking_wait(latch, ec);
}

/**
 * await_write_some -- one write on an OS thread; see await_read for the
 * blocking contract and async_write_some for the completion condition.
 */
template <class AsyncWriteStream, class ConstBufferSequence>
std::size_t await_write_some(AsyncWriteStream& stream,
                             const ConstBufferSequence& buffers,
                             boost::system::error_code& ec) {
    detail::blocking_latch latch;
    stream.async_write_some(buffers, detail::blocking_waiter(&latch));
    return detail::blocking_wait(latch, ec);
}

} /* namespace asio_host */
} /* namespace corocc */

#endif /* COROCC_BOOST_ASIO_HOST_HPP */
