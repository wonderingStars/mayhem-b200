/*
 * mayhem-b200 — tests for VOR TX, ADS-B TX and EPIRB TX.
 *
 * Every expected value here is either a trivial closed-form (bearing degrees
 * -> radians), an already-published/pre-validated constant already trusted
 * elsewhere in this repo (tests/test_adsb.cpp's documented Mode S frames), or
 * a value re-derived independently of the code under test (the VOR
 * correlator phase-shift relationship below is proven algebraically in the
 * comment next to it, not copied from the generator's own source). Nothing
 * here asserts "whatever the port happens to produce".
 *
 * No hardware is attached, so these prove the encoders' bit-level and
 * arithmetic correctness only; actual radiation is unverified.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_registry.hpp"
#include "ui_adsb_tx.hpp"
#include "ui_epirb_tx.hpp"
#include "ui_vor_tx.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using app::adsb::AdsbFrame;

/* --- Hex helpers, same pattern as tests/test_adsb.cpp ---------------------- */

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

}  // namespace

/* =============================================================================
 * VOR TX
 * ========================================================================== */

TEST(vor_radial_to_offset_rad_matches_documented_angles) {
    /* Task spec: 0/90/180/270 degrees -> the phase offset the composite
     * generator subtracts from the reference tone's phase. */
    CHECK_NEAR(app::vor_tx::vor_radial_to_offset_rad(0), 0.0, 1e-12);
    CHECK_NEAR(app::vor_tx::vor_radial_to_offset_rad(90), M_PI / 2.0, 1e-12);
    CHECK_NEAR(app::vor_tx::vor_radial_to_offset_rad(180), M_PI, 1e-12);
    CHECK_NEAR(app::vor_tx::vor_radial_to_offset_rad(270), 3.0 * M_PI / 2.0, 1e-12);
}

TEST(vor_radial_to_offset_rad_wraps_boundary_values) {
    /* Boundary / "empty payload" cases: a full turn and negative bearings. */
    CHECK_NEAR(app::vor_tx::vor_radial_to_offset_rad(360), 0.0, 1e-12);
    CHECK_NEAR(app::vor_tx::vor_radial_to_offset_rad(720), 0.0, 1e-12);
    CHECK_NEAR(app::vor_tx::vor_radial_to_offset_rad(-90), 3.0 * M_PI / 2.0, 1e-12);
    CHECK_NEAR(app::vor_tx::vor_radial_to_offset_rad(-360), 0.0, 1e-12);
}

TEST(vor_generator_radial_accessor_round_trips) {
    app::vor_tx::VorTxGenerator gen;
    gen.configure(48000.0);
    gen.set_radial(123);
    CHECK_EQ(gen.radial(), int32_t{123});
}

namespace {

/* Correlates the real part of `count` composite samples (Q is always 0 for
 * this DSB-AM signal) against a local unit-amplitude 30 Hz reference
 * exp(-j*2*pi*30*t) starting at phase 0 — the same reference phase the
 * generator's own ref_phase_ starts at after configure(). `count` must be a
 * whole number of 30 Hz cycles at 48 kHz (1600 samples/cycle) so the DC
 * carrier and every subcarrier/identification component are exactly
 * orthogonal to the correlator and only the 30 Hz "variable" tone survives:
 *
 *   variable(i) = sin(i*step - offset), where step = 2*pi*30/fs
 *
 *   sum_i variable(i) * exp(-j*i*step)
 *     = sum_i [ (e^{j(i*step-offset)} - e^{-j(i*step-offset)}) / 2j ] * e^{-j*i*step}
 *     = (N/2j) * e^{-j*offset}   (the e^{-j*2*i*step} term sums to zero over
 *                                 whole cycles, same as the carrier does)
 *
 * which is a fixed complex constant K = N/2j (not depending on offset) times
 * e^{-j*offset}. So for any two radials the correlator's phase DIFFERENCE is
 * exactly -( offset(radial) - offset(reference) ), regardless of what K's own
 * phase is — that is what the test below checks, so it does not depend on
 * (or need to re-derive) that constant. */
double correlate_variable_tone_phase(int32_t radial_deg, size_t cycles = 3,
                                     double sample_rate_hz = 48000.0) {
    app::vor_tx::VorTxGenerator gen;
    gen.configure(sample_rate_hz);
    gen.set_ident_enabled(false);
    gen.set_radial(radial_deg);

    const double tone_hz = 30.0;
    const size_t window =
        static_cast<size_t>(cycles * static_cast<size_t>(sample_rate_hz / tone_hz));

    std::vector<dsp::cfloat> buf(window);
    gen.process(buf.data(), window);

    double corr_re = 0.0, corr_im = 0.0;
    double phase = 0.0;
    const double step = 2.0 * M_PI * tone_hz / sample_rate_hz;
    for (size_t i = 0; i < window; i++) {
        const double s = static_cast<double>(buf[i].real());
        corr_re += s * std::cos(phase);
        corr_im -= s * std::sin(phase);
        phase += step;
    }
    return std::atan2(corr_im, corr_re);
}

double wrap_pi(double a) {
    while (a <= -M_PI) a += 2.0 * M_PI;
    while (a > M_PI) a -= 2.0 * M_PI;
    return a;
}

}  // namespace

TEST(vor_generator_encodes_bearing_as_correlator_phase_shift) {
    const double phase0 = correlate_variable_tone_phase(0);

    struct Case {
        int32_t radial;
        double offset_rad;
    };
    const Case cases[] = {
        {90, M_PI / 2.0},
        {180, M_PI},
        {270, 3.0 * M_PI / 2.0},
    };

    for (const auto& c : cases) {
        const double phase_r = correlate_variable_tone_phase(c.radial);
        const double delta = wrap_pi(phase_r - phase0);
        const double expected = wrap_pi(-c.offset_rad);
        CHECK_NEAR(delta, expected, 1e-3);
    }
}

TEST(vor_generator_full_turn_reproduces_the_same_phase) {
    /* Boundary case: 360 degrees is the same bearing as 0. */
    const double phase0 = correlate_variable_tone_phase(0);
    const double phase360 = correlate_variable_tone_phase(360);
    CHECK_NEAR(wrap_pi(phase360 - phase0), 0.0, 1e-3);
}

/* =============================================================================
 * ADS-B TX
 * ========================================================================== */

TEST(adsb_tx_known_frame_crc_round_trips_with_rx_decoder) {
    /* KLM1023, ICAO 4840D6 — already published and already proven against
     * this project's own CRC engine in tests/test_adsb.cpp. Parsing it and
     * checking check_CRC() exercises exactly the RX decoder's CRC path on a
     * known-good DF17 frame. */
    const AdsbFrame frame = frame_from_hex("8D4840D6202CC371C32CE0576098");
    CHECK_EQ(frame.size(), size_t{14});
    CHECK_EQ(frame.check_CRC(), 0u);
}

TEST(adsb_tx_encoded_frames_have_valid_crc) {
    AdsbFrame id_frame;
    app::adsb_tx::encode_frame_id(id_frame, 0x4840D6u, "KLM1023 ");
    CHECK_EQ(id_frame.check_CRC(), 0u);

    AdsbFrame velo_frame;
    app::adsb_tx::encode_frame_velo(velo_frame, 0x485020u, 159, 183.0, -832);
    CHECK_EQ(velo_frame.check_CRC(), 0u);

    AdsbFrame squawk_frame;
    app::adsb_tx::encode_frame_squawk(squawk_frame, 7700);
    CHECK_EQ(squawk_frame.check_CRC(), 0u);
    CHECK_EQ(squawk_frame.get_DF(), uint8_t{21});
}

TEST(adsb_tx_position_encoder_round_trips_a_documented_fix) {
    /* 52.2572 N / 3.91937 E, 38000 ft — the published result for the
     * 40621D even/odd pair in tests/test_adsb.cpp. Encoding this decimal fix
     * with our own encoder and decoding it back with app::adsb::
     * decode_frame_pos() (the exact function the RX view uses) must return
     * very close to the same numbers: CPR resolution is 360/2^17 ~ 0.0027
     * degrees, so a 0.01 degree tolerance comfortably separates "correct"
     * from "broken" without being tautological. */
    AdsbFrame even, odd;
    app::adsb_tx::encode_frame_pos(even, 0x40621Du, 38000, 52.2572, 3.91937, 0);
    app::adsb_tx::encode_frame_pos(odd, 0x40621Du, 38000, 52.2572, 3.91937, 1);

    CHECK_EQ(even.check_CRC(), 0u);
    CHECK_EQ(odd.check_CRC(), 0u);

    even.set_rx_timestamp(100);
    odd.set_rx_timestamp(99);

    const auto pos = app::adsb::decode_frame_pos(even, odd);
    CHECK(pos.pos_valid);
    CHECK(pos.alt_valid);
    CHECK_EQ(pos.altitude, 38000);
    CHECK_NEAR(pos.latitude, 52.2572, 0.01);
    CHECK_NEAR(pos.longitude, 3.91937, 0.01);
}

TEST(adsb_tx_position_encoder_handles_southern_western_hemisphere) {
    /* Boundary case: negative lat/lon (S/W), which flips several sign bits
     * through the CPR path that a north/east-only test would never touch. */
    AdsbFrame even, odd;
    app::adsb_tx::encode_frame_pos(even, 0x123456u, 1000, -33.8688, 151.2093, 0);
    app::adsb_tx::encode_frame_pos(odd, 0x123456u, 1000, -33.8688, 151.2093, 1);
    CHECK_EQ(even.check_CRC(), 0u);
    CHECK_EQ(odd.check_CRC(), 0u);

    even.set_rx_timestamp(100);
    odd.set_rx_timestamp(99);

    const auto pos = app::adsb::decode_frame_pos(even, odd);
    CHECK(pos.pos_valid);
    CHECK_NEAR(pos.latitude, -33.8688, 0.01);
    CHECK_NEAR(pos.longitude, 151.2093, 0.01);
}

TEST(adsb_tx_callsign_encoder_empty_string_is_all_blank) {
    /* Boundary / empty payload: an empty callsign must not read past the
     * input string, and every one of the 8 characters falls back to space
     * (icao_id_lut index 32), which decode_frame_id() reads back as ' '. */
    AdsbFrame frame;
    app::adsb_tx::encode_frame_id(frame, 0xABCDEFu, "");
    CHECK_EQ(frame.check_CRC(), 0u);

    const std::string callsign = app::adsb::decode_frame_id(frame);
    CHECK_EQ(callsign.size(), size_t{8});
    for (char c : callsign) CHECK_EQ(c, ' ');
}

TEST(adsb_tx_squawk_encoder_boundary_codes) {
    /* 0000 and 7777 are the two ends of the four-octal-digit squawk range. */
    AdsbFrame low, high;
    app::adsb_tx::encode_frame_squawk(low, 0);
    app::adsb_tx::encode_frame_squawk(high, 7777);
    CHECK_EQ(low.check_CRC(), 0u);
    CHECK_EQ(high.check_CRC(), 0u);
}

TEST(adsb_tx_waveform_length_and_preamble_shape) {
    /* 16 preamble chips + 224 Manchester payload chips = 240 chips at 2
     * samples/chip (4 Msps / 2 Mchip/s) = 480 samples, matching upstream's
     * ADSBTXProcessor (bit_pos wraps at 240 << 1). */
    AdsbFrame frame;
    app::adsb_tx::encode_frame_squawk(frame, 1200);

    const auto wave = app::adsb_tx::adsb_frame_waveform(frame);
    CHECK_EQ(wave.size(), size_t{480});

    /* adsb_preamble = {1,0,1,0,...}: first chip on, second off, each held
     * for exactly 2 samples. */
    CHECK_NEAR(wave[0].real(), 1.0f, 1e-6f);
    CHECK_NEAR(wave[1].real(), 1.0f, 1e-6f);
    CHECK_NEAR(wave[2].real(), 0.0f, 1e-6f);
    CHECK_NEAR(wave[3].real(), 0.0f, 1e-6f);
    for (const auto& s : wave) CHECK_NEAR(s.imag(), 0.0f, 1e-6f);
}

/* =============================================================================
 * EPIRB TX
 * ========================================================================== */

TEST(epirb_tx_standard_protocol_bch_and_position_round_trip) {
    app::epirb_tx::BeaconParams params{};
    params.type = app::epirb_tx::BeaconType::EPIRB;
    params.protocol = app::epirb_tx::BeaconProtocol::STANDARD;
    params.country = 227; /* France, per this project's own epirb::lookup_country table */
    params.is_test = true;
    params.is_internal = true;
    params.has_121_5 = true;
    params.location.south = false;
    params.location.lat_deg = 45;
    params.location.lat_min = 30; /* multiple of 15: exact in the 1/4-degree PDF1 field */
    params.location.lat_sec = 0;  /* multiple of 4: exact in the PDF2 offset field */
    params.location.west = false;
    params.location.long_deg = 3;
    params.location.long_min = 15;
    params.location.long_sec = 0;

    std::array<uint8_t, 18> frame{};
    CHECK_EQ(app::epirb_tx::generate_beacon(frame, params), size_t{18});

    app::epirb::Beacon decoded;
    decoded.set_frame(frame.data());

    CHECK(decoded.bch1_valid());
    CHECK(decoded.bch2_valid());
    CHECK(decoded.frame_valid());
    CHECK(!decoded.bch1_corrected);
    CHECK(!decoded.bch2_corrected);
    CHECK(decoded.protocol_is_standard());
    CHECK_EQ(static_cast<int>(decoded.country.code), 227);

    /* Standard-protocol latitude/longitude are minute-exact (the PDF1
     * quarter-degree field plus the PDF2 offset field sum back to the exact
     * input minutes: floor(min/15)*15 + (min mod 15) == min), and the
     * seconds field round-trips exactly for any multiple of 4. */
    CHECK(!decoded.location.latitude.orientation);
    CHECK_EQ(decoded.location.latitude.degrees, 45);
    CHECK_EQ(decoded.location.latitude.minutes, 30);
    CHECK_EQ(decoded.location.latitude.seconds, 0);

    CHECK(!decoded.location.longitude.orientation);
    CHECK_EQ(decoded.location.longitude.degrees, 3);
    CHECK_EQ(decoded.location.longitude.minutes, 15);
    CHECK_EQ(decoded.location.longitude.seconds, 0);
}

TEST(epirb_tx_test_flag_selects_documented_frame_sync_byte) {
    /* Frame byte 2 (0-based) is bits 17-24: the tail of the 9-bit frame-sync
     * word after its leading bit lands in byte 1. C/S T.001's two sync words
     * put 0x2F there for a normal frame and 0xD0 for a self-test frame —
     * exactly the two bytes this project's own RX side keys frame_mode on
     * (ui_epirb_rx.cpp: `frame[2] == 0xD0` / `== 0x2F`). */
    app::epirb_tx::BeaconParams params{};

    params.is_test = true;
    std::array<uint8_t, 18> test_frame{};
    app::epirb_tx::generate_beacon(test_frame, params);
    CHECK_EQ(test_frame[2], uint8_t{0xD0});

    params.is_test = false;
    std::array<uint8_t, 18> real_frame{};
    app::epirb_tx::generate_beacon(real_frame, params);
    CHECK_EQ(real_frame[2], uint8_t{0x2F});
}

TEST(epirb_tx_user_protocol_position_is_minute_exact_no_seconds) {
    /* Boundary: a different protocol code path (User Location, no BCH1
     * position field, position lives entirely in PDF2 and has no seconds
     * resolution at all: RX never reads a seconds field for this protocol,
     * so it stays at the Angle default of 0 regardless of what is supplied
     * here). Minutes must be a multiple of 4 to round-trip exactly. */
    app::epirb_tx::BeaconParams params{};
    params.type = app::epirb_tx::BeaconType::EPIRB;
    params.protocol = app::epirb_tx::BeaconProtocol::USER;
    params.country = 366; /* USA */
    params.is_test = true;
    params.is_internal = true;
    params.has_121_5 = true;
    params.location.south = true;
    params.location.lat_deg = 10;
    params.location.lat_min = 24;
    params.location.west = true;
    params.location.long_deg = 20;
    params.location.long_min = 8;

    std::array<uint8_t, 18> frame{};
    app::epirb_tx::generate_beacon(frame, params);

    app::epirb::Beacon decoded;
    decoded.set_frame(frame.data());

    CHECK(decoded.frame_valid());
    CHECK(decoded.protocol_is_user());

    CHECK(decoded.location.latitude.orientation); /* south */
    CHECK_EQ(decoded.location.latitude.degrees, 10);
    CHECK_EQ(decoded.location.latitude.minutes, 24);

    CHECK(decoded.location.longitude.orientation); /* west */
    CHECK_EQ(decoded.location.longitude.degrees, 20);
    CHECK_EQ(decoded.location.longitude.minutes, 8);
}

TEST(epirb_tx_waveform_carrier_padding_and_chip_count) {
    /* 160 ms pre-carrier + 288 chips (144 bits, Manchester) at 48 kHz / 800
     * chips/s = 60 samples/chip + 100 ms post-carrier. */
    app::epirb_tx::BeaconParams params{};
    std::array<uint8_t, 18> frame{};
    app::epirb_tx::generate_beacon(frame, params);

    const auto wave = app::epirb_tx::epirb_frame_waveform(frame, 48000.0);

    const size_t pre = static_cast<size_t>(0.160 * 48000.0);
    const size_t chips = 144 * 2;
    const size_t samples_per_chip = static_cast<size_t>(48000.0 / 800.0);
    const size_t post = static_cast<size_t>(0.100 * 48000.0);
    CHECK_EQ(wave.size(), pre + (chips * samples_per_chip) + post);

    /* Pre-carrier is unmodulated: unit magnitude, zero phase. */
    CHECK_NEAR(wave[0].real(), 1.0f, 1e-6f);
    CHECK_NEAR(wave[0].imag(), 0.0f, 1e-6f);

    /* Every sample stays on the unit circle (phase modulation only, no AM). */
    for (size_t i = 0; i < wave.size(); i += 997) {
        const double mag = std::hypot(static_cast<double>(wave[i].real()),
                                      static_cast<double>(wave[i].imag()));
        CHECK_NEAR(mag, 1.0, 1e-5);
    }
}
