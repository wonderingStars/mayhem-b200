/*
 * mayhem-b200 — sdrlink network radio backend.
 *
 * Talks to a remote SDR over the sdrlink protocol (see PROTOCOL.md in the
 * sdrlink repository — this file implements the client side only; no sdrlink
 * source is copied here, only the wire format it documents).
 *
 * Two TCP connections, per the protocol:
 *   - control: newline-delimited JSON, request/reply by "id". open() does the
 *     hello handshake and the remote "open" here; every setter below sends
 *     its command synchronously and returns the ACTUAL value the reply
 *     carries, exactly like UsrpRadio returns what UHD actually accepted.
 *   - stream: opened by start_rx(), a one-line session handshake followed by
 *     binary frames (24-byte header + IQ payload) until stop_rx().
 *
 * The socket layer is reached only through the `net::Socket` interface below
 * so the protocol logic — handshake, JSON parsing, frame parsing, seq-gap
 * accounting — can be unit-tested without ws2_32 or a live server. NetworkRadio
 * takes a `net::SocketFactory`; production code (and the default constructor)
 * uses `net::make_platform_socket`, which is Winsock on Windows. That is the
 * only function in this backend that touches ws2_32, so a POSIX BSD-sockets
 * implementation later only has to replace that one function and the .cpp's
 * WinsockSocket class — nothing above the Socket interface changes.
 *
 * Deliberate scope limits, both because the protocol does not cover them:
 *   - set_master_clock_rate() has no wire command (PROTOCOL.md section 2.1
 *     lists none) — it reports the cached value unchanged and sets
 *     last_error(), same honesty rule as any unsupported capability.
 *   - start_tx() fails cleanly: the IQ stream (section 3) is server->client
 *     only, so there is no way in protocol v1 to send TX samples over the
 *     wire, whatever set_tx_* the remote device would otherwise accept.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_NETWORK_RADIO_H__
#define __MB200_NETWORK_RADIO_H__

#include "../dsp/ring_buffer.hpp"
#include "radio_device.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace radio {
namespace net {

/* --- Socket seam --------------------------------------------------------- */

/* Thin interface over a single TCP connection. WinsockSocket (network_radio.cpp)
 * is the only production implementation; tests supply a FakeSocket instead so
 * the handshake, request/reply and frame-reading logic run without a live
 * server or any OS socket at all. */
class Socket {
   public:
    virtual ~Socket() = default;

    /* Connects with a bounded timeout so an unreachable host fails in
     * timeout_ms rather than hanging on the OS's own multi-minute default.
     * Returns false and fills `error` on failure (refused, timed out,
     * resolution failure — the caller does not need to tell those apart). */
    virtual bool connect(const std::string& host, uint16_t port, int timeout_ms,
                          std::string& error) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    /* Sends the exact bytes given, looping internally. False on any error. */
    virtual bool send_all(const void* data, size_t len) = 0;

    /* Reads whatever is available, up to `len` bytes, respecting the receive
     * timeout set below. Return values:
     *   > 0        bytes read
     *   kClosed    peer closed the connection in an orderly way
     *   kTimeout   no data within the receive timeout — not an error, the
     *              caller should re-check its own stop condition and retry
     *   kError     a real socket error */
    static constexpr long kClosed = 0;
    static constexpr long kTimeout = -1;
    static constexpr long kError = -2;
    virtual long recv_some(void* data, size_t len) = 0;

    virtual void set_recv_timeout(int timeout_ms) = 0;
};

using SocketFactory = std::function<std::unique_ptr<Socket>()>;

/* The Winsock implementation. Safe to call repeatedly (WSAStartup is
 * refcounted internally). */
std::unique_ptr<Socket> make_platform_socket();

/* --- Frame format, PROTOCOL.md section 3, exposed for testing ------------ */

enum class SampleFormat { Cf32, Ci16, Ci8 };

bool sample_format_from_string(const std::string& s, SampleFormat& out);
const char* sample_format_to_string(SampleFormat f);
size_t bytes_per_sample(SampleFormat f);

struct FrameHeader {
    uint32_t seq{0};
    uint64_t timestamp_ns{0};
    uint32_t samples{0};
    uint32_t flags{0};

    static constexpr size_t kSize = 24;
    static constexpr uint32_t kOverflowFlag = 1u << 0;
};

/* Parses the 24-byte little-endian header at `data` (which must be at least
 * FrameHeader::kSize bytes). False if the "SDRK" magic does not match. */
bool parse_frame_header(const uint8_t* data, size_t len, FrameHeader& out);

/* Converts `count` complex samples of `format` from `payload` (which must
 * hold count * bytes_per_sample(format) bytes) into `out` (room for count). */
void convert_samples(const uint8_t* payload, size_t count, SampleFormat format, cfloat* out);

/* How many frames were dropped between two seq numbers seen back-to-back on
 * the stream, per PROTOCOL.md section 3 ("A client MUST tolerate a seq gap").
 * uint32 arithmetic wraps by design, so a gap across the seq wraparound reads
 * as zero rather than 4 billion. A gap larger than kMaxSaneGap is far more
 * likely a reordered or duplicated frame than a real multi-billion-frame
 * drop, so it is reported as zero rather than corrupting the stat with noise. */
constexpr uint32_t kMaxSaneSeqGap = 1u << 24;
uint32_t frames_dropped_between(uint32_t last_seq, uint32_t seq);

/* --- Minimal JSON, just enough for sdrlink's control protocol ------------ */

/* Every value the protocol carries is a number, string, bool, null, object or
 * array of those (PROTOCOL.md section 2) — not worth a general dependency. */
class JsonValue {
   public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type{Type::Null};
    bool bool_value{false};
    double number_value{0.0};
    std::string string_value{};
    std::vector<JsonValue> array_value{};
    std::vector<std::pair<std::string, JsonValue>> object_value{};

    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }

    /* Object member lookup. Returns nullptr if not an object or the key is
     * absent — callers use as_number()/as_string()/as_bool()'s defaults for
     * "the server didn't send this field", which is not an error per
     * PROTOCOL.md section 5 ("a client must ignore keys it does not
     * recognise" — the converse, a client tolerating an absent optional key,
     * follows the same spirit). */
    const JsonValue* find(const std::string& key) const;

    double as_number(double def = 0.0) const { return type == Type::Number ? number_value : def; }
    std::string as_string(const std::string& def = "") const {
        return type == Type::String ? string_value : def;
    }
    bool as_bool(bool def = false) const { return type == Type::Bool ? bool_value : def; }
};

/* Parses one JSON value from `text`. False and fills `error` on malformed
 * input. Trailing whitespace after the value is tolerated (the caller strips
 * the newline delimiter before this is called). */
bool json_parse(const std::string& text, JsonValue& out, std::string& error);

/* Formats a double the way a JSON number needs to look: whole values print
 * without a trailing ".0" clutter (matches the request examples in
 * PROTOCOL.md section 2, e.g. {"hz": 100000000}); anything else prints with
 * enough precision to round-trip. */
std::string format_json_number(double v);

/* Escapes a string for embedding in a JSON string literal (quotes, backslash,
 * control characters). Does not add the surrounding quotes. */
std::string json_escape(const std::string& s);

}  // namespace net

/* --- The backend ---------------------------------------------------------- */

class NetworkRadio : public RadioDevice {
   public:
    NetworkRadio();
    /* Test-only: injects the socket factory so the handshake, setters and RX
     * reader thread run against a FakeSocket instead of ws2_32. */
    explicit NetworkRadio(net::SocketFactory socket_factory);
    ~NetworkRadio() override;

    NetworkRadio(const NetworkRadio&) = delete;
    NetworkRadio& operator=(const NetworkRadio&) = delete;

    /* `args` is "host[:port]"; port defaults to 5960 (PROTOCOL.md section 1).
     * The stream port is assumed to be control_port + 1 — the protocol fixes
     * 5960/5961 as its defaults but gives a client no way to discover a
     * non-default stream port, so this is the reference server's convention,
     * not a protocol guarantee. Connects, does the hello handshake, opens the
     * remote device (with an empty device-selection string — the server's own
     * default, mirroring UsrpRadio's default when its args are empty) and
     * populates caps() from the reply's DeviceCaps. */
    bool open(const std::string& args = "") override;
    void close() override;
    bool is_open() const override { return open_.load(); }

    const DeviceCaps& caps() const override { return caps_; }
    const std::string& last_error() const override { return last_error_; }

    const char* driver_name() const override { return "sdrlink"; }

    double set_master_clock_rate(double rate_hz) override;

    double set_rx_rate(double rate_hz) override;
    double rx_rate() const override { return rx_rate_; }
    double set_tx_rate(double rate_hz) override;
    double tx_rate() const override { return tx_rate_; }

    double set_rx_frequency(double freq_hz) override;
    double rx_frequency() const override { return rx_freq_; }
    double set_tx_frequency(double freq_hz) override;
    double tx_frequency() const override { return tx_freq_; }

    void set_lo_offset(double offset_hz) override;
    double lo_offset() const override { return lo_offset_; }

    double set_rx_gain(double gain_db) override;
    double rx_gain() const override { return rx_gain_; }
    double set_tx_gain(double gain_db) override;
    double tx_gain() const override { return tx_gain_; }

    double set_rx_bandwidth(double bw_hz) override;
    double rx_bandwidth() const override { return rx_bw_; }
    double set_tx_bandwidth(double bw_hz) override;
    double tx_bandwidth() const override { return tx_bw_; }

    bool set_rx_antenna(const std::string& antenna) override;
    const std::string& rx_antenna() const override { return rx_antenna_; }
    bool set_tx_antenna(const std::string& antenna) override;
    const std::string& tx_antenna() const override { return tx_antenna_; }

    void set_rx_dc_offset_auto(bool enable) override;
    void set_rx_iq_balance_auto(bool enable) override;

    bool set_rx_agc(bool enable) override;

    /* Opens the stream socket, sends {"session_id":...}, starts the reader
     * thread. Samples land in rx_ring() as std::complex<float> regardless of
     * the wire format. */
    bool start_rx() override;
    void stop_rx() override;
    bool rx_running() const override { return rx_running_.load(); }

    /* Protocol v1's IQ stream is server->client only (PROTOCOL.md section 3)
     * — there is no wire path for TX samples, so this fails cleanly rather
     * than pretending to transmit. */
    bool start_tx() override;
    void stop_tx() override;
    bool tx_running() const override { return false; }

    dsp::RingBuffer<cfloat>& rx_ring() override { return *rx_ring_; }
    dsp::RingBuffer<cfloat>& tx_ring() override { return *tx_ring_; }

    StreamStats& stats() override { return stats_; }
    const StreamStats& stats() const override { return stats_; }

    float rx_level_db() const override { return rx_level_db_.load(); }

    void set_ring_capacity(size_t samples) override;

   private:
    void rx_thread_main();

    /* Sends {"id":N,"cmd":cmd,"args":args_json} on the control socket (args_json
     * may be empty to omit the field) and blocks for the reply with matching
     * id, skipping any unsolicited {"event":...} lines in between. On an
     * ok:false reply, `error` is set from the reply's "error" field and this
     * returns false; `result` is only valid when this returns true. */
    bool send_request(const std::string& cmd, const std::string& args_json, net::JsonValue& result,
                       std::string& error, int timeout_ms = 5000);

    /* Reads one '\n'-terminated line from `sock`, using `leftover` to carry
     * bytes read past the newline into the next call. False on a closed
     * connection or socket error. */
    bool read_line(net::Socket& sock, std::string& leftover, std::string& line, int timeout_ms);

    net::SocketFactory socket_factory_;
    std::unique_ptr<net::Socket> control_;
    std::string control_leftover_;
    std::string host_;
    uint16_t control_port_{5960};
    std::string session_id_;
    int next_id_{1};

    DeviceCaps caps_{};
    std::string last_error_{};

    std::atomic<bool> open_{false};
    std::atomic<bool> rx_running_{false};
    std::atomic<bool> rx_stop_{false};
    std::atomic<float> rx_level_db_{-140.0f};

    std::thread rx_thread_;
    std::unique_ptr<net::Socket> stream_socket_;

    std::unique_ptr<dsp::RingBuffer<cfloat>> rx_ring_;
    std::unique_ptr<dsp::RingBuffer<cfloat>> tx_ring_;
    size_t ring_capacity_{1 << 20};

    /* Guards the control socket and every cached setting against concurrent
     * setter calls; the RX reader thread never touches the control socket. */
    mutable std::mutex config_mutex_;

    double rx_rate_{0.0};
    double tx_rate_{0.0};
    double rx_freq_{0.0};
    double tx_freq_{0.0};
    double rx_gain_{0.0};
    double tx_gain_{0.0};
    double rx_bw_{0.0};
    double tx_bw_{0.0};
    double lo_offset_{0.0};
    std::string rx_antenna_{};
    std::string tx_antenna_{};

    net::SampleFormat rx_format_{net::SampleFormat::Cf32};

    StreamStats stats_{};
};

}  // namespace radio

#endif /*__MB200_NETWORK_RADIO_H__*/
