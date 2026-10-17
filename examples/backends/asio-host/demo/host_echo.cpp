// corocc asio-host example -- demo: an echo server and client in one process.
//
// The accept side is a plain classic Asio callback (accept is not one of the
// wrapped operations); the accepted connection is handed to a host-loop
// coroutine that reads and echoes until the peer closes. The client side is a
// second host-loop coroutine doing three request/response round trips.
//
// Run it: it prints a summary and exits 0 on success, non-zero on failure.

#include <corocc/boost/asio_host.hpp>

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace asio = boost::asio;

typedef asio::io_context::executor_type executor_type;
typedef asio::strand<executor_type> strand_type;
typedef corocc::asio_host::yield_context<strand_type> yield_type;

namespace {

int g_failures = 0;

#define DEMO_CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

}  // namespace

int main() {
    asio::io_context ioc;
    strand_type strand(ioc.get_executor());

    boost::system::error_code ec;
    asio::ip::tcp::acceptor acceptor(
        ioc, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));

    asio::ip::tcp::socket server(ioc);
    asio::ip::tcp::socket client(ioc);

    std::atomic<int> server_rc(-1);
    std::atomic<long long> server_rounds(0);

    // One-shot accept: a classic callback spawns the per-connection host-loop
    // coroutine on the connection's strand. The acceptor is closed once the
    // connection has been accepted (closing it earlier would reset the
    // connection queued in the backlog).
    acceptor.async_accept(server, asio::bind_executor(strand,
        [&](const boost::system::error_code& aec) {
            if (aec) {
                server_rc.store(-2);
                return;
            }
            boost::system::error_code cec;
            acceptor.close(cec);
            const coro_status cs = corocc::asio_host::spawn(strand, [&](yield_type& y) {
                char buf[512];
                int rc = 0;
                for (;;) {
                    boost::system::error_code rec;
                    const std::size_t n = corocc::asio_host::async_read_some(
                        y, server, asio::buffer(buf, sizeof buf), rec);
                    if (rec) {
                        // eof: the client closed; the echo session is done.
                        rc = (rec == asio::error::eof) ? 0 : 1;
                        break;
                    }
                    boost::system::error_code wec;
                    const std::size_t w = corocc::asio_host::async_write(
                        y, server, asio::buffer(buf, n), wec);
                    if (wec || w != n) {
                        rc = 2;
                        break;
                    }
                    server_rounds.fetch_add(1);
                }
                boost::system::error_code cec;
                server.close(cec);
                server_rc.store(rc);
            });
            if (cs != CORO_OK) {
                server_rc.store(-3);
            }
        }));

    // Client: a synchronous connect is fine here (setup, not io thread work);
    // the round trips run inside a host-loop coroutine on the same strand.
    client.connect(acceptor.local_endpoint(), ec);
    if (ec) {
        std::fprintf(stderr, "client connect failed: %s\n", ec.message().c_str());
        return 1;
    }

    std::atomic<int> client_rc(-1);
    std::atomic<long long> client_rounds(0);

    const coro_status cs = corocc::asio_host::spawn(strand, [&](yield_type& y) {
        int rc = 0;
        for (int i = 0; i < 3 && rc == 0; ++i) {
            char msg[32];
            std::snprintf(msg, sizeof msg, "ping %d", i);
            const std::size_t len = std::strlen(msg);

            boost::system::error_code wec;
            const std::size_t w = corocc::asio_host::async_write(
                y, client, asio::buffer(msg, len), wec);
            if (wec || w != len) {
                rc = 1;
                break;
            }

            char back[32];
            std::memset(back, 0, sizeof back);
            std::size_t acc = 0;
            while (acc < len && rc == 0) {
                boost::system::error_code rec;
                const std::size_t n = corocc::asio_host::async_read_some(
                    y, client, asio::buffer(back + acc, len - acc), rec);
                if (rec || n == 0) {
                    rc = 2;
                    break;
                }
                acc += n;
            }
            if (rc == 0 && std::memcmp(back, msg, len) != 0) {
                rc = 3;
            }
            if (rc == 0) {
                client_rounds.fetch_add(1);
            }
        }
        boost::system::error_code cec;
        client.close(cec);  // the server session sees eof and finishes
        client_rc.store(rc);
    });
    if (cs != CORO_OK) {
        std::fprintf(stderr, "spawn failed: %s\n", coro_status_str(cs));
        return 1;
    }

    ioc.run();

    DEMO_CHECK(client_rc.load() == 0);
    DEMO_CHECK(server_rc.load() == 0);
    DEMO_CHECK(client_rounds.load() == 3);
    DEMO_CHECK(server_rounds.load() == 3);

    if (g_failures != 0) {
        return 1;
    }
    std::printf("asio-host demo: ok (3 round trips, %lld client ops, %lld server ops)\n",
                client_rounds.load() * 2, server_rounds.load() * 2);
    return 0;
}
