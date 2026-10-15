/*
 * corocc -- a zero-dependency C/C++ coroutine library.
 *
 * corocc/boost/asio.hpp -- Boost.Asio calls with blocking shape inside a
 * corocc runtime coroutine (optional Boost integration; header-only).
 *
 * This header is standalone: no other corocc header includes it and the
 * library itself never depends on Boost. A program that does not include it
 * is unaffected -- no Boost header is parsed, no symbol is referenced, and
 * no build setting changes. Including it requires the Boost.Asio headers
 * plus the corocc runtime (<corocc/coro_rt.h>), and a compiler in C++11
 * mode or later.
 *
 * The four functions below give an Asio asynchronous operation the shape of
 * a blocking call made from inside a coroutine scheduled by a coro_rt
 * runtime:
 *
 *     boost::system::error_code ec;
 *     std::size_t n = corocc::asio::async_read(rt, sock, buf, ec);
 *
 * What happens during such a call:
 *
 *   1. The Asio operation is initiated on the calling coroutine's thread
 *      (Asio allows initiation from any thread as long as no second
 *      operation is outstanding on the same object).
 *   2. The completion handler runs on whatever io_context thread finishes
 *      the operation -- with or without a strand -- writes the outcome into
 *      a one-shot latch living on the coroutine's stack, and posts a single
 *      token with coro_rt_sem_post, which is callable from any thread.
 *   3. The coroutine parks in coro_rt_sem_wait and is woken on some runtime
 *      worker when the token arrives. The completion thread never touches
 *      the coroutine's stack; resume travels through the runtime's own
 *      wakeup path, so the handoff is strand-safe and thread-safe by
 *      construction.
 *   4. The wait returns the outcome stored in the latch: the number of
 *      bytes transferred and, through @p ec, the Asio result (success, the
 *      operation's error, or operation_aborted when a pending cancellation
 *      was observed while waiting).
 *
 * Contracts and notes:
 *
 *   - Call these functions only from inside a coroutine scheduled by @p rt
 *     (a coro_rt_create runtime; corocc::runtime::native() hands out the
 *     handle). A call made outside a runtime coroutine cannot park; the
 *     function then blocks the calling thread on the latch until the
 *     operation completes and reports the outcome the same way -- safe,
 *     but a programming error worth avoiding.
 *   - The io_context must be running on at least one thread, or the
 *     completion never arrives and the coroutine stays parked. That is the
 *     standard Asio contract, not something this header can repair.
 *   - @p stream and @p buffers must stay alive (and unchanged, for reads)
 *     until the call returns, and the operation must be the only one
 *     outstanding on that stream. Both are plain Asio requirements.
 *   - A cancellation requested through coro_rt_cancel while the coroutine
 *     is parked here is not acted on immediately: cancellation is sane
 *     only together with ending the I/O (cancel or close the socket), and
 *     the wait folds the cancellation into "keep waiting for the
 *     completion", because the pending handler still owns the right to
 *     write the latch and must never outlive the coroutine frame. When the
 *     completion does arrive, a cancellation observed earlier is reported
 *     as operation_aborted with zero bytes.
 *   - No heap allocation is added by the wrappers themselves: the latch is
 *     a stack local, and coro_rt_sem_t is an embeddable value type.
 */
#ifndef COROCC_BOOST_ASIO_HPP
#define COROCC_BOOST_ASIO_HPP

#if !defined(__cplusplus)
#error "corocc/boost/asio.hpp is a C++ header; compile as C++11 or later"
#endif

#include <atomic>
#include <cstddef>
#include <thread>

#include <boost/asio/error.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <corocc/coro_rt.h>

namespace corocc {
namespace asio {

namespace detail {

/* wait_latch -- one-shot outcome cell shared between the suspended coroutine
 * frame and the Asio completion handler.
 *
 * Lives on the coroutine's stack: while the coroutine is parked the frame is
 * alive, and the wrappers never return before the completion has run, so the
 * handler can never outlive the storage it writes.
 *
 * Memory ordering: the completion writes @c ec and @c bytes, then release-
 * stores @c done and posts the token. The woken coroutine does an acquire
 * load of @c done before reading the fields, so the outcome is visible
 * whether the wakeup raced the latch or arrived through the runtime queue. */
struct wait_latch {
    coro_rt_sem_t             token; /**< Zero-initialized is a valid semaphore (count 0). */
    boost::system::error_code ec;    /**< Asio result of the operation. */
    std::size_t               bytes; /**< Bytes transferred (0 on error). */
    std::atomic<int>          done;  /**< Set once when the handler has run. */

    wait_latch() : token(), ec(), bytes(0), done(0) {}
};

/* Complete the latch: record the outcome and wake the parked coroutine. Runs
 * on an io_context thread; coro_rt_sem_post is thread-safe. */
inline void latch_signal(wait_latch* l, coro_rt_t* rt,
                         const boost::system::error_code& e,
                         std::size_t n) noexcept {
    l->ec = e;
    l->bytes = n;
    l->done.store(1, std::memory_order_release);
    (void)coro_rt_sem_post(rt, &l->token);
}

/* Wait until the completion has run. Returns true when a cancellation was
 * observed while waiting. A cancellation does not cut the wait short: the
 * pending handler must be allowed to run while the frame is still alive. */
inline bool latch_wait(coro_rt_t* rt, wait_latch* l) noexcept {
    bool cancelled = false;
    for (;;) {
        const coro_status st = coro_rt_sem_wait(rt, &l->token);
        if (st == CORO_OK) break;
        if (st == CORO_ECANCELED) {
            cancelled = true;
            continue;
        }
        /* Not inside a runtime coroutine (CORO_ESTALE) -- the precondition
         * was violated; block the caller thread on the latch instead of
         * parking, so the completion still cannot write into a dead frame. */
        while (l->done.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
        break;
    }
    (void)l->done.load(std::memory_order_acquire);
    return cancelled;
}

/* Observable outcome after latch_wait. */
inline std::size_t latch_result(wait_latch* l, bool cancelled,
                                boost::system::error_code& ec) noexcept {
    if (cancelled) {
        ec = ::boost::asio::error::operation_aborted;
        return 0;
    }
    ec = l->ec;
    return l->bytes;
}

} /* namespace detail */

/**
 * async_read -- read until the buffer sequence is full, blocking the
 * coroutine. Same completion condition as boost::asio::async_read: the call
 * returns only after the whole sequence is filled or the operation fails.
 *
 * @param rt      Runtime scheduling the calling coroutine.
 * @param stream  AsyncReadStream (socket, ssl stream, any Asio stream).
 * @param buffers MutableBufferSequence; must remain valid until return.
 * @param ec      Receives the Asio result; operation_aborted when a pending
 *                cancellation was observed.
 * @return Bytes read (the full buffer size on success), 0 on error/cancel.
 */
template <class AsyncReadStream, class MutableBufferSequence>
std::size_t async_read(coro_rt_t* rt, AsyncReadStream& stream,
                       const MutableBufferSequence& buffers,
                       boost::system::error_code& ec) {
    detail::wait_latch latch;
    ::boost::asio::async_read(
        stream, buffers,
        [&latch, rt](const boost::system::error_code& e,
                     std::size_t n) noexcept {
            detail::latch_signal(&latch, rt, e, n);
        });
    const bool cancelled = detail::latch_wait(rt, &latch);
    return detail::latch_result(&latch, cancelled, ec);
}

/**
 * async_read_some -- one stream read, blocking the coroutine. Same completion
 * condition as boost::asio::async_read_some: returns as soon as at least one
 * byte was read (or the operation fails).
 *
 * @return Bytes read (> 0 on success), 0 on error/cancel.
 * @see async_read for the parameter and contract description.
 */
template <class AsyncReadStream, class MutableBufferSequence>
std::size_t async_read_some(coro_rt_t* rt, AsyncReadStream& stream,
                            const MutableBufferSequence& buffers,
                            boost::system::error_code& ec) {
    detail::wait_latch latch;
    stream.async_read_some(
        buffers,
        [&latch, rt](const boost::system::error_code& e,
                     std::size_t n) noexcept {
            detail::latch_signal(&latch, rt, e, n);
        });
    const bool cancelled = detail::latch_wait(rt, &latch);
    return detail::latch_result(&latch, cancelled, ec);
}

/**
 * async_write -- write the whole buffer sequence, blocking the coroutine.
 * Same completion condition as boost::asio::async_write: the call returns
 * only after the entire sequence was written or the operation fails.
 *
 * @return Bytes written (the full buffer size on success), 0 on error/cancel.
 * @see async_read for the parameter and contract description.
 */
template <class AsyncWriteStream, class ConstBufferSequence>
std::size_t async_write(coro_rt_t* rt, AsyncWriteStream& stream,
                        const ConstBufferSequence& buffers,
                        boost::system::error_code& ec) {
    detail::wait_latch latch;
    ::boost::asio::async_write(
        stream, buffers,
        [&latch, rt](const boost::system::error_code& e,
                     std::size_t n) noexcept {
            detail::latch_signal(&latch, rt, e, n);
        });
    const bool cancelled = detail::latch_wait(rt, &latch);
    return detail::latch_result(&latch, cancelled, ec);
}

/**
 * async_write_some -- one stream write, blocking the coroutine. Same
 * completion condition as boost::asio::async_write_some: returns as soon as
 * at least one byte was written (or the operation fails).
 *
 * @return Bytes written (> 0 on success), 0 on error/cancel.
 * @see async_read for the parameter and contract description.
 */
template <class AsyncWriteStream, class ConstBufferSequence>
std::size_t async_write_some(coro_rt_t* rt, AsyncWriteStream& stream,
                             const ConstBufferSequence& buffers,
                             boost::system::error_code& ec) {
    detail::wait_latch latch;
    stream.async_write_some(
        buffers,
        [&latch, rt](const boost::system::error_code& e,
                     std::size_t n) noexcept {
            detail::latch_signal(&latch, rt, e, n);
        });
    const bool cancelled = detail::latch_wait(rt, &latch);
    return detail::latch_result(&latch, cancelled, ec);
}

} /* namespace asio */
} /* namespace corocc */

#endif /* COROCC_BOOST_ASIO_HPP */
