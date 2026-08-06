/*
 * mayhem-b200 — sdrlink network radio backend.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "network_radio.hpp"

#include "../dsp/demod.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace radio {
namespace net {

/* --- JsonValue -------------------------------------------------------------- */

const JsonValue* JsonValue::find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : object_value)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

/* --- JSON parsing ------------------------------------------------------------
 *
 * A small recursive-descent parser covering exactly what PROTOCOL.md section 2
 * needs: object, array, string (with the standard escapes), number (including
 * scientific notation — the DeviceCaps example uses "42e6"), true/false/null.
 * Unrecognised object keys are simply left in JsonValue::object_value and never
 * looked up by name, which is how an unknown field gets ignored per section 5
 * without any special-case code.
 */

namespace {

class JsonParser {
   public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    bool parse(JsonValue& out, std::string& error) {
        skip_ws();
        return parse_value(out, error);
    }

   private:
    const std::string& text_;
    size_t pos_{0};

    bool at_end() const { return pos_ >= text_.size(); }
    char peek() const { return text_[pos_]; }

    void skip_ws() {
        while (!at_end()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            pos_++;
        }
    }

    bool parse_value(JsonValue& out, std::string& error) {
        skip_ws();
        if (at_end()) {
            error = "unexpected end of input";
            return false;
        }
        switch (peek()) {
            case '{': return parse_object(out, error);
            case '[': return parse_array(out, error);
            case '"': return parse_string_value(out, error);
            case 't':
            case 'f': return parse_bool(out, error);
            case 'n': return parse_null(out, error);
            default: return parse_number(out, error);
        }
    }

    bool parse_object(JsonValue& out, std::string& error) {
        pos_++; /* '{' */
        out = JsonValue{};
        out.type = JsonValue::Type::Object;
        skip_ws();
        if (!at_end() && peek() == '}') {
            pos_++;
            return true;
        }
        for (;;) {
            skip_ws();
            if (at_end() || peek() != '"') {
                error = "expected object key";
                return false;
            }
            std::string key;
            if (!parse_raw_string(key, error)) return false;
            skip_ws();
            if (at_end() || peek() != ':') {
                error = "expected ':'";
                return false;
            }
            pos_++;
            JsonValue value;
            if (!parse_value(value, error)) return false;
            out.object_value.emplace_back(std::move(key), std::move(value));
            skip_ws();
            if (at_end()) {
                error = "unterminated object";
                return false;
            }
            if (peek() == ',') {
                pos_++;
                continue;
            }
            if (peek() == '}') {
                pos_++;
                break;
            }
            error = "expected ',' or '}'";
            return false;
        }
        return true;
    }

    bool parse_array(JsonValue& out, std::string& error) {
        pos_++; /* '[' */
        out = JsonValue{};
        out.type = JsonValue::Type::Array;
        skip_ws();
        if (!at_end() && peek() == ']') {
            pos_++;
            return true;
        }
        for (;;) {
            JsonValue value;
            if (!parse_value(value, error)) return false;
            out.array_value.push_back(std::move(value));
            skip_ws();
            if (at_end()) {
                error = "unterminated array";
                return false;
            }
            if (peek() == ',') {
                pos_++;
                continue;
            }
            if (peek() == ']') {
                pos_++;
                break;
            }
            error = "expected ',' or ']'";
            return false;
        }
        return true;
    }

    bool parse_string_value(JsonValue& out, std::string& error) {
        std::string s;
        if (!parse_raw_string(s, error)) return false;
        out = JsonValue{};
        out.type = JsonValue::Type::String;
        out.string_value = std::move(s);
        return true;
    }

    bool parse_raw_string(std::string& out, std::string& error) {
        pos_++; /* opening quote */
        out.clear();
        for (;;) {
            if (at_end()) {
                error = "unterminated string";
                return false;
            }
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_end()) {
                error = "unterminated escape";
                return false;
            }
            const char esc = text_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) {
                        error = "bad \\u escape";
                        return false;
                    }
                    unsigned code = 0;
                    for (int i = 0; i < 4; i++) {
                        const char h = text_[pos_++];
                        code <<= 4;
                        if (h >= '0' && h <= '9')
                            code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f')
                            code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F')
                            code |= static_cast<unsigned>(h - 'A' + 10);
                        else {
                            error = "bad \\u escape";
                            return false;
                        }
                    }
                    /* Everything sdrlink actually sends through this path (ids,
                     * device names, error text) is ASCII/BMP; a plain UTF-8
                     * encode of the code point covers it without carrying
                     * surrogate-pair handling this client never needs. */
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default:
                    error = "bad escape";
                    return false;
            }
        }
    }

    bool parse_bool(JsonValue& out, std::string& error) {
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out = JsonValue{};
            out.type = JsonValue::Type::Bool;
            out.bool_value = true;
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out = JsonValue{};
            out.type = JsonValue::Type::Bool;
            out.bool_value = false;
            return true;
        }
        error = "expected true/false";
        return false;
    }

    bool parse_null(JsonValue& out, std::string& error) {
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            out = JsonValue{};
            out.type = JsonValue::Type::Null;
            return true;
        }
        error = "expected null";
        return false;
    }

    bool parse_number(JsonValue& out, std::string& error) {
        const size_t start = pos_;
        if (!at_end() && peek() == '-') pos_++;
        if (at_end() || !std::isdigit(static_cast<unsigned char>(peek()))) {
            error = "expected number";
            return false;
        }
        while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) pos_++;
        if (!at_end() && peek() == '.') {
            pos_++;
            if (at_end() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                error = "malformed number";
                return false;
            }
            while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) pos_++;
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            pos_++;
            if (!at_end() && (peek() == '+' || peek() == '-')) pos_++;
            if (at_end() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                error = "malformed exponent";
                return false;
            }
            while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) pos_++;
        }
        const std::string token = text_.substr(start, pos_ - start);
        out = JsonValue{};
        out.type = JsonValue::Type::Number;
        out.number_value = std::strtod(token.c_str(), nullptr);
        return true;
    }
};

}  // namespace

bool json_parse(const std::string& text, JsonValue& out, std::string& error) {
    JsonParser parser(text);
    return parser.parse(out, error);
}

std::string format_json_number(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        return std::string{buf};
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string{buf};
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

/* --- Sample formats, PROTOCOL.md section 3 ---------------------------------- */

bool sample_format_from_string(const std::string& s, SampleFormat& out) {
    if (s == "cf32") {
        out = SampleFormat::Cf32;
        return true;
    }
    if (s == "ci16") {
        out = SampleFormat::Ci16;
        return true;
    }
    if (s == "ci8") {
        out = SampleFormat::Ci8;
        return true;
    }
    return false;
}

const char* sample_format_to_string(SampleFormat f) {
    switch (f) {
        case SampleFormat::Cf32: return "cf32";
        case SampleFormat::Ci16: return "ci16";
        case SampleFormat::Ci8: return "ci8";
    }
    return "cf32";
}

size_t bytes_per_sample(SampleFormat f) {
    switch (f) {
        case SampleFormat::Cf32: return 8;
        case SampleFormat::Ci16: return 4;
        case SampleFormat::Ci8: return 2;
    }
    return 8;
}

/* --- Frame header, PROTOCOL.md section 3 ------------------------------------ */

bool parse_frame_header(const uint8_t* data, size_t len, FrameHeader& out) {
    if (data == nullptr || len < FrameHeader::kSize) return false;
    if (data[0] != 0x53 || data[1] != 0x44 || data[2] != 0x52 || data[3] != 0x4B) return false; /* "SDRK" */

    auto read_u32 = [data](size_t off) -> uint32_t {
        return static_cast<uint32_t>(data[off]) | (static_cast<uint32_t>(data[off + 1]) << 8) |
               (static_cast<uint32_t>(data[off + 2]) << 16) | (static_cast<uint32_t>(data[off + 3]) << 24);
    };
    auto read_u64 = [&](size_t off) -> uint64_t {
        return static_cast<uint64_t>(read_u32(off)) | (static_cast<uint64_t>(read_u32(off + 4)) << 32);
    };

    out.seq = read_u32(4);
    out.timestamp_ns = read_u64(8);
    out.samples = read_u32(16);
    out.flags = read_u32(20);
    return true;
}

namespace {
constexpr float clamp_unit(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }
}  // namespace

void convert_samples(const uint8_t* payload, size_t count, SampleFormat format, cfloat* out) {
    switch (format) {
        case SampleFormat::Cf32: {
            for (size_t i = 0; i < count; i++) {
                float i_val = 0.0f;
                float q_val = 0.0f;
                std::memcpy(&i_val, payload + i * 8, sizeof(float));
                std::memcpy(&q_val, payload + i * 8 + 4, sizeof(float));
                out[i] = cfloat{i_val, q_val};
            }
            break;
        }
        case SampleFormat::Ci16: {
            for (size_t i = 0; i < count; i++) {
                const size_t off = i * 4;
                const int16_t iv = static_cast<int16_t>(static_cast<uint16_t>(payload[off]) |
                                                          (static_cast<uint16_t>(payload[off + 1]) << 8));
                const int16_t qv = static_cast<int16_t>(static_cast<uint16_t>(payload[off + 2]) |
                                                          (static_cast<uint16_t>(payload[off + 3]) << 8));
                /* Full scale is 32767 per PROTOCOL.md section 3, so the most
                 * negative value (-32768) divides out to just past -1.0;
                 * clamp rather than hand the DSP chain a sample that overshoots
                 * unit magnitude. */
                out[i] = cfloat{clamp_unit(static_cast<float>(iv) / 32767.0f),
                                 clamp_unit(static_cast<float>(qv) / 32767.0f)};
            }
            break;
        }
        case SampleFormat::Ci8: {
            for (size_t i = 0; i < count; i++) {
                const size_t off = i * 2;
                const int8_t iv = static_cast<int8_t>(payload[off]);
                const int8_t qv = static_cast<int8_t>(payload[off + 1]);
                out[i] = cfloat{clamp_unit(static_cast<float>(iv) / 127.0f),
                                 clamp_unit(static_cast<float>(qv) / 127.0f)};
            }
            break;
        }
    }
}

uint32_t frames_dropped_between(uint32_t last_seq, uint32_t seq) {
    const uint32_t gap = seq - last_seq - 1u; /* wraps by design */
    return gap <= kMaxSaneSeqGap ? gap : 0u;
}

/* --- Socket implementation ---------------------------------------------------
 *
 * WinsockSocket is the only piece of this backend that touches ws2_32. A POSIX
 * port later replaces this block (and CMakeLists' ws2_32 link) with a BSD
 * sockets equivalent of the same Socket interface — nothing else in this file
 * or in NetworkRadio changes.
 */

#if defined(_WIN32)

namespace {

/* WSAStartup/WSACleanup are refcounted by Winsock itself, but calling them
 * from every connect() would still race two NetworkRadio instances starting
 * up together; a function-local static keeps it to exactly one pair for the
 * process. */
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

class WinsockSocket : public Socket {
   public:
    WinsockSocket() = default;
    ~WinsockSocket() override { close(); }

    WinsockSocket(const WinsockSocket&) = delete;
    WinsockSocket& operator=(const WinsockSocket&) = delete;

    bool connect(const std::string& host, uint16_t port, int timeout_ms, std::string& error) override {
        close();
        if (!wsa_init().ok()) {
            error = "sdrlink: WSAStartup failed";
            return false;
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* results = nullptr;
        const std::string port_str = std::to_string(port);
        const int gai = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &results);
        if (gai != 0 || results == nullptr) {
            error = "sdrlink: cannot resolve " + host + " (getaddrinfo error " + std::to_string(gai) + ")";
            return false;
        }

        bool connected = false;
        for (addrinfo* p = results; p != nullptr && !connected; p = p->ai_next) {
            const SOCKET s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (s == INVALID_SOCKET) continue;

            /* Non-blocking connect + select(), so an unreachable host fails in
             * timeout_ms instead of the OS's own multi-minute TCP SYN retry
             * budget — the requirement that open() never hangs. */
            u_long nonblocking = 1;
            ioctlsocket(s, FIONBIO, &nonblocking);

            const int rc = ::connect(s, p->ai_addr, static_cast<int>(p->ai_addrlen));
            bool ok = (rc == 0);
            if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set write_set;
                fd_set err_set;
                FD_ZERO(&write_set);
                FD_ZERO(&err_set);
                FD_SET(s, &write_set);
                FD_SET(s, &err_set);
                timeval tv{};
                tv.tv_sec = timeout_ms / 1000;
                tv.tv_usec = (timeout_ms % 1000) * 1000;

                const int sel = select(0, nullptr, &write_set, &err_set, &tv);
                if (sel > 0 && FD_ISSET(s, &write_set) && !FD_ISSET(s, &err_set)) {
                    int soerr = 0;
                    int soerr_len = sizeof(soerr);
                    if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &soerr_len) == 0 &&
                        soerr == 0) {
                        ok = true;
                    }
                }
            }

            if (!ok) {
                closesocket(s);
                continue;
            }

            u_long blocking = 0;
            ioctlsocket(s, FIONBIO, &blocking);
            sock_ = s;
            connected = true;
        }

        freeaddrinfo(results);

        if (!connected) {
            error = "sdrlink: could not connect to " + host + ":" + port_str;
            return false;
        }

        /* Control traffic is one small request/reply at a time; Nagle would
         * only add latency here, never save bandwidth worth having. */
        const BOOL nodelay = TRUE;
        setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        set_recv_timeout(timeout_ms);
        return true;
    }

    void close() override {
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }

    bool is_open() const override { return sock_ != INVALID_SOCKET; }

    bool send_all(const void* data, size_t len) override {
        if (sock_ == INVALID_SOCKET) return false;
        const char* p = static_cast<const char*>(data);
        size_t sent = 0;
        while (sent < len) {
            const int n = ::send(sock_, p + sent, static_cast<int>(len - sent), 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    long recv_some(void* data, size_t len) override {
        if (sock_ == INVALID_SOCKET) return kError;
        const int n = ::recv(sock_, static_cast<char*>(data), static_cast<int>(len), 0);
        if (n > 0) return n;
        if (n == 0) return kClosed;
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) return kTimeout;
        return kError;
    }

    void set_recv_timeout(int timeout_ms) override {
        if (sock_ == INVALID_SOCKET) return;
        const DWORD tv = static_cast<DWORD>(timeout_ms);
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    }

   private:
    SOCKET sock_{INVALID_SOCKET};
};

}  // namespace

std::unique_ptr<Socket> make_platform_socket() { return std::make_unique<WinsockSocket>(); }

#else

std::unique_ptr<Socket> make_platform_socket() {
    /* Not built yet — the Socket interface above is exactly what a POSIX BSD
     * sockets implementation would fill in. See the header comment. */
    return nullptr;
}

#endif

}  // namespace net

/* --- NetworkRadio ------------------------------------------------------------ */

namespace {

constexpr int kConnectTimeoutMs = 3000;
constexpr int kControlRecvTimeoutMs = 500;

/* How long to wait for a reply. Most commands are a register write on the far
 * side and answer immediately, so a short deadline keeps a dead server from
 * hanging the UI. `open` is the exception: bringing up a USRP means loading an
 * FPGA image and calibrating, which measurably takes 8-10 s on a B200 over
 * USB 2 — a 5 s deadline aborts a perfectly healthy open half way through. */
constexpr int kControlOverallTimeoutMs = 5000;
constexpr int kOpenOverallTimeoutMs = 30000;
constexpr int kStreamRecvTimeoutMs = 200; /* mirrors UsrpRadio's kRecvTimeout responsiveness */

Range range_from_json(const net::JsonValue* v) {
    Range r;
    if (v == nullptr || !v->is_object()) return r;
    if (const auto* mn = v->find("min")) r.min = mn->as_number(0.0);
    if (const auto* mx = v->find("max")) r.max = mx->as_number(0.0);
    if (const auto* st = v->find("step")) r.step = st->as_number(0.0);
    return r;
}

std::vector<std::string> string_array_from_json(const net::JsonValue* v) {
    std::vector<std::string> out;
    if (v == nullptr || !v->is_array()) return out;
    out.reserve(v->array_value.size());
    for (const auto& item : v->array_value) out.push_back(item.as_string());
    return out;
}

DeviceCaps caps_from_json(const net::JsonValue& obj) {
    DeviceCaps c;

    const auto* label = obj.find("label");
    const auto* driver = obj.find("driver");
    c.mboard = label != nullptr ? label->as_string()
                                 : (driver != nullptr ? driver->as_string() : std::string{"sdrlink remote"});
    if (const auto* serial = obj.find("serial")) c.serial = serial->as_string();

    c.rx_freq = range_from_json(obj.find("rx_freq"));
    c.tx_freq = range_from_json(obj.find("tx_freq"));
    c.rx_gain = range_from_json(obj.find("rx_gain"));
    c.tx_gain = range_from_json(obj.find("tx_gain"));
    c.rx_rate = range_from_json(obj.find("rx_rate"));
    c.tx_rate = range_from_json(obj.find("tx_rate"));
    c.rx_bandwidth = range_from_json(obj.find("rx_bandwidth"));
    c.tx_bandwidth = range_from_json(obj.find("tx_bandwidth"));

    /* PROTOCOL.md section 2.2's own example omits tx_rate/tx_bandwidth
     * entirely. Most full-duplex radios (the B200 this project targets
     * included) share one clock and one analog filter design across RX and
     * TX, so falling back to the RX range is the least surprising default
     * when the field is genuinely absent; a server that does send its own
     * tx_rate/tx_bandwidth always wins, since range_from_json only returns an
     * empty Range{} (max<=min) when the key was missing. */
    if (c.tx_rate.max <= c.tx_rate.min) c.tx_rate = c.rx_rate;
    if (c.tx_bandwidth.max <= c.tx_bandwidth.min) c.tx_bandwidth = c.rx_bandwidth;

    if (const auto* mcr = obj.find("master_clock_rate")) c.master_clock_rate = mcr->as_number(0.0);

    c.rx_antennas = string_array_from_json(obj.find("rx_antennas"));
    c.tx_antennas = string_array_from_json(obj.find("tx_antennas"));

    if (const auto* hr = obj.find("has_rx")) c.has_rx = hr->as_bool(true);
    if (const auto* ht = obj.find("has_tx")) c.has_tx = ht->as_bool(true);
    if (const auto* fd = obj.find("full_duplex")) c.full_duplex = fd->as_bool(true);

    /* The protocol has no per-channel concept — every command implicitly
     * addresses "the" opened device — so this is always a single channel,
     * the same simplification UsrpRadio makes by pinning itself to one
     * channel even on a multi-channel B210. */
    c.rx_channels = 1;
    c.tx_channels = c.has_tx ? 1 : 0;

    return c;
}

/* Reads exactly `len` bytes from `sock`, looping over short reads and
 * transport-level timeouts. Returns false (without necessarily having read
 * anything) on a closed connection, a hard socket error, or `stop` being
 * raised — the last of those is what keeps stop_rx() responsive instead of
 * waiting out the current read. */
bool read_exact(net::Socket& sock, uint8_t* buf, size_t len, const std::atomic<bool>& stop) {
    size_t got = 0;
    while (got < len) {
        if (stop.load()) return false;
        const long n = sock.recv_some(buf + got, len - got);
        if (n == net::Socket::kTimeout) continue;
        if (n == net::Socket::kClosed || n == net::Socket::kError) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

NetworkRadio::NetworkRadio() : NetworkRadio(net::make_platform_socket) {}

NetworkRadio::NetworkRadio(net::SocketFactory socket_factory) : socket_factory_(std::move(socket_factory)) {
    caps_.mboard = "sdrlink (not connected)";
    /* Unlike UsrpRadio there is no published fallback spec here — sdrlink can
     * front any device the server chooses to expose — so every Range stays at
     * Range{} (min==max==0, which Range::clamp treats as "don't clamp", see
     * radio_device.cpp) until open() reads the real DeviceCaps back. */
}

NetworkRadio::~NetworkRadio() { close(); }

bool NetworkRadio::open(const std::string& args) {
    close();

    /* Accepted forms:
     *   host
     *   host:port
     *   host:port/<remote device args>     e.g. 127.0.0.1:5960/driver=uhd,serial=X
     * Everything after the first '/' is passed straight through to the server's
     * open command to select a device on that machine; the server picks its
     * default when it is empty. */
    std::string endpoint = args;
    std::string remote_args;
    const auto slash = args.find('/');
    if (slash != std::string::npos) {
        endpoint = args.substr(0, slash);
        remote_args = args.substr(slash + 1);
    }

    std::string host = endpoint;
    uint16_t port = 5960;
    const auto colon = endpoint.find_last_of(':');
    if (colon != std::string::npos) {
        host = endpoint.substr(0, colon);
        const std::string port_str = endpoint.substr(colon + 1);
        int parsed = -1;
        try {
            parsed = std::stoi(port_str);
        } catch (const std::exception&) {
            parsed = -1;
        }
        if (parsed <= 0 || parsed > 65535) {
            last_error_ = "sdrlink: bad port in \"" + endpoint + "\"";
            return false;
        }
        port = static_cast<uint16_t>(parsed);
    }
    if (host.empty()) host = "127.0.0.1";

    auto sock = socket_factory_();
    if (!sock) {
        last_error_ = "sdrlink: no socket implementation available on this platform";
        return false;
    }
    std::string err;
    if (!sock->connect(host, port, kConnectTimeoutMs, err)) {
        last_error_ = err.empty() ? "sdrlink: connect failed" : err;
        return false;
    }
    sock->set_recv_timeout(kControlRecvTimeoutMs);

    control_ = std::move(sock);
    control_leftover_.clear();
    host_ = host;
    control_port_ = port;
    next_id_ = 1;

    net::JsonValue result;
    if (!send_request("hello", "{\"client\":\"mayhem-b200\",\"proto\":1}", result, err)) {
        last_error_ = "sdrlink: hello failed: " + err;
        control_->close();
        control_.reset();
        return false;
    }
    const net::JsonValue* sid = result.find("session_id");
    if (sid == nullptr) {
        last_error_ = "sdrlink: hello reply missing session_id";
        control_->close();
        control_.reset();
        return false;
    }
    session_id_ = sid->as_string();

    /* Empty device-selection args asks the server to open its default
     * device, the same convention UsrpRadio uses for an empty args string. */
    std::string open_args = "{\"args\":\"" + net::json_escape(remote_args) + "\"}";
    if (!send_request("open", open_args, result, err, kOpenOverallTimeoutMs)) {
        last_error_ = "sdrlink: open failed: " + err;
        control_->close();
        control_.reset();
        return false;
    }
    const net::JsonValue* caps_json = result.find("caps");
    if (caps_json == nullptr || !caps_json->is_object()) {
        last_error_ = "sdrlink: open reply missing caps";
        control_->close();
        control_.reset();
        return false;
    }
    caps_ = caps_from_json(*caps_json);

    rx_ring_ = std::make_unique<dsp::RingBuffer<cfloat>>(ring_capacity_);
    tx_ring_ = std::make_unique<dsp::RingBuffer<cfloat>>(ring_capacity_);

    /* Nothing has been explicitly set yet; seed the cached values from caps
     * rather than guessing at get_state's field names, which PROTOCOL.md
     * section 2.1 leaves unspecified ("{...} all current settings"). The
     * first real values arrive the moment the caller's setters run — exactly
     * how main.cpp drives a freshly opened UsrpRadio too. */
    rx_antenna_ = caps_.rx_antennas.empty() ? std::string{} : caps_.rx_antennas.front();
    tx_antenna_ = caps_.tx_antennas.empty() ? std::string{} : caps_.tx_antennas.front();
    rx_rate_ = caps_.rx_rate.min;
    tx_rate_ = caps_.tx_rate.min;
    rx_freq_ = caps_.rx_freq.min;
    tx_freq_ = caps_.tx_freq.min;
    rx_gain_ = caps_.rx_gain.min;
    tx_gain_ = caps_.tx_gain.min;
    rx_bw_ = caps_.rx_bandwidth.min;
    tx_bw_ = caps_.tx_bandwidth.min;

    stats_.reset();
    last_error_.clear();
    open_.store(true);
    return true;
}

void NetworkRadio::close() {
    stop_rx();

    std::lock_guard<std::mutex> g{config_mutex_};
    if (control_) {
        if (open_.load()) {
            net::JsonValue result;
            std::string err;
            send_request("close", "", result, err); /* best-effort */
        }
        control_->close();
        control_.reset();
    }
    session_id_.clear();
    open_.store(false);
}

bool NetworkRadio::read_line(net::Socket& sock, std::string& leftover, std::string& line, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto nl = leftover.find('\n');
        if (nl != std::string::npos) {
            line = leftover.substr(0, nl);
            leftover.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;

        char buf[4096];
        const long n = sock.recv_some(buf, sizeof(buf));
        if (n == net::Socket::kTimeout) continue;
        if (n == net::Socket::kClosed || n == net::Socket::kError) return false;
        leftover.append(buf, static_cast<size_t>(n));
    }
}

bool NetworkRadio::send_request(const std::string& cmd, const std::string& args_json, net::JsonValue& result,
                                 std::string& error, int timeout_ms) {
    if (!control_ || !control_->is_open()) {
        error = "sdrlink: control connection not open";
        return false;
    }

    const int id = next_id_++;
    std::string line = "{\"id\":" + std::to_string(id) + ",\"cmd\":\"" + net::json_escape(cmd) + "\"";
    if (!args_json.empty()) line += ",\"args\":" + args_json;
    line += "}\n";

    if (!control_->send_all(line.data(), line.size())) {
        error = "sdrlink: send failed (connection lost)";
        control_->close();
        return false;
    }

    for (;;) {
        std::string reply_line;
        if (!read_line(*control_, control_leftover_, reply_line, timeout_ms)) {
            error = "sdrlink: connection lost waiting for a reply";
            control_->close();
            return false;
        }

        net::JsonValue reply;
        std::string parse_err;
        if (!net::json_parse(reply_line, reply, parse_err)) {
            /* PROTOCOL.md section 4 has the server keep the connection alive
             * through a malformed line rather than disconnect; skipping a
             * line we can't parse (instead of failing the whole request)
             * follows the same spirit. */
            continue;
        }

        const net::JsonValue* id_field = reply.find("id");
        if (id_field == nullptr) continue; /* an unsolicited event; nothing here consumes it yet */
        if (static_cast<int>(id_field->as_number(-1)) != id) continue; /* a reply to an earlier request */

        const net::JsonValue* ok_field = reply.find("ok");
        const bool ok = ok_field != nullptr && ok_field->as_bool(false);
        if (!ok) {
            const net::JsonValue* err_field = reply.find("error");
            error = err_field != nullptr ? err_field->as_string("(no error message)") : "(server did not say why)";
            return false;
        }

        const net::JsonValue* result_field = reply.find("result");
        if (result_field != nullptr) result = *result_field;
        return true;
    }
}

double NetworkRadio::set_master_clock_rate(double rate_hz) {
    (void)rate_hz;
    /* PROTOCOL.md section 2.1 has no command for this — it is not something
     * the wire protocol exposes, so honestly report that rather than fake
     * acceptance. */
    last_error_ = "sdrlink: master clock rate is not configurable over the network protocol";
    return caps_.master_clock_rate;
}

double NetworkRadio::set_rx_rate(double rate_hz) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!control_ || !open_.load()) {
        rx_rate_ = caps_.rx_rate.clamp(rate_hz);
        return rx_rate_;
    }
    net::JsonValue result;
    std::string err;
    const std::string args = "{\"hz\":" + net::format_json_number(rate_hz) + "}";
    if (!send_request("set_rx_rate", args, result, err)) {
        last_error_ = err;
        return rx_rate_;
    }
    if (const auto* hz = result.find("hz")) rx_rate_ = hz->as_number(rx_rate_);
    return rx_rate_;
}

double NetworkRadio::set_tx_rate(double rate_hz) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!control_ || !open_.load()) {
        tx_rate_ = caps_.tx_rate.clamp(rate_hz);
        return tx_rate_;
    }
    net::JsonValue result;
    std::string err;
    const std::string args = "{\"hz\":" + net::format_json_number(rate_hz) + "}";
    if (!send_request("set_tx_rate", args, result, err)) {
        last_error_ = err;
        return tx_rate_;
    }
    if (const auto* hz = result.find("hz")) tx_rate_ = hz->as_number(tx_rate_);
    return tx_rate_;
}

double NetworkRadio::set_rx_frequency(double freq_hz) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!control_ || !open_.load()) {
        rx_freq_ = caps_.rx_freq.clamp(freq_hz);
        return rx_freq_;
    }
    net::JsonValue result;
    std::string err;
    const std::string args = "{\"hz\":" + net::format_json_number(freq_hz) + "}";
    if (!send_request("set_rx_freq", args, result, err)) {
        last_error_ = err;
        return rx_freq_;
    }
    if (const auto* hz = result.find("hz")) rx_freq_ = hz->as_number(rx_freq_);
    return rx_freq_;
}

double NetworkRadio::set_tx_frequency(double freq_hz) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!control_ || !open_.load()) {
        tx_freq_ = caps_.tx_freq.clamp(freq_hz);
        return tx_freq_;
    }
    net::JsonValue result;
    std::string err;
    const std::string args = "{\"hz\":" + net::format_json_number(freq_hz) + "}";
    if (!send_request("set_tx_freq", args, result, err)) {
        last_error_ = err;
        return tx_freq_;
    }
    if (const auto* hz = result.find("hz")) tx_freq_ = hz->as_number(tx_freq_);
    return tx_freq_;
}

void NetworkRadio::set_lo_offset(double offset_hz) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!control_ || !open_.load()) {
        lo_offset_ = offset_hz;
        return;
    }
    net::JsonValue result;
    std::string err;
    const std::string args = "{\"hz\":" + net::format_json_number(offset_hz) + "}";
    if (!send_request("set_lo_offset", args, result, err)) {
        last_error_ = err;
        return;
    }
    if (const auto* hz = result.find("hz")) lo_offset_ = hz->as_number(lo_offset_);
}

double NetworkRadio::set_rx_gain(double gain_db) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!control_ || !open_.load()) {
        rx_gain_ = caps_.rx_gain.clamp(gain_db);
        return rx_gain_;
    }
    net::JsonValue result;
    std::string err;
    const std::string args = "{\"db\":" + net::format_json_number(gain_db) + "}";
    if (!send_request("set_rx_gain", args, result, err)) {
        last_error_ = err;
        return rx_gain_;
    }
    if (const auto* db = result.find("db")) rx_gain_ = db->as_number(rx_gain_);
    return rx_gain_;
}

double NetworkRadio::set_tx_gain(double gain_db) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!control_ || !open_.load()) {
        tx_gain_ = caps_.tx_gain.clamp(gain_db);
        return tx_gain_;
    }
    net::JsonValue result;
    std::string err;
    const std::string args = "{\"db\":" + net::format_json_number(gain_db) + "}";
    if (!send_request("set_tx_gain", args, result, err)) {
        last_error_ = err;
        return tx_gain_;
    }
    if (const auto* db = result.find("db")) tx_gain_ = db->as_number(tx_gain_);
    return tx_gain_;
}

double NetworkRadio::set_rx_bandwidth(double bw_hz) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!control_ || !open_.load()) {
        rx_bw_ = caps_.rx_bandwidth.clamp(bw_hz);
        return rx_bw_;
    }
    net::JsonValue result;
    std::string err;
    const std::string args = "{\"hz\":" + net::format_json_number(bw_hz) + "}";
    if (!send_request("set_rx_bandwidth", args, result, err)) {
        last_error_ = err;
        return rx_bw_;
    }
    if (const auto* hz = result.find("hz")) rx_bw_ = hz->as_number(rx_bw_);
    return rx_bw_;
}

double NetworkRadio::set_tx_bandwidth(double bw_hz) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!control_ || !open_.load()) {
        tx_bw_ = caps_.tx_bandwidth.clamp(bw_hz);
        return tx_bw_;
    }
    net::JsonValue result;
    std::string err;
    const std::string args = "{\"hz\":" + net::format_json_number(bw_hz) + "}";
    if (!send_request("set_tx_bandwidth", args, result, err)) {
        last_error_ = err;
        return tx_bw_;
    }
    if (const auto* hz = result.find("hz")) tx_bw_ = hz->as_number(tx_bw_);
    return tx_bw_;
}

bool NetworkRadio::set_rx_antenna(const std::string& antenna) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!control_ || !open_.load()) {
        rx_antenna_ = antenna;
        return true;
    }
    net::JsonValue result;
    std::string err;
    const std::string args = "{\"name\":\"" + net::json_escape(antenna) + "\"}";
    if (!send_request("set_rx_antenna", args, result, err)) {
        last_error_ = err;
        return false;
    }
    if (const auto* name = result.find("name")) rx_antenna_ = name->as_string(rx_antenna_);
    return true;
}

bool NetworkRadio::set_tx_antenna(const std::string& antenna) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!control_ || !open_.load()) {
        tx_antenna_ = antenna;
        return true;
    }
    net::JsonValue result;
    std::string err;
    const std::string args = "{\"name\":\"" + net::json_escape(antenna) + "\"}";
    if (!send_request("set_tx_antenna", args, result, err)) {
        last_error_ = err;
        return false;
    }
    if (const auto* name = result.find("name")) tx_antenna_ = name->as_string(tx_antenna_);
    return true;
}

void NetworkRadio::set_rx_dc_offset_auto(bool enable) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!control_ || !open_.load()) return;
    net::JsonValue result;
    std::string err;
    const std::string args = std::string("{\"on\":") + (enable ? "true" : "false") + "}";
    if (!send_request("set_rx_dc_offset_auto", args, result, err)) last_error_ = err;
}

void NetworkRadio::set_rx_iq_balance_auto(bool enable) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!control_ || !open_.load()) return;
    net::JsonValue result;
    std::string err;
    const std::string args = std::string("{\"on\":") + (enable ? "true" : "false") + "}";
    if (!send_request("set_rx_iq_balance_auto", args, result, err)) last_error_ = err;
}

bool NetworkRadio::set_rx_agc(bool enable) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!control_ || !open_.load()) return false;
    net::JsonValue result;
    std::string err;
    const std::string args = std::string("{\"on\":") + (enable ? "true" : "false") + "}";
    if (!send_request("set_rx_agc", args, result, err)) {
        last_error_ = err;
        return false;
    }
    return true;
}

void NetworkRadio::set_ring_capacity(size_t samples) {
    if (rx_running_.load()) return;
    ring_capacity_ = std::max<size_t>(samples, 4096);
    if (open_.load()) {
        rx_ring_ = std::make_unique<dsp::RingBuffer<cfloat>>(ring_capacity_);
        tx_ring_ = std::make_unique<dsp::RingBuffer<cfloat>>(ring_capacity_);
    }
}

bool NetworkRadio::start_rx() {
    if (rx_running_.load()) return true;
    if (!open_.load()) {
        last_error_ = "sdrlink: no device open";
        return false;
    }

    net::JsonValue result;
    std::string err;
    net::SampleFormat fmt = net::SampleFormat::Cf32;
    {
        std::lock_guard<std::mutex> g{config_mutex_};
        /* cf32 by default: lossless from the driver (PROTOCOL.md section 3),
         * and this backend has no bandwidth pressure UsrpRadio's USB link
         * doesn't already dictate — the conversion path below still handles
         * ci16/ci8 for a server or a future setting that prefers them. */
        if (!send_request("start_rx", "{\"format\":\"cf32\"}", result, err)) {
            last_error_ = "sdrlink: start_rx failed: " + err;
            return false;
        }
        if (const auto* f = result.find("format")) {
            std::string fs = f->as_string("cf32");
            if (!net::sample_format_from_string(fs, fmt)) fmt = net::SampleFormat::Cf32;
        }
    }
    rx_format_ = fmt;

    auto stream = socket_factory_();
    if (!stream) {
        last_error_ = "sdrlink: no socket implementation available on this platform";
        return false;
    }
    /* The protocol fixes 5960/5961 as its defaults but has no way for a
     * client to discover a non-default stream port (see the class-level
     * comment in network_radio.hpp); control_port_ + 1 is the reference
     * server's convention. */
    const uint16_t stream_port = static_cast<uint16_t>(control_port_ + 1);
    if (!stream->connect(host_, stream_port, kConnectTimeoutMs, err)) {
        last_error_ = "sdrlink: stream connect failed: " + err;
        std::lock_guard<std::mutex> g{config_mutex_};
        net::JsonValue ignored_result;
        std::string ignored_err;
        send_request("stop_rx", "", ignored_result, ignored_err); /* tell the server we gave up */
        return false;
    }
    stream->set_recv_timeout(kStreamRecvTimeoutMs);

    const std::string session_line = "{\"session_id\":\"" + net::json_escape(session_id_) + "\"}\n";
    if (!stream->send_all(session_line.data(), session_line.size())) {
        last_error_ = "sdrlink: stream session handshake failed";
        stream->close();
        std::lock_guard<std::mutex> g{config_mutex_};
        net::JsonValue ignored_result;
        std::string ignored_err;
        send_request("stop_rx", "", ignored_result, ignored_err);
        return false;
    }

    if (!rx_ring_) rx_ring_ = std::make_unique<dsp::RingBuffer<cfloat>>(ring_capacity_);
    rx_ring_->clear();

    stream_socket_ = std::move(stream);
    rx_stop_.store(false);
    rx_running_.store(true);
    rx_thread_ = std::thread(&NetworkRadio::rx_thread_main, this);
    return true;
}

void NetworkRadio::stop_rx() {
    if (!rx_running_.load()) {
        if (rx_thread_.joinable()) rx_thread_.join();
        return;
    }

    rx_stop_.store(true);
    if (rx_thread_.joinable()) rx_thread_.join();
    rx_running_.store(false);

    if (stream_socket_) {
        stream_socket_->close();
        stream_socket_.reset();
    }

    std::lock_guard<std::mutex> g{config_mutex_};
    net::JsonValue result;
    std::string err;
    send_request("stop_rx", "", result, err); /* best-effort */
}

void NetworkRadio::rx_thread_main() {
    net::Socket* stream = stream_socket_.get();
    if (stream == nullptr) {
        rx_running_.store(false);
        return;
    }

    const net::SampleFormat fmt = rx_format_;
    const size_t sample_bytes = net::bytes_per_sample(fmt);

    uint8_t header_buf[net::FrameHeader::kSize];
    std::vector<uint8_t> payload;
    std::vector<cfloat> converted;
    bool have_last_seq = false;
    uint32_t last_seq = 0;

    while (!rx_stop_.load()) {
        if (!read_exact(*stream, header_buf, sizeof(header_buf), rx_stop_)) {
            if (!rx_stop_.load()) stats_.errors++;
            break;
        }

        net::FrameHeader header;
        if (!net::parse_frame_header(header_buf, sizeof(header_buf), header)) {
            stats_.errors++;
            break; /* bad magic: the stream is desynced, nothing to recover to */
        }

        if (header.flags & net::FrameHeader::kOverflowFlag) stats_.overflows++;

        if (have_last_seq) {
            const uint32_t gap = net::frames_dropped_between(last_seq, header.seq);
            if (gap > 0) stats_.rx_dropped += gap * (header.samples > 0 ? header.samples : 1u);
        }
        have_last_seq = true;
        last_seq = header.seq;

        const size_t payload_bytes = static_cast<size_t>(header.samples) * sample_bytes;
        if (payload_bytes > 0) {
            payload.resize(payload_bytes);
            if (!read_exact(*stream, payload.data(), payload_bytes, rx_stop_)) {
                if (!rx_stop_.load()) stats_.errors++;
                break;
            }
        }

        if (header.samples == 0) continue;

        converted.resize(header.samples);
        net::convert_samples(payload.data(), header.samples, fmt, converted.data());

        rx_level_db_.store(dsp::to_db(dsp::rms(converted.data(), converted.size())));

        const size_t written = rx_ring_->write(converted.data(), converted.size());
        stats_.rx_samples += written;
        if (written < converted.size()) stats_.rx_dropped += static_cast<uint32_t>(converted.size() - written);
    }

    rx_running_.store(false);
}

bool NetworkRadio::start_tx() {
    /* PROTOCOL.md section 3: the IQ stream is server -> client only. There is
     * no wire path for TX samples in protocol v1, so this fails cleanly
     * rather than pretending to transmit — the honesty rule in PORTING.md. */
    last_error_ = "sdrlink: TX streaming is not supported by protocol v1 (the IQ stream is server to client only)";
    return false;
}

void NetworkRadio::stop_tx() { /* nothing was ever started */ }

}  // namespace radio
