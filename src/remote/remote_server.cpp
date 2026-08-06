/*
 * mayhem-b200 — the web portal's HTTP server.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "remote_server.hpp"

#include "app_bridge.hpp"
#include "app_data.hpp"

#include "../apps/app_registry.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <thread>

namespace remote {

namespace {

constexpr const char* kJson = "application/json; charset=utf-8";
constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr size_t kMaxBodyBytes = 1 * 1024 * 1024;
constexpr int kRecvTimeoutMs = 5000;

/* WSAStartup/WSACleanup are refcounted by Winsock itself; a function-local
 * static keeps the process to exactly one pair regardless of how many
 * RemoteServers are started and stopped. Mirrors network_radio.cpp's
 * WsaInit — duplicated rather than shared because that one is private to an
 * anonymous namespace in a different translation unit. */
class WsaInit {
   public:
    WsaInit() {
        WSADATA data{};
        ok_ = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
    }
    ~WsaInit() {
        if (ok_) WSACleanup();
    }
    WsaInit(const WsaInit&) = delete;
    WsaInit& operator=(const WsaInit&) = delete;
    bool ok() const { return ok_; }

   private:
    bool ok_{false};
};

WsaInit& wsa_init() {
    static WsaInit w;
    return w;
}

const char* status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        default: return "Internal Server Error";
    }
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

void send_response(SOCKET s, int status, const char* content_type, const std::string& body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << status_text(status) << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        /* Local dev/control API; the portal's own frontend is the only
         * intended client, but a permissive CORS header keeps a page served
         * from a different origin (e.g. a dev server) usable against it. */
        << "Access-Control-Allow-Origin: *\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    const std::string text = out.str();

    size_t sent = 0;
    while (sent < text.size()) {
        const int n = send(s, text.data() + static_cast<ptrdiff_t>(sent),
                           static_cast<int>(text.size() - sent), 0);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

void send_json_error(SOCKET s, int status, const std::string& message) {
    JsonValue v = JsonValue::object();
    v.set("error", JsonValue::string(message));
    send_response(s, status, kJson, v.dump());
}

/* Reads one HTTP/1.1 request off `s`: the request line, headers (only
 * Content-Length is consulted) and body. Returns false on a malformed
 * request, an oversized header/body, a client that never sends a full
 * request, or a socket error/timeout. */
bool read_request(SOCKET s, HttpRequest& out) {
    std::string buf;
    char chunk[4096];

    size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const int n = recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0) return false;
        buf.append(chunk, static_cast<size_t>(n));
        if (buf.size() > kMaxHeaderBytes) return false;
        header_end = buf.find("\r\n\r\n");
    }

    const std::string header_block = buf.substr(0, header_end);
    std::string leftover = buf.substr(header_end + 4);

    std::istringstream header_stream(header_block);
    std::string request_line;
    if (!std::getline(header_stream, request_line)) return false;
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

    std::istringstream rl(request_line);
    std::string http_version;
    rl >> out.method >> out.path >> http_version;
    if (out.method.empty() || out.path.empty()) return false;

    const auto query = out.path.find('?');
    if (query != std::string::npos) out.path = out.path.substr(0, query);

    size_t content_length = 0;
    std::string line;
    while (std::getline(header_stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string name = line.substr(0, colon);
        for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name != "content-length") continue;

        std::string value = line.substr(colon + 1);
        const auto first = value.find_first_not_of(' ');
        if (first != std::string::npos) content_length = static_cast<size_t>(std::strtoul(value.c_str() + first, nullptr, 10));
    }
    if (content_length > kMaxBodyBytes) return false;

    out.body = std::move(leftover);
    while (out.body.size() < content_length) {
        const int n = recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        out.body.append(chunk, static_cast<size_t>(n));
    }
    if (out.body.size() > content_length) out.body.resize(content_length);

    return true;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}
bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/* POST /api/apps/{id}/launch. Empty when the path doesn't match that shape
 * or `id` itself contains a '/' (app ids are flat slugs; anything with a
 * slash cannot be one and is treated as an unmatched route). */
std::string extract_launch_id(const std::string& path) {
    static const std::string kPrefix = "/api/apps/";
    static const std::string kSuffix = "/launch";
    if (!starts_with(path, kPrefix) || !ends_with(path, kSuffix)) return {};
    if (path.size() < kPrefix.size() + kSuffix.size()) return {};

    const std::string id = path.substr(kPrefix.size(), path.size() - kPrefix.size() - kSuffix.size());
    if (id.empty() || id.find('/') != std::string::npos) return {};
    return id;
}

void route(SOCKET client, const HttpRequest& req) {
    auto& bridge = AppBridge::instance();

    if (req.method == "GET" && req.path == "/api/apps") {
        send_response(client, 200, kJson, bridge.apps_json());
    } else if (req.method == "GET" && req.path == "/api/apps/current") {
        send_response(client, 200, kJson, bridge.current_app_json());
    } else if (req.method == "GET" && req.path == "/api/panel") {
        send_response(client, 200, kJson, bridge.panel_json());
    } else if (req.method == "GET" && req.path == "/api/status") {
        send_response(client, 200, kJson, bridge.status_json());
    } else if (req.method == "POST" && req.path == "/api/apps/home") {
        bridge.request_home();
        send_response(client, 202, kJson, R"({"queued":true})");
    } else if (req.method == "POST" && starts_with(req.path, "/api/apps/") && ends_with(req.path, "/launch")) {
        const std::string id = extract_launch_id(req.path);
        if (id.empty() || app::AppRegistry::instance().by_id(id) == nullptr) {
            send_json_error(client, 404, "unknown app id");
        } else {
            bridge.request_launch(id);
            JsonValue v = JsonValue::object();
            v.set("queued", JsonValue::boolean(true));
            v.set("id", JsonValue::string(id));
            send_response(client, 202, kJson, v.dump());
        }
    } else if (req.method != "GET" && req.method != "POST") {
        send_json_error(client, 405, "method not allowed");
    } else {
        send_json_error(client, 404, "not found");
    }
}

/* Owns nothing but the socket it was handed: it reads only through
 * AppBridge::instance(), a Meyers singleton with static storage duration, so
 * it is safe to run detached from a RemoteServer that may since have been
 * destroyed (see RemoteServer::accept_loop()). */
void handle_connection(SOCKET client) {
    const int timeout_ms = kRecvTimeoutMs;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    HttpRequest req;
    if (read_request(client, req)) {
        route(client, req);
    } else {
        send_json_error(client, 400, "bad request");
    }

    shutdown(client, SD_SEND);
    closesocket(client);
}

}  // namespace

RemoteServer::RemoteServer() = default;

RemoteServer::~RemoteServer() {
    stop();
}

bool RemoteServer::start(uint16_t port) {
    if (running_.load()) {
        last_error_ = "already running";
        return false;
    }
    if (!wsa_init().ok()) {
        last_error_ = "WSAStartup failed";
        return false;
    }

    listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket_ == INVALID_SOCKET) {
        last_error_ = "socket() failed (WSA error " + std::to_string(WSAGetLastError()) + ")";
        return false;
    }

    /* So a restart (e.g. after a crash-free process relaunch) can reuse the
     * port immediately instead of sitting in TIME_WAIT. */
    BOOL reuse = TRUE;
    setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        last_error_ = "bind() failed on port " + std::to_string(port) +
                      " (WSA error " + std::to_string(WSAGetLastError()) + ")";
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
        return false;
    }

    if (listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR) {
        last_error_ = "listen() failed (WSA error " + std::to_string(WSAGetLastError()) + ")";
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
        return false;
    }

    /* port 0 asks the OS to pick; find out what it picked. */
    sockaddr_in bound{};
    int bound_len = sizeof(bound);
    if (getsockname(listen_socket_, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        port_ = ntohs(bound.sin_port);
    } else {
        port_ = port;
    }

    running_.store(true);
    accept_thread_ = std::thread([this] { accept_loop(); });
    return true;
}

void RemoteServer::stop() {
    if (!running_.exchange(false)) return;

    if (listen_socket_ != INVALID_SOCKET) {
        /* Unblocks the accept() call in accept_loop(). */
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    port_ = 0;
}

void RemoteServer::accept_loop() {
    while (running_.load()) {
        sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);
        const SOCKET client = accept(listen_socket_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client == INVALID_SOCKET) {
            /* Either a real accept() error, or stop() closed the listening
             * socket out from under us — either way there is nothing to
             * serve, so re-check running_ and either retry or exit. */
            continue;
        }
        std::thread(handle_connection, client).detach();
    }
}

}  // namespace remote
