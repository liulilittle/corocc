// corocc asio-host example -- error-code paths.
//
// Four deterministic scenarios, all spawned up front and observed by one
// io_context run:
//   a) peer closed before the read   -> eof through async_read
//   b) socket closed while parked    -> operation_aborted through
//                                       async_read_some (the completion is
//                                       delivered through the strand and
//                                       resumes the coroutine inline)
//   c) failed connect + a follow-up  -> an immediate-error completion (the
//      write on that socket            handler runs inline during initiation;
//                                      the coroutine must not yield at all)
//   d) a second operation while one  -> the busy guard: an error is reported
//      is in flight                     through ec, the first operation is
//                                       untouched, nothing terminates

#include <corocc/boost/asio_host.hpp>

#include "harness.h"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <chrono>
#include <cstring>

namespace asio = boost::asio;

typedef asio::io_context::executor_type executor_type;
typedef asio::strand<executor_type> strand_type;
typedef corocc::asio_host::yield_context<strand_type> yield_type;

int main() {
    asio::io_context ioc;
    strand_type strand(ioc.get_executor());

    std::atomic<int> a_rc(-1);
    std::atomic<int> b_rc(-1);
    std::atomic<int> c_rc(-1);
    std::atomic<int> d_rc(-1);
    std::atomic<int> probe_rc(-1);

    // a) The peer closes before the coroutine reads: eof, zero bytes.
    asio::ip::tcp::socket a_client(ioc);
    asio::ip::tcp::socket a_peer(ioc);
    CHECK(make_loopback_pair(ioc, a_client, a_peer));
    a_peer.close();
    CHECK(corocc::asio_host::spawn(strand, [&](yield_type& y) {
        char buf[8];
        boost::system::error_code lec;
        const std::size_t n =
            corocc::asio_host::async_read(y, a_client, asio::buffer(buf, 8), lec);
        a_rc.store((lec == asio::error::eof && n == 0) ? 0 : 1);
        boost::system::error_code cec;
        a_client.close(cec);
    }) == CORO_OK);

    // b) The socket is closed while the coroutine is parked in a read: the
    //    pending operation completes with an abort error and zero bytes. The
    //    exact code is platform dependent (Linux: operation_aborted; Windows:
    //    connection_aborted, i.e. WSAECONNABORTED from closesocket with a
    //    pending read). The timer is bound to the same strand, so its handler
    //    can only run once the coroutine has parked (the strand was busy
    //    running it).
    asio::ip::tcp::socket b_client(ioc);
    asio::ip::tcp::socket b_peer(ioc);
    CHECK(make_loopback_pair(ioc, b_client, b_peer));
    asio::steady_timer closer(ioc);
    closer.expires_after(std::chrono::milliseconds(1));
    closer.async_wait(asio::bind_executor(strand,
        [&](const boost::system::error_code&) {
            boost::system::error_code cec;
            b_client.close(cec);
        }));
    CHECK(corocc::asio_host::spawn(strand, [&](yield_type& y) {
        char buf[8];
        boost::system::error_code lec;
        const std::size_t n = corocc::asio_host::async_read_some(
            y, b_client, asio::buffer(buf, 8), lec);
        // Closing the socket cancels the parked read: zero bytes and an
        // abort-class error. The spelling depends on platform and backend
        // (observed: Linux reports operation_aborted; the Windows IOCP
        // backend reports ERROR_CONNECTION_ABORTED (1236) when closesocket
        // cancels a pending overlapped read; WSAECONNABORTED (10053) and
        // ERROR_OPERATION_ABORTED (995) belong to the same family), so the
        // check accepts the family rather than one spelling.
        const bool aborted = (lec == asio::error::operation_aborted) ||
                             (lec.value() == asio::error::connection_aborted) ||
                             (lec.value() == 1236) ||  // ERROR_CONNECTION_ABORTED
                             (lec.value() == 995);     // ERROR_OPERATION_ABORTED
        b_rc.store((aborted && n == 0) ? 0 : 1);
    }) == CORO_OK);

    // c) A refused connect, then a write on that still-unconnected socket:
    //    the completion is an immediate error, which exercises the
    //    inline-completion path (the handler runs during initiation, before
    //    any yield; the coroutine must simply observe the latch). The exact
    //    error code is platform dependent (not_connected on Linux,
    //    bad_descriptor-like on Windows); the contract under test is "an
    //    error comes back through ec with zero bytes".
    unsigned short refused_port = 0;
    {
        asio::ip::tcp::acceptor probe(
            ioc, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        refused_port = probe.local_endpoint().port();
    }
    asio::ip::tcp::socket c_sock(ioc);
    {
        boost::system::error_code connect_ec;
        c_sock.connect(
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), refused_port),
            connect_ec);
        CHECK(connect_ec);  // the connect itself must fail on both platforms
    }
    CHECK(corocc::asio_host::spawn(strand, [&](yield_type& y) {
        char buf[4] = {'x', 'y', 'z', 'w'};
        boost::system::error_code wec;
        const std::size_t n =
            corocc::asio_host::async_write(y, c_sock, asio::buffer(buf, 4), wec);
        c_rc.store((wec && n == 0) ? 0 : 1);
    }) == CORO_OK);

    // d) Misuse probe: while coroutine D is parked in a read, a handler on
    //    the same strand calls async_read on D's yield context again. The
    //    busy guard must reject it with an error through ec (nothing
    //    terminates, the parked read is untouched); the probe then feeds the
    //    parked read, which must complete normally.
    asio::ip::tcp::socket d_client(ioc);
    asio::ip::tcp::socket d_peer(ioc);
    CHECK(make_loopback_pair(ioc, d_client, d_peer));
    yield_type* parked = 0;
    CHECK(corocc::asio_host::spawn(strand, [&](yield_type& y) {
        parked = &y;
        char buf[4];
        boost::system::error_code lec;
        const std::size_t n =
            corocc::asio_host::async_read(y, d_client, asio::buffer(buf, 4), lec);
        d_rc.store((!lec && n == 4) ? 0 : 1);
        boost::system::error_code cec;
        d_client.close(cec);
    }) == CORO_OK);

    asio::post(strand, [&]() {
        char buf[4];
        boost::system::error_code pec;
        const std::size_t n =
            corocc::asio_host::async_read(*parked, d_peer, asio::buffer(buf, 4), pec);
        const boost::system::error_code busy =
            boost::system::errc::make_error_code(boost::system::errc::device_or_resource_busy);
        probe_rc.store((pec == busy && n == 0) ? 0 : 1);

        boost::system::error_code wec;
        const std::size_t w = asio::write(d_peer, asio::buffer("abcd", 4), wec);
        if (wec || w != 4) {
            probe_rc.store(2);
        }
    });

    ioc.run();

    CHECK(probe_rc.load() == 0);
    CHECK(d_rc.load() == 0);
    CHECK(a_rc.load() == 0);
    CHECK(b_rc.load() == 0);
    CHECK(c_rc.load() == 0);

    return test_finish("host_errors");
}
