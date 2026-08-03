/*
 * mayhem-b200 — tests for the TPMS (Weather) and ERT meter decoders.
 *
 * No radio is attached, so nothing here proves reception. What it does prove is
 * that the decoders are correct when fed samples or bits: the frame layouts and
 * checksums are exercised against frames built from the protocol definitions in
 * upstream's common/tpms_packet.cpp and common/ert_packet.cpp, the CRC engines
 * are cross-checked against an independently written polynomial long division,
 * and the whole sample -> reading pipeline is driven with signals produced by
 * Phase A's modulators.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "../src/apps/ui_ert.hpp"
#include "../src/apps/ui_weather.hpp"
#include "../src/dsp/demod_digital.hpp"
#include "../src/dsp/protocol.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using Bits = std::vector<uint8_t>;

/* --- Bit helpers ------------------------------------------------------------ */

/* Appends the low `count` bits of `value`, most significant first — the same
 * order read_field() reads them back in. */
void put_bits(Bits& bits, uint64_t value, size_t count) {
    for (size_t i = count; i > 0; i--) {
        bits.push_back(static_cast<uint8_t>((value >> (i - 1)) & 1ull));
    }
}

Bits bits_from_string(const char* s) {
    Bits b;
    for (const char* p = s; *p != '\0'; p++) {
        if (*p == '0' || *p == '1') b.push_back(static_cast<uint8_t>(*p - '0'));
    }
    return b;
}

/* Polynomial long division over a bit array, deliberately written in a
 * different style from dsp::Crc (which is a register shifter) so it is a real
 * cross-check rather than a copy of the same mistake. Returns the CRC with a
 * zero initial register: the remainder of message * x^width modulo `poly`. */
uint32_t long_division_crc(const Bits& message, uint32_t poly, size_t width) {
    Bits r = message;
    r.insert(r.end(), width, uint8_t{0});

    for (size_t i = 0; (i + width) < r.size(); i++) {
        if (!r[i]) continue;
        r[i] = 0;
        for (size_t k = 0; k < width; k++) {
            const uint8_t p = static_cast<uint8_t>((poly >> (width - 1 - k)) & 1u);
            r[i + 1 + k] ^= p;
        }
    }

    uint32_t remainder = 0;
    for (size_t i = r.size() - width; i < r.size(); i++) {
        remainder = (remainder << 1) | (r[i] & 1u);
    }
    return remainder;
}

/* --- TPMS frame builders ---------------------------------------------------- */

/* An FSK Schrader FLM_72 frame: nine bytes whose CRC-8 (poly x^8+1, zero init)
 * residue is zero. Bytes 0-3 transponder ID, byte 5 pressure, byte 6
 * temperature, byte 8 the CRC. */
Bits make_flm72(uint32_t id, uint8_t pressure_raw, uint8_t temp_raw) {
    std::vector<uint8_t> bytes(9, 0);
    bytes[0] = static_cast<uint8_t>(id >> 24);
    bytes[1] = static_cast<uint8_t>(id >> 16);
    bytes[2] = static_cast<uint8_t>(id >> 8);
    bytes[3] = static_cast<uint8_t>(id);
    bytes[4] = 0x5a;
    bytes[5] = pressure_raw;
    bytes[6] = temp_raw;
    bytes[7] = 0xa5;

    dsp::Crc<8> crc{0x01, 0x00};
    for (size_t i = 0; i < 8; i++) crc.process_byte(bytes[i]);
    bytes[8] = static_cast<uint8_t>(crc.checksum());

    Bits bits;
    for (const uint8_t b : bytes) put_bits(bits, b, 8);
    return bits;
}

/* An FLM_80 frame: ten bytes whose CRC-8 over bytes 1..9 has a zero residue.
 * Byte 0 is outside the CRC; the ID starts at bit 8. */
Bits make_flm80(uint32_t id, uint8_t pressure_raw, uint8_t temp_raw) {
    std::vector<uint8_t> bytes(10, 0);
    bytes[0] = 0x3c;
    bytes[1] = static_cast<uint8_t>(id >> 24);
    bytes[2] = static_cast<uint8_t>(id >> 16);
    bytes[3] = static_cast<uint8_t>(id >> 8);
    bytes[4] = static_cast<uint8_t>(id);
    bytes[5] = 0x11;
    bytes[6] = pressure_raw;
    bytes[7] = temp_raw;
    bytes[8] = 0x22;

    dsp::Crc<8> crc{0x01, 0x00};
    for (size_t i = 1; i < 9; i++) crc.process_byte(bytes[i]);
    bytes[9] = static_cast<uint8_t>(crc.checksum());

    Bits bits;
    for (const uint8_t b : bytes) put_bits(bits, b, 8);
    return bits;
}

/* An FLM_64 frame: eight bytes where the sum of bytes 0..6 equals byte 7. */
Bits make_flm64(uint32_t id, uint8_t pressure_raw, uint8_t temp_raw) {
    std::vector<uint8_t> bytes(8, 0);
    bytes[0] = static_cast<uint8_t>(id >> 24);
    bytes[1] = static_cast<uint8_t>(id >> 16);
    bytes[2] = static_cast<uint8_t>(id >> 8);
    bytes[3] = static_cast<uint8_t>(id);
    bytes[4] = pressure_raw;
    bytes[5] = temp_raw;
    bytes[6] = 0x00;

    uint32_t sum = 0;
    for (size_t i = 0; i < 7; i++) sum += bytes[i];
    bytes[7] = static_cast<uint8_t>(sum & 0xffu);

    Bits bits;
    for (const uint8_t b : bytes) put_bits(bits, b, 8);
    return bits;
}

/* OOK 8k192 Schrader: 37 symbols, [0:3) flags, [3:27) ID, [27:35) pressure,
 * [35:37) a checksum chosen so bit 0 plus the eighteen 2-bit groups starting at
 * bit 1 (the checksum field included) has 3 in its low two bits. */
Bits make_schrader_8k192(uint32_t flags3, uint32_t id24, uint8_t pressure_raw) {
    Bits bits;
    put_bits(bits, flags3 & 0x7u, 3);
    put_bits(bits, id24 & 0xffffffu, 24);
    put_bits(bits, pressure_raw, 8);
    put_bits(bits, 0, 2);  /* placeholder for the checksum */

    uint32_t sum = bits[0];
    for (size_t i = 1; i < 35; i += 2) {
        sum += app::tpms::read_field(bits, i, 2);
    }
    const uint32_t checksum = (3u - (sum & 3u)) & 3u;

    bits[35] = static_cast<uint8_t>((checksum >> 1) & 1u);
    bits[36] = static_cast<uint8_t>(checksum & 1u);
    return bits;
}

/* OOK 8k4 Schrader (GMC_96): 76 symbols, [0:20) rest of the system ID,
 * [20:52) ID, [52:60) pressure, [60:68) temperature, [68:76) the 8-bit sum of
 * the nine bytes beginning with the assumed 0x4 nibble. */
Bits make_schrader_8k4(uint32_t system_id20, uint32_t id32, uint8_t pressure_raw,
                       uint8_t temp_raw) {
    Bits bits;
    put_bits(bits, system_id20 & 0xfffffu, 20);
    put_bits(bits, id32, 32);
    put_bits(bits, pressure_raw, 8);
    put_bits(bits, temp_raw, 8);
    put_bits(bits, 0, 8);  /* placeholder for the checksum */

    uint8_t sum = static_cast<uint8_t>((0x4u << 4) | app::tpms::read_field(bits, 0, 4));
    for (size_t i = 4; i < 68; i += 8) {
        sum = static_cast<uint8_t>(sum + app::tpms::read_field(bits, i, 8));
    }

    for (size_t k = 0; k < 8; k++) {
        bits[68 + k] = static_cast<uint8_t>((sum >> (7 - k)) & 1u);
    }
    return bits;
}

/* --- ERT frame builders ----------------------------------------------------- */

/* An ERT SCM payload: the 75 message bits that follow the 21-bit preamble.
 *
 *   [0:2)   ID, most significant 2 bits      [11:35) consumption
 *   [3:5)   encoder tamper                   [35:59) ID, low 24 bits
 *   [5:9)   commodity type                   [59:75) BCH-16, poly 0x6F63
 *   [9:11)  physical tamper
 *
 * The BCH covers the five zero bits at the tail of the preamble plus bits
 * [0:59), which is why the message handed to the divider is prefixed with five
 * zeros. */
Bits make_ert_scm(uint32_t id, uint32_t consumption, uint32_t commodity,
                  uint32_t encoder_tamper, uint32_t physical_tamper) {
    Bits bits;
    put_bits(bits, (id >> 24) & 0x3u, 2);
    put_bits(bits, 0, 1);
    put_bits(bits, encoder_tamper & 0x3u, 2);
    put_bits(bits, commodity & 0xfu, 4);
    put_bits(bits, physical_tamper & 0x3u, 2);
    put_bits(bits, consumption & 0xffffffu, 24);
    put_bits(bits, id & 0xffffffu, 24);

    Bits covered(5, uint8_t{0});
    covered.insert(covered.end(), bits.begin(), bits.end());
    const uint32_t bch = long_division_crc(covered, 0x6f63, 16);

    put_bits(bits, bch, 16);
    return bits;
}

/* An ERT SCM+ payload: 14 bytes.
 *
 *   byte 0..1   protocol / endpoint header, commodity in the low nibble of 1
 *   byte 2..5   ID          byte 6..9  consumption
 *   byte 10..11 tamper      byte 12..13 CRC-16/CCITT
 *
 * The CRC is found by search rather than computed, because upstream (and
 * rtlamr) validate the frame against a residue of 0x1D0F rather than zero. A
 * search also proves the check is a real 16-bit one: exactly one of the 65536
 * candidates may pass. */
std::vector<uint8_t> ert_scmplus_bytes(uint32_t id, uint32_t consumption,
                                       uint32_t commodity, uint16_t tamper) {
    std::vector<uint8_t> bytes(14, 0);
    bytes[0] = 0x1e;
    bytes[1] = static_cast<uint8_t>(0x50u | (commodity & 0x0fu));
    bytes[2] = static_cast<uint8_t>(id >> 24);
    bytes[3] = static_cast<uint8_t>(id >> 16);
    bytes[4] = static_cast<uint8_t>(id >> 8);
    bytes[5] = static_cast<uint8_t>(id);
    bytes[6] = static_cast<uint8_t>(consumption >> 24);
    bytes[7] = static_cast<uint8_t>(consumption >> 16);
    bytes[8] = static_cast<uint8_t>(consumption >> 8);
    bytes[9] = static_cast<uint8_t>(consumption);
    bytes[10] = static_cast<uint8_t>(tamper >> 8);
    bytes[11] = static_cast<uint8_t>(tamper);
    return bytes;
}

Bits bits_from_bytes(const std::vector<uint8_t>& bytes) {
    Bits bits;
    for (const uint8_t b : bytes) put_bits(bits, b, 8);
    return bits;
}

/* --- Signal helpers --------------------------------------------------------- */

void append(Bits& dst, const Bits& src) { dst.insert(dst.end(), src.begin(), src.end()); }

Bits alternating(size_t count, uint8_t first) {
    Bits b;
    for (size_t i = 0; i < count; i++) b.push_back(static_cast<uint8_t>((first + i) & 1u));
    return b;
}

Bits repeat(const Bits& pattern, size_t times) {
    Bits b;
    for (size_t i = 0; i < times; i++) append(b, pattern);
    return b;
}

}  // namespace

/* ===========================================================================
 * Manchester (Phase A's dsp::manchester_*, which both decoders sit on)
 * ===========================================================================*/

TEST(manchester_decode_known_symbol_stream) {
    /* Sense 0: a data 1 is chips {1,0}, a data 0 is chips {0,1}. */
    const Bits chips = bits_from_string("10 01 10 10 01 01");
    const auto bits = dsp::manchester_decode(chips, 0);

    CHECK_EQ(bits.size(), size_t{6});
    CHECK_EQ(bits[0], uint8_t{1});
    CHECK_EQ(bits[1], uint8_t{0});
    CHECK_EQ(bits[2], uint8_t{1});
    CHECK_EQ(bits[3], uint8_t{1});
    CHECK_EQ(bits[4], uint8_t{0});
    CHECK_EQ(bits[5], uint8_t{0});
}

TEST(manchester_decode_flags_missing_transition) {
    /* A chip pair with no transition cannot be a Manchester symbol. */
    const Bits chips = bits_from_string("10 11 01 00");
    Bits errors;
    const auto bits = dsp::manchester_decode(chips, 0, &errors);

    CHECK_EQ(bits.size(), size_t{4});
    CHECK_EQ(errors.size(), size_t{4});
    CHECK_EQ(errors[0], uint8_t{0});
    CHECK_EQ(errors[1], uint8_t{1});
    CHECK_EQ(errors[2], uint8_t{0});
    CHECK_EQ(errors[3], uint8_t{1});
}

TEST(manchester_round_trip_matches_original_bits) {
    const Bits bits = bits_from_string("1011000111010010");
    const auto chips = dsp::manchester_encode(bits, 0);
    CHECK_EQ(chips.size(), bits.size() * 2);

    const auto decoded = dsp::manchester_decode(chips, 0);
    CHECK(decoded == bits);

    /* Sense 1 is the complement, and decodes back with sense 1. */
    const auto chips_s1 = dsp::manchester_encode(bits, 1);
    CHECK(dsp::manchester_decode(chips_s1, 1) == bits);
}

/* ===========================================================================
 * TPMS — CRCs
 * ===========================================================================*/

TEST(tpms_crc8_matches_independent_long_division) {
    /* The Schrader FSK frames use CRC-8 with truncated polynomial 0x01 and a
     * zero initial register. Cross-checked against a divider written from the
     * polynomial rather than as a shift register. */
    const std::vector<uint8_t> data{0x8f, 0x12, 0x34, 0x56, 0x78, 0x60, 0x50, 0xa5};

    dsp::Crc<8> crc{0x01, 0x00};
    crc.process_bytes(data.data(), data.size());

    Bits bits;
    for (const uint8_t b : data) put_bits(bits, b, 8);

    CHECK_EQ(crc.checksum(), long_division_crc(bits, 0x01, 8));
}

TEST(tpms_crc8_appending_the_remainder_zeroes_the_residue) {
    const std::vector<uint8_t> data{0xde, 0xad, 0xbe, 0xef};

    dsp::Crc<8> crc{0x01, 0x00};
    crc.process_bytes(data.data(), data.size());
    const uint8_t remainder = static_cast<uint8_t>(crc.checksum());

    dsp::Crc<8> check{0x01, 0x00};
    check.process_bytes(data.data(), data.size());
    check.process_byte(remainder);
    CHECK_EQ(check.checksum(), uint32_t{0});
}

TEST(tpms_crc_valid_length_picks_the_right_frame_length) {
    CHECK_EQ(app::tpms::crc_valid_length(make_flm72(0x12345678, 0x60, 0x50)), size_t{72});
    CHECK_EQ(app::tpms::crc_valid_length(make_flm80(0x0badf00d, 0x48, 0x55)), size_t{80});
    CHECK_EQ(app::tpms::crc_valid_length(make_flm64(0xa1b2c3d4, 0x5a, 0x4b)), size_t{64});
}

TEST(tpms_crc_valid_length_rejects_a_corrupted_frame) {
    Bits frame = make_flm72(0x12345678, 0x60, 0x50);
    CHECK_EQ(app::tpms::crc_valid_length(frame), size_t{72});

    frame[17] ^= 1;  /* one bit inside the ID */
    CHECK_EQ(app::tpms::crc_valid_length(frame), size_t{0});

    const auto reading = app::tpms::reading_fsk_19k2_schrader(frame);
    CHECK(!reading.valid());
}

/* ===========================================================================
 * TPMS — field decode
 * ===========================================================================*/

TEST(tpms_flm72_known_frame_decodes) {
    /* Pressure byte 0x60 = 96 -> 96 * 4 / 3 = 128 kPa.
     * Temperature byte 0x50 = 80 -> 80 - 56 = 24 C. */
    const Bits frame = make_flm72(0x12345678, 0x60, 0x50);
    const auto r = app::tpms::decode_reading(app::tpms::SignalType::Fsk19k2Schrader, frame);

    CHECK(r.valid());
    CHECK(r.type == app::tpms::Reading::Type::FLM_72);
    CHECK_EQ(r.id, uint32_t{0x12345678});
    CHECK(r.has_pressure);
    CHECK_EQ(r.pressure.kilopascal(), 128);
    CHECK(r.has_temperature);
    CHECK_EQ(r.temperature.celsius(), 24);
    CHECK(!r.has_flags);

    /* 128 kPa is 18 psi and 1 bar, with upstream's integer arithmetic. */
    CHECK_EQ(r.pressure.psi(), 18);
    CHECK_EQ(r.pressure.bar(), 1);
    CHECK_EQ(r.temperature.fahrenheit(), 75);
}

TEST(tpms_flm80_known_frame_decodes) {
    /* 0x48 = 72 -> 96 kPa; 0x55 = 85 -> 29 C. */
    const Bits frame = make_flm80(0x0badf00d, 0x48, 0x55);
    const auto r = app::tpms::decode_reading(app::tpms::SignalType::Fsk19k2Schrader, frame);

    CHECK(r.valid());
    CHECK(r.type == app::tpms::Reading::Type::FLM_80);
    CHECK_EQ(r.id, uint32_t{0x0badf00d});
    CHECK_EQ(r.pressure.kilopascal(), 96);
    CHECK_EQ(r.temperature.celsius(), 29);
}

TEST(tpms_flm64_known_frame_decodes) {
    /* 0x5a = 90 -> 120 kPa; 0x4b = 75, masked to 7 bits -> 75 - 56 = 19 C. */
    const Bits frame = make_flm64(0xa1b2c3d4, 0x5a, 0x4b);
    const auto r = app::tpms::decode_reading(app::tpms::SignalType::Fsk19k2Schrader, frame);

    CHECK(r.valid());
    CHECK(r.type == app::tpms::Reading::Type::FLM_64);
    CHECK_EQ(r.id, uint32_t{0xa1b2c3d4});
    CHECK_EQ(r.pressure.kilopascal(), 120);
    CHECK_EQ(r.temperature.celsius(), 19);
}

TEST(tpms_flm64_masks_the_temperature_sign_bit) {
    /* Upstream masks the temperature byte with 0x7f in the 64-bit variant, so
     * 0xC8 and 0x48 must give the same reading. */
    const auto lo = app::tpms::decode_reading(
        app::tpms::SignalType::Fsk19k2Schrader, make_flm64(1, 0x5a, 0x48));
    const auto hi = app::tpms::decode_reading(
        app::tpms::SignalType::Fsk19k2Schrader, make_flm64(1, 0x5a, 0xc8));

    CHECK(lo.valid());
    CHECK(hi.valid());
    CHECK_EQ(lo.temperature.celsius(), hi.temperature.celsius());
    CHECK_EQ(lo.temperature.celsius(), 72 - 56);
}

TEST(tpms_ook_8k192_known_frame_decodes) {
    /* 0x66 = 102 -> 102 * 4 / 3 = 136 kPa. Flags are the 3-bit function code in
     * the high nibble with the 2-bit checksum in the low. */
    const Bits frame = make_schrader_8k192(0b101, 0xABCDEF, 0x66);
    const auto r = app::tpms::decode_reading(app::tpms::SignalType::Ook8k192Schrader, frame);

    CHECK(r.valid());
    CHECK(r.type == app::tpms::Reading::Type::Schrader);
    CHECK_EQ(r.id, uint32_t{0xABCDEF});
    CHECK(r.has_pressure);
    CHECK_EQ(r.pressure.kilopascal(), 136);
    CHECK(!r.has_temperature);
    CHECK(r.has_flags);
    CHECK_EQ(static_cast<uint32_t>(r.flags >> 4), uint32_t{0b101});
}

TEST(tpms_ook_8k192_rejects_a_bad_checksum) {
    Bits frame = make_schrader_8k192(0b101, 0xABCDEF, 0x66);
    CHECK(app::tpms::reading_ook_8k192_schrader(frame).valid());

    frame[36] ^= 1;  /* the low checksum bit */
    CHECK(!app::tpms::reading_ook_8k192_schrader(frame).valid());
}

TEST(tpms_ook_8k4_known_frame_decodes) {
    /* 0x40 = 64 -> 64 * 11 / 4 = 176 kPa; 0x64 = 100 -> 100 - 61 = 39 C. */
    const Bits frame = make_schrader_8k4(0x35A2C, 0xDEADBEEF, 0x40, 0x64);
    const auto r = app::tpms::decode_reading(app::tpms::SignalType::Ook8k4Schrader, frame);

    CHECK(r.valid());
    CHECK(r.type == app::tpms::Reading::Type::GMC_96);
    CHECK_EQ(r.id, uint32_t{0xDEADBEEF});
    CHECK_EQ(r.pressure.kilopascal(), 176);
    CHECK_EQ(r.temperature.celsius(), 39);
}

TEST(tpms_ook_8k4_rejects_a_bad_checksum) {
    Bits frame = make_schrader_8k4(0x35A2C, 0xDEADBEEF, 0x40, 0x64);
    CHECK(app::tpms::reading_ook_8k4_schrader(frame).valid());

    frame[30] ^= 1;  /* one bit inside the ID */
    CHECK(!app::tpms::reading_ook_8k4_schrader(frame).valid());
}

TEST(tpms_truncated_and_empty_frames_do_not_crash) {
    const Bits empty{};

    /* An all-zero (or absent) frame satisfies the 80-bit CRC, because a zero
     * register fed zeros stays zero — upstream behaves the same way, and this
     * is why the preamble matcher, not the checksum, is what keeps noise out.
     * Recorded here so a future change to crc_valid_length is a deliberate one. */
    CHECK_EQ(app::tpms::crc_valid_length(empty), size_t{80});

    const auto r = app::tpms::decode_reading(app::tpms::SignalType::Fsk19k2Schrader, empty);
    CHECK(r.type == app::tpms::Reading::Type::FLM_80);
    CHECK_EQ(r.id, uint32_t{0});

    /* The OOK formats have real checksums over their payloads, so an empty
     * frame is rejected by both. */
    CHECK(!app::tpms::decode_reading(app::tpms::SignalType::Ook8k192Schrader, empty).valid());
    CHECK(!app::tpms::decode_reading(app::tpms::SignalType::Ook8k4Schrader, empty).valid());

    Bits half = make_schrader_8k4(0x35A2C, 0xDEADBEEF, 0x40, 0x64);
    half.resize(40);
    CHECK(!app::tpms::decode_reading(app::tpms::SignalType::Ook8k4Schrader, half).valid());
}

TEST(tpms_decode_chips_manchester_decodes_first) {
    const Bits frame = make_flm72(0x12345678, 0x60, 0x50);
    const auto chips = dsp::manchester_encode(frame, 0);

    const auto r = app::tpms::decode_chips(app::tpms::SignalType::Fsk19k2Schrader, chips);
    CHECK(r.valid());
    CHECK(r.type == app::tpms::Reading::Type::FLM_72);
    CHECK_EQ(r.id, uint32_t{0x12345678});
    CHECK_EQ(r.pressure.kilopascal(), 128);
    CHECK_EQ(r.temperature.celsius(), 24);
}

TEST(tpms_format_symbols_matches_the_hex_of_the_decoded_bits) {
    const Bits bits = bits_from_string("1010 0101 1111 0000");
    const auto chips = dsp::manchester_encode(bits, 0);

    const auto formatted = app::tpms::format_symbols(chips);
    CHECK_STR_EQ(formatted.data, "A5F0");
    CHECK_STR_EQ(formatted.errors, "0000");
}

/* ===========================================================================
 * TPMS — recent-entries bookkeeping
 * ===========================================================================*/

TEST(tpms_recent_entry_keeps_the_last_valid_field_values) {
    app::tpms::RecentEntry entry{{app::tpms::Reading::Type::FLM_72, 0x12345678}};

    app::tpms::Reading first{};
    first.type = app::tpms::Reading::Type::FLM_72;
    first.id = 0x12345678;
    first.has_pressure = true;
    first.pressure = app::tpms::Pressure{200};
    first.has_temperature = true;
    first.temperature = app::tpms::Temperature{21};
    entry.update(first);

    /* A second frame carrying only pressure must not wipe the temperature. */
    app::tpms::Reading second{};
    second.type = app::tpms::Reading::Type::FLM_72;
    second.id = 0x12345678;
    second.has_pressure = true;
    second.pressure = app::tpms::Pressure{210};
    entry.update(second);

    CHECK_EQ(entry.received_count, size_t{2});
    CHECK_EQ(entry.last_pressure.kilopascal(), 210);
    CHECK(entry.has_temperature);
    CHECK_EQ(entry.last_temperature.celsius(), 21);
}

TEST(tpms_column_formatting_converts_units) {
    const app::tpms::Pressure p{200};
    CHECK_STR_EQ(app::tpms::format_pressure(p, app::tpms::PressureUnit::Kpa), "200");
    CHECK_STR_EQ(app::tpms::format_pressure(p, app::tpms::PressureUnit::Psi), " 29");
    CHECK_STR_EQ(app::tpms::format_pressure(p, app::tpms::PressureUnit::Bar), "  2");

    const app::tpms::Temperature t{21};
    CHECK_STR_EQ(app::tpms::format_temperature(t, app::tpms::TempUnit::Celsius), " 21");
    CHECK_STR_EQ(app::tpms::format_temperature(t, app::tpms::TempUnit::Fahrenheit), " 69");
}

/* ===========================================================================
 * TPMS — the receive pipeline
 * ===========================================================================*/

TEST(tpms_decoder_reproduces_upstream_rates_at_2457600) {
    app::tpms::TpmsDecoder decoder;
    decoder.configure(2'457'600.0);

    CHECK(decoder.configured());
    /* proc_tpms decimates 2.4576 MHz by 8 to 307.2 kHz, then its matched filter
     * decimates by 8 more to 38.4 kHz, twice the 19200 symbol rate. */
    CHECK_EQ(decoder.decimation(), size_t{8});
    CHECK_NEAR(decoder.channel_rate(), 307200.0, 1e-6);
    CHECK_EQ(decoder.matched_filter_decimation(), size_t{8});
    CHECK_NEAR(decoder.ook_rate(), 153600.0, 1e-6);
}

TEST(tpms_decoder_recovers_an_fsk_frame_from_samples) {
    constexpr float channel_rate = 307200.0f;

    /* Preamble run-in, then proc_tpms's 30-chip sync, then 160 payload chips. */
    Bits chips = alternating(40, 0);
    append(chips, bits_from_string("010101010101010101010101010110"));

    Bits payload = make_flm72(0x12345678, 0x60, 0x50);
    payload.resize(80, 0);  /* the builder collects 80 Manchester symbols */
    append(chips, dsp::manchester_encode(payload, 0));

    /* The timing loop swallows one symbol while it acquires, so a burst that
     * ends exactly at the last payload chip leaves the packet one chip short of
     * the 160 the builder waits for. A real transmission is followed by more
     * signal; this stands in for it. */
    append(chips, alternating(16, 0));

    /* Deviation 38400 Hz: the tones the matched filter is tuned to. */
    const auto signal = dsp::fsk_modulate(chips, channel_rate, 19200.0f, 38400.0f);

    app::tpms::Reading got{};
    size_t frames = 0;

    app::tpms::TpmsDecoder decoder;
    decoder.set_packet_handler(
        [&](app::tpms::SignalType type, const std::vector<uint8_t>& frame_chips) {
            frames++;
            const auto r = app::tpms::decode_chips(type, frame_chips);
            if (r.valid()) got = r;
        });
    decoder.configure(channel_rate);
    decoder.process(signal.data(), signal.size());

    CHECK(frames >= 1);
    CHECK(got.valid());
    CHECK(got.type == app::tpms::Reading::Type::FLM_72);
    CHECK_EQ(got.id, uint32_t{0x12345678});
    CHECK_EQ(got.pressure.kilopascal(), 128);
    CHECK_EQ(got.temperature.celsius(), 24);
}

TEST(tpms_decoder_recovers_an_ook_8k192_frame_from_samples) {
    constexpr float channel_rate = 307200.0f;

    /* proc_tpms's preamble: 11 twice, 01 fourteen times, then 11 and 10. */
    Bits chips = repeat(bits_from_string("11"), 2);
    append(chips, repeat(bits_from_string("01"), 14));
    append(chips, bits_from_string("11"));
    append(chips, bits_from_string("10"));

    const Bits payload = make_schrader_8k192(0b101, 0xABCDEF, 0x66);
    append(chips, dsp::manchester_encode(payload, 0));

    /* The OOK arm runs at half the channel rate; the chip rate is 8192. */
    const auto signal = dsp::ook_modulate(chips, channel_rate, 8192.0f, 1.0f);

    app::tpms::Reading got{};
    app::tpms::TpmsDecoder decoder;
    decoder.set_packet_handler(
        [&](app::tpms::SignalType type, const std::vector<uint8_t>& frame_chips) {
            const auto r = app::tpms::decode_chips(type, frame_chips);
            if (r.valid()) got = r;
        });
    decoder.configure(channel_rate);
    decoder.process(signal.data(), signal.size());

    CHECK(got.valid());
    CHECK(got.type == app::tpms::Reading::Type::Schrader);
    CHECK_EQ(got.id, uint32_t{0xABCDEF});
    CHECK_EQ(got.pressure.kilopascal(), 136);
}

TEST(tpms_decoder_emits_nothing_for_noise_free_silence) {
    app::tpms::TpmsDecoder decoder;
    size_t frames = 0;
    decoder.set_packet_handler(
        [&](app::tpms::SignalType, const std::vector<uint8_t>&) { frames++; });
    decoder.configure(307200.0);

    const std::vector<dsp::cfloat> silence(20000, dsp::cfloat{0.0f, 0.0f});
    decoder.process(silence.data(), silence.size());

    CHECK_EQ(frames, size_t{0});
}

/* ===========================================================================
 * ERT — CRCs
 * ===========================================================================*/

TEST(ert_bch_matches_independent_long_division) {
    /* The SCM frame's 16-bit BCH, polynomial 0x6F63, zero initial register. */
    const Bits message = bits_from_string(
        "0110100101101001011010010110100101101001011010010110100101101001");

    /* dsp::Crc XORs each message bit into the top of the register rather than
     * shifting the message through it, so with a zero initial value its
     * remainder after the last message bit already is M(x) * x^16 mod P(x) —
     * no trailing zero bits to append. */
    dsp::Crc<16> crc{0x6f63};
    for (const uint8_t b : message) crc.process_bit(b != 0);

    CHECK_EQ(crc.checksum(), long_division_crc(message, 0x6f63, 16));
}

TEST(ert_ccitt_check_value_matches_the_published_vector) {
    /* CRC-16/CCITT-FALSE of "123456789" is 0x29B1 — the standard check value
     * for the parameters ERT's SCM+ and IDM frames use. */
    dsp::Crc<16> crc{0x1021, 0xffff, 0x0000};
    const char* check = "123456789";
    for (const char* p = check; *p != '\0'; p++) {
        crc.process_byte(static_cast<uint8_t>(*p));
    }
    CHECK_EQ(crc.checksum(), uint32_t{0x29B1});
}

/* ===========================================================================
 * ERT — SCM
 * ===========================================================================*/

TEST(ert_scm_known_frame_decodes) {
    const Bits payload = make_ert_scm(/*id=*/0x2123456, /*consumption=*/0x0ABCDE,
                                      /*commodity=*/7, /*encoder_tamper=*/1,
                                      /*physical_tamper=*/2);
    CHECK_EQ(payload.size(), size_t{75});

    const auto packet = app::ert::Packet::from_bits(app::ert::PacketType::SCM, payload);

    CHECK(packet.crc_ok());
    CHECK_EQ(packet.length(), size_t{75});
    CHECK_EQ(packet.id(), uint32_t{0x2123456});
    CHECK_EQ(packet.consumption(), uint32_t{0x0ABCDE});
    CHECK_EQ(packet.commodity_type(), uint32_t{7});
    /* Physical tamper in the high nibble, encoder tamper in the low. */
    CHECK_EQ(packet.tamper_flags(), uint32_t{0x21});
}

TEST(ert_scm_consumption_field_spans_bits_11_to_34) {
    /* The consumption field is 24 bits wide, so the largest value round-trips
     * and a value one bit wider must not leak into the ID. */
    const Bits payload = make_ert_scm(0x0000001, 0xFFFFFF, 3, 0, 0);
    const auto packet = app::ert::Packet::from_bits(app::ert::PacketType::SCM, payload);

    CHECK(packet.crc_ok());
    CHECK_EQ(packet.consumption(), uint32_t{0xFFFFFF});
    CHECK_EQ(packet.id(), uint32_t{0x0000001});
}

TEST(ert_scm_rejects_every_single_bit_error) {
    const Bits payload = make_ert_scm(0x2123456, 0x0ABCDE, 7, 1, 2);
    CHECK(app::ert::Packet::from_bits(app::ert::PacketType::SCM, payload).crc_ok());

    size_t accepted = 0;
    for (size_t i = 0; i < payload.size(); i++) {
        Bits corrupt = payload;
        corrupt[i] ^= 1;
        if (app::ert::Packet::from_bits(app::ert::PacketType::SCM, corrupt).crc_ok()) {
            accepted++;
        }
    }
    CHECK_EQ(accepted, size_t{0});
}

TEST(ert_scm_exactly_one_checksum_value_validates) {
    /* A real 16-bit check: of the 65536 possible values of the BCH field,
     * precisely one may pass, and it must be the one the divider computed. */
    Bits payload = make_ert_scm(0x2123456, 0x0ABCDE, 7, 1, 2);

    uint32_t expected = 0;
    for (size_t k = 0; k < 16; k++) expected = (expected << 1) | payload[59 + k];

    size_t accepted = 0;
    uint32_t accepted_value = 0xffffffffu;
    for (uint32_t c = 0; c < 65536u; c++) {
        for (size_t k = 0; k < 16; k++) {
            payload[59 + k] = static_cast<uint8_t>((c >> (15 - k)) & 1u);
        }
        if (app::ert::Packet::from_bits(app::ert::PacketType::SCM, payload).crc_ok()) {
            accepted++;
            accepted_value = c;
        }
    }

    CHECK_EQ(accepted, size_t{1});
    CHECK_EQ(accepted_value, expected);
}

/* ===========================================================================
 * ERT — SCM+
 * ===========================================================================*/

TEST(ert_scmplus_known_frame_decodes) {
    auto bytes = ert_scmplus_bytes(/*id=*/0x12345678, /*consumption=*/0x000ABCDE,
                                   /*commodity=*/7, /*tamper=*/0x00FF);

    /* Upstream validates SCM+ against a 0x1D0F residue rather than zero, so the
     * matching CRC field is found by search. Exactly one of the 65536 values
     * may pass — which is also the proof that the check is a real one. */
    size_t accepted = 0;
    uint32_t accepted_value = 0;
    for (uint32_t c = 0; c < 65536u; c++) {
        bytes[12] = static_cast<uint8_t>(c >> 8);
        bytes[13] = static_cast<uint8_t>(c);
        const auto p =
            app::ert::Packet::from_bits(app::ert::PacketType::SCMPLUS, bits_from_bytes(bytes));
        if (p.crc_ok()) {
            accepted++;
            accepted_value = c;
        }
    }
    CHECK_EQ(accepted, size_t{1});

    bytes[12] = static_cast<uint8_t>(accepted_value >> 8);
    bytes[13] = static_cast<uint8_t>(accepted_value);
    const auto packet =
        app::ert::Packet::from_bits(app::ert::PacketType::SCMPLUS, bits_from_bytes(bytes));

    CHECK(packet.crc_ok());
    CHECK_EQ(packet.length(), size_t{112});
    CHECK_EQ(packet.id(), uint32_t{0x12345678});
    CHECK_EQ(packet.consumption(), uint32_t{0x000ABCDE});
    CHECK_EQ(packet.commodity_type(), uint32_t{7});
    CHECK_EQ(packet.tamper_flags(), uint32_t{0x00FF});
}

TEST(ert_unknown_type_never_validates) {
    const Bits payload = make_ert_scm(0x2123456, 0x0ABCDE, 7, 1, 2);
    const auto packet = app::ert::Packet::from_bits(app::ert::PacketType::Unknown, payload);

    CHECK(!packet.crc_ok());
    CHECK_EQ(packet.id(), app::ert::invalid_id);
    CHECK_EQ(packet.consumption(), app::ert::invalid_consumption);
    CHECK_EQ(packet.commodity_type(), app::ert::invalid_commodity_type);
}

TEST(ert_recent_entry_tracks_the_latest_packet) {
    const Bits payload = make_ert_scm(0x2123456, 0x0ABCDE, 7, 1, 2);
    const auto packet = app::ert::Packet::from_bits(app::ert::PacketType::SCM, payload);

    app::ert::RecentEntry entry{{packet.id(), packet.commodity_type()}};
    entry.update(packet);
    entry.update(packet);

    CHECK_EQ(entry.received_count, size_t{2});
    CHECK_EQ(entry.id, uint32_t{0x2123456});
    CHECK_EQ(entry.commodity_type, uint32_t{7});
    CHECK_EQ(entry.last_consumption, uint32_t{0x0ABCDE});
    CHECK(entry.packet_type == app::ert::PacketType::SCM);
    const app::ert::Key expected_key{0x2123456, 7};
    CHECK(entry.key() == expected_key);
}

TEST(ert_column_formatting_matches_upstream_widths) {
    CHECK_STR_EQ(app::ert::format_id(34746454u), "  34746454");
    CHECK_STR_EQ(app::ert::format_consumption(703710u), "  703710");
    CHECK_STR_EQ(app::ert::format_commodity_type(7u), " 7");
    CHECK_STR_EQ(app::ert::format_tamper_flags(0x1234u), "1234");
    CHECK_STR_EQ(app::ert::format_tamper_flags_scm(0x21u), " 1/2");
    CHECK_STR_EQ(app::ert::packet_type_name(app::ert::PacketType::SCMPLUS), "SCM+");
}

/* ===========================================================================
 * ERT — the receive pipeline
 * ===========================================================================*/

TEST(ert_decoder_reproduces_upstream_rates_at_4194304) {
    app::ert::ErtDecoder decoder;
    decoder.configure(4'194'304.0);

    CHECK(decoder.configured());
    /* proc_ert integrates 64 samples per half symbol at 4.194304 MHz, giving a
     * 65536 Hz detector rate — twice the 32768 chip rate. */
    CHECK_EQ(decoder.half_symbol_samples(), size_t{64});
    CHECK_NEAR(decoder.detector_rate(), 65536.0, 1e-6);
}

/* This is also the regression test for the DC tracker: an earlier version of
 * ErtDecoder averaged every sample over a 2048-sample window, which on an OOK
 * signal converges to about half the amplitude and makes |x - dc| identical for
 * a mark and a space. With that code this test decoded nothing at all. */
TEST(ert_decoder_recovers_an_scm_frame_from_samples) {
    /* A quarter of upstream's baseband rate still gives 16 samples per half
     * symbol, and keeps the synthetic burst small enough to run quickly. */
    constexpr float sample_rate = 1'048'576.0f;

    /* The 21-bit preamble/sync 0x1F2A60, then the 75-bit payload, all
     * Manchester coded at 32768 chips per second. */
    Bits message = bits_from_string("111110010101001100000");
    const Bits payload = make_ert_scm(0x2123456, 0x0ABCDE, 7, 1, 2);
    append(message, payload);
    CHECK_EQ(message.size(), size_t{96});

    Bits chips = repeat(bits_from_string("10"), 16);  /* carrier run-in */
    append(chips, dsp::manchester_encode(message, 0));
    append(chips, repeat(bits_from_string("10"), 8));  /* tail past the payload */

    const auto signal = dsp::ook_modulate(chips, sample_rate, 32768.0f, 1.0f);

    size_t frames = 0;
    size_t valid = 0;
    app::ert::Packet got{};

    app::ert::ErtDecoder decoder;
    decoder.set_packet_handler(
        [&](app::ert::PacketType type, const std::vector<uint8_t>& frame_chips) {
            frames++;
            const app::ert::Packet packet{type, frame_chips};
            if (packet.crc_ok()) {
                valid++;
                got = packet;
            }
        });
    decoder.configure(sample_rate);
    decoder.process(signal.data(), signal.size());

    CHECK(frames >= 1);
    CHECK(valid >= 1);
    CHECK_EQ(got.id(), uint32_t{0x2123456});
    CHECK_EQ(got.consumption(), uint32_t{0x0ABCDE});
    CHECK_EQ(got.commodity_type(), uint32_t{7});
    CHECK_EQ(got.tamper_flags(), uint32_t{0x21});
}

TEST(ert_decoder_tolerates_a_receiver_dc_offset) {
    constexpr float sample_rate = 1'048'576.0f;

    Bits message = bits_from_string("111110010101001100000");
    append(message, make_ert_scm(0x2123456, 0x0ABCDE, 7, 1, 2));

    Bits chips = repeat(bits_from_string("10"), 16);
    append(chips, dsp::manchester_encode(message, 0));
    append(chips, repeat(bits_from_string("10"), 8));

    auto signal = dsp::ook_modulate(chips, sample_rate, 32768.0f, 1.0f);
    /* A standing I/Q offset of the kind the tracker exists to remove. */
    for (auto& s : signal) s += dsp::cfloat{0.06f, -0.04f};

    size_t valid = 0;
    app::ert::ErtDecoder decoder;
    decoder.set_packet_handler(
        [&](app::ert::PacketType type, const std::vector<uint8_t>& frame_chips) {
            if (app::ert::Packet{type, frame_chips}.crc_ok()) valid++;
        });
    decoder.configure(sample_rate);
    decoder.process(signal.data(), signal.size());

    CHECK(valid >= 1);
}

TEST(ert_decoder_emits_nothing_for_silence) {
    app::ert::ErtDecoder decoder;
    size_t frames = 0;
    decoder.set_packet_handler(
        [&](app::ert::PacketType, const std::vector<uint8_t>&) { frames++; });
    decoder.configure(1'048'576.0);

    const std::vector<dsp::cfloat> silence(40000, dsp::cfloat{0.0f, 0.0f});
    decoder.process(silence.data(), silence.size());

    CHECK_EQ(frames, size_t{0});
}
