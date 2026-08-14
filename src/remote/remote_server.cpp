/*
 * mayhem-b200 — the web portal's HTTP server.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "remote_server.hpp"

#include "app_bridge.hpp"
#include "app_data.hpp"
#include "morse_tx.hpp"

#include "../radio/network_radio.hpp"  /* radio::net::json_parse for request bodies */

#include "../apps/app_registry.hpp"

#if !defined(_WIN32)
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <vector>

namespace remote {

namespace {

constexpr const char* kJson = "application/json; charset=utf-8";
constexpr const char* kOctetStream = "application/octet-stream";
constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr size_t kMaxBodyBytes = 1 * 1024 * 1024;
constexpr int kRecvTimeoutMs = 5000;
/* Contract cap on GET /api/screen's ?wait_ms=. AppBridge::screen_frame()
 * clamps to the same value; this one keeps a nonsense query from even being
 * parsed into something larger. */
constexpr unsigned long kMaxScreenWaitMs = 10000;

/* --- Platform socket compatibility -------------------------------------------
 *
 * Everything below this block — HTTP parsing, routing, the accept loop — is
 * platform-neutral. Only these few primitives differ between Winsock and BSD
 * sockets, so they are wrapped once here rather than #ifdef-ed at every call
 * site, which keeps the request path reading identically on both.
 */

#if defined(_WIN32)

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

using socklen_type = int;
/* The int-returning calls (bind/listen) report failure as -1 on both
 * platforms; SOCKET_ERROR is just Winsock's spelling of it. */
constexpr int kSocketError = SOCKET_ERROR;

bool socket_startup() { return wsa_init().ok(); }

std::string last_socket_error_suffix() {
    return " (WSA error " + std::to_string(WSAGetLastError()) + ")";
}

void set_reuse_addr(socket_t s) {
    const BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
}

void set_recv_timeout(socket_t s, int timeout_ms) {
    const DWORD tv = static_cast<DWORD>(timeout_ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
}

long socket_send(socket_t s, const char* data, size_t len) {
    return ::send(s, data, static_cast<int>(len), 0);
}

long socket_recv(socket_t s, char* data, size_t len) {
    return ::recv(s, data, static_cast<int>(len), 0);
}

void socket_finish_and_close(socket_t s) {
    shutdown(s, SD_SEND);
    closesocket(s);
}

/* closesocket() on a listening socket unblocks a concurrent accept(), so
 * stopping the server needs nothing more than closing it. */
void listener_close(socket_t s) { closesocket(s); }

#else

using socklen_type = socklen_t;
constexpr int kSocketError = -1;

/* Nothing to initialise: BSD sockets need no per-process startup call. */
bool socket_startup() { return true; }

std::string last_socket_error_suffix() {
    return " (errno " + std::to_string(errno) + ")";
}

void set_reuse_addr(socket_t s) {
    const int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
}

void set_recv_timeout(socket_t s, int timeout_ms) {
    /* SO_RCVTIMEO takes a struct timeval here, not the millisecond DWORD
     * Winsock takes. Passing the Windows shape would set a garbage timeout. */
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

long socket_send(socket_t s, const char* data, size_t len) {
    /* MSG_NOSIGNAL: a client that closes the connection before reading the
     * response must make send() fail with EPIPE, not raise SIGPIPE — whose
     * default disposition would terminate the whole application. */
    return static_cast<long>(::send(s, data, len, MSG_NOSIGNAL));
}

long socket_recv(socket_t s, char* data, size_t len) {
    return static_cast<long>(::recv(s, data, len, 0));
}

void socket_finish_and_close(socket_t s) {
    shutdown(s, SHUT_WR);
    ::close(s);
}

/* Unlike closesocket(), a bare close() does NOT wake a thread already blocked
 * in accept() on this socket — the accept stays parked and RemoteServer::stop()
 * would hang forever joining it. shutdown() does wake it (accept then fails
 * with EINVAL), so it has to come first. */
void listener_close(socket_t s) {
    shutdown(s, SHUT_RDWR);
    ::close(s);
}

#endif

const char* status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        default: return "Internal Server Error";
    }
}

struct HttpRequest {
    std::string method;
    std::string path;
    /* Everything after the '?', undecoded and without the '?' itself. Kept
     * rather than discarded because /api/screen's long poll is entirely
     * expressed in it: dropping the query string would silently turn every
     * "?after=N&wait_ms=M" into a bare "give me the current frame", and the
     * stream would spin instead of blocking with both sides' tests green. */
    std::string query;
    std::string body;
};

void send_response(socket_t s, int status, const char* content_type, const std::string& body) {
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
        const long n = socket_send(s, text.data() + static_cast<ptrdiff_t>(sent),
                                   text.size() - sent);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

/* 204 carries no body by definition (RFC 7230 section 3.3.3), so it gets no
 * Content-Length either — send_response() would emit "Content-Length: 0",
 * which browsers tolerate but which is not what a 204 means. */
void send_no_content(socket_t s) {
    static const std::string text =
        "HTTP/1.1 204 No Content\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n";
    size_t sent = 0;
    while (sent < text.size()) {
        const long n = socket_send(s, text.data() + static_cast<ptrdiff_t>(sent), text.size() - sent);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

void send_json_error(socket_t s, int status, const std::string& message) {
    JsonValue v = JsonValue::object();
    v.set("error", JsonValue::string(message));
    send_response(s, status, kJson, v.dump());
}

/* Reads one HTTP/1.1 request off `s`: the request line, headers (only
 * Content-Length is consulted) and body. Returns false on a malformed
 * request, an oversized header/body, a client that never sends a full
 * request, or a socket error/timeout. */
bool read_request(socket_t s, HttpRequest& out) {
    std::string buf;
    char chunk[4096];

    size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const long n = socket_recv(s, chunk, sizeof(chunk));
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
    if (query != std::string::npos) {
        out.query = out.path.substr(query + 1);
        out.path = out.path.substr(0, query);
    }

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
        const long n = socket_recv(s, chunk, sizeof(chunk));
        if (n <= 0) break;
        out.body.append(chunk, static_cast<size_t>(n));
    }
    if (out.body.size() > content_length) out.body.resize(content_length);

    return true;
}

/* Value of `name` in an "a=1&b=2" query string, or an empty string when it is
 * absent or has no value. No percent-decoding: the only parameters this server
 * reads are decimal integers, and a caller that percent-encodes a digit gets
 * the same answer as one that sends garbage — the default. */
std::string query_param(const std::string& query, const std::string& name) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();

        const size_t eq = query.find('=', pos);
        if (eq != std::string::npos && eq < amp && query.compare(pos, eq - pos, name) == 0)
            return query.substr(eq + 1, amp - eq - 1);

        pos = amp + 1;
    }
    return {};
}

/* Decimal parse with a caller-supplied default for "absent", "not a number"
 * and "out of range" alike — a malformed ?after= is not worth a 400 when the
 * honest fallback (start from the beginning / do not wait) is well defined. */
unsigned long query_param_ulong(const std::string& query, const std::string& name,
                                unsigned long fallback, unsigned long max_value) {
    const std::string raw = query_param(query, name);
    if (raw.empty()) return fallback;
    for (char c : raw)
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) return fallback;

    errno = 0;
    char* end = nullptr;
    const unsigned long v = std::strtoul(raw.c_str(), &end, 10);
    if (end == raw.c_str() || errno == ERANGE || v > max_value) return fallback;
    return v;
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

void route(socket_t client, const HttpRequest& req) {
    auto& bridge = AppBridge::instance();

    if (req.method == "GET" && req.path == "/api/apps") {
        send_response(client, 200, kJson, bridge.apps_json());
    } else if (req.method == "GET" && req.path == "/api/apps/current") {
        send_response(client, 200, kJson, bridge.current_app_json());
    } else if (req.method == "GET" && req.path == "/api/panel") {
        /* ?have_image_rev=N is the client telling us which image rev it is
         * already showing, so an unchanged picture costs a few hundred bytes
         * instead of re-sending its base64 every poll. Absent or malformed
         * means "the client has nothing" — the same answer as before this
         * parameter existed, which is what makes ignoring it safe rather than
         * merely tolerable. */
        const uint32_t have_image_rev = static_cast<uint32_t>(
            query_param_ulong(req.query, "have_image_rev", 0, 0xFFFFFFFFul));
        send_response(client, 200, kJson, bridge.panel_json(have_image_rev));
    } else if (req.method == "GET" && req.path == "/api/status") {
        send_response(client, 200, kJson, bridge.status_json());
    } else if (req.method == "GET" && req.path == "/api/screen") {
        /* Blocking is deliberate and safe here: every connection already has
         * its own thread (see accept_loop()), and screen_frame() parks on its
         * own condition variable holding nothing another handler needs.
         * RemoteServer::stop() releases anyone still waiting. */
        const uint32_t after = static_cast<uint32_t>(
            query_param_ulong(req.query, "after", 0, 0xFFFFFFFFul));
        /* Parsed wide, then CLAMPED — not rejected. "cap 10000" means an
         * over-long wait is shortened to the cap; treating it as malformed and
         * falling back to 0 would turn a client asking to block for a minute
         * into a client polling flat out, which is the opposite of what it
         * asked for. (Found by driving a live --portal with wait_ms=60000: it
         * answered 204 in 4 ms.) */
        const unsigned long requested_wait =
            query_param_ulong(req.query, "wait_ms", 0, 0xFFFFFFFFul);
        const int wait_ms = static_cast<int>(
            requested_wait > kMaxScreenWaitMs ? kMaxScreenWaitMs : requested_wait);

        std::string frame;
        if (bridge.screen_frame(after, wait_ms, frame))
            send_response(client, 200, kOctetStream, frame);
        else
            send_no_content(client); /* nothing captured yet, or the wait expired */
    } else if (req.method == "POST" && req.path == "/api/input") {
        std::vector<RemoteInput> events;
        size_t dropped = 0;
        if (!parse_input_events(req.body, events, dropped)) {
            send_json_error(client, 400, "expected {\"events\":[...]}");
        } else {
            /* 200 rather than the 202 the launch routes use: those promise an
             * action for a later frame, this reports a count that is already
             * final. Events an older or newer client sent that this build does
             * not understand are in `dropped`, not an error — see
             * parse_input_events(). */
            bridge.queue_input(events);

            /* `dropped` is about THIS request's events and nothing else. The
             * queue evicts oldest-first, so a full queue discards events from
             * earlier requests that were already, truthfully, reported as
             * queued — folding those in here would tell a client that the
             * five keys it just sent were refused when all five were taken.
             * A request longer than the whole queue is the one case where it
             * really does lose events of its own, and those are counted. */
            const size_t self_evicted = events.size() > AppBridge::kMaxQueuedInputs
                                            ? events.size() - AppBridge::kMaxQueuedInputs
                                            : 0;
            JsonValue v = JsonValue::object();
            v.set("queued", JsonValue::number(static_cast<double>(events.size() - self_evicted)));
            v.set("dropped", JsonValue::number(static_cast<double>(dropped + self_evicted)));
            send_response(client, 200, kJson, v.dump());
        }
    } else if (req.method == "POST" && req.path == "/api/morse/transmit") {
        /* The one panel path that keys the radio. Parse {text, wpm}; the
         * safety gates (a transmit-capable, idle radio; a legal frequency;
         * text that encodes) live in morse_tx_request, which only QUEUES —
         * the actual keying is on the UI thread in morse_tx_tick. */
        radio::net::JsonValue root;
        std::string parse_err;
        if (!radio::net::json_parse(req.body, root, parse_err)) {
            send_json_error(client, 400, "expected {\"text\":\"...\",\"wpm\":N}");
        } else {
            const auto* t = root.find("text");
            const auto* w = root.find("wpm");
            const std::string text = (t != nullptr) ? t->as_string("") : "";
            const uint16_t wpm =
                (w != nullptr) ? static_cast<uint16_t>(w->as_number(18.0)) : 18;

            const MorseTxResult r = morse_tx_request(text, wpm);
            JsonValue v = JsonValue::object();
            v.set("ok", JsonValue::boolean(r.ok));
            if (r.ok) {
                v.set("duration_ms", JsonValue::integer(static_cast<int64_t>(r.duration_ms)));
                v.set("frequency_hz", JsonValue::integer(static_cast<int64_t>(r.frequency_hz)));
            } else {
                v.set("error", JsonValue::string(r.error));
            }
            /* 200 with ok:false for a refusal (no radio, busy, bad freq): the
             * request was well-formed and answered, the browser branches on
             * ok. A malformed body is the only 4xx above. */
            send_response(client, 200, kJson, v.dump());
        }
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
void handle_connection(socket_t client) {
    set_recv_timeout(client, kRecvTimeoutMs);

    HttpRequest req;
    if (read_request(client, req)) {
        route(client, req);
    } else {
        send_json_error(client, 400, "bad request");
    }

    socket_finish_and_close(client);
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
    if (!socket_startup()) {
        last_error_ = "WSAStartup failed";
        return false;
    }

    listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket_ == kInvalidSocket) {
        last_error_ = "socket() failed" + last_socket_error_suffix();
        return false;
    }

    /* So a restart (e.g. after a crash-free process relaunch) can reuse the
     * port immediately instead of sitting in TIME_WAIT. */
    set_reuse_addr(listen_socket_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == kSocketError) {
        last_error_ = "bind() failed on port " + std::to_string(port) + last_socket_error_suffix();
        listener_close(listen_socket_);
        listen_socket_ = kInvalidSocket;
        return false;
    }

    if (listen(listen_socket_, SOMAXCONN) == kSocketError) {
        last_error_ = "listen() failed" + last_socket_error_suffix();
        listener_close(listen_socket_);
        listen_socket_ = kInvalidSocket;
        return false;
    }

    /* port 0 asks the OS to pick; find out what it picked. */
    sockaddr_in bound{};
    socklen_type bound_len = sizeof(bound);
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

    /* Detached /api/screen handlers may each be parked for up to 10 s. stop()
     * does not join them, but leaving them parked delays process exit and
     * leaves clients hanging on a server that is gone — so release them first,
     * with a 204, which is exactly what they would send on a timeout anyway. */
    AppBridge::instance().cancel_screen_waits();

    if (listen_socket_ != kInvalidSocket) {
        /* Unblocks the accept() call in accept_loop(). */
        listener_close(listen_socket_);
        listen_socket_ = kInvalidSocket;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    port_ = 0;
}

void RemoteServer::accept_loop() {
    while (running_.load()) {
        sockaddr_in client_addr{};
        socklen_type addr_len = sizeof(client_addr);
        const socket_t client = accept(listen_socket_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client == kInvalidSocket) {
            /* Either a real accept() error, or stop() closed the listening
             * socket out from under us — either way there is nothing to
             * serve, so re-check running_ and either retry or exit. */
            continue;
        }
        std::thread(handle_connection, client).detach();
    }
}

}  // namespace remote
