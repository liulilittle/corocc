// corocc -- smoke for include/corocc/boost/asio.hpp.
//
// A corocc runtime coroutine performs blocking-shaped Boost.Asio operations
// (async_write / async_read / async_write_some / async_read_some) against a
// socketpair peer driven by a two-thread io_context, and verifies the bytes
// that come back. Exit code 0 means both round trips succeeded.
//
// Build recipe (from the repository root; the corocc runtime objects are
// needed at link time -- the wrappers themselves are header-only):
//   mkdir -p smoke-build
//   for f in src/coroutine/*.c src/scheduler/*.c src/platform/*.c; do
//       gcc -std=gnu99 -O2 -Iinclude -Isrc -c "$f" -o "smoke-build/$(basename "$f" .c).o"
//   done
//   gcc -c src/arch/x86_64_sysv_elf_gas.S -o smoke-build/x86_64_gas.o
//   g++ -std=c++11 -Wall -Wextra -Werror -Iinclude tests/boost/asio_smoke.cpp smoke-build/*.o -pthread -o smoke
//   ./smoke
//
// The ci.yml boost-asio job exercises the same recipe with g++ and clang++.

#include <corocc/boost/asio.hpp>

#include <boost/asio.hpp>

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

namespace asio = boost::asio;
using local_socket = asio::local::stream_protocol::socket;

namespace {

constexpr std::size_t kFirst = 16;  // write/read round trip
constexpr std::size_t kSecond = 5;  // write_some/read_some round trip

struct smoke_state {
    local_socket* client = nullptr;
    local_socket* peer = nullptr;
    int rc = -1;
};

coro_rt_t* g_rt = nullptr;

// ---- peer side: echo driven by the io_context -------------------------

struct echo_state {
    local_socket* peer = nullptr;
    std::array<char, kFirst> first{};
    std::array<char, kSecond> second{};
    std::size_t second_got = 0;
};

void peer_read_some(const std::shared_ptr<echo_state>& e);

void peer_begin(const std::shared_ptr<echo_state>& e) {
    asio::async_read(*e->peer, asio::buffer(e->first),
        [e](const boost::system::error_code& ec, std::size_t n) {
            if (ec || n != e->first.size()) { return; }
            asio::async_write(*e->peer, asio::buffer(e->first),
                [e](const boost::system::error_code& ec2, std::size_t) {
                    if (ec2) { return; }
                    peer_read_some(e);
                });
        });
}

void peer_read_some(const std::shared_ptr<echo_state>& e) {
    e->peer->async_read_some(
        asio::buffer(e->second.data() + e->second_got,
                     e->second.size() - e->second_got),
        [e](const boost::system::error_code& ec, std::size_t n) {
            if (ec) { return; }
            e->second_got += n;
            if (e->second_got < e->second.size()) {
                peer_read_some(e);
                return;
            }
            e->peer->async_write_some(asio::buffer(e->second),
                [](const boost::system::error_code&, std::size_t) {});
        });
}

// ---- coroutine side ---------------------------------------------------

void client_coroutine(void* arg) {
    smoke_state* s = static_cast<smoke_state*>(arg);

    std::array<char, kFirst> out;
    std::array<char, kFirst> in;
    out.fill(' ');
    std::memcpy(out.data(), "corocc+asio", 11);
    in.fill(0);

    boost::system::error_code ec;

    // 1) async_write / async_read round trip.
    const std::size_t written =
        corocc::asio::async_write(g_rt, *s->client, asio::buffer(out), ec);
    if (ec || written != out.size()) { s->rc = 1; return; }

    const std::size_t read =
        corocc::asio::async_read(g_rt, *s->client, asio::buffer(in), ec);
    if (ec || read != in.size()) { s->rc = 2; return; }
    if (std::memcmp(in.data(), out.data(), out.size()) != 0) {
        s->rc = 3;
        return;
    }

    // 2) async_write_some / async_read_some round trip.
    std::array<char, kSecond> msg2 = {'f', 'i', 'v', 'e', '!'};
    const std::size_t sent = corocc::asio::async_write_some(
        g_rt, *s->client, asio::buffer(msg2), ec);
    if (ec || sent != msg2.size()) { s->rc = 4; return; }

    std::array<char, kSecond> got;
    got.fill(0);
    std::size_t acc = 0;
    while (acc < got.size()) {
        const std::size_t n = corocc::asio::async_read_some(
            g_rt, *s->client,
            asio::buffer(got.data() + acc, got.size() - acc), ec);
        if (ec) { s->rc = 5; return; }
        if (n == 0) { s->rc = 6; return; }
        acc += n;
    }
    if (std::memcmp(got.data(), msg2.data(), msg2.size()) != 0) {
        s->rc = 7;
        return;
    }

    s->rc = 0;
}

}  // namespace

int main() {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        std::perror("socketpair");
        return 1;
    }

    asio::io_context ioc;
    asio::executor_work_guard<asio::io_context::executor_type> guard =
        asio::make_work_guard(ioc);

    local_socket client(ioc), peer(ioc);
    client.assign(asio::local::stream_protocol(), sv[0]);
    peer.assign(asio::local::stream_protocol(), sv[1]);

    smoke_state s;
    s.client = &client;
    s.peer = &peer;

    auto e = std::make_shared<echo_state>();
    e->peer = &peer;
    peer_begin(e);

    std::thread io1([&ioc] { ioc.run(); });
    std::thread io2([&ioc] { ioc.run(); });

    g_rt = coro_rt_create(2);
    if (!g_rt) {
        std::printf("coro_rt_create failed\n");
        return 1;
    }
    if (coro_rt_spawn(g_rt, nullptr, client_coroutine, &s) != CORO_OK) {
        std::printf("coro_rt_spawn failed\n");
        return 1;
    }
    coro_rt_wait_all(g_rt);
    coro_rt_destroy(g_rt);
    g_rt = nullptr;

    guard.reset();
    io1.join();
    io2.join();

    std::printf("smoke rc=%d (0=pass)\n", s.rc);
    return s.rc == 0 ? 0 : 1;
}
