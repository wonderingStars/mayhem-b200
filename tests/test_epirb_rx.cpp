/*
 * mayhem-b200 — tests for the 406 MHz COSPAS-SARSAT beacon decoder.
 *
 * The BCH check is verified two ways that were written independently: the
 * ported long-division routine from upstream's beacon.hpp, and a plain
 * systematic LFSR encoder written here from the generator polynomials. Beyond
 * agreement, the code's defining property is exercised directly — every single
 * bit error inside a protected span must be detected and corrected back.
 *
 * The position test builds a Standard Location Protocol frame field by field
 * from the C/S T.001 bit assignment and checks the decoder recovers exactly
 * the latitude and longitude that were encoded.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_epirb_rx.hpp"

#include <cmath>
#include <vector>

using namespace app::epirb;

namespace {

/* --- Frame bit helpers (1-based bit numbers, as C/S numbers them) --------- */

bool frame_bit(const uint8_t* frame, int bit) {
    const int byte = (bit - 1) / 8;
    const int off = 7 - ((bit - 1) % 8);
    return ((frame[byte] >> off) & 1) != 0;
}

void set_bits(uint8_t* frame, int start, int end, uint64_t value) {
    const int n = end - start + 1;
    for (int i = 0; i < n; i++) {
        const int bit = start + i;
        const bool v = ((value >> (n - 1 - i)) & 1ULL) != 0;
        const int byte = (bit - 1) / 8;
        const int off = 7 - ((bit - 1) % 8);
        if (v)
            frame[byte] = static_cast<uint8_t>(frame[byte] | (1u << off));
        else
            frame[byte] = static_cast<uint8_t>(frame[byte] & ~(1u << off));
    }
}

/* Systematic BCH remainder of bits [start, end] under `gen`, written as a
 * plain LFSR so it shares no code with Beacon::compute_bch(). */
uint32_t bch_remainder(const uint8_t* frame, int start, int end, uint32_t gen, int gen_bits) {
    const int deg = gen_bits - 1;
    const uint32_t mask = (1u << deg) - 1u;
    const uint32_t glow = gen & mask;
    uint32_t reg = 0;
    for (int b = start; b <= end; b++) {
        const bool feedback = (((reg >> (deg - 1)) & 1u) != 0) ^ frame_bit(frame, b);
        reg = (reg << 1) & mask;
        if (feedback) reg ^= glow;
    }
    return reg;
}

/* Fills in the sync bits and both BCH fields. */
void finish_frame(uint8_t* frame, bool self_test) {
    set_bits(frame, 1, 24, self_test ? kTestPreamble : kRealPreamble);
    set_bits(frame, 86, 106, bch_remainder(frame, 25, 85, kBch21Polynomial, kBch21PolyLength));
    if (frame_bit(frame, 25)) {
        set_bits(frame, 133, 144,
                 bch_remainder(frame, 107, 132, kBch12Polynomial, kBch12PolyLength));
    }
}

/* A Standard Location Protocol PLB (protocol code 0b0111), France (MID 227),
 * positioned at 43deg 45' 20" N, 1deg 30' 08" E.
 *
 *   bit  25      format flag        1 = long frame
 *   bit  26      protocol flag      0 = location protocol
 *   bits 27-36   country code       227
 *   bits 37-40   protocol code      0b0111 = PLB serial location
 *   bits 41-50   C/S type approval  123
 *   bits 51-64   serial number      4567
 *   bit  65      latitude N/S       0 = North
 *   bits 66-72   latitude degrees   43
 *   bits 73-74   latitude minutes   3 (units of 15')
 *   bit  75      longitude E/W      0 = East
 *   bits 76-83   longitude degrees  1
 *   bits 84-85   longitude minutes  2 (units of 15')
 *   bits 107-110 fixed pattern      1101
 *   bit  111     encoded position   1 = internal navigation source
 *   bit  112     121.5 MHz homer    1
 *   bit  113     lat offset sign    1 = add
 *   bits 114-118 lat offset minutes 0
 *   bits 119-122 lat offset seconds 5 (units of 4 s) = 20 s
 *   bit  123     lon offset sign    1 = add
 *   bits 124-128 lon offset minutes 0
 *   bits 129-132 lon offset seconds 2 (units of 4 s) = 8 s
 */
void build_standard_location_frame(uint8_t* frame, uint64_t protocol_code = 0b0111) {
    for (size_t i = 0; i < kBeaconDataSize; i++) frame[i] = 0;

    set_bits(frame, 25, 25, 1);
    set_bits(frame, 26, 26, 0);
    set_bits(frame, 27, 36, 227);
    set_bits(frame, 37, 40, protocol_code);
    set_bits(frame, 41, 50, 123);
    set_bits(frame, 51, 64, 4567);

    set_bits(frame, 65, 65, 0);
    set_bits(frame, 66, 72, 43);
    set_bits(frame, 73, 74, 3);
    set_bits(frame, 75, 75, 0);
    set_bits(frame, 76, 83, 1);
    set_bits(frame, 84, 85, 2);

    set_bits(frame, 107, 110, 0b1101);
    set_bits(frame, 111, 111, 1);
    set_bits(frame, 112, 112, 1);
    set_bits(frame, 113, 113, 1);
    set_bits(frame, 114, 118, 0);
    set_bits(frame, 119, 122, 5);
    set_bits(frame, 123, 123, 1);
    set_bits(frame, 124, 128, 0);
    set_bits(frame, 129, 132, 2);

    finish_frame(frame, false);
}

/* --- Biphase-L 406 MHz burst synthesis ----------------------------------- */

constexpr float kEpirbRate = 48000.0f;
constexpr size_t kSamplesPerSymbol = 60;  /* 800 baud at 48 kHz */
constexpr double kPhaseShift = 1.1;       /* radians, C/S T.001 */

/* 160 ms of unmodulated carrier, then the frame biphase-L encoded as +/-1.1
 * rad of phase: a 1 is (+1.1, -1.1), a 0 is (-1.1, +1.1). `offset_hz` adds a
 * constant carrier frequency error so the AFC can be exercised. */
std::vector<dsp::cfloat> synth_burst(const uint8_t* frame, size_t bit_count,
                                     double offset_hz = 0.0) {
    std::vector<double> phase;
    phase.reserve(static_cast<size_t>(0.160 * kEpirbRate) + bit_count * 2 * kSamplesPerSymbol +
                  4 * kSamplesPerSymbol);

    for (size_t i = 0; i < static_cast<size_t>(0.160 * kEpirbRate); i++) phase.push_back(0.0);
    for (size_t b = 0; b < bit_count; b++) {
        const bool one = frame_bit(frame, static_cast<int>(b) + 1);
        const double first = one ? kPhaseShift : -kPhaseShift;
        for (size_t i = 0; i < kSamplesPerSymbol; i++) phase.push_back(first);
        for (size_t i = 0; i < kSamplesPerSymbol; i++) phase.push_back(-first);
    }
    /* Post-frame carrier. */
    for (size_t i = 0; i < 4 * kSamplesPerSymbol; i++) phase.push_back(0.0);

    std::vector<dsp::cfloat> iq;
    iq.reserve(phase.size());
    const double rot = 2.0 * 3.14159265358979323846 * offset_hz / kEpirbRate;
    for (size_t i = 0; i < phase.size(); i++) {
        const double p = phase[i] + rot * static_cast<double>(i);
        iq.push_back(dsp::cfloat{static_cast<float>(std::cos(p)),
                                 static_cast<float>(std::sin(p))});
    }
    return iq;
}

}  // namespace

/* --- BCH ------------------------------------------------------------------ */

TEST(epirb_bch_generator_polynomials) {
    /* BCH-1: x^21+x^18+x^17+x^15+x^14+x^12+x^11+x^8+x^7+x^6+x^5+x+1 */
    uint32_t g21 = 0;
    for (int e : {21, 18, 17, 15, 14, 12, 11, 8, 7, 6, 5, 1, 0}) g21 |= (1u << e);
    CHECK_EQ(g21, kBch21Polynomial);

    /* BCH-2: x^12+x^10+x^8+x^5+x^4+x^3+1 */
    uint32_t g12 = 0;
    for (int e : {12, 10, 8, 5, 4, 3, 0}) g12 |= (1u << e);
    CHECK_EQ(g12, kBch12Polynomial);

    /* Register widths, one more than the polynomial degree. Compile-time
     * facts, so a static_assert says it more strongly than a runtime check. */
    static_assert(kBch21PolyLength == 22, "BCH-1 register is 22 bits");
    static_assert(kBch12PolyLength == 13, "BCH-2 register is 13 bits");
}

TEST(epirb_bch_matches_an_independent_lfsr) {
    Beacon beacon;
    uint8_t frame[kBeaconDataSize];

    /* Several unrelated payloads, so agreement is not an accident of one. */
    for (uint32_t seed = 1; seed <= 8; seed++) {
        for (size_t i = 0; i < kBeaconDataSize; i++) frame[i] = 0;
        set_bits(frame, 25, 25, 1);
        uint32_t s = seed * 2654435761u;
        for (int b = 26; b <= 85; b++) {
            s = s * 1664525u + 1013904223u;
            set_bits(frame, b, b, (s >> 20) & 1u);
        }
        for (int b = 107; b <= 132; b++) {
            s = s * 1664525u + 1013904223u;
            set_bits(frame, b, b, (s >> 20) & 1u);
        }
        finish_frame(frame, false);
        beacon.set_frame(frame);

        CHECK_EQ(beacon.compute_bch1(),
                 static_cast<uint64_t>(bch_remainder(frame, 25, 85,
                                                     kBch21Polynomial, kBch21PolyLength)));
        CHECK_EQ(beacon.compute_bch2(),
                 static_cast<uint64_t>(bch_remainder(frame, 107, 132,
                                                     kBch12Polynomial, kBch12PolyLength)));
        CHECK(beacon.bch1_valid());
        CHECK(beacon.bch2_valid());
        CHECK(beacon.frame_valid());
        CHECK(!beacon.bch1_corrected);
        CHECK(!beacon.bch2_corrected);
    }
}

TEST(epirb_bch1_corrects_every_single_bit_error) {
    uint8_t original[kBeaconDataSize];
    build_standard_location_frame(original);

    Beacon beacon;
    beacon.set_frame(original);
    CHECK(beacon.frame_valid());

    int corrected = 0;
    for (int bit = 25; bit <= 85; bit++) {
        uint8_t broken[kBeaconDataSize];
        for (size_t i = 0; i < kBeaconDataSize; i++) broken[i] = original[i];
        const int byte = (bit - 1) / 8;
        broken[byte] = static_cast<uint8_t>(broken[byte] ^ (0x80u >> ((bit - 1) % 8)));

        Beacon b;
        b.set_frame(broken);
        if (!b.bch1_corrected) continue;
        corrected++;
        CHECK(b.frame_valid());
        /* The correction must restore the original bits, not just any codeword. */
        for (int i = 25; i <= 85; i++) {
            CHECK_EQ(frame_bit(b.frame.data(), i), frame_bit(original, i));
        }
    }
    /* All 61 protected bits. */
    CHECK_EQ(corrected, 61);
}

TEST(epirb_bch2_corrects_every_single_bit_error) {
    uint8_t original[kBeaconDataSize];
    build_standard_location_frame(original);

    int corrected = 0;
    for (int bit = 107; bit <= 132; bit++) {
        uint8_t broken[kBeaconDataSize];
        for (size_t i = 0; i < kBeaconDataSize; i++) broken[i] = original[i];
        const int byte = (bit - 1) / 8;
        broken[byte] = static_cast<uint8_t>(broken[byte] ^ (0x80u >> ((bit - 1) % 8)));

        Beacon b;
        b.set_frame(broken);
        if (!b.bch2_corrected) continue;
        corrected++;
        CHECK(b.frame_valid());
        for (int i = 107; i <= 132; i++) {
            CHECK_EQ(frame_bit(b.frame.data(), i), frame_bit(original, i));
        }
    }
    CHECK_EQ(corrected, 26);
}

TEST(epirb_two_bit_error_is_not_silently_accepted) {
    uint8_t original[kBeaconDataSize];
    build_standard_location_frame(original);

    /* Any weight-2 error pattern is at distance >= 3 from every codeword of a
     * distance-5 BCH, so the single-bit search cannot "fix" it. */
    for (int a = 30; a <= 40; a++) {
        uint8_t broken[kBeaconDataSize];
        for (size_t i = 0; i < kBeaconDataSize; i++) broken[i] = original[i];
        for (int bit : {a, a + 7}) {
            const int byte = (bit - 1) / 8;
            broken[byte] = static_cast<uint8_t>(broken[byte] ^ (0x80u >> ((bit - 1) % 8)));
        }
        Beacon b;
        b.set_frame(broken);
        CHECK(!b.bch1_valid());
        CHECK(!b.frame_valid());
    }
}

TEST(epirb_get_bits_reads_the_last_field_without_overrunning) {
    uint8_t frame[kBeaconDataSize];
    build_standard_location_frame(frame);

    Beacon b;
    b.set_frame(frame);
    /* Bits 133..144 end exactly on the final frame bit; upstream reads one
     * byte past the buffer here. */
    CHECK_EQ(b.get_bits(133, 144), static_cast<uint64_t>(b.bch2));
    CHECK_EQ(b.get_bits(144, 144), frame_bit(frame, 144) ? 1u : 0u);
    /* And a one-bit read of the first frame bit. */
    CHECK_EQ(b.get_bits(1, 1), 1u);
}

/* --- Frame parsing -------------------------------------------------------- */

TEST(epirb_sync_words_and_frame_mode) {
    /* 15 ones then the 9-bit frame sync, and the low byte of each is what
     * Beacon reads out of frame[2] to tell a live burst from a self test. */
    static_assert((kRealPreamble >> 9) == 0b111111111111111u, "bit sync is 15 ones");
    static_assert((kRealPreamble & 0x1FFu) == 0b000101111u, "normal frame sync");
    static_assert((kTestPreamble >> 9) == 0b111111111111111u, "bit sync is 15 ones");
    static_assert((kTestPreamble & 0x1FFu) == 0b011010000u, "self-test frame sync");
    static_assert((kRealPreamble & 0xFFu) == 0x2Fu, "frame[2] for a normal burst");
    static_assert((kTestPreamble & 0xFFu) == 0xD0u, "frame[2] for a self test");

    uint8_t frame[kBeaconDataSize];
    build_standard_location_frame(frame);
    Beacon normal;
    normal.set_frame(frame);
    CHECK(normal.frame_mode == Beacon::FrameMode::Normal);

    finish_frame(frame, true);
    Beacon self_test;
    self_test.set_frame(frame);
    CHECK(self_test.frame_mode == Beacon::FrameMode::SelfTest);
    /* The sync bits are outside the BCH span, so the frame is still valid. */
    CHECK(self_test.frame_valid());
}

TEST(epirb_standard_location_position_decode) {
    uint8_t frame[kBeaconDataSize];
    build_standard_location_frame(frame);

    Beacon b;
    b.set_frame(frame);

    CHECK(b.frame_valid());
    CHECK(b.long_frame);
    CHECK(!b.protocol_flag);
    CHECK(b.protocol == Beacon::Protocol::StdPlbSerial);
    CHECK(b.protocol_is_standard());
    CHECK_STR_EQ(b.type_name(), "PLB");
    CHECK_STR_EQ(b.protocol_name(), "Standard");

    CHECK_EQ(b.get_bits(27, 36), 227u);
    CHECK_STR_EQ(b.country.alpha_code, "FRA");
    CHECK_STR_EQ(b.country.short_name, "France");

    /* 43deg 45' 20" N */
    CHECK(!b.location.is_unknown());
    CHECK_EQ(b.location.latitude.orientation, false);
    CHECK_EQ(b.location.latitude.degrees, 43);
    CHECK_EQ(b.location.latitude.minutes, 45);
    CHECK_EQ(b.location.latitude.seconds, 20);

    /* 1deg 30' 08" E */
    CHECK_EQ(b.location.longitude.orientation, false);
    CHECK_EQ(b.location.longitude.degrees, 1);
    CHECK_EQ(b.location.longitude.minutes, 30);
    CHECK_EQ(b.location.longitude.seconds, 8);

    CHECK_NEAR(b.location.latitude.float_value(), 43.0 + 45.0 / 60.0 + 20.0 / 3600.0, 1e-4);
    CHECK_NEAR(b.location.longitude.float_value(), 1.0 + 30.0 / 60.0 + 8.0 / 3600.0, 1e-4);

    CHECK_STR_EQ(b.location.to_string(Location::Format::MaidenheadLocator), "JN03ss");
    CHECK_STR_EQ(b.location.to_string(Location::Format::Sexagesimal),
                 "43\xB0" "45'20\"N, 1\xB0" "30'08\"E");

    /* Standard location additional data: C/S type approval number and serial. */
    CHECK(b.has_serial_number);
    CHECK_STR_EQ(b.serial_number, "4567 (0x000011D7)");
    CHECK(b.has_additional_data);
    CHECK_STR_EQ(b.additional_data, "C/S TA #=123");

    /* Locating devices: bit 111 = 1 internal nav, bit 112 = 1 -> 121.5 MHz. */
    CHECK(b.main_locating_device == Beacon::MainLocatingDevice::InternalNav);
    CHECK(b.aux_locating_device == Beacon::AuxLocatingDevice::Mhz121_5);
    CHECK_STR_EQ(b.aux_locating_device_name(), "121.5 MHz");

    CHECK_EQ(b.hex_id.size(), 15u);
    CHECK_EQ(b.short_id().size(), 4u);
}

TEST(epirb_negative_offsets_borrow_into_minutes_and_degrees) {
    uint8_t frame[kBeaconDataSize];
    build_standard_location_frame(frame);
    /* Make both offsets negative: 0' 20" south of 43deg 45', 0' 08" west of
     * 1deg 30'. The borrow path is what upstream's apply_offset() exists for. */
    set_bits(frame, 113, 113, 0);
    set_bits(frame, 123, 123, 0);
    finish_frame(frame, false);

    Beacon b;
    b.set_frame(frame);
    CHECK(b.frame_valid());
    CHECK_EQ(b.location.latitude.degrees, 43);
    CHECK_EQ(b.location.latitude.minutes, 44);
    CHECK_EQ(b.location.latitude.seconds, 40);
    CHECK_EQ(b.location.longitude.degrees, 1);
    CHECK_EQ(b.location.longitude.minutes, 29);
    CHECK_EQ(b.location.longitude.seconds, 52);
}

TEST(epirb_serialised_epirb_is_a_standard_location_protocol) {
    /* Protocol code 0b0110. Upstream's getProtocolType() leaves this one out of
     * the standard-location group, so the position is silently dropped. */
    uint8_t frame[kBeaconDataSize];
    build_standard_location_frame(frame, 0b0110);

    Beacon b;
    b.set_frame(frame);
    CHECK(b.frame_valid());
    CHECK(b.protocol == Beacon::Protocol::StdEpirbSerial);
    CHECK(b.protocol_is_standard());
    CHECK_STR_EQ(b.type_name(), "EPIRB");
    CHECK(!b.location.is_unknown());
    CHECK_EQ(b.location.latitude.degrees, 43);
    CHECK_EQ(b.location.longitude.degrees, 1);
}

TEST(epirb_country_lookup) {
    Country c{};
    lookup_country(227, c);
    CHECK_EQ(c.code, 227);
    CHECK_STR_EQ(c.alpha_code, "FRA");
    CHECK_STR_EQ(c.short_name, "France");

    lookup_country(338, c);
    CHECK_STR_EQ(c.alpha_code, "USA");
    CHECK_STR_EQ(c.short_name, "USA");

    lookup_country(232, c);
    CHECK_STR_EQ(c.alpha_code, "UKM");

    /* Unlisted code: the fields go blank but the code is kept, as upstream. */
    lookup_country(999, c);
    CHECK_EQ(c.code, 999);
    CHECK_STR_EQ(c.alpha_code, "");
    CHECK_STR_EQ(c.short_name, "");
}

/* --- Packet builder ------------------------------------------------------- */

TEST(epirb_packet_builder_finds_real_and_test_sync) {
    uint8_t frame[kBeaconDataSize];
    build_standard_location_frame(frame);

    for (int pass = 0; pass < 2; pass++) {
        finish_frame(frame, pass == 1);

        PacketBuilder builder;
        std::vector<std::vector<uint8_t>> packets;
        builder.set_handler([&packets](const std::vector<uint8_t>& bits) {
            packets.push_back(bits);
        });

        /* Preamble noise the matcher must ignore, then the frame. */
        for (int i = 0; i < 40; i++) builder.execute((i * 5 + 1) & 1);
        for (int b = 1; b <= 144; b++) builder.execute(frame_bit(frame, b) ? 1 : 0);

        CHECK_EQ(packets.size(), 1u);
        if (packets.empty()) continue;
        CHECK_EQ(packets[0].size(), kLongFrameBits);
        for (size_t i = 0; i < packets[0].size(); i++) {
            CHECK_EQ(packets[0][i], frame_bit(frame, static_cast<int>(i) + 1) ? 1u : 0u);
        }
    }
}

TEST(epirb_packet_builder_takes_112_bits_for_a_short_frame) {
    uint8_t frame[kBeaconDataSize];
    for (size_t i = 0; i < kBeaconDataSize; i++) frame[i] = 0;
    set_bits(frame, 1, 24, kRealPreamble);
    set_bits(frame, 25, 25, 0);  /* short frame */
    set_bits(frame, 26, 26, 1);  /* user protocol */
    set_bits(frame, 27, 36, 227);
    set_bits(frame, 37, 39, 0b010);
    for (int b = 40; b <= 85; b++) set_bits(frame, b, b, (b & 1));
    set_bits(frame, 86, 106, bch_remainder(frame, 25, 85, kBch21Polynomial, kBch21PolyLength));

    PacketBuilder builder;
    std::vector<std::vector<uint8_t>> packets;
    builder.set_handler([&packets](const std::vector<uint8_t>& b) { packets.push_back(b); });
    for (int b = 1; b <= 144; b++) builder.execute(frame_bit(frame, b) ? 1 : 0);

    CHECK_EQ(packets.size(), 1u);
    if (packets.empty()) return;
    CHECK_EQ(packets[0].size(), kShortFrameBits);

    Beacon beacon;
    beacon.set_frame_bits(packets[0]);
    CHECK(!beacon.long_frame);
    CHECK(beacon.protocol_flag);
    CHECK(beacon.protocol == Beacon::Protocol::UserEpirbMaritime);
    CHECK(beacon.bch1_valid());
    /* A short frame carries no BCH-2. */
    CHECK(!beacon.has_bch2);
    CHECK(beacon.frame_valid());
}

/* --- Demodulator ---------------------------------------------------------- */

TEST(epirb_demodulator_decodes_a_synthesised_burst) {
    uint8_t frame[kBeaconDataSize];
    build_standard_location_frame(frame);

    Demodulator demod;
    demod.configure(kEpirbRate);
    CHECK_EQ(demod.samples_per_symbol(), kSamplesPerSymbol);

    std::vector<std::vector<uint8_t>> packets;
    demod.set_handler([&packets](const std::vector<uint8_t>& bits) { packets.push_back(bits); });

    const auto iq = synth_burst(frame, kLongFrameBits);
    demod.process(iq.data(), iq.size());

    CHECK(!packets.empty());
    if (packets.empty()) return;
    CHECK_EQ(packets[0].size(), kLongFrameBits);
    for (size_t i = 0; i < kLongFrameBits && i < packets[0].size(); i++) {
        CHECK_EQ(packets[0][i], frame_bit(frame, static_cast<int>(i) + 1) ? 1u : 0u);
    }

    Beacon beacon;
    beacon.set_frame_bits(packets[0]);
    CHECK(beacon.frame_valid());
    CHECK(!beacon.bch1_corrected);
    CHECK(!beacon.bch2_corrected);
    CHECK_EQ(beacon.location.latitude.degrees, 43);
    CHECK_EQ(beacon.location.latitude.minutes, 45);
    CHECK_EQ(beacon.location.latitude.seconds, 20);
    CHECK_STR_EQ(beacon.location.to_string(Location::Format::MaidenheadLocator), "JN03ss");
}

TEST(epirb_demodulator_tracks_a_carrier_frequency_offset) {
    /* +2 kHz of tuning error is 0.262 rad/sample at 48 kHz — far more than the
     * 0.6 rad the carrier-stability detector tolerates over its 12-sample
     * accumulator, so nothing decodes unless the AFC pulls it in. */
    uint8_t frame[kBeaconDataSize];
    build_standard_location_frame(frame);

    Demodulator demod;
    demod.configure(kEpirbRate);
    std::vector<std::vector<uint8_t>> packets;
    demod.set_handler([&packets](const std::vector<uint8_t>& bits) { packets.push_back(bits); });

    const auto iq = synth_burst(frame, kLongFrameBits, 2000.0);
    demod.process(iq.data(), iq.size());

    CHECK(!packets.empty());
    if (packets.empty()) return;
    CHECK_EQ(packets[0].size(), kLongFrameBits);

    Beacon beacon;
    beacon.set_frame_bits(packets[0]);
    CHECK(beacon.frame_valid());

    /* The estimate should sit near 2*pi*2000/48000 = 0.2618 rad/sample. */
    CHECK_NEAR(demod.frequency_offset_estimate(), 0.2618, 0.02);
}

TEST(epirb_demodulator_ignores_a_carrier_with_no_frame) {
    Demodulator demod;
    demod.configure(kEpirbRate);
    size_t packets = 0;
    demod.set_handler([&packets](const std::vector<uint8_t>&) { packets++; });

    /* One second of clean unmodulated carrier: it locks, then times out. */
    std::vector<dsp::cfloat> iq(static_cast<size_t>(kEpirbRate), dsp::cfloat{1.0f, 0.0f});
    demod.process(iq.data(), iq.size());
    CHECK_EQ(packets, 0u);
}
