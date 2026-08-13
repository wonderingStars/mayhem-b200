/*
 * mayhem-b200 — sdrlink network radio backend tests.
 *
 * NetworkRadio's protocol logic (JSON parsing, frame parsing, sample
 * conversion, seq-gap accounting, the hello/open handshake) is tested here
 * without ws2_32 or a live server, through the net::Socket seam declared in
 * network_radio.hpp — FakeSocket below plays the server's part. The one test
 * that touches a real socket confirms open() fails cleanly, and quickly,
 * against a host nothing is listening on; it never talks to the network.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "network_radio.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

/* Plays the server side of both the control and stream connections: connect()
 * always succeeds (unless configured not to), send_all() just records what was
 * sent, and recv_some() hands back a pre-scripted byte string a few bytes at a
 * time — short on purpose, so a test that passes is also exercising read_line's
 * and read_exact's partial-read/buffering paths, not just the happy "one big
 * recv" case. */
class FakeSocket : public radio::net::Socket {
   public:
    explicit FakeSocket(std::string script) : script_(std::move(script)) {}
    FakeSocket(bool connect_ok, std::string connect_error)
        : connect_ok_(connect_ok), connect_error_(std::move(connect_error)) {}

    bool connect(const std::string&, uint16_t, int, std::string& error) override {
        if (!connect_ok_) {
            error = connect_error_;
            return false;
        }
        open_ = true;
        return true;
    }

    void close() override { open_ = false; }
    bool is_open() const override { return open_; }

    bool send_all(const void* data, size_t len) override {
        if (!open_) return false;
        sent_.append(static_cast<const char*>(data), len);
        return true;
    }

    long recv_some(void* data, size_t len) override {
        if (!open_) return kError;
        if (read_pos_ >= script_.size()) return kClosed;
        const size_t chunk = std::min<size_t>(len, std::min<size_t>(7, script_.size() - read_pos_));
        std::memcpy(data, script_.data() + read_pos_, chunk);
        read_pos_ += chunk;
        return static_cast<long>(chunk);
    }

    void set_recv_timeout(int) override {}

    const std::string& sent() const { return sent_; }

   private:
    std::string script_{};
    size_t read_pos_{0};
    std::string sent_{};
    bool open_{false};
    bool connect_ok_{true};
    std::string connect_error_{};
};

/* A DeviceCaps JSON object with an intentionally-omitted tx_rate/tx_bandwidth
 * (PROTOCOL.md section 2.2's own example omits them too) so the fallback-to-rx
 * logic in caps_from_json gets exercised, plus one field ("extra") no client
 * code looks up — that is the "unknown field is ignored" case at the
 * NetworkRadio level rather than the bare-parser level. */
const std::string kHelloReply =
    "{\"id\":1,\"ok\":true,\"result\":{\"session_id\":\"sess1\",\"server\":\"sdrlink 1.0\",\"proto\":1},"
    "\"extra\":123}\n";

const std::string kOpenReply =
    "{\"id\":2,\"ok\":true,\"result\":{\"caps\":{"
    "\"driver\":\"uhd\",\"label\":\"B200 TEST\",\"serial\":\"TESTSER\","
    "\"rx_freq\":{\"min\":70000000,\"max\":6000000000,\"step\":1},"
    "\"rx_gain\":{\"min\":0,\"max\":76,\"step\":1},"
    "\"rx_rate\":{\"min\":200000,\"max\":61440000,\"step\":1},"
    "\"rx_bandwidth\":{\"min\":200000,\"max\":56000000,\"step\":1},"
    "\"tx_freq\":{\"min\":70000000,\"max\":6000000000,\"step\":1},"
    "\"tx_gain\":{\"min\":0,\"max\":89.8,\"step\":0.2},"
    "\"rx_antennas\":[\"TX/RX\",\"RX2\"],\"tx_antennas\":[\"TX/RX\"],"
    "\"has_rx\":true,\"has_tx\":true,\"full_duplex\":true,"
    "\"master_clock_rate\":16000000}}}\n";

const std::string kSetRxFreqReply = "{\"id\":3,\"ok\":true,\"result\":{\"hz\":100000042.0}}\n";

radio::net::SocketFactory single_socket_factory(std::string script) {
    return [script = std::move(script)] { return std::make_unique<FakeSocket>(script); };
}

/* --- Reconnection harness ---------------------------------------------------
 *
 * Vends one FakeSocket per connect() from a queue, keeping raw pointers so a
 * test can read back what was sent on each connection separately -- which is
 * the whole point when the assertion is "the settings were replayed onto the
 * SECOND connection". An empty script, or running off the end of the queue,
 * vends a socket that refuses to connect: that is what "the server is still
 * down" looks like from here. */
struct SocketQueue {
    std::vector<std::string> scripts{};
    size_t next{0};
    std::vector<FakeSocket*> handed_out{};

    radio::net::SocketFactory factory() {
        return [this]() -> std::unique_ptr<radio::net::Socket> {
            std::unique_ptr<FakeSocket> s;
            if (next < scripts.size() && !scripts[next].empty()) {
                s = std::make_unique<FakeSocket>(scripts[next]);
            } else {
                s = std::make_unique<FakeSocket>(false, "connection refused");
            }
            next++;
            handed_out.push_back(s.get());
            return s;
        };
    }
};

std::string ok_reply(int id, const std::string& result_json) {
    return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"result\":" + result_json + "}\n";
}

/* Generic ok replies for ids [first, last], for the run of replayed settings
 * whose individual values no assertion cares about. */
std::string ok_replies(int first, int last) {
    std::string out;
    for (int id = first; id <= last; id++) out += ok_reply(id, "{\"hz\":1.0}");
    return out;
}

/* A reconnect re-runs the handshake (hello=1, open=2) and then replays the
 * cached settings: rate, frequency, gain, bandwidth, antenna -- ids 3..7. So
 * the retried request that triggered the whole thing lands on id 8. */
constexpr int kFirstIdAfterReplay = 8;

}  // namespace

/* --- JSON parsing ------------------------------------------------------------ */

TEST(json_parse_ok_reply_with_nested_result) {
    radio::net::JsonValue v;
    std::string err;
    CHECK(radio::net::json_parse(R"({"id":7,"ok":true,"result":{"hz":100000000.0}})", v, err));
    CHECK(v.is_object());

    const auto* id = v.find("id");
    CHECK(id != nullptr);
    CHECK_NEAR(id->as_number(), 7.0, 1e-9);

    const auto* ok = v.find("ok");
    CHECK(ok != nullptr);
    CHECK(ok->as_bool());

    const auto* result = v.find("result");
    CHECK(result != nullptr);
    CHECK(result->is_object());
    const auto* hz = result->find("hz");
    CHECK(hz != nullptr);
    CHECK_NEAR(hz->as_number(), 100000000.0, 1e-6);
}

TEST(json_parse_error_reply) {
    radio::net::JsonValue v;
    std::string err;
    CHECK(radio::net::json_parse(R"({"id":7,"ok":false,"error":"no device open"})", v, err));

    const auto* ok = v.find("ok");
    CHECK(ok != nullptr);
    CHECK(!ok->as_bool(true));

    const auto* error_field = v.find("error");
    CHECK(error_field != nullptr);
    CHECK_STR_EQ(error_field->as_string(), "no device open");
}

TEST(json_parse_ignores_unrecognised_fields) {
    radio::net::JsonValue v;
    std::string err;
    /* "extra" and "meta" are fields no client of this protocol looks up.
     * Parsing must not choke on them, and the field the caller does want
     * must still be reachable. */
    CHECK(radio::net::json_parse(
        R"({"id":7,"ok":true,"result":{"hz":42.0},"extra":"stuff","meta":{"nested":[1,2,3]}})", v, err));

    const auto* result = v.find("result");
    CHECK(result != nullptr);
    const auto* hz = result->find("hz");
    CHECK(hz != nullptr);
    CHECK_NEAR(hz->as_number(), 42.0, 1e-9);
    CHECK(v.find("nonexistent_field") == nullptr);
}

TEST(json_parse_rejects_malformed_input) {
    radio::net::JsonValue v;
    std::string err;
    CHECK(!radio::net::json_parse(R"({"id":7,"ok":true,)", v, err));
    CHECK(!err.empty());
}

TEST(json_parse_array_of_strings) {
    radio::net::JsonValue v;
    std::string err;
    CHECK(radio::net::json_parse(R"(["TX/RX","RX2"])", v, err));
    CHECK(v.is_array());
    CHECK_EQ(v.array_value.size(), size_t{2});
    CHECK_STR_EQ(v.array_value[0].as_string(), "TX/RX");
    CHECK_STR_EQ(v.array_value[1].as_string(), "RX2");
}

/* --- Frame header, PROTOCOL.md section 3 ------------------------------------- */

TEST(frame_header_parses_little_endian_fields) {
    uint8_t buf[24] = {};
    buf[0] = 'S';
    buf[1] = 'D';
    buf[2] = 'R';
    buf[3] = 'K';
    /* seq = 0x11223344 */
    buf[4] = 0x44;
    buf[5] = 0x33;
    buf[6] = 0x22;
    buf[7] = 0x11;
    /* timestamp = 0x0102030405060708 */
    buf[8] = 0x08;
    buf[9] = 0x07;
    buf[10] = 0x06;
    buf[11] = 0x05;
    buf[12] = 0x04;
    buf[13] = 0x03;
    buf[14] = 0x02;
    buf[15] = 0x01;
    /* samples = 512 */
    buf[16] = 0x00;
    buf[17] = 0x02;
    buf[18] = 0x00;
    buf[19] = 0x00;
    /* flags = 1 (overflow) */
    buf[20] = 0x01;
    buf[21] = 0x00;
    buf[22] = 0x00;
    buf[23] = 0x00;

    radio::net::FrameHeader h;
    CHECK(radio::net::parse_frame_header(buf, sizeof(buf), h));
    CHECK_EQ(h.seq, uint32_t{0x11223344});
    CHECK_EQ(h.timestamp_ns, uint64_t{0x0102030405060708ULL});
    CHECK_EQ(h.samples, uint32_t{512});
    CHECK_EQ(h.flags, uint32_t{1});
    CHECK((h.flags & radio::net::FrameHeader::kOverflowFlag) != 0);
}

TEST(frame_header_rejects_bad_magic) {
    uint8_t buf[24] = {};
    buf[0] = 'X';
    buf[1] = 'X';
    buf[2] = 'X';
    buf[3] = 'X';
    radio::net::FrameHeader h;
    CHECK(!radio::net::parse_frame_header(buf, sizeof(buf), h));
}

TEST(frame_header_rejects_short_buffer) {
    uint8_t buf[10] = {};
    radio::net::FrameHeader h;
    CHECK(!radio::net::parse_frame_header(buf, sizeof(buf), h));
}

TEST(bytes_per_sample_and_payload_size_maths_match_protocol_table) {
    CHECK_EQ(radio::net::bytes_per_sample(radio::net::SampleFormat::Cf32), size_t{8});
    CHECK_EQ(radio::net::bytes_per_sample(radio::net::SampleFormat::Ci16), size_t{4});
    CHECK_EQ(radio::net::bytes_per_sample(radio::net::SampleFormat::Ci8), size_t{2});

    const uint32_t samples = 733;
    CHECK_EQ(samples * radio::net::bytes_per_sample(radio::net::SampleFormat::Cf32), uint32_t{733 * 8});
    CHECK_EQ(samples * radio::net::bytes_per_sample(radio::net::SampleFormat::Ci16), uint32_t{733 * 4});
    CHECK_EQ(samples * radio::net::bytes_per_sample(radio::net::SampleFormat::Ci8), uint32_t{733 * 2});
}

TEST(sample_format_string_round_trip) {
    radio::net::SampleFormat fmt;
    CHECK(radio::net::sample_format_from_string("cf32", fmt));
    CHECK(fmt == radio::net::SampleFormat::Cf32);
    CHECK(radio::net::sample_format_from_string("ci16", fmt));
    CHECK(fmt == radio::net::SampleFormat::Ci16);
    CHECK(radio::net::sample_format_from_string("ci8", fmt));
    CHECK(fmt == radio::net::SampleFormat::Ci8);
    CHECK(!radio::net::sample_format_from_string("bogus", fmt));

    CHECK_STR_EQ(radio::net::sample_format_to_string(radio::net::SampleFormat::Cf32), "cf32");
    CHECK_STR_EQ(radio::net::sample_format_to_string(radio::net::SampleFormat::Ci16), "ci16");
    CHECK_STR_EQ(radio::net::sample_format_to_string(radio::net::SampleFormat::Ci8), "ci8");
}

/* --- Sample conversion -------------------------------------------------------- */

TEST(convert_samples_cf32_is_a_lossless_passthrough) {
    float vals[4] = {1.5f, -2.5f, 0.0f, 100.0f}; /* cf32 is never clamped */
    uint8_t payload[16];
    std::memcpy(payload, vals, sizeof(vals));

    radio::cfloat out[2];
    radio::net::convert_samples(payload, 2, radio::net::SampleFormat::Cf32, out);
    CHECK_NEAR(out[0].real(), 1.5, 1e-6);
    CHECK_NEAR(out[0].imag(), -2.5, 1e-6);
    CHECK_NEAR(out[1].real(), 0.0, 1e-6);
    CHECK_NEAR(out[1].imag(), 100.0, 1e-6);
}

TEST(convert_samples_ci16_full_scale_and_clamping) {
    /* Full scale is 32767 (PROTOCOL.md section 3). +32767 lands exactly on
     * 1.0; -32768 (int16's most negative value) divides out to -1.0000305,
     * which must be clamped to -1.0 rather than handed to the DSP chain. */
    int16_t vals[2] = {32767, -32768};
    uint8_t payload[4];
    std::memcpy(payload, vals, sizeof(vals));

    radio::cfloat out[1];
    radio::net::convert_samples(payload, 1, radio::net::SampleFormat::Ci16, out);
    CHECK_NEAR(out[0].real(), 1.0, 1e-6);
    CHECK_NEAR(out[0].imag(), -1.0, 1e-6);
}

TEST(convert_samples_ci16_mid_scale) {
    int16_t vals[2] = {16384, -16384};
    uint8_t payload[4];
    std::memcpy(payload, vals, sizeof(vals));

    radio::cfloat out[1];
    radio::net::convert_samples(payload, 1, radio::net::SampleFormat::Ci16, out);
    CHECK_NEAR(out[0].real(), 16384.0 / 32767.0, 1e-6);
    CHECK_NEAR(out[0].imag(), -16384.0 / 32767.0, 1e-6);
}

TEST(convert_samples_ci8_full_scale_and_clamping) {
    /* Full scale is 127; -128 (int8's most negative value) divides out to
     * -1.0079 and must clamp to -1.0. */
    int8_t vals[2] = {127, -128};
    uint8_t payload[2];
    std::memcpy(payload, vals, sizeof(vals));

    radio::cfloat out[1];
    radio::net::convert_samples(payload, 1, radio::net::SampleFormat::Ci8, out);
    CHECK_NEAR(out[0].real(), 1.0, 1e-6);
    CHECK_NEAR(out[0].imag(), -1.0, 1e-6);
}

TEST(convert_samples_multiple_samples_advance_correctly) {
    int8_t vals[4] = {10, -10, 20, -20};
    uint8_t payload[4];
    std::memcpy(payload, vals, sizeof(vals));

    radio::cfloat out[2];
    radio::net::convert_samples(payload, 2, radio::net::SampleFormat::Ci8, out);
    CHECK_NEAR(out[0].real(), 10.0 / 127.0, 1e-6);
    CHECK_NEAR(out[0].imag(), -10.0 / 127.0, 1e-6);
    CHECK_NEAR(out[1].real(), 20.0 / 127.0, 1e-6);
    CHECK_NEAR(out[1].imag(), -20.0 / 127.0, 1e-6);
}

/* --- Seq-gap detection --------------------------------------------------------
 *
 * PROTOCOL.md section 3: "A client MUST tolerate a seq gap". These check the
 * exact function the RX reader thread uses to turn two consecutive seq numbers
 * into a dropped-frame count.
 */

TEST(seq_gap_zero_when_consecutive) { CHECK_EQ(radio::net::frames_dropped_between(5, 6), uint32_t{0}); }

TEST(seq_gap_counts_dropped_frames) { CHECK_EQ(radio::net::frames_dropped_between(5, 9), uint32_t{3}); }

TEST(seq_gap_handles_uint32_wraparound) {
    CHECK_EQ(radio::net::frames_dropped_between(0xFFFFFFFFu, 0u), uint32_t{0});
    CHECK_EQ(radio::net::frames_dropped_between(0xFFFFFFFEu, 1u), uint32_t{2});
}

TEST(seq_gap_treats_absurd_backward_jump_as_zero_not_billions) {
    /* seq going "backwards" a little (reordering, a duplicated frame) wraps
     * to a huge value under plain subtraction; frames_dropped_between must
     * report that as zero rather than corrupting rx_dropped with noise. */
    CHECK_EQ(radio::net::frames_dropped_between(100, 99), uint32_t{0});
}

/* --- NetworkRadio: driven entirely through the FakeSocket seam --------------- */

TEST(network_radio_driver_name_is_sdrlink) {
    radio::NetworkRadio radio{single_socket_factory("")};
    CHECK_STR_EQ(radio.driver_name(), "sdrlink");
}

TEST(network_radio_open_populates_caps_from_server_reply) {
    radio::net::SocketFactory factory = single_socket_factory(kHelloReply + kOpenReply + kSetRxFreqReply);
    radio::NetworkRadio radio{factory};

    CHECK(radio.open("127.0.0.1:5960"));
    CHECK(radio.is_open());
    CHECK_STR_EQ(radio.caps().serial, "TESTSER");
    CHECK_NEAR(radio.caps().rx_gain.max, 76.0, 1e-9);
    CHECK_NEAR(radio.caps().rx_gain.min, 0.0, 1e-9);
    CHECK(radio.caps().has_rx);
    CHECK(radio.caps().has_tx);
    CHECK(radio.caps().full_duplex);
    CHECK_EQ(radio.caps().rx_antennas.size(), size_t{2});
    CHECK_STR_EQ(radio.caps().rx_antennas[0], "TX/RX");
    CHECK_STR_EQ(radio.caps().rx_antennas[1], "RX2");

    /* tx_rate/tx_bandwidth were absent from kOpenReply, same as PROTOCOL.md
     * section 2.2's own example — caps_from_json must fall back to the rx
     * range rather than leaving them at zero. */
    CHECK_NEAR(radio.caps().tx_rate.max, radio.caps().rx_rate.max, 1e-9);
    CHECK_NEAR(radio.caps().tx_bandwidth.max, radio.caps().rx_bandwidth.max, 1e-9);

    /* Every setter sends its command and returns the ACTUAL value from the
     * reply, per PROTOCOL.md section 2.1 — not an echo of what was asked. */
    CHECK_NEAR(radio.set_rx_frequency(100000000.0), 100000042.0, 1e-6);
}

TEST(network_radio_start_tx_fails_cleanly_no_wire_path_for_tx_samples) {
    radio::net::SocketFactory factory = single_socket_factory(kHelloReply + kOpenReply);
    radio::NetworkRadio radio{factory};
    CHECK(radio.open("127.0.0.1:5960"));

    CHECK(!radio.start_tx());
    CHECK(!radio.last_error().empty());
    CHECK(!radio.tx_running());
}

TEST(network_radio_set_master_clock_rate_is_honestly_unsupported) {
    radio::net::SocketFactory factory = single_socket_factory(kHelloReply + kOpenReply);
    radio::NetworkRadio radio{factory};
    CHECK(radio.open("127.0.0.1:5960"));

    const double before = radio.caps().master_clock_rate;
    CHECK_NEAR(radio.set_master_clock_rate(32000000.0), before, 1e-9);
    CHECK(!radio.last_error().empty());
}

TEST(network_radio_open_fails_cleanly_when_hello_is_rejected) {
    radio::net::SocketFactory factory =
        single_socket_factory("{\"id\":1,\"ok\":false,\"error\":\"unsupported proto\"}\n");
    radio::NetworkRadio radio{factory};

    CHECK(!radio.open("127.0.0.1:5960"));
    CHECK(!radio.is_open());
    CHECK(radio.last_error().find("unsupported proto") != std::string::npos);
}

TEST(network_radio_open_fails_cleanly_when_remote_open_is_rejected) {
    radio::net::SocketFactory factory =
        single_socket_factory(kHelloReply + "{\"id\":2,\"ok\":false,\"error\":\"device in use\"}\n");
    radio::NetworkRadio radio{factory};

    CHECK(!radio.open("127.0.0.1:5960"));
    CHECK(!radio.is_open());
    CHECK(radio.last_error().find("device in use") != std::string::npos);
}

TEST(network_radio_open_fails_cleanly_when_the_socket_cannot_connect) {
    radio::net::SocketFactory factory = [] { return std::make_unique<FakeSocket>(false, "connection refused"); };
    radio::NetworkRadio radio{factory};

    CHECK(!radio.open("127.0.0.1:5960"));
    CHECK(!radio.is_open());
    CHECK_STR_EQ(radio.last_error(), "connection refused");
}

TEST(network_radio_setters_fall_back_to_local_clamp_before_open) {
    /* No socket has connected yet, so these must not blow up trying to
     * dereference the (absent) control connection — they clamp locally and
     * report a value, exactly like UsrpRadio does with no USRP attached. */
    radio::NetworkRadio radio{single_socket_factory("")};
    CHECK(!radio.is_open());
    const double got = radio.set_rx_frequency(100000000.0);
    CHECK_NEAR(got, 100000000.0, 1e-6); /* caps unpopulated -> Range::clamp is a no-op */
}

/* --- open() against a host nothing is listening on ---------------------------
 *
 * Uses the real platform socket (net::make_platform_socket, the default
 * NetworkRadio constructor) — the one test here that is not run through
 * FakeSocket — because the thing being verified is the actual OS-level
 * connect-timeout/refusal path. Port 1 on loopback is not a live server,
 * never will be, and refuses the connection immediately, so this cannot hang
 * or need a real sdrlink server.
 */

TEST(network_radio_open_against_unreachable_host_fails_cleanly) {
    radio::NetworkRadio radio; /* real Winsock socket */

    const auto start = std::chrono::steady_clock::now();
    const bool ok = radio.open("127.0.0.1:1");
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(!ok);
    CHECK(!radio.is_open());
    CHECK(!radio.last_error().empty());
    /* Refused, not merely slow: well under the 3 s connect timeout the
     * backend configures internally. */
    CHECK(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 5);
}

/* =============================================================================
 * Reconnection
 *
 * A dropped link used to be terminal. These pin the four things that make
 * recovery trustworthy rather than merely present: that it happens at all,
 * that the session comes back configured the way the caller left it, that the
 * retries are actually spaced, and that giving up is reported honestly instead
 * of being papered over.
 * ========================================================================== */

TEST(network_radio_reconnects_after_the_control_link_drops) {
    SocketQueue q;
    /* Connection 1 answers the handshake and nothing more: the next request
     * finds the script exhausted, which is a closed socket from here. */
    q.scripts.push_back(kHelloReply + kOpenReply);
    q.scripts.push_back(kHelloReply + kOpenReply + ok_replies(3, 7) +
                        ok_reply(kFirstIdAfterReplay, "{\"hz\":100000042.0}"));

    radio::NetworkRadio radio{q.factory()};
    radio.set_sleep_fn([](int) {});

    CHECK(radio.open("127.0.0.1:5960"));
    CHECK(radio.link_state() == radio::NetworkRadio::LinkState::Connected);

    /* This request dies with the link, and must come back with the answer
     * from the reconnected one rather than a failure. */
    const double hz = radio.set_rx_frequency(100'000'000.0);

    CHECK_NEAR(hz, 100000042.0, 1e-6);
    CHECK(radio.link_state() == radio::NetworkRadio::LinkState::Connected);
    CHECK(radio.is_open());
    CHECK_EQ(radio.reconnects(), 1u);
}

TEST(network_radio_replays_the_tuning_it_had_before_the_drop) {
    /* The point of the replay. A fresh `open` on the server starts at the
     * device's defaults, so a reconnect without this comes back live and
     * quietly tuned somewhere else -- which is worse than staying down,
     * because nothing looks wrong. */
    SocketQueue q;
    q.scripts.push_back(kHelloReply + kOpenReply + ok_reply(3, "{\"hz\":100000042.0}"));
    q.scripts.push_back(kHelloReply + kOpenReply + ok_replies(3, 7) +
                        ok_reply(kFirstIdAfterReplay, "{\"db\":40.0}"));

    radio::NetworkRadio radio{q.factory()};
    radio.set_sleep_fn([](int) {});
    CHECK(radio.open("127.0.0.1:5960"));

    /* Tune on the first connection, then lose it. */
    CHECK_NEAR(radio.set_rx_frequency(100'000'000.0), 100000042.0, 1e-6);
    radio.set_rx_gain(40.0);

    CHECK(q.handed_out.size() >= 2);
    if (q.handed_out.size() < 2) return;
    const std::string& replayed = q.handed_out[1]->sent();

    /* The frequency the server accepted, not the one that was asked for. */
    CHECK(replayed.find("100000042") != std::string::npos);
    CHECK(replayed.find("set_rx_rate") != std::string::npos);
    CHECK(replayed.find("set_rx_freq") != std::string::npos);
    CHECK(replayed.find("set_rx_gain") != std::string::npos);
    CHECK(replayed.find("set_rx_bandwidth") != std::string::npos);
    /* Seeded from caps' first RX antenna by open(); it has to survive too. */
    CHECK(replayed.find("TX/RX") != std::string::npos);
}

TEST(network_radio_backs_off_between_attempts_and_caps_the_delay) {
    SocketQueue q;
    q.scripts.push_back(kHelloReply + kOpenReply); /* then every socket refuses */

    radio::NetworkRadio radio{q.factory()};
    std::vector<int> delays;
    radio.set_sleep_fn([&delays](int ms) { delays.push_back(ms); });

    radio::NetworkRadio::ReconnectPolicy policy;
    policy.max_attempts = 5;
    policy.first_delay_ms = 100;
    policy.max_delay_ms = 400;
    radio.set_reconnect_policy(policy);

    CHECK(radio.open("127.0.0.1:5960"));
    radio.set_rx_frequency(100'000'000.0); /* fails: nothing is listening */

    /* One sleep per attempt, doubling until the cap, then flat. Hammering a
     * server that is down is how a client turns one outage into two. */
    CHECK_EQ(delays.size(), size_t{5});
    if (delays.size() == 5) {
        CHECK_EQ(delays[0], 100);
        CHECK_EQ(delays[1], 200);
        CHECK_EQ(delays[2], 400);
        CHECK_EQ(delays[3], 400);
        CHECK_EQ(delays[4], 400);
    }
}

TEST(network_radio_reports_giving_up_rather_than_hiding_it) {
    SocketQueue q;
    q.scripts.push_back(kHelloReply + kOpenReply);

    radio::NetworkRadio radio{q.factory()};
    radio.set_sleep_fn([](int) {});
    radio::NetworkRadio::ReconnectPolicy policy;
    policy.max_attempts = 2;
    radio.set_reconnect_policy(policy);

    CHECK(radio.open("127.0.0.1:5960"));
    radio.set_rx_frequency(100'000'000.0);

    CHECK(radio.link_state() == radio::NetworkRadio::LinkState::Disconnected);
    CHECK_STR_EQ(radio.link_state_string(), "disconnected");
    /* is_open() must go false: a caller that keeps believing the radio is
     * there will keep issuing requests into a hole. */
    CHECK(!radio.is_open());
    CHECK(!radio.last_error().empty());
    CHECK_EQ(radio.reconnects(), 0u);
}

TEST(network_radio_reconnect_can_be_switched_off) {
    SocketQueue q;
    q.scripts.push_back(kHelloReply + kOpenReply);
    /* A second connection is available; with the policy off it must go
     * untouched, so this also proves the disable is real and not just slow. */
    q.scripts.push_back(kHelloReply + kOpenReply + ok_replies(3, 8));

    radio::NetworkRadio radio{q.factory()};
    bool slept = false;
    radio.set_sleep_fn([&slept](int) { slept = true; });
    radio::NetworkRadio::ReconnectPolicy policy;
    policy.max_attempts = 0;
    radio.set_reconnect_policy(policy);

    CHECK(radio.open("127.0.0.1:5960"));
    radio.set_rx_frequency(100'000'000.0);

    CHECK(!slept);
    CHECK_EQ(q.handed_out.size(), size_t{1});
    /* Still honest: reconnect being disabled does not make the link present. */
    CHECK(radio.link_state() == radio::NetworkRadio::LinkState::Disconnected);
    CHECK(!radio.is_open());
}

TEST(network_radio_does_not_reconnect_when_the_server_merely_says_no) {
    /* An ok:false reply means the server is reachable and refused. Retrying
     * that on a fresh connection would get the same refusal, having thrown
     * away a working link and the session with it. */
    SocketQueue q;
    q.scripts.push_back(kHelloReply + kOpenReply +
                        "{\"id\":3,\"ok\":false,\"error\":\"frequency out of range\"}\n");

    radio::NetworkRadio radio{q.factory()};
    bool slept = false;
    radio.set_sleep_fn([&slept](int) { slept = true; });

    CHECK(radio.open("127.0.0.1:5960"));
    radio.set_rx_frequency(100'000'000.0);

    CHECK(!slept);
    CHECK_EQ(q.handed_out.size(), size_t{1});
    CHECK(radio.link_state() == radio::NetworkRadio::LinkState::Connected);
    CHECK(radio.is_open());
}

/* --- The keepalive ----------------------------------------------------------
 *
 * These exist because of a bug the fake-socket tests structurally could not
 * find. Every reconnect test above probes the link by CALLING something, so
 * they all proved recovery works for a client that is busy. Against a real
 * sdrlink server on 2026-08-13, killing the server left an idle client
 * reporting link "connected" and device_ok true for as long as it was watched:
 * with no stream running and nobody touching the controls, nothing issued a
 * request, so nothing failed, so nothing noticed. A radio that reports itself
 * present when the server is gone is precisely the lie LinkState exists to
 * prevent. */

TEST(network_radio_notices_a_dead_link_while_completely_idle) {
    SocketQueue q;
    q.scripts.push_back(kHelloReply + kOpenReply); /* dies after the handshake */

    radio::NetworkRadio radio{q.factory()};
    radio.set_sleep_fn([](int) {});
    radio::NetworkRadio::ReconnectPolicy policy;
    policy.max_attempts = 1;
    radio.set_reconnect_policy(policy);
    radio.set_keepalive_interval_ms(25);

    CHECK(radio.open("127.0.0.1:5960"));
    CHECK(radio.link_state() == radio::NetworkRadio::LinkState::Connected);

    /* Touch nothing at all. The keepalive is the only thing that can find out. */
    for (int waited = 0; waited < 4000; waited += 25) {
        if (radio.link_state() != radio::NetworkRadio::LinkState::Connected) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    CHECK(radio.link_state() == radio::NetworkRadio::LinkState::Disconnected);
    CHECK(!radio.is_open());
}

TEST(network_radio_keepalive_can_be_switched_off) {
    /* The negative control for the test above: with no keepalive, an idle
     * client genuinely cannot know, and must not pretend otherwise by
     * guessing. It keeps reporting what it last knew until something asks. */
    SocketQueue q;
    q.scripts.push_back(kHelloReply + kOpenReply);

    radio::NetworkRadio radio{q.factory()};
    radio.set_sleep_fn([](int) {});
    radio.set_keepalive_interval_ms(0);

    CHECK(radio.open("127.0.0.1:5960"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    CHECK(radio.link_state() == radio::NetworkRadio::LinkState::Connected);
    /* Only one connection was ever made: nothing probed in the background. */
    CHECK_EQ(q.handed_out.size(), size_t{1});
}
