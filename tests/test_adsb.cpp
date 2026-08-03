/*
 * mayhem-b200 — tests for the ADS-B / Mode S receiver.
 *
 * Every expected value here comes from one of three places, never from what
 * this port happens to produce:
 *
 *   - Documented Mode S frames whose decode is published (the KLM1023 identity
 *     message, the 40621D even/odd position pair and its 52.2572 / 3.91937
 *     result, the 485020 velocity message at 159 kt / 183 deg / -832 ft/min).
 *   - The ICAO bit layouts, from which fields are built by hand and then read
 *     back (identity/squawk interleave, AC13 altitude, airspeed subtypes).
 *   - Upstream's own implementation, run side by side: firmware's CRC routine
 *     with its 0x1205FFF reversed-offset polynomial is reimplemented verbatim
 *     below and cross-checked against the port's 0xFFF409 CRC engine.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_registry.hpp"
#include "ui_adsb_rx.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using app::adsb::AdsbFrame;

/* --- Helpers --------------------------------------------------------------- */

uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    return 0;
}

std::vector<uint8_t> bytes_from_hex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<uint8_t>((hex_nibble(hex[i]) << 4) | hex_nibble(hex[i + 1])));
    return out;
}

AdsbFrame frame_from_hex(const std::string& hex) {
    AdsbFrame frame;
    frame.clear();
    for (uint8_t b : bytes_from_hex(hex)) frame.push_byte(b);
    return frame;
}

/* Upstream's compute_CRC(), transcribed from firmware/common/adsb_frame.hpp.
 * It walks the message bit by bit and XORs the 25-bit generator in at each set
 * bit, with the generator held in reversed-offset form. */
uint32_t upstream_compute_crc(const uint8_t* raw_data, size_t data_len) {
    uint8_t adsb_crc[14] = {0};
    const uint32_t crc_poly = 0x1205FFF;

    std::memcpy(adsb_crc, raw_data, data_len);

    for (size_t c = 0; c < data_len; c++) {
        for (size_t b = 0; b < 8; b++) {
            if ((adsb_crc[c] << b) & 0x80) {
                for (size_t s = 0; s < 25; s++) {
                    const size_t bitn = (c * 8) + b + s;
                    if ((crc_poly >> s) & 1)
                        adsb_crc[bitn >> 3] ^= static_cast<uint8_t>(0x80 >> (bitn & 7));
                }
            }
        }
    }

    return (static_cast<uint32_t>(adsb_crc[data_len]) << 16) +
           (static_cast<uint32_t>(adsb_crc[data_len + 1]) << 8) +
           static_cast<uint32_t>(adsb_crc[data_len + 2]);
}

/* Documented frames. */
const char* kFrameIdent = "8D4840D6202CC371C32CE0576098";     /* KLM1023, ICAO 4840D6 */
const char* kFramePosEven = "8D40621D58C382D690C8AC2863A7";   /* ICAO 40621D */
const char* kFramePosOdd = "8D40621D58C386435CC412692AD6";
const char* kFrameVelo = "8D485020994409940838175B284F";      /* ICAO 485020 */

/* --- PPM waveform synthesis ------------------------------------------------
 *
 * A Mode S downlink burst at the firmware's 2 Msps: 8 us of preamble with
 * 0.5 us pulses at 0, 1.0, 3.5 and 4.5 us (samples 0, 2, 7 and 9), then one
 * bit per microsecond, energy in the first half for a 1 and the second half
 * for a 0. */
std::vector<float> make_ppm_magnitudes(const std::vector<uint8_t>& bytes,
                                       size_t bit_count,
                                       float high = 1.0f,
                                       float low = 0.0f,
                                       size_t lead = 40,
                                       size_t tail = 40) {
    static const int preamble[16] = {1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0};

    std::vector<float> out(lead, low);

    for (int i = 0; i < 16; i++) out.push_back(preamble[i] ? high : low);

    for (size_t b = 0; b < bit_count; b++) {
        const bool bit = ((bytes[b >> 3] >> (7 - (b & 7))) & 1) != 0;
        out.push_back(bit ? high : low);
        out.push_back(bit ? low : high);
    }

    out.insert(out.end(), tail, low);
    return out;
}

/* The same waveform at an integer multiple of 2 Msps, for the resampler. */
std::vector<float> oversample(const std::vector<float>& in, size_t factor) {
    std::vector<float> out;
    out.reserve(in.size() * factor);
    for (float v : in)
        for (size_t k = 0; k < factor; k++) out.push_back(v);
    return out;
}

std::vector<dsp::cfloat> to_complex(const std::vector<float>& mags) {
    /* AdsbDemod::process() computes (127*re)^2 + (127*im)^2, so a magnitude of
     * m needs an amplitude of sqrt(m)/127 on the real axis. */
    std::vector<dsp::cfloat> out;
    out.reserve(mags.size());
    for (float m : mags)
        out.emplace_back(std::sqrt(m) / 127.0f, 0.0f);
    return out;
}

/* Builds an airborne-position message (TC 11) from its fields, using upstream's
 * encode_frame_pos layout: altitude with the Q bit inserted at position 4, then
 * the frame parity bit and the two 17-bit CPR words. Verified against the
 * published frame in adsb_pos_frame_encoder_matches_documented_frame below. */
AdsbFrame make_pos_frame(uint32_t icao, int32_t altitude_ft,
                         uint32_t lat_cpr, uint32_t lon_cpr, uint32_t parity) {
    uint32_t ac = static_cast<uint32_t>((altitude_ft + 1000) / 25);
    ac = ((ac & 0x7F0) << 1) | 0x10 | (ac & 0x0F);

    AdsbFrame frame;
    frame.clear();
    frame.push_byte(static_cast<uint8_t>((17 << 3) | 5));  /* DF 17, CA 5 */
    frame.push_byte(static_cast<uint8_t>(icao >> 16));
    frame.push_byte(static_cast<uint8_t>(icao >> 8));
    frame.push_byte(static_cast<uint8_t>(icao));
    frame.push_byte(static_cast<uint8_t>(11 << 3));  /* TC 11 */
    frame.push_byte(static_cast<uint8_t>(ac >> 4));
    frame.push_byte(static_cast<uint8_t>((ac << 4) | (parity << 2) | (lat_cpr >> 15)));
    frame.push_byte(static_cast<uint8_t>(lat_cpr >> 7));
    frame.push_byte(static_cast<uint8_t>((lat_cpr << 1) | (lon_cpr >> 16)));
    frame.push_byte(static_cast<uint8_t>(lon_cpr >> 8));
    frame.push_byte(static_cast<uint8_t>(lon_cpr));
    frame.make_CRC();

    return frame;
}

struct Capture {
    std::vector<AdsbFrame> frames;
    std::vector<float> amps;

    app::adsb::AdsbDemod::FrameHandler handler() {
        return [this](const AdsbFrame& f, float amp) {
            frames.push_back(f);
            amps.push_back(amp);
        };
    }
};

bool same_bytes(const AdsbFrame& frame, const std::vector<uint8_t>& expected) {
    const uint8_t* raw = frame.get_raw_data();
    for (size_t i = 0; i < expected.size(); i++)
        if (raw[i] != expected[i]) return false;
    return true;
}

std::string hex_of(const AdsbFrame& frame, size_t n) {
    static const char* digits = "0123456789ABCDEF";
    const uint8_t* raw = frame.get_raw_data();
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += digits[raw[i] >> 4];
        s += digits[raw[i] & 0xF];
    }
    return s;
}

}  // namespace

/* =========================================================================
 * CRC-24
 * ========================================================================= */

TEST(adsb_crc_matches_upstream_algorithm) {
    /* The port uses the truncated polynomial 0xFFF409 in a conventional
     * MSB-first CRC register; upstream XORs 0x1205FFF in at each set bit. They
     * have to agree on every input. */
    std::vector<uint8_t> data(14, 0);
    uint32_t seed = 12345;

    for (int trial = 0; trial < 200; trial++) {
        for (size_t i = 0; i < data.size(); i++) {
            seed = seed * 1103515245u + 12345u;
            data[i] = static_cast<uint8_t>(seed >> 16);
        }

        CHECK_EQ(app::adsb::mode_s_crc(data.data(), 11), upstream_compute_crc(data.data(), 11));
        CHECK_EQ(app::adsb::mode_s_crc(data.data(), 4), upstream_compute_crc(data.data(), 4));
    }
}

TEST(adsb_crc_known_all_zero_message) {
    /* An all-zero message has a zero remainder: no set bit ever pulls the
     * generator in. */
    const uint8_t zeros[11] = {0};
    CHECK_EQ(app::adsb::mode_s_crc(zeros, 11), 0u);
}

TEST(adsb_crc_polynomial_identity) {
    /* The message 0x01FFF409 *is* the Mode S generator: x^24 + x^23..x^12 +
     * x^10 + x^3 + 1. A CRC is the remainder modulo the generator, so feeding
     * the generator in must leave nothing behind. This pins the polynomial
     * down: no other 24-bit generator divides this message. */
    const uint8_t generator_message[4] = {0x01, 0xFF, 0xF4, 0x09};
    CHECK_EQ(app::adsb::mode_s_crc(generator_message, 4), 0u);

    /* Any single-bit change to it must not. */
    for (int byte = 0; byte < 4; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t damaged[4];
            std::memcpy(damaged, generator_message, 4);
            damaged[byte] ^= static_cast<uint8_t>(1 << bit);
            CHECK(app::adsb::mode_s_crc(damaged, 4) != 0u);
        }
    }
}

TEST(adsb_crc_documented_frames_verify) {
    for (const char* hex : {kFrameIdent, kFramePosEven, kFramePosOdd, kFrameVelo}) {
        AdsbFrame frame = frame_from_hex(hex);
        CHECK_EQ(frame.size(), size_t{14});
        CHECK(frame.is_long());
        CHECK_EQ(frame.data_length(), size_t{11});
        CHECK_EQ(frame.check_CRC(), 0u);
    }
}

TEST(adsb_crc_detects_corruption) {
    AdsbFrame frame = frame_from_hex(kFrameIdent);
    CHECK_EQ(frame.check_CRC(), 0u);

    /* Every single-bit error in the payload has to show up. */
    for (size_t byte = 0; byte < 11; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            AdsbFrame damaged = frame_from_hex(kFrameIdent);
            damaged.get_raw_data()[byte] ^= static_cast<uint8_t>(1 << bit);
            CHECK(damaged.check_CRC() != 0u);
        }
    }

    /* And so does a corrupted parity field. */
    AdsbFrame bad_parity = frame_from_hex(kFrameIdent);
    bad_parity.get_raw_data()[13] ^= 0x01;
    CHECK_EQ(bad_parity.check_CRC(), 1u);
}

TEST(adsb_crc_make_reproduces_documented_parity) {
    /* Recomputing the parity of a known-good frame must put back exactly the
     * bytes that were already there. */
    AdsbFrame frame = frame_from_hex(kFrameIdent);
    const std::vector<uint8_t> original = bytes_from_hex(kFrameIdent);

    frame.get_raw_data()[11] = 0;
    frame.get_raw_data()[12] = 0;
    frame.get_raw_data()[13] = 0;
    frame.make_CRC();

    CHECK(same_bytes(frame, original));
    CHECK_STR_EQ(hex_of(frame, 14), std::string{kFrameIdent});
}

TEST(adsb_crc_short_frame_parity_position) {
    /* DF < 16 is a 56-bit frame: 4 payload bytes and parity in bytes 4..6. */
    AdsbFrame frame;
    frame.clear();
    frame.push_byte(0x20);  /* DF = 4 */
    frame.push_byte(0x00);
    frame.push_byte(0x18);
    frame.push_byte(0x38);
    frame.make_CRC();

    CHECK(!frame.is_long());
    CHECK_EQ(frame.data_length(), size_t{4});
    CHECK_EQ(frame.check_CRC(), 0u);
    CHECK_EQ(frame.size(), size_t{7});

    /* The parity of a short frame lives in bytes 4-6, and bytes 7..13 stay
     * untouched. */
    const uint32_t expected = upstream_compute_crc(frame.get_raw_data(), 4);
    const uint8_t* raw = frame.get_raw_data();
    CHECK_EQ((static_cast<uint32_t>(raw[4]) << 16) | (static_cast<uint32_t>(raw[5]) << 8) | raw[6],
             expected);
    for (size_t i = 7; i < 14; i++) CHECK_EQ(raw[i], uint8_t{0});
}

/* =========================================================================
 * Frame field accessors
 * ========================================================================= */

TEST(adsb_frame_fields) {
    AdsbFrame frame = frame_from_hex(kFrameIdent);

    CHECK_EQ(frame.get_DF(), uint8_t{17});
    CHECK_EQ(frame.get_ICAO_address(), 0x4840D6u);
    CHECK_EQ(frame.get_msg_type(), uint8_t{4});   /* aircraft identification */
    CHECK_EQ(frame.get_msg_sub(), uint8_t{0});

    AdsbFrame velo = frame_from_hex(kFrameVelo);
    CHECK_EQ(velo.get_DF(), uint8_t{17});
    CHECK_EQ(velo.get_ICAO_address(), 0x485020u);
    CHECK_EQ(velo.get_msg_type(), uint8_t{19});   /* airborne velocity */
    CHECK_EQ(velo.get_msg_sub(), uint8_t{1});     /* ground speed, subsonic */
}

TEST(adsb_frame_push_byte_is_bounded) {
    AdsbFrame frame;
    frame.clear();
    for (int i = 0; i < 32; i++) frame.push_byte(static_cast<uint8_t>(i));

    CHECK_EQ(frame.size(), AdsbFrame::max_bytes);
    CHECK_EQ(frame.get_raw_data()[13], uint8_t{13});
}

/* =========================================================================
 * Callsign (6-bit identification)
 * ========================================================================= */

TEST(adsb_callsign_documented_message) {
    AdsbFrame frame = frame_from_hex(kFrameIdent);
    CHECK_STR_EQ(app::adsb::decode_frame_id(frame), "KLM1023 ");
}

TEST(adsb_callsign_charset_round_trip) {
    /* Build the identification ME by hand from the 6-bit alphabet: A=1..Z=26,
     * space=32, '0'..'9' = 48..57. "AZ09 X Y" exercises all three runs. */
    const char* text = "AZ09 X Y";
    const uint8_t codes[8] = {1, 26, 48, 57, 32, 24, 32, 25};

    uint64_t coded = 0;
    for (int i = 0; i < 8; i++) coded = (coded << 6) | codes[i];

    AdsbFrame frame;
    frame.clear();
    frame.push_byte(0x8D);
    frame.push_byte(0x40);
    frame.push_byte(0x62);
    frame.push_byte(0x1D);
    frame.push_byte(4 << 3);  /* TC = 4 */
    for (int c = 0; c < 6; c++)
        frame.push_byte(static_cast<uint8_t>((coded >> ((5 - c) * 8)) & 0xFF));
    frame.make_CRC();

    CHECK_STR_EQ(app::adsb::decode_frame_id(frame), text);
    CHECK_EQ(frame.check_CRC(), 0u);
}

TEST(adsb_callsign_invalid_code_points_show_as_hash) {
    /* Code point 0 and 27..31 carry no character; upstream's table maps them to
     * '#', which is how the Comm-B path tells a real callsign from noise. */
    uint64_t coded = 0;
    const uint8_t codes[8] = {0, 27, 28, 29, 30, 31, 1, 2};
    for (int i = 0; i < 8; i++) coded = (coded << 6) | codes[i];

    AdsbFrame frame;
    frame.clear();
    for (int i = 0; i < 5; i++) frame.push_byte(0);
    for (int c = 0; c < 6; c++)
        frame.push_byte(static_cast<uint8_t>((coded >> ((5 - c) * 8)) & 0xFF));

    CHECK_STR_EQ(app::adsb::decode_frame_id(frame), "######AB");
    CHECK(app::adsb::decode_frame_id(frame).find('#') != std::string::npos);
}

/* =========================================================================
 * CPR position
 * ========================================================================= */

TEST(adsb_cpr_nl_boundaries) {
    CHECK_EQ(app::adsb::cpr_NL(0.0), 59);
    CHECK_EQ(app::adsb::cpr_NL(87.0), 2);
    CHECK_EQ(app::adsb::cpr_NL(-87.0), 2);
    CHECK_EQ(app::adsb::cpr_NL(87.5), 1);
    CHECK_EQ(app::adsb::cpr_NL(90.0), 1);
    CHECK_EQ(app::adsb::cpr_NL(-90.0), 1);

    /* Upstream's lookup table gives the zone boundaries: NL is 59 - index of
     * the first table entry the latitude is below. 52.2572 sits below the
     * entry at index 23 (53.09516153), so NL = 36. */
    CHECK_EQ(app::adsb::cpr_NL(52.2572), 36);
    CHECK_EQ(app::adsb::cpr_NL(-52.2572), 36);
    CHECK_EQ(app::adsb::cpr_NL(10.0), 59);
    CHECK_EQ(app::adsb::cpr_NL(11.0), 58);

    /* N is NL minus the frame parity, never below 1. */
    CHECK_EQ(app::adsb::cpr_N(52.2572, 0), 36);
    CHECK_EQ(app::adsb::cpr_N(52.2572, 1), 35);
    CHECK_EQ(app::adsb::cpr_N(89.0, 1), 1);
    CHECK_NEAR(app::adsb::cpr_Dlon(52.2572, 0), 10.0, 1e-9);
}

TEST(adsb_cpr_mod_is_positive) {
    CHECK_NEAR(app::adsb::cpr_mod(8.0, 60.0), 8.0, 1e-9);
    CHECK_NEAR(app::adsb::cpr_mod(-1.0, 60.0), 59.0, 1e-9);
    CHECK_NEAR(app::adsb::cpr_mod(61.0, 60.0), 1.0, 1e-9);
}

TEST(adsb_cpr_raw_fields) {
    /* The CPR fields the published worked example quotes for this pair. */
    AdsbFrame even = frame_from_hex(kFramePosEven);
    AdsbFrame odd = frame_from_hex(kFramePosOdd);

    const uint8_t* e = even.get_raw_data();
    const uint8_t* o = odd.get_raw_data();

    const uint32_t lat_cpr_even = static_cast<uint32_t>(((e[6] & 3) << 15) | (e[7] << 7) | (e[8] >> 1));
    const uint32_t lon_cpr_even = static_cast<uint32_t>(((e[8] & 1) << 16) | (e[9] << 8) | e[10]);
    const uint32_t lat_cpr_odd = static_cast<uint32_t>(((o[6] & 3) << 15) | (o[7] << 7) | (o[8] >> 1));
    const uint32_t lon_cpr_odd = static_cast<uint32_t>(((o[8] & 1) << 16) | (o[9] << 8) | o[10]);

    CHECK_EQ(lat_cpr_even, 93000u);
    CHECK_EQ(lon_cpr_even, 51372u);
    CHECK_EQ(lat_cpr_odd, 74158u);
    CHECK_EQ(lon_cpr_odd, 50194u);

    /* Frame parity: bit 2 of byte 6, clear for even and set for odd. */
    CHECK_EQ(e[6] & 4, 0);
    CHECK(o[6] & 4);

    /* Both carry the same barometric altitude, 38000 ft. */
    bool valid = false;
    CHECK_EQ(app::adsb::decode_me_altitude(e, valid), 38000);
    CHECK(valid);
    valid = false;
    CHECK_EQ(app::adsb::decode_me_altitude(o, valid), 38000);
    CHECK(valid);
}

TEST(adsb_cpr_position_even_newer) {
    AdsbFrame even = frame_from_hex(kFramePosEven);
    AdsbFrame odd = frame_from_hex(kFramePosOdd);

    even.set_rx_timestamp(100);
    odd.set_rx_timestamp(99);

    const auto pos = app::adsb::decode_frame_pos(even, odd);

    CHECK(pos.pos_valid);
    CHECK(pos.alt_valid);
    CHECK_EQ(pos.altitude, 38000);
    /* The published result for this pair with the even frame the more recent. */
    CHECK_NEAR(pos.latitude, 52.2572, 1e-4);
    CHECK_NEAR(pos.longitude, 3.91937, 1e-4);
}

TEST(adsb_cpr_position_odd_newer) {
    AdsbFrame even = frame_from_hex(kFramePosEven);
    AdsbFrame odd = frame_from_hex(kFramePosOdd);

    even.set_rx_timestamp(99);
    odd.set_rx_timestamp(100);

    const auto pos = app::adsb::decode_frame_pos(even, odd);

    CHECK(pos.pos_valid);
    /* Latitude of the odd frame: (360/59) * (mod(j,59) + 74158/131072) with
     * j = 8, i.e. 52.26578 -- the published intermediate for this pair. The
     * longitude then uses N = NL - 1 = 35 zones:
     * (360/35) * 50194/131072 = 3.93891. */
    CHECK_NEAR(pos.latitude, 52.26578, 1e-4);
    CHECK_NEAR(pos.longitude, 3.93891, 1e-4);
}

TEST(adsb_pos_frame_encoder_matches_documented_frame) {
    /* The test's own frame builder, checked against the published hex. If this
     * passes, frames built from arbitrary CPR values below are trustworthy. */
    const AdsbFrame even = make_pos_frame(0x40621D, 38000, 93000, 51372, 0);
    CHECK_STR_EQ(hex_of(even, 14), std::string{kFramePosEven});

    const AdsbFrame odd = make_pos_frame(0x40621D, 38000, 74158, 50194, 1);
    CHECK_STR_EQ(hex_of(odd, 14), std::string{kFramePosOdd});
}

TEST(adsb_cpr_rejects_different_latitude_zones) {
    /* CPR pins the even and odd latitudes to within 6/59 of a degree of each
     * other, so a zone mismatch only happens when the pair straddles a zone
     * boundary. NL changes from 36 to 35 at 53.09516153 degrees; these two CPR
     * latitudes land either side of it:
     *   even 110949/131072 -> 6*(8 + 0.846472)      = 53.07883  (NL 36)
     *   odd   92498/131072 -> (360/59)*(8+0.705698) = 53.11951  (NL 35)
     * and the latitude index j is still 8 for the pair. */
    AdsbFrame even = make_pos_frame(0x40621D, 38000, 110949, 51372, 0);
    AdsbFrame odd = make_pos_frame(0x40621D, 38000, 92498, 50194, 1);

    even.set_rx_timestamp(100);
    odd.set_rx_timestamp(99);

    const auto pos = app::adsb::decode_frame_pos(even, odd);
    CHECK(!pos.pos_valid);
    /* Altitude still comes back: it does not depend on the pair. */
    CHECK(pos.alt_valid);
    CHECK_EQ(pos.altitude, 38000);

    /* Spell out the reason the pair was refused. */
    const double lat_even = 6.0 * (8.0 + (110949.0 / 131072.0));
    const double lat_odd = (360.0 / 59.0) * (8.0 + (92498.0 / 131072.0));
    CHECK_NEAR(lat_even, 53.07883, 1e-4);
    CHECK_NEAR(lat_odd, 53.11951, 1e-4);
    CHECK_EQ(app::adsb::cpr_NL(lat_even), 36);
    CHECK_EQ(app::adsb::cpr_NL(lat_odd), 35);
}

TEST(adsb_me_altitude_without_q_bit_is_invalid) {
    AdsbFrame even = frame_from_hex(kFramePosEven);
    even.get_raw_data()[5] &= static_cast<uint8_t>(~1);  /* clear Q */

    bool valid = true;
    const int32_t alt = app::adsb::decode_me_altitude(even.get_raw_data(), valid);
    CHECK(!valid);
    CHECK_EQ(alt, 0);
}

/* =========================================================================
 * Velocity
 * ========================================================================= */

TEST(adsb_velocity_documented_ground_speed) {
    AdsbFrame frame = frame_from_hex(kFrameVelo);
    const auto v = app::adsb::decode_frame_velo(frame);

    CHECK(v.valid);
    CHECK_EQ(static_cast<int>(v.type), static_cast<int>(app::adsb::SPD_GND));
    /* Components are 8 kt west and 159 kt south: hypot = 159.20 -> 159 kt,
     * bearing atan2(-8, -159) = 182.88 deg -> 183. */
    CHECK_EQ(v.speed, 159);
    CHECK_EQ(static_cast<int>(v.heading), 183);
    CHECK_EQ(v.v_rate, -832);
}

TEST(adsb_velocity_airspeed_subtype) {
    /* Built from the ME layout for subtype 3 (airborne, subsonic airspeed):
     *   byte5 bit 2   heading status = 1
     *   byte5 bits1-0 + byte6  10-bit heading, 512 -> 512*45/128 = 180 deg
     *   byte7 bit 7   0 = IAS, 1 = TAS
     *   byte7 bits6-0 + byte8 bits7-5   10-bit airspeed, 251 -> 250 kt
     *   byte8 bit 3   vertical rate sign, 0 = climb
     *   byte8 bits2-0 + byte9 bits7-2   9-bit rate, 17 -> (17-1)*64 = 1024 */
    AdsbFrame frame;
    frame.clear();
    frame.push_byte(0x8D);
    frame.push_byte(0x48);
    frame.push_byte(0x50);
    frame.push_byte(0x20);
    frame.push_byte(static_cast<uint8_t>((19 << 3) | 3));
    frame.push_byte(0x06);  /* status set, heading bit 8 set */
    frame.push_byte(0x00);
    frame.push_byte(0x1F);  /* IAS, airspeed high bits */
    frame.push_byte(0x60);  /* airspeed low bits, rate high bits = 0 */
    frame.push_byte(0x44);  /* rate low bits = 17 */
    frame.push_byte(0x00);
    frame.make_CRC();

    const auto v = app::adsb::decode_frame_velo(frame);
    CHECK(v.valid);
    CHECK_EQ(static_cast<int>(v.type), static_cast<int>(app::adsb::SPD_IAS));
    CHECK_EQ(static_cast<int>(v.heading), 180);
    CHECK_EQ(v.speed, 250);
    CHECK_EQ(v.v_rate, 1024);

    /* Subtype 4 is the same encoding for supersonic aircraft: speeds are in
     * units of 4 knots. */
    frame.get_raw_data()[4] = static_cast<uint8_t>((19 << 3) | 4);
    const auto v4 = app::adsb::decode_frame_velo(frame);
    CHECK_EQ(v4.speed, 1000);
    CHECK_EQ(static_cast<int>(v4.type), static_cast<int>(app::adsb::SPD_IAS));
}

TEST(adsb_velocity_missing_components_are_not_valid) {
    /* A ground-speed message with a zero east-west or north-south field means
     * "not available", not "zero knots". */
    AdsbFrame frame = frame_from_hex(kFrameVelo);
    frame.get_raw_data()[5] &= static_cast<uint8_t>(~0x03);
    frame.get_raw_data()[6] = 0x00;

    const auto v = app::adsb::decode_frame_velo(frame);
    CHECK(!v.valid);
    CHECK_EQ(v.speed, 0);
    /* The vertical rate is still present in every subtype. */
    CHECK_EQ(v.v_rate, -832);
}

TEST(adsb_velocity_unknown_subtype_decodes_nothing) {
    AdsbFrame frame = frame_from_hex(kFrameVelo);
    frame.get_raw_data()[4] = static_cast<uint8_t>((19 << 3) | 0);

    const auto v = app::adsb::decode_frame_velo(frame);
    CHECK(!v.valid);
    CHECK_EQ(v.speed, 0);
    CHECK_EQ(v.v_rate, 0);
}

TEST(adsb_velocity_direction_signs) {
    /* East-west sign is byte5 bit 2, north-south sign is byte7 bit 7. Clearing
     * both turns the documented 8 kt west / 159 kt south into 8 kt east /
     * 159 kt north: bearing atan2(8, 159) = 2.88 deg -> 3. */
    AdsbFrame frame = frame_from_hex(kFrameVelo);
    frame.get_raw_data()[5] &= static_cast<uint8_t>(~0x04);
    frame.get_raw_data()[7] &= static_cast<uint8_t>(~0x80);

    const auto v = app::adsb::decode_frame_velo(frame);
    CHECK(v.valid);
    CHECK_EQ(v.speed, 159);
    CHECK_EQ(static_cast<int>(v.heading), 3);
}

/* =========================================================================
 * Identity (squawk) and AC13 altitude
 * ========================================================================= */

TEST(adsb_squawk_interleave) {
    /* The 13-bit identity field is C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4.
     * Squawk 7500 is A=7, B=5, C=0, D=0. */
    uint8_t s[2] = {0, 0};
    s[0] = 0x08 | 0x02;         /* A1, A2 */
    s[1] = 0x80 | 0x20 | 0x02;  /* A4, B1, B4 */
    CHECK_EQ(app::adsb::decode_squawk(s), uint16_t{7500});

    /* 1200: A=1, B=2. */
    s[0] = 0x08;  /* A1 */
    s[1] = 0x08;  /* B2 */
    CHECK_EQ(app::adsb::decode_squawk(s), uint16_t{1200});

    /* 0000 and the all-ones 7777. */
    s[0] = 0x00;
    s[1] = 0x00;
    CHECK_EQ(app::adsb::decode_squawk(s), uint16_t{0});

    s[0] = 0x10 | 0x08 | 0x04 | 0x02 | 0x01;                       /* C1 A1 C2 A2 C4 */
    s[1] = 0x80 | 0x20 | 0x10 | 0x08 | 0x04 | 0x02 | 0x01;         /* A4 B1 D1 B2 D2 B4 D4 */
    CHECK_EQ(app::adsb::decode_squawk(s), uint16_t{7777});
}

TEST(adsb_ac13_altitude) {
    /* DF4 surveillance reply. The 11-bit N field is spread over bytes 2 and 3
     * around the M and Q bits; N = 1560 gives 25*1560 - 1000 = 38000 ft. */
    uint8_t raw[14] = {0};
    raw[0] = 0x20;  /* DF = 4 */
    raw[2] = 0x18;  /* N bits 10..6 = 24 */
    raw[3] = 0x38;  /* N bit 4, Q = 1, N bits 3..0 = 8 */

    bool valid = false;
    CHECK_EQ(app::adsb::decode_ac13_altitude(raw, valid), 38000);
    CHECK(valid);

    /* Q clear means a Gillham-coded altitude, which is not decoded. */
    raw[3] = static_cast<uint8_t>(raw[3] & ~0x10);
    valid = true;
    CHECK_EQ(app::adsb::decode_ac13_altitude(raw, valid), 0);
    CHECK(!valid);

    /* M set means metric units, also not decoded. */
    raw[3] = 0x38 | 0x40;
    valid = true;
    CHECK_EQ(app::adsb::decode_ac13_altitude(raw, valid), 0);
    CHECK(!valid);

    /* The minimum: N = 0 is -1000 ft. */
    raw[2] = 0x00;
    raw[3] = 0x10;  /* Q only */
    valid = false;
    CHECK_EQ(app::adsb::decode_ac13_altitude(raw, valid), -1000);
    CHECK(valid);
}

/* =========================================================================
 * Demodulator
 * ========================================================================= */

TEST(adsb_demod_decodes_synthesised_long_frame) {
    const auto bytes = bytes_from_hex(kFrameIdent);
    const auto mags = make_ppm_magnitudes(bytes, 112);

    app::adsb::AdsbDemod demod;
    Capture cap;
    demod.process_magnitudes(mags.data(), mags.size(), cap.handler());

    CHECK_EQ(cap.frames.size(), size_t{1});
    if (cap.frames.empty()) return;

    CHECK_STR_EQ(hex_of(cap.frames[0], 14), std::string{kFrameIdent});
    CHECK_EQ(cap.frames[0].check_CRC(), 0u);
    CHECK_EQ(cap.frames[0].get_ICAO_address(), 0x4840D6u);
    CHECK_EQ(demod.preambles_detected(), 1u);

    /* amp is the sum of the four preamble spikes. */
    CHECK_NEAR(cap.amps[0], 4.0f, 1e-5);
}

TEST(adsb_demod_decodes_synthesised_short_frame) {
    /* A 56-bit frame: the demodulator has to notice from the first byte that
     * only 56 bits are coming. */
    AdsbFrame source;
    source.clear();
    source.push_byte(0x20);  /* DF = 4, short */
    source.push_byte(0x00);
    source.push_byte(0x18);
    source.push_byte(0x38);
    source.make_CRC();

    std::vector<uint8_t> bytes(source.get_raw_data(), source.get_raw_data() + 7);
    const auto mags = make_ppm_magnitudes(bytes, 56);

    app::adsb::AdsbDemod demod;
    Capture cap;
    demod.process_magnitudes(mags.data(), mags.size(), cap.handler());

    CHECK_EQ(cap.frames.size(), size_t{1});
    if (cap.frames.empty()) return;

    CHECK(!cap.frames[0].is_long());
    CHECK_EQ(cap.frames[0].check_CRC(), 0u);
    for (size_t i = 0; i < 7; i++)
        CHECK_EQ(cap.frames[0].get_raw_data()[i], bytes[i]);
}

TEST(adsb_demod_decodes_two_frames_back_to_back) {
    const auto a = bytes_from_hex(kFramePosEven);
    const auto b = bytes_from_hex(kFramePosOdd);

    auto mags = make_ppm_magnitudes(a, 112);
    const auto second = make_ppm_magnitudes(b, 112);
    mags.insert(mags.end(), second.begin(), second.end());

    app::adsb::AdsbDemod demod;
    Capture cap;
    demod.process_magnitudes(mags.data(), mags.size(), cap.handler());

    CHECK_EQ(cap.frames.size(), size_t{2});
    if (cap.frames.size() < 2) return;

    CHECK_STR_EQ(hex_of(cap.frames[0], 14), std::string{kFramePosEven});
    CHECK_STR_EQ(hex_of(cap.frames[1], 14), std::string{kFramePosOdd});
}

TEST(adsb_demod_survives_chunking) {
    /* Upstream keeps its bit accumulator in a local that resets on every
     * baseband buffer, so a frame split across two buffers decodes wrong. The
     * host keeps it in the demodulator, so any chunk size gives the same
     * answer. */
    const auto bytes = bytes_from_hex(kFrameVelo);
    const auto mags = make_ppm_magnitudes(bytes, 112);

    for (size_t chunk : {size_t{1}, size_t{3}, size_t{17}, size_t{64}, size_t{129}}) {
        app::adsb::AdsbDemod demod;
        Capture cap;

        for (size_t i = 0; i < mags.size(); i += chunk) {
            const size_t n = std::min(chunk, mags.size() - i);
            demod.process_magnitudes(mags.data() + i, n, cap.handler());
        }

        CHECK_EQ(cap.frames.size(), size_t{1});
        if (!cap.frames.empty())
            CHECK_STR_EQ(hex_of(cap.frames[0], 14), std::string{kFrameVelo});
    }
}

TEST(adsb_demod_from_complex_samples) {
    const auto bytes = bytes_from_hex(kFrameIdent);
    const auto mags = make_ppm_magnitudes(bytes, 112);
    const auto iq = to_complex(mags);

    app::adsb::AdsbDemod demod;
    demod.set_input_rate(app::adsb::AdsbDemod::kNativeSampleRate);

    Capture cap;
    demod.process(iq.data(), iq.size(), cap.handler());

    CHECK_EQ(cap.frames.size(), size_t{1});
    if (!cap.frames.empty())
        CHECK_STR_EQ(hex_of(cap.frames[0], 14), std::string{kFrameIdent});
}

TEST(adsb_demod_resamples_from_4msps) {
    /* 4 Msps is what a device that cannot do exactly 2 Msps might deliver. The
     * interpolator has to bring it back to two samples per bit. */
    const auto bytes = bytes_from_hex(kFramePosEven);
    const auto mags = make_ppm_magnitudes(bytes, 112);
    const auto fast = oversample(mags, 2);
    const auto iq = to_complex(fast);

    app::adsb::AdsbDemod demod;
    demod.set_input_rate(4'000'000.0);

    Capture cap;
    demod.process(iq.data(), iq.size(), cap.handler());

    CHECK_EQ(cap.frames.size(), size_t{1});
    if (!cap.frames.empty()) {
        CHECK_STR_EQ(hex_of(cap.frames[0], 14), std::string{kFramePosEven});
        CHECK_EQ(cap.frames[0].check_CRC(), 0u);
    }
}

TEST(adsb_demod_ignores_noise) {
    /* A pseudo-random envelope must not produce a CRC-clean frame. Preambles
     * can and will trigger on noise -- that is what the CRC is for -- but a
     * frame that passes parity is a one-in-16-million accident. */
    std::vector<float> noise;
    uint32_t seed = 987654321u;
    for (size_t i = 0; i < 200000; i++) {
        seed = seed * 1103515245u + 12345u;
        noise.push_back(static_cast<float>((seed >> 16) & 0xFF) / 255.0f);
    }

    app::adsb::AdsbDemod demod;
    Capture cap;
    demod.process_magnitudes(noise.data(), noise.size(), cap.handler());

    for (const auto& f : cap.frames) CHECK(f.check_CRC() != 0u);
}

TEST(adsb_demod_truncated_burst_emits_nothing) {
    /* A preamble followed by only half a frame never completes. */
    const auto bytes = bytes_from_hex(kFrameIdent);
    auto mags = make_ppm_magnitudes(bytes, 112, 1.0f, 0.0f, 40, 0);
    mags.resize(40 + 16 + 100);  /* 50 bits of data, then nothing */

    app::adsb::AdsbDemod demod;
    Capture cap;
    demod.process_magnitudes(mags.data(), mags.size(), cap.handler());

    CHECK_EQ(cap.frames.size(), size_t{0});
    CHECK_EQ(demod.preambles_detected(), 1u);
}

TEST(adsb_demod_reset_clears_state) {
    const auto bytes = bytes_from_hex(kFrameIdent);
    auto mags = make_ppm_magnitudes(bytes, 112, 1.0f, 0.0f, 40, 0);
    mags.resize(40 + 16 + 100);

    app::adsb::AdsbDemod demod;
    Capture cap;
    demod.process_magnitudes(mags.data(), mags.size(), cap.handler());
    CHECK_EQ(cap.frames.size(), size_t{0});

    /* After a reset the leftover half-frame must not contaminate the next
     * burst. */
    demod.reset();
    const auto whole = make_ppm_magnitudes(bytes, 112);
    demod.process_magnitudes(whole.data(), whole.size(), cap.handler());

    CHECK_EQ(cap.frames.size(), size_t{1});
    if (!cap.frames.empty())
        CHECK_STR_EQ(hex_of(cap.frames[0], 14), std::string{kFrameIdent});
}

/* =========================================================================
 * Magnitude resampler
 * ========================================================================= */

TEST(adsb_resampler_bypass_is_exact) {
    app::adsb::MagnitudeResampler r;
    r.set_rate(2'000'000.0, 2'000'000.0);
    CHECK(r.bypass());

    const float in[5] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    std::vector<float> out;
    r.process(in, 5, out);

    CHECK_EQ(out.size(), size_t{5});
    for (size_t i = 0; i < 5; i++) CHECK_NEAR(out[i], in[i], 1e-6);
}

TEST(adsb_resampler_halves_an_integer_rate) {
    app::adsb::MagnitudeResampler r;
    r.set_rate(4'000'000.0, 2'000'000.0);
    CHECK(!r.bypass());
    CHECK_NEAR(r.step(), 2.0, 1e-12);

    std::vector<float> in;
    for (int i = 0; i < 20; i++) in.push_back(static_cast<float>(i));

    std::vector<float> out;
    r.process(in.data(), in.size(), out);

    /* Every other input sample, starting at the first. */
    CHECK_EQ(out.size(), size_t{10});
    for (size_t i = 0; i < out.size(); i++)
        CHECK_NEAR(out[i], static_cast<float>(i * 2), 1e-5);
}

TEST(adsb_resampler_interpolates_a_fractional_rate) {
    app::adsb::MagnitudeResampler r;
    r.set_rate(3'000'000.0, 2'000'000.0);
    CHECK_NEAR(r.step(), 1.5, 1e-12);

    /* A ramp makes the output positions readable: output j should land at
     * input position 1.5j, and a linear interpolation of a ramp is exact. */
    std::vector<float> in;
    for (int i = 0; i < 21; i++) in.push_back(static_cast<float>(i));

    std::vector<float> out;
    r.process(in.data(), in.size(), out);

    CHECK(out.size() >= 13);
    for (size_t j = 0; j < out.size(); j++)
        CHECK_NEAR(out[j], static_cast<float>(1.5 * static_cast<double>(j)), 1e-4);
}

TEST(adsb_resampler_upsamples) {
    app::adsb::MagnitudeResampler r;
    r.set_rate(1'000'000.0, 2'000'000.0);
    CHECK_NEAR(r.step(), 0.5, 1e-12);

    std::vector<float> in;
    for (int i = 0; i < 10; i++) in.push_back(static_cast<float>(i));

    std::vector<float> out;
    r.process(in.data(), in.size(), out);

    /* Two outputs per input, at input positions 0, 0.5, 1.0, ... */
    CHECK_EQ(out.size(), size_t{18});
    for (size_t j = 0; j < out.size(); j++)
        CHECK_NEAR(out[j], static_cast<float>(0.5 * static_cast<double>(j)), 1e-4);
}

TEST(adsb_resampler_is_continuous_across_calls) {
    app::adsb::MagnitudeResampler r;
    r.set_rate(3'000'000.0, 2'000'000.0);

    std::vector<float> in;
    for (int i = 0; i < 30; i++) in.push_back(static_cast<float>(i));

    std::vector<float> one_shot;
    r.process(in.data(), in.size(), one_shot);

    r.reset();
    std::vector<float> chunked;
    r.process(in.data(), 7, chunked);
    r.process(in.data() + 7, 11, chunked);
    r.process(in.data() + 18, in.size() - 18, chunked);

    CHECK_EQ(chunked.size(), one_shot.size());
    for (size_t i = 0; i < chunked.size() && i < one_shot.size(); i++)
        CHECK_NEAR(chunked[i], one_shot[i], 1e-5);
}

/* =========================================================================
 * Aircraft tracking
 * ========================================================================= */

TEST(adsb_tracker_accepts_and_parses_a_burst) {
    app::AircraftTracker tracker;

    AdsbFrame ident = frame_from_hex(kFrameIdent);
    CHECK(tracker.handle_frame(ident, 1000, 10));

    const auto* entry = tracker.find(0x4840D6);
    CHECK(entry != nullptr);
    if (!entry) return;

    CHECK_STR_EQ(entry->callsign, "KLM1023 ");
    CHECK_STR_EQ(entry->icao_str, "4840D6");
    CHECK_EQ(entry->hits, uint16_t{1});
    CHECK_EQ(entry->amp, 1000u);
}

TEST(adsb_tracker_builds_a_position_from_a_pair) {
    app::AircraftTracker tracker;

    AdsbFrame odd = frame_from_hex(kFramePosOdd);
    AdsbFrame even = frame_from_hex(kFramePosEven);

    CHECK(tracker.handle_frame(odd, 500, 10));
    CHECK(tracker.handle_frame(even, 500, 11));

    const auto* entry = tracker.find(0x40621D);
    CHECK(entry != nullptr);
    if (!entry) return;

    CHECK(entry->pos.pos_valid);
    CHECK_NEAR(entry->pos.latitude, 52.2572, 1e-4);
    CHECK_NEAR(entry->pos.longitude, 3.91937, 1e-4);
    CHECK_EQ(entry->pos.altitude, 38000);
    CHECK_EQ(entry->hits, uint16_t{2});

    /* And the info line the table and details view show. */
    CHECK_STR_EQ(entry->info_string, "Alt:38000 Lat:52.25 Lon:3.91");
}

TEST(adsb_tracker_ignores_a_stale_pair) {
    /* Frames more than 20 seconds apart describe different places. */
    app::AircraftTracker tracker;

    AdsbFrame odd = frame_from_hex(kFramePosOdd);
    AdsbFrame even = frame_from_hex(kFramePosEven);

    CHECK(tracker.handle_frame(odd, 500, 10));
    CHECK(tracker.handle_frame(even, 500, 10 + 25));

    const auto* entry = tracker.find(0x40621D);
    CHECK(entry != nullptr);
    if (entry) CHECK(!entry->pos.pos_valid);
}

TEST(adsb_tracker_rejects_bad_crc) {
    app::AircraftTracker tracker;

    AdsbFrame damaged = frame_from_hex(kFrameIdent);
    damaged.get_raw_data()[6] ^= 0x40;

    CHECK(!tracker.handle_frame(damaged, 100, 1));
    CHECK_EQ(tracker.entries().size(), size_t{0});
    CHECK_EQ(tracker.frames_seen(), 1u);
    CHECK_EQ(tracker.frames_accepted(), 0u);
}

TEST(adsb_tracker_rejects_zero_icao) {
    app::AircraftTracker tracker;

    AdsbFrame frame;
    frame.clear();
    frame.push_byte(0x8D);
    for (int i = 0; i < 10; i++) frame.push_byte(0x00);
    frame.make_CRC();

    CHECK_EQ(frame.check_CRC(), 0u);
    CHECK(!tracker.handle_frame(frame, 100, 1));
    CHECK_EQ(tracker.entries().size(), size_t{0});
}

TEST(adsb_tracker_accepts_address_parity_frames_from_known_aircraft) {
    /* DF 4/20/21 overlay the ICAO address on the parity field, so a clean frame
     * leaves the address as the syndrome. Upstream keys the entry on that only
     * when the address is already known from an extended squitter. */
    app::AircraftTracker tracker;

    AdsbFrame ident = frame_from_hex(kFrameIdent);
    CHECK(tracker.handle_frame(ident, 100, 1));

    /* Build a DF20 Comm-B altitude reply whose parity is CRC xor ICAO. */
    AdsbFrame commb;
    commb.clear();
    commb.push_byte(0xA0);  /* DF = 20 */
    commb.push_byte(0x00);
    commb.push_byte(0x18);
    commb.push_byte(0x38);  /* AC13 altitude, 38000 ft */
    for (int i = 0; i < 7; i++) commb.push_byte(0x00);
    commb.make_CRC();

    /* Overlay the address. */
    const uint32_t icao = 0x4840D6;
    commb.get_raw_data()[11] ^= static_cast<uint8_t>((icao >> 16) & 0xFF);
    commb.get_raw_data()[12] ^= static_cast<uint8_t>((icao >> 8) & 0xFF);
    commb.get_raw_data()[13] ^= static_cast<uint8_t>(icao & 0xFF);

    CHECK_EQ(commb.check_CRC(), icao);
    CHECK(tracker.handle_frame(commb, 100, 2));

    const auto* entry = tracker.find(icao);
    CHECK(entry != nullptr);
    if (!entry) return;
    CHECK_EQ(entry->hits, uint16_t{2});
    CHECK(entry->pos.alt_valid);
    CHECK_EQ(entry->pos.altitude, 38000);

    /* The same frame from an unknown aircraft is indistinguishable from noise
     * and must be dropped. */
    app::AircraftTracker fresh;
    AdsbFrame copy = commb;
    CHECK(!fresh.handle_frame(copy, 100, 2));
}

TEST(adsb_tracker_amplitude_is_smoothed) {
    app::AircraftTracker tracker;

    AdsbFrame f1 = frame_from_hex(kFrameIdent);
    CHECK(tracker.handle_frame(f1, 1600, 1));
    CHECK_EQ(tracker.find(0x4840D6)->amp, 1600u);

    /* Upstream's 1/16 exponential average: (1600*15 + 3200)/16 = 1700. */
    AdsbFrame f2 = frame_from_hex(kFrameIdent);
    CHECK(tracker.handle_frame(f2, 3200, 2));
    CHECK_EQ(tracker.find(0x4840D6)->amp, 1700u);
}

TEST(adsb_tracker_ages_and_expires) {
    app::AircraftTracker tracker;

    AdsbFrame ident = frame_from_hex(kFrameIdent);
    CHECK(tracker.handle_frame(ident, 100, 1));

    tracker.age_entries(5);
    CHECK_EQ(static_cast<int>(tracker.find(0x4840D6)->state),
             static_cast<int>(app::ADSBAgeState::Invalid));  /* no position yet */

    tracker.age_entries(10);  /* age 15 */
    CHECK_EQ(static_cast<int>(tracker.find(0x4840D6)->state),
             static_cast<int>(app::ADSBAgeState::Recent));

    tracker.age_entries(20);  /* age 35 */
    CHECK_EQ(static_cast<int>(tracker.find(0x4840D6)->state),
             static_cast<int>(app::ADSBAgeState::Old));

    tracker.age_entries(300);  /* age 335, past the 300 s limit */
    CHECK_EQ(tracker.entries().size(), size_t{0});
}

TEST(adsb_tracker_position_marks_entry_current) {
    app::AircraftTracker tracker;

    AdsbFrame odd = frame_from_hex(kFramePosOdd);
    AdsbFrame even = frame_from_hex(kFramePosEven);
    CHECK(tracker.handle_frame(odd, 100, 1));
    CHECK(tracker.handle_frame(even, 100, 2));

    tracker.age_entries(1);
    CHECK_EQ(static_cast<int>(tracker.find(0x40621D)->state),
             static_cast<int>(app::ADSBAgeState::Current));
}

TEST(adsb_tracker_truncates) {
    app::AircraftTracker tracker;

    /* Feed 80 distinct aircraft; only 64 may survive the ageing pass. */
    for (uint32_t i = 1; i <= 80; i++) {
        AdsbFrame frame = frame_from_hex(kFrameIdent);
        frame.get_raw_data()[1] = static_cast<uint8_t>(i);
        frame.get_raw_data()[2] = static_cast<uint8_t>(i >> 8);
        frame.get_raw_data()[3] = 0x01;
        frame.make_CRC();
        CHECK(tracker.handle_frame(frame, 100, 1));
    }

    CHECK_EQ(tracker.entries().size(), size_t{80});
    tracker.age_entries(1);
    CHECK_EQ(tracker.entries().size(), size_t{64});
}

TEST(adsb_tracker_ground_speed_corrects_ias) {
    app::AircraftRecentEntry entry{0x4840D6};

    entry.velo.valid = true;
    entry.velo.type = app::adsb::SPD_IAS;
    entry.velo.speed = 250;

    /* Without an altitude there is nothing to correct with. */
    CHECK_EQ(entry.get_ground_speed(), 250);

    /* +2% per 1000 ft: 250 * (1 + 0.02*30) = 400 in exact arithmetic. Upstream
     * evaluates this in float and truncates, and 0.02f is a hair under 1/50, so
     * the product is 399.9999966 and the result is 399. Kept as upstream has
     * it; a knot either way is below the resolution of the data. */
    entry.pos.alt_valid = true;
    entry.pos.altitude = 30000;
    CHECK_EQ(entry.get_ground_speed(), 399);

    entry.velo.type = app::adsb::SPD_GND;
    CHECK_EQ(entry.get_ground_speed(), 250);

    entry.velo.valid = false;
    CHECK_EQ(entry.get_ground_speed(), 0);
}

TEST(adsb_tracker_decodes_squawk_from_df21) {
    app::AircraftTracker tracker;

    /* Get the aircraft on the books with a clean extended squitter first. */
    AdsbFrame ident = frame_from_hex(kFrameIdent);
    CHECK(tracker.handle_frame(ident, 100, 1));

    AdsbFrame df21;
    df21.clear();
    df21.push_byte(static_cast<uint8_t>((21 << 3) | 0));  /* DF 21 */
    df21.push_byte(0x00);
    df21.push_byte(0x0A);  /* identity: A1, A2  */
    df21.push_byte(0xA2);  /* identity: A4, B1, B4 -> squawk 7500 */
    for (int i = 0; i < 7; i++) df21.push_byte(0x00);
    df21.make_CRC();

    const uint32_t icao = 0x4840D6;
    df21.get_raw_data()[11] ^= static_cast<uint8_t>((icao >> 16) & 0xFF);
    df21.get_raw_data()[12] ^= static_cast<uint8_t>((icao >> 8) & 0xFF);
    df21.get_raw_data()[13] ^= static_cast<uint8_t>(icao & 0xFF);

    CHECK(tracker.handle_frame(df21, 100, 2));
    CHECK_EQ(tracker.find(icao)->sqwk, uint16_t{7500});
}

TEST(adsb_tracker_skips_logging_df11) {
    /* DF11 all-call replies arrive constantly; upstream deliberately keeps them
     * out of the log. */
    app::AircraftTracker tracker;

    AdsbFrame df11;
    df11.clear();
    df11.push_byte(static_cast<uint8_t>(11 << 3));
    df11.push_byte(0x48);
    df11.push_byte(0x40);
    df11.push_byte(0xD6);
    df11.make_CRC();

    app::AdsbLogEntry log_entry;
    bool logged = true;
    CHECK(tracker.handle_frame(df11, 100, 1, &log_entry, &logged));
    CHECK(!logged);
    CHECK_STR_EQ(log_entry.raw_data, "");
}

/* =========================================================================
 * Log formatting
 * ========================================================================= */

TEST(adsb_log_line_format) {
    app::AdsbLogEntry entry;
    entry.raw_data = "8D4840D6202CC371C32CE0576098";
    entry.icao = "4840D6";
    entry.callsign = "KLM1023 ";
    entry.pos.alt_valid = true;
    entry.pos.altitude = 38000;
    entry.pos.pos_valid = true;
    entry.pos.latitude = 52.2572f;
    entry.pos.longitude = 3.91937f;
    entry.vel.valid = true;
    entry.vel.type = app::adsb::SPD_GND;
    entry.vel.speed = 159;
    entry.vel.heading = 183;
    entry.vel.v_rate = -832;
    entry.vel_type = 1;
    entry.sqwk = 7500;
    entry.sil = 2;

    const std::string line = app::format_adsb_log_line(entry);

    CHECK(line.find("8D4840D6202CC371C32CE0576098") == 0);
    CHECK(line.find(" ICAO:4840D6") != std::string::npos);
    CHECK(line.find(" Squawk:7500") != std::string::npos);
    CHECK(line.find(" KLM1023 ") != std::string::npos);
    CHECK(line.find(" Alt:38000") != std::string::npos);
    CHECK(line.find(" Lat:52.") != std::string::npos);
    CHECK(line.find(" Lon:3.") != std::string::npos);
    CHECK(line.find(" Type:1") != std::string::npos);
    CHECK(line.find(" Hdg:183") != std::string::npos);
    CHECK(line.find(" Spd:159") != std::string::npos);
    CHECK(line.find(" Vrate:-832") != std::string::npos);
    CHECK(line.find(" Sil:2") != std::string::npos);
}

TEST(adsb_log_line_omits_absent_fields) {
    app::AdsbLogEntry entry;
    entry.raw_data = "AAAA";
    entry.icao = "ABCDEF";

    const std::string line = app::format_adsb_log_line(entry);
    CHECK_STR_EQ(line, "AAAA ICAO:ABCDEF");
}

TEST(adsb_log_line_uses_airspeed_labels) {
    app::AdsbLogEntry entry;
    entry.raw_data = "AAAA";
    entry.icao = "ABCDEF";
    entry.vel.valid = true;
    entry.vel.type = app::adsb::SPD_IAS;
    entry.vel.speed = 250;
    entry.vel.heading = 180;
    entry.vel_type = 3;

    CHECK(app::format_adsb_log_line(entry).find(" IAS:250") != std::string::npos);

    entry.vel.type = app::adsb::SPD_TAS;
    CHECK(app::format_adsb_log_line(entry).find(" TAS:250") != std::string::npos);
}

/* =========================================================================
 * End to end: waveform in, aircraft out
 * ========================================================================= */

TEST(adsb_end_to_end_waveform_to_tracked_aircraft) {
    /* Three bursts on one synthetic 2 Msps stream: identity, then an odd and an
     * even position frame. Everything from preamble hunting to CPR runs. */
    auto stream = make_ppm_magnitudes(bytes_from_hex(kFrameIdent), 112);
    for (const char* hex : {kFramePosOdd, kFramePosEven}) {
        const auto burst = make_ppm_magnitudes(bytes_from_hex(hex), 112);
        stream.insert(stream.end(), burst.begin(), burst.end());
    }

    app::adsb::AdsbDemod demod;
    app::AircraftTracker tracker;
    uint32_t rx_time = 100;

    demod.process_magnitudes(stream.data(), stream.size(),
                             [&tracker, &rx_time](const AdsbFrame& f, float amp) {
                                 AdsbFrame frame = f;
                                 tracker.handle_frame(frame, static_cast<uint32_t>(amp),
                                                      rx_time++);
                             });

    CHECK_EQ(tracker.frames_seen(), 3u);
    CHECK_EQ(tracker.frames_accepted(), 3u);
    CHECK_EQ(tracker.entries().size(), size_t{2});

    const auto* klm = tracker.find(0x4840D6);
    CHECK(klm != nullptr);
    if (klm) CHECK_STR_EQ(klm->callsign, "KLM1023 ");

    const auto* pos_ac = tracker.find(0x40621D);
    CHECK(pos_ac != nullptr);
    if (!pos_ac) return;

    CHECK(pos_ac->pos.pos_valid);
    CHECK_NEAR(pos_ac->pos.latitude, 52.2572, 1e-4);
    CHECK_NEAR(pos_ac->pos.longitude, 3.91937, 1e-4);
    CHECK_EQ(pos_ac->pos.altitude, 38000);
}

/* =========================================================================
 * Registration
 * ========================================================================= */

TEST(adsb_app_is_registered) {
    /* The app reaches the menu through its file-scope Registrar; nothing else
     * references it, so if the Registrar stops running the app silently
     * disappears. */
    const auto* entry = app::AppRegistry::instance().by_id("adsbrx");
    CHECK(entry != nullptr);
    if (!entry) return;

    CHECK_STR_EQ(entry->display_name, "ADS-B");
    CHECK_EQ(static_cast<int>(entry->category), static_cast<int>(app::Category::Receive));
    CHECK(entry->icon != nullptr);
    CHECK(!entry->hardware_limited);
    CHECK(static_cast<bool>(entry->factory));

    /* And it shows up under Receive. */
    bool found = false;
    for (const auto* e : app::AppRegistry::instance().by_category(app::Category::Receive))
        if (e->id == "adsbrx") found = true;
    CHECK(found);
}
