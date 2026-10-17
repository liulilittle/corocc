// corocc asio-pump example -- tiny assertion helper plus the loopback / echo
// helpers shared by the example's tests.
//
// The example is self-contained: this harness only depends on Boost.Asio and
// the C++ standard library, never on the repository's C test harness.
#ifndef COROCC_EXAMPLE_ASIO_PUMP_TEST_HARNESS_H
#define COROCC_EXAMPLE_ASIO_PUMP_TEST_HARNESS_H

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <utility>

inline std::atomic<int>& check_failures() {
    static std::atomic<int> failures(0);
    return failures;
}

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        check_failures().fetch_add(1); \
    } \
} while (0)

inline int test_finish(const char* name) {
    const int failed = check_failures().load();
    if (failed != 0) {
        std::fprintf(stderr, "%s: %d check(s) failed\n", name, failed);
        return 1;
    }
    std::printf("%s: ok\n", name);
    return 0;
}

// make_loopback_pair -- two connected TCP sockets on 127.0.0.1 (the same
// portable recipe the asio-host example uses: TCP loopback instead of a POSIX
// socketpair, so the test source also builds on Windows).
inline bool make_loopback_pair(boost::asio::io_context& ioc,
                               boost::asio::ip::tcp::socket& client,
                               boost::asio::ip::tcp::socket& peer) {
    boost::system::error_code ec;
    boost::asio::ip::tcp::acceptor acceptor(
        ioc, boost::asio::ip::tcp::endpoint(
                 boost::asio::ip::address_v4::loopback(), 0));
    client.connect(acceptor.local_endpoint(), ec);
    if (ec) {
        return false;
    }
    acceptor.accept(peer, ec);
    return !ec;
}

// echo_peer -- a classic (callback) echo peer: everything it reads is written
// back, until the client side closes.
class echo_peer : public std::enable_shared_from_this<echo_peer> {
public:
    typedef boost::asio::ip::tcp::socket socket_type;

    explicit echo_peer(socket_type sock)
        : sock_(std::move(sock)), received_(0), aborted_(false) {}

    void start() { read_some(); }

    std::size_t received() const { return received_; }
    bool aborted() const { return aborted_; }

    void close() {
        boost::system::error_code ec;
        sock_.close(ec);
    }

private:
    void read_some() {
        const std::shared_ptr<echo_peer> self = shared_from_this();
        sock_.async_read_some(
            boost::asio::buffer(buf_.data(), buf_.size()),
            [this, self](const boost::system::error_code& ec, std::size_t n) {
                if (ec) {
                    if (ec == boost::asio::error::operation_aborted) {
                        aborted_ = true;
                    }
                    return;
                }
                received_ += n;
                boost::asio::async_write(
                    sock_, boost::asio::buffer(buf_.data(), n),
                    [this, self](const boost::system::error_code& ec2,
                                 std::size_t) {
                        if (!ec2) {
                            read_some();
                        }
                    });
            });
    }

    socket_type sock_;
    std::array<char, 4096> buf_;
    std::size_t received_;
    bool aborted_;
};

#endif  // COROCC_EXAMPLE_ASIO_PUMP_TEST_HARNESS_H
