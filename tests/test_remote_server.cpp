/*
 * mayhem-b200 — RemoteServer socket-layer tests.
 *
 * These drive the real listening socket rather than a fake, because what they
 * guard is the OS behaviour itself, and Winsock and BSD sockets differ in two
 * ways that a port gets wrong silently:
 *
 *   - closesocket() on a listening socket unblocks a concurrent accept(); a
 *     bare POSIX close() does NOT, so stop() would park forever joining the
 *     accept thread. remote_server.cpp's listener_close() shuts the socket
 *     down first. stop_returns_promptly is the test that would hang without it.
 *
 *   - writing to a socket whose peer has gone away raises SIGPIPE on POSIX,
 *     whose default disposition kills the process; Winsock has no equivalent.
 *     remote_server.cpp sends with MSG_NOSIGNAL. survives_a_client_that_leaves
 *     is the test that would take the whole test binary down without it.
 *
 * Both are no-ops on Windows, where they simply confirm the same behaviour the
 * server has always had.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "remote/remote_server.hpp"

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace {

/* A throwaway loopback client, just enough to talk to the server under test.
 * The same handful of primitives the server itself has to abstract. */
class TestClient {
   public:
    ~TestClient() { close(); }

    bool connect_to(uint16_t port) {
        close();
        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == remote::kInvalidSocket) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
            return false;
        }
        return true;
    }

    bool send_line(const std::string& s) {
#if defined(_WIN32)
        return ::send(sock_, s.data(), static_cast<int>(s.size()), 0) ==
               static_cast<int>(s.size());
#else
        return ::send(sock_, s.data(), s.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(s.size());
#endif
    }

    /* Reads until the peer closes, which is what "Connection: close" gives us. */
    std::string read_all() {
        std::string out;
        char buf[2048];
        for (;;) {
#if defined(_WIN32)
            const int n = ::recv(sock_, buf, static_cast<int>(sizeof(buf)), 0);
#else
            const ssize_t n = ::recv(sock_, buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
        }
        return out;
    }

    void close() {
        if (sock_ == remote::kInvalidSocket) return;
#if defined(_WIN32)
        closesocket(sock_);
#else
        ::close(sock_);
#endif
        sock_ = remote::kInvalidSocket;
    }

    /* Drops the connection with an RST rather than a FIN, so the server's next
     * write to it fails hard instead of draining into a half-closed socket. */
    void abort_connection() {
        if (sock_ == remote::kInvalidSocket) return;
        linger lg{};
        lg.l_onoff = 1;
        lg.l_linger = 0;
        setsockopt(sock_, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof(lg));
        close();
    }

   private:
    remote::socket_t sock_{remote::kInvalidSocket};
};

}  // namespace

TEST(remote_server_starts_on_an_os_assigned_port) {
    remote::RemoteServer server;
    /* Port 0 so the test never collides with a real service or another run. */
    CHECK(server.start(0));
    CHECK(server.running());
    CHECK(server.port() != 0);
    server.stop();
    CHECK(!server.running());
    CHECK_EQ(server.port(), static_cast<uint16_t>(0));
}

TEST(remote_server_refuses_to_start_twice) {
    remote::RemoteServer server;
    CHECK(server.start(0));
    CHECK(!server.start(0));
    CHECK_STR_EQ(server.last_error(), "already running");
    server.stop();
}

TEST(remote_server_answers_a_request_over_a_real_socket) {
    remote::RemoteServer server;
    CHECK(server.start(0));

    TestClient client;
    CHECK(client.connect_to(server.port()));
    CHECK(client.send_line("GET /api/status HTTP/1.1\r\nHost: x\r\n\r\n"));

    const std::string response = client.read_all();
    CHECK(response.rfind("HTTP/1.1 200 OK", 0) == 0);
    CHECK(response.find("application/json") != std::string::npos);
    /* status_json() always carries these, whether or not a radio is attached. */
    CHECK(response.find("\"receiving\"") != std::string::npos);

    server.stop();
}

TEST(remote_server_answers_404_for_an_unknown_route) {
    remote::RemoteServer server;
    CHECK(server.start(0));

    TestClient client;
    CHECK(client.connect_to(server.port()));
    CHECK(client.send_line("GET /api/nope HTTP/1.1\r\nHost: x\r\n\r\n"));

    const std::string response = client.read_all();
    CHECK(response.rfind("HTTP/1.1 404 Not Found", 0) == 0);

    server.stop();
}

/* The accept-wakeup guard. If listener_close() ever loses its shutdown() on
 * POSIX, stop() blocks forever in the join and this test hangs instead of
 * failing — so it runs stop() on its own thread and reports a timeout. */
TEST(remote_server_stop_returns_promptly_while_accept_is_blocked) {
    /* Heap-allocated and deliberately leaked on timeout: if stop() really is
     * stuck, the thread below still holds a reference to this object and must
     * not see it destroyed when the test returns. */
    auto* server = new remote::RemoteServer();
    CHECK(server->start(0));

    /* Give the accept loop time to actually park inside accept(); stopping
     * before it blocks would not exercise the wakeup at all. */
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    std::atomic<bool> finished{false};
    std::thread stopper([server, &finished] {
        server->stop();
        finished.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!finished.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    CHECK(finished.load());

    if (finished.load()) {
        stopper.join();
        delete server;
    } else {
        stopper.detach();  /* wedged in accept(); leak rather than crash */
    }
}

/* The SIGPIPE guard. A client that fires a request and vanishes before reading
 * the reply makes the server write to a dead socket; on POSIX that raises
 * SIGPIPE unless the send asks for MSG_NOSIGNAL, and the default disposition
 * would take this whole test binary down rather than fail an assertion. */
TEST(remote_server_survives_a_client_that_leaves_without_reading) {
    remote::RemoteServer server;
    CHECK(server.start(0));

    /* /api/apps is the largest response the server produces — the one most
     * likely to need more than one send() call, which is where the second
     * write to a reset socket raises the signal. */
    for (int i = 0; i < 25; i++) {
        TestClient client;
        CHECK(client.connect_to(server.port()));
        CHECK(client.send_line("GET /api/apps HTTP/1.1\r\nHost: x\r\n\r\n"));
        client.abort_connection();
    }

    /* Reaching here at all is most of the point; confirm the server is still
     * serving rather than merely still linked into a live process. */
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    TestClient client;
    CHECK(client.connect_to(server.port()));
    CHECK(client.send_line("GET /api/status HTTP/1.1\r\nHost: x\r\n\r\n"));
    CHECK(client.read_all().rfind("HTTP/1.1 200 OK", 0) == 0);

    server.stop();
}

/* A request that is never completed must be dropped on the receive timeout
 * rather than holding its worker thread forever. */
TEST(remote_server_drops_a_half_sent_request) {
    remote::RemoteServer server;
    CHECK(server.start(0));

    TestClient client;
    CHECK(client.connect_to(server.port()));
    CHECK(client.send_line("GET /api/sta"));  /* no blank line: never completes */
    client.close();

    TestClient healthy;
    CHECK(healthy.connect_to(server.port()));
    CHECK(healthy.send_line("GET /api/status HTTP/1.1\r\nHost: x\r\n\r\n"));
    CHECK(healthy.read_all().rfind("HTTP/1.1 200 OK", 0) == 0);

    server.stop();
}
