/*
 * mayhem-b200 — Radiosonde decoder tests.
 *
 * Everything here runs on synthesised data. What is checked against an outside
 * specification rather than against this code's own output:
 *
 *   - the RS41 block CRC is CRC-16/CCITT-FALSE, pinned by the Rocksoft check
 *     value 0x29B1 over "123456789";
 *   - the RS41 descrambler is pinned by the documented frame signature: on-air
 *     header bytes 0x10 B6 CA 11 22 96 12 F8 descramble to
 *     0x86 35 F4 40 93 DF 1A 60, and upstream's own comment in ui_sonde.cpp
 *     names "93DF1A60" as the four bytes the captured payload starts with;
 *   - the RS41 sync word constant is derived here from those header bytes in
 *     on-air (least-significant-bit-first) order and compared with the
 *     constant proc_sonde.hpp uses;
 *   - the RS41 position decode is checked against an independent closed-form
 *     WGS84 geodetic-to-ECEF forward transform, so the decoder's Bowring
 *     inverse has to agree with a different piece of maths, not with itself;
 *   - the Meteomodem bit decode is bi-phase mark (FM1), checked chip pattern by
 *     chip pattern and then end to end through the M20 field positions from
 *     m20mod.c.
 *
 * NOT covered, and not claimable: reception over the air. No USRP is attached,
 * so nothing here proves the app receives a real radiosonde — only that the
 * decode is correct when it is fed samples or bits.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "sonde_packet.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using app::sonde::Decoder;
using app::sonde::GPS_data;
using app::sonde::Packet;
using app::sonde::Rs41Calibration;

namespace {

constexpr double kPi = 3.14159265358979323846;

/* --- frame construction --------------------------------------------------- */

/* On-air bytes are transmitted least-significant bit first, which is what
 * Packet::vaisala_descramble undoes. */
dsp::Packet packet_from_raw_lsb_first(const std::vector<uint8_t>& raw) {
    dsp::Packet p;
    for (const uint8_t v : raw)
        for (int b = 0; b < 8; b++) p.add(((v >> b) & 1) != 0);
    return p;
}

/* Scramble a clear 320-byte payload the way the sonde does, then bit it out.
 * `pos + 4` because the packet builder consumed the four sync bytes. */
dsp::Packet make_rs41_packet(const std::vector<uint8_t>& clear) {
    std::vector<uint8_t> raw(clear.size());
    for (size_t pos = 0; pos < clear.size(); pos++)
        raw[pos] = static_cast<uint8_t>(clear[pos] ^ Packet::vaisala_mask[(pos + 4) % 64]);
    return packet_from_raw_lsb_first(raw);
}

/* Block layout: ID, data length, data, CRC-16 over the data, little-endian. */
void put_block(std::vector<uint8_t>& frame, uint32_t start, uint8_t id,
               const std::vector<uint8_t>& data) {
    frame[start] = id;
    frame[start + 1] = static_cast<uint8_t>(data.size());
    for (size_t i = 0; i < data.size(); i++) frame[start + 2 + i] = data[i];
    const uint16_t crc = dsp::crc16_ccitt(data.data(), data.size());
    frame[start + 2 + data.size()] = static_cast<uint8_t>(crc & 0xFF);
    frame[start + 3 + data.size()] = static_cast<uint8_t>((crc >> 8) & 0xFF);
}

void put_le32(std::vector<uint8_t>& v, size_t at, int32_t value) {
    const uint32_t u = static_cast<uint32_t>(value);
    v[at + 0] = static_cast<uint8_t>(u & 0xFF);
    v[at + 1] = static_cast<uint8_t>((u >> 8) & 0xFF);
    v[at + 2] = static_cast<uint8_t>((u >> 16) & 0xFF);
    v[at + 3] = static_cast<uint8_t>((u >> 24) & 0xFF);
}

struct Ecef {
    double x, y, z;
};

/* Independent forward transform: WGS84 geodetic -> ECEF, closed form. */
Ecef geodetic_to_ecef(double lat_deg, double lon_deg, double height_m) {
    const double a = 6378137.0;
    const double b = 6356752.31424518;
    const double e2 = (a * a - b * b) / (a * a);
    const double phi = lat_deg * kPi / 180.0;
    const double lam = lon_deg * kPi / 180.0;
    const double N = a / std::sqrt(1.0 - e2 * std::sin(phi) * std::sin(phi));
    return {(N + height_m) * std::cos(phi) * std::cos(lam),
            (N + height_m) * std::cos(phi) * std::sin(lam),
            (N * (1.0 - e2) + height_m) * std::sin(phi)};
}

/* A complete, CRC-valid RS41 frame carrying the given identity and position. */
std::vector<uint8_t> build_rs41_frame(const std::string& serial,
                                      uint16_t frame_number,
                                      uint8_t voltage_tenths,
                                      double lat_deg, double lon_deg, double alt_m) {
    std::vector<uint8_t> frame(320, 0);

    /* Status block, 40 data bytes at 0x37. */
    std::vector<uint8_t> status(40, 0);
    status[0] = static_cast<uint8_t>(frame_number & 0xFF);        /* 0x37 */
    status[1] = static_cast<uint8_t>((frame_number >> 8) & 0xFF); /* 0x38 */
    for (size_t i = 0; i < 8; i++)                                /* 0x39 .. 0x40 */
        status[2 + i] = (i < serial.size()) ? static_cast<uint8_t>(serial[i]) : uint8_t{0x20};
    status[10] = voltage_tenths;                                  /* 0x41 */
    status[23] = 0x03;                                            /* 0x4E: subframe slot */
    put_block(frame, Packet::block_status, 0x79, status);

    /* Measurement block, 42 data bytes at 0x63. */
    const std::vector<uint8_t> meas(42, 0);
    put_block(frame, Packet::block_meas, 0x7A, meas);

    /* GPS position block, 21 data bytes at 0x110. */
    std::vector<uint8_t> gps(21, 0);
    const Ecef e = geodetic_to_ecef(lat_deg, lon_deg, alt_m);
    put_le32(gps, 0, static_cast<int32_t>(std::llround(e.x * 100.0)));
    put_le32(gps, 4, static_cast<int32_t>(std::llround(e.y * 100.0)));
    put_le32(gps, 8, static_cast<int32_t>(std::llround(e.z * 100.0)));
    put_block(frame, Packet::block_gpspos, 0x7B, gps);

    return frame;
}

/* --- Meteomodem ----------------------------------------------------------- */

/* Decoded Meteomodem bytes are read most-significant bit first (FieldReader
 * with no remap), so byte 0 bit 7 is decoded symbol 0. */
std::vector<uint8_t> bytes_to_msb_bits(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (const uint8_t v : bytes)
        for (int b = 7; b >= 0; b--) bits.push_back(static_cast<uint8_t>((v >> b) & 1));
    return bits;
}

dsp::Packet make_meteomodem_packet(const std::vector<uint8_t>& bytes) {
    const auto chips = dsp::biphase_m_encode(bytes_to_msb_bits(bytes), 0);
    dsp::Packet p;
    for (const uint8_t c : chips) p.add(c != 0);
    return p;
}

/* An M20 frame: 88 bytes, the length the upstream packet builder captures. */
std::vector<uint8_t> build_m20_frame(uint32_t serial, uint8_t frame_number,
                                     double lat_deg, double lon_deg, double alt_m,
                                     uint8_t battery_byte) {
    std::vector<uint8_t> f(88, 0);
    f[0] = 0x45;  /* frame length marker, m20mod.c */
    f[1] = 0x20;

    const uint32_t alt_cm = static_cast<uint32_t>(std::llround(alt_m * 100.0));
    f[8] = static_cast<uint8_t>((alt_cm >> 16) & 0xFF);
    f[9] = static_cast<uint8_t>((alt_cm >> 8) & 0xFF);
    f[10] = static_cast<uint8_t>(alt_cm & 0xFF);

    f[0x12] = static_cast<uint8_t>(serial & 0xFF);
    f[0x13] = static_cast<uint8_t>((serial >> 8) & 0xFF);
    f[0x14] = static_cast<uint8_t>((serial >> 16) & 0xFF);
    f[0x15] = frame_number;

    const uint32_t lat_u = static_cast<uint32_t>(
        static_cast<int32_t>(std::llround(lat_deg * 1000000.0)));
    const uint32_t lon_u = static_cast<uint32_t>(
        static_cast<int32_t>(std::llround(lon_deg * 1000000.0)));
    for (int i = 0; i < 4; i++) {
        f[28 + i] = static_cast<uint8_t>((lat_u >> (24 - 8 * i)) & 0xFF);
        f[32 + i] = static_cast<uint8_t>((lon_u >> (24 - 8 * i)) & 0xFF);
    }

    f[0x26] = battery_byte;
    return f;
}

/* --- bit stream helpers for the DSP tests --------------------------------- */

void append_lsb_first_bytes(std::vector<uint8_t>& bits, const std::vector<uint8_t>& bytes) {
    for (const uint8_t v : bytes)
        for (int b = 0; b < 8; b++) bits.push_back(static_cast<uint8_t>((v >> b) & 1));
}

void append_msb_first_word(std::vector<uint8_t>& bits, uint64_t value, size_t length) {
    for (size_t i = length; i > 0; i--)
        bits.push_back(static_cast<uint8_t>((value >> (i - 1)) & 1));
}

/* The four RS41 sync bytes, scrambled/on-air. */
const std::vector<uint8_t> kRs41HeaderBytes = {0x10, 0xB6, 0xCA, 0x11};

uint8_t descramble_at(uint8_t on_air_byte, size_t mask_index) {
    return static_cast<uint8_t>(on_air_byte ^ Packet::vaisala_mask[mask_index % 64]);
}

/* `tail_bits` matters for the signal-level tests: a sonde keeps transmitting
 * after a frame, and a timing-recovery loop needs at least one more symbol
 * period of signal to emit the frame's last bit. Modulating a stream that stops
 * dead on the final symbol boundary loses that bit and the frame never
 * completes. */
std::vector<uint8_t> rs41_bit_frame(const std::vector<uint8_t>& clear_frame,
                                    size_t lead_in_bits,
                                    size_t tail_bits = 0) {
    std::vector<uint8_t> bits;
    for (size_t i = 0; i < lead_in_bits; i++) bits.push_back(static_cast<uint8_t>(i & 1));
    append_lsb_first_bytes(bits, kRs41HeaderBytes);
    std::vector<uint8_t> raw(clear_frame.size());
    for (size_t pos = 0; pos < clear_frame.size(); pos++)
        raw[pos] = static_cast<uint8_t>(clear_frame[pos] ^ Packet::vaisala_mask[(pos + 4) % 64]);
    append_lsb_first_bytes(bits, raw);
    for (size_t i = 0; i < tail_bits; i++) bits.push_back(static_cast<uint8_t>(i & 1));
    return bits;
}

std::vector<uint8_t> meteomodem_chip_frame(const std::vector<uint8_t>& frame_bytes,
                                           size_t lead_in_chips,
                                           size_t tail_chips = 0) {
    std::vector<uint8_t> chips;
    for (size_t i = 0; i < lead_in_chips; i++) chips.push_back(static_cast<uint8_t>(i & 1));
    append_msb_first_word(chips, Decoder::meteomodem_sync, Decoder::meteomodem_sync_bits);
    const auto payload = dsp::biphase_m_encode(bytes_to_msb_bits(frame_bytes), 0);
    for (const uint8_t c : payload) chips.push_back(c);
    for (size_t i = 0; i < tail_chips; i++) chips.push_back(static_cast<uint8_t>(i & 1));
    return chips;
}

}  // namespace

/* ===========================================================================
 * CRC
 * ===========================================================================*/

TEST(sonde_rs41_crc_is_ccitt_false) {
    /* The RS41 block CRC is poly 0x1021, init 0xFFFF, no reflection, no final
     * XOR. Its Rocksoft check value over "123456789" is 0x29B1. */
    const char check[] = "123456789";
    CHECK_EQ(dsp::crc16_ccitt(check, 9), static_cast<uint16_t>(0x29B1));
}

TEST(sonde_rs41_block_crc_accepts_a_well_formed_block) {
    const auto clear = build_rs41_frame("S1234567", 0x1234, 26, 48.0, 2.0, 1000.0);
    const Packet p{make_rs41_packet(clear), Packet::Type::Vaisala_RS41_SG};

    CHECK(p.crc16rs41(Packet::block_status));
    CHECK(p.crc16rs41(Packet::block_meas));
    CHECK(p.crc16rs41(Packet::block_gpspos));
    CHECK(p.crc_ok());
}

TEST(sonde_rs41_block_crc_rejects_a_flipped_data_bit) {
    auto clear = build_rs41_frame("S1234567", 0x1234, 26, 48.0, 2.0, 1000.0);
    /* Flip one bit inside the status block's data, leaving its CRC alone. */
    clear[Packet::pos_FrameNb] ^= 0x01;

    const Packet p{make_rs41_packet(clear), Packet::Type::Vaisala_RS41_SG};

    CHECK(!p.crc16rs41(Packet::block_status));
    CHECK(!p.crc_ok());
    /* The other two blocks are untouched and must still verify. */
    CHECK(p.crc16rs41(Packet::block_meas));
    CHECK(p.crc16rs41(Packet::block_gpspos));
}

TEST(sonde_rs41_block_crc_rejects_a_flipped_crc_bit) {
    auto clear = build_rs41_frame("S1234567", 0x1234, 26, 48.0, 2.0, 1000.0);
    clear[Packet::block_gpspos + 2 + 21] ^= 0x80;  /* low CRC byte */

    const Packet p{make_rs41_packet(clear), Packet::Type::Vaisala_RS41_SG};
    CHECK(!p.crc16rs41(Packet::block_gpspos));
}

TEST(sonde_rs41_block_crc_rejects_a_truncated_packet) {
    /* 64 bytes: the status block's length byte points past the end. */
    const auto clear = build_rs41_frame("S1234567", 1, 26, 48.0, 2.0, 100.0);
    const std::vector<uint8_t> shortened(clear.begin(), clear.begin() + 64);

    const Packet p{make_rs41_packet(shortened), Packet::Type::Vaisala_RS41_SG};
    CHECK(!p.crc16rs41(Packet::block_meas));
    CHECK(!p.crc16rs41(Packet::block_gpspos));
    CHECK(!p.crc_ok());
}

TEST(sonde_rs41_block_crc_rejects_an_empty_packet) {
    const Packet p{dsp::Packet{}, Packet::Type::Vaisala_RS41_SG};
    CHECK(!p.crc16rs41(Packet::block_status));
    CHECK(!p.crc_ok());
    CHECK_EQ(p.length(), size_t{0});
}

/* ===========================================================================
 * Descrambling and the sync word
 * ===========================================================================*/

TEST(sonde_rs41_descramble_reproduces_the_documented_signature) {
    /* On-air header 0x10 B6 CA 11 22 96 12 F8. The packet builder eats the
     * first four, so payload byte 0 is 0x22 and must descramble to 0x93 — the
     * first byte of the "93DF1A60" signature upstream names in ui_sonde.cpp. */
    const std::vector<uint8_t> raw_payload = {0x22, 0x96, 0x12, 0xF8};
    const Packet p{packet_from_raw_lsb_first(raw_payload), Packet::Type::Vaisala_RS41_SG};

    CHECK_EQ(p.vaisala_descramble(0), static_cast<uint8_t>(0x93));
    CHECK_EQ(p.vaisala_descramble(1), static_cast<uint8_t>(0xDF));
    CHECK_EQ(p.vaisala_descramble(2), static_cast<uint8_t>(0x1A));
    CHECK_EQ(p.vaisala_descramble(3), static_cast<uint8_t>(0x60));
}

TEST(sonde_rs41_mask_descrambles_the_consumed_sync_bytes_too) {
    /* Same table, mask index 0..3, which is where the +4 offset comes from:
     * 0x10^0x96 = 0x86, 0xB6^0x83 = 0x35, 0xCA^0x3E = 0xF4, 0x11^0x51 = 0x40. */
    const std::vector<uint8_t> expected = {0x86, 0x35, 0xF4, 0x40};
    for (size_t i = 0; i < kRs41HeaderBytes.size(); i++)
        CHECK_EQ(descramble_at(kRs41HeaderBytes[i], i), expected[i]);
}

TEST(sonde_rs41_sync_word_matches_the_header_bytes) {
    /* Derive the sync constant from the header bytes in on-air order and check
     * it against the one proc_sonde.hpp uses. */
    uint64_t derived = 0;
    for (const uint8_t v : kRs41HeaderBytes)
        for (int b = 0; b < 8; b++) derived = (derived << 1) | ((v >> b) & 1u);

    CHECK_EQ(derived, Decoder::rs41_sync);
    CHECK_EQ(derived, static_cast<uint64_t>(0x086D5388));
}

/* ===========================================================================
 * RS41 field decode
 * ===========================================================================*/

TEST(sonde_rs41_decodes_identity_frame_and_voltage) {
    const auto clear = build_rs41_frame("R3140571", 0x0ABC, 27, 48.0, 2.0, 1000.0);
    const Packet p{make_rs41_packet(clear), Packet::Type::Vaisala_RS41_SG};

    CHECK_STR_EQ(p.type_string(), "Vaisala RS41-SG");
    CHECK_STR_EQ(p.serial_number(), "R3140571");
    CHECK_EQ(p.frame(), static_cast<uint32_t>(0x0ABC));
    /* Byte holds volts * 10, so 27 -> 2.7 V -> 2700 mV. */
    CHECK_EQ(p.battery_voltage(), static_cast<uint32_t>(2700));
    CHECK_EQ(p.length(), size_t{1280});  /* symbols_count() == bits / 2 */
}

TEST(sonde_rs41_serial_marks_unprintable_bytes) {
    auto clear = build_rs41_frame("ABCDEFGH", 1, 26, 48.0, 2.0, 100.0);
    clear[Packet::pos_SondeID + 2] = 0x01;  /* control character */
    clear[Packet::pos_SondeID + 5] = 0xFF;  /* above ASCII */
    /* Re-CRC the status block so the frame stays well formed. */
    const std::vector<uint8_t> status(clear.begin() + Packet::block_status + 2,
                                      clear.begin() + Packet::block_status + 2 + 40);
    put_block(clear, Packet::block_status, 0x79, status);

    const Packet p{make_rs41_packet(clear), Packet::Type::Vaisala_RS41_SG};
    CHECK_STR_EQ(p.serial_number(), "AB?DE?GH");
}

TEST(sonde_rs41_decodes_position_against_forward_wgs84) {
    struct Site {
        double lat, lon, alt;
    };
    /* Northern/eastern, southern/western, and a high-altitude point of the kind
     * a sonde actually reports. */
    const Site sites[] = {
        {48.858370, 2.294481, 300.0},
        {-22.906800, -43.172900, 50.0},
        {60.170800, 24.938400, 24000.0},
    };

    for (const Site& s : sites) {
        const auto clear = build_rs41_frame("S0000001", 1, 26, s.lat, s.lon, s.alt);
        const Packet p{make_rs41_packet(clear), Packet::Type::Vaisala_RS41_SG};

        const GPS_data g = p.get_GPS_data();
        CHECK(g.is_valid());
        CHECK_NEAR(g.lat, s.lat, 1e-4);
        CHECK_NEAR(g.lon, s.lon, 1e-4);
        /* alt is truncated to a uint32_t, and the wire holds centimetres. */
        CHECK_NEAR(static_cast<double>(g.alt), s.alt, 2.0);
    }
}

TEST(sonde_rs41_position_is_withheld_when_its_block_crc_fails) {
    auto clear = build_rs41_frame("S0000001", 1, 26, 48.858370, 2.294481, 300.0);
    clear[Packet::pos_GPSecefX] ^= 0x40;  /* corrupt X, leave the CRC alone */

    const Packet p{make_rs41_packet(clear), Packet::Type::Vaisala_RS41_SG};
    const GPS_data g = p.get_GPS_data();

    CHECK(!g.is_valid());
    CHECK_EQ(g.alt, static_cast<uint32_t>(0));
    CHECK_NEAR(g.lat, 0.0, 1e-9);
    CHECK_NEAR(g.lon, 0.0, 1e-9);
}

TEST(sonde_rs41_reports_no_temperature_before_calibration_arrives) {
    Rs41Calibration cal;
    const auto clear = build_rs41_frame("S0000001", 1, 26, 48.0, 2.0, 1000.0);
    const Packet p{make_rs41_packet(clear), Packet::Type::Vaisala_RS41_SG, &cal};

    const auto th = p.get_temp_humid();
    /* Subframe 0x03 is stored by this frame, but 0x04..0x07 have not been seen,
     * so neither temperature nor humidity is computable yet. */
    CHECK_NEAR(th.temp, 0.0, 1e-9);
    CHECK_NEAR(th.humid, 0.0, 1e-9);
    CHECK(cal.have(0x03));
    CHECK(!cal.have(0x04));
}

TEST(sonde_rs41_calibration_store_rejects_an_out_of_range_slot) {
    /* Upstream writes calibytes[calfr * 16 + i] with calfr straight off the
     * air, overrunning a 51-slot table. */
    Rs41Calibration cal;
    uint8_t subframe[Rs41Calibration::subframe_size];
    for (size_t i = 0; i < sizeof(subframe); i++) subframe[i] = static_cast<uint8_t>(i);

    CHECK(cal.store(50, subframe));
    CHECK(!cal.store(51, subframe));
    CHECK(!cal.store(255, subframe));
    CHECK(!cal.have(51));
    CHECK(cal.have(50));
    CHECK_EQ(cal.calibytes[50 * 16 + 3], static_cast<uint8_t>(3));
}

TEST(sonde_rs41_has_no_pressure_sensor) {
    const auto clear = build_rs41_frame("S0000001", 1, 26, 48.0, 2.0, 1000.0);
    const Packet p{make_rs41_packet(clear), Packet::Type::Vaisala_RS41_SG};
    CHECK_NEAR(p.get_pressure(), 0.0, 1e-9);
}

/* ===========================================================================
 * Meteomodem bit decode
 * ===========================================================================*/

TEST(sonde_biphase_mark_chip_pairs_follow_fm1) {
    /* Bi-phase mark: a transition at every symbol boundary, plus a mid-symbol
     * transition for a 1. So 0 -> a pair of equal chips, 1 -> a pair that
     * differs, and the pair's starting level alternates on every 0. */
    const std::vector<uint8_t> bits = {1, 0, 1, 1, 0, 0, 1, 0};
    const auto chips = dsp::biphase_m_encode(bits, 0);

    CHECK_EQ(chips.size(), bits.size() * 2);
    for (size_t i = 0; i < bits.size(); i++) {
        const bool differ = (chips[2 * i] != chips[2 * i + 1]);
        CHECK_EQ(differ, bits[i] != 0);
    }

    const auto decoded = dsp::biphase_m_decode(chips);
    CHECK_EQ(decoded.size(), bits.size());
    for (size_t i = 0; i < bits.size(); i++) CHECK_EQ(decoded[i], bits[i]);
}

TEST(sonde_meteomodem_reader_recovers_the_encoded_bytes) {
    std::vector<uint8_t> bytes(88, 0);
    bytes[0] = 0x64;
    bytes[1] = 0x9F;
    bytes[7] = 0xA5;
    bytes[42] = 0x5A;
    bytes[87] = 0xF0;

    const Packet p{make_meteomodem_packet(bytes), Packet::Type::Meteomodem_unknown};

    /* 88 bytes -> 704 symbols -> 1408 chips. */
    CHECK_EQ(p.raw().size(), size_t{1408});
    CHECK_EQ(p.biphase_bits().size(), size_t{704});
    CHECK_EQ(p.length(), size_t{704});

    const auto& bits = p.biphase_bits();
    for (size_t i = 0; i < bytes.size(); i++) {
        uint32_t v = 0;
        for (size_t b = 0; b < 8; b++) v = (v << 1) | bits[i * 8 + b];
        CHECK_EQ(static_cast<uint8_t>(v), bytes[i]);
    }
}

TEST(sonde_meteomodem_type_is_taken_from_the_first_two_bytes) {
    struct Case {
        uint8_t b0, b1;
        Packet::Type type;
        const char* name;
    };
    const Case cases[] = {
        {0x64, 0x9F, Packet::Type::Meteomodem_M10, "Meteomodem M10"},
        {0x64, 0x8F, Packet::Type::Meteomodem_M2K2, "Meteomodem M2K2"},
        {0x45, 0x20, Packet::Type::Meteomodem_M20, "Meteomodem M20"},
        {0x43, 0x20, Packet::Type::Meteomodem_M20, "Meteomodem M20"},
        {0x00, 0x00, Packet::Type::Meteomodem_unknown, "Meteomodem ???"},
    };

    for (const Case& c : cases) {
        std::vector<uint8_t> bytes(88, 0);
        bytes[0] = c.b0;
        bytes[1] = c.b1;
        const Packet p{make_meteomodem_packet(bytes), Packet::Type::Meteomodem_unknown};
        CHECK(p.type() == c.type);
        CHECK_STR_EQ(p.type_string(), c.name);
    }
}

TEST(sonde_m20_decodes_identity_position_and_battery) {
    const auto frame = build_m20_frame(0x0F4240u, 0x2A, -33.868800, 151.209300, 12345.0, 200);
    const Packet p{make_meteomodem_packet(frame), Packet::Type::Meteomodem_unknown};

    CHECK(p.type() == Packet::Type::Meteomodem_M20);
    CHECK(p.crc_ok());  /* M20 uses the length/marker check, not a CRC */
    CHECK_STR_EQ(p.serial_number(), "1000000");
    CHECK_EQ(p.frame(), static_cast<uint32_t>(0x2A));

    const GPS_data g = p.get_GPS_data();
    CHECK(g.is_valid());
    CHECK_NEAR(g.lat, -33.868800, 1e-5);
    CHECK_NEAR(g.lon, 151.209300, 1e-5);
    CHECK_EQ(g.alt, static_cast<uint32_t>(12345));

    /* 200 * 3.3 / 255 V = 2.588 V. */
    CHECK_NEAR(static_cast<double>(p.battery_voltage()), 2588.0, 2.0);
}

TEST(sonde_m20_check_rejects_a_wrong_marker) {
    auto frame = build_m20_frame(1, 1, 0.0, 0.0, 0.0, 0);
    frame[0] = 0x44;  /* neither 0x45 nor 0x43 */
    const Packet bad{make_meteomodem_packet(frame), Packet::Type::Meteomodem_M20};
    CHECK(!bad.crc_ok());

    frame[0] = 0x45;
    frame[1] = 0x21;  /* second marker byte must be 0x20 */
    const Packet bad2{make_meteomodem_packet(frame), Packet::Type::Meteomodem_M20};
    CHECK(!bad2.crc_ok());

    frame[1] = 0x20;
    const Packet good{make_meteomodem_packet(frame), Packet::Type::Meteomodem_M20};
    CHECK(good.crc_ok());
}

TEST(sonde_m20_rejects_a_frame_shorter_than_its_advertised_length) {
    /* 8 decoded bytes -> 128 chips; packet_.size() / 8 = 16, under 0x45. */
    std::vector<uint8_t> tiny(8, 0);
    tiny[0] = 0x45;
    tiny[1] = 0x20;
    const Packet p{make_meteomodem_packet(tiny), Packet::Type::Meteomodem_unknown};
    CHECK(p.type() == Packet::Type::Meteomodem_M20);
    CHECK(!p.crc_ok());
}

TEST(sonde_m10_position_reads_are_signed) {
    /* Upstream's FieldReader returns a signed value so that a west/south M10
     * position comes out negative. lat = raw / (2^32 / 360). */
    std::vector<uint8_t> bytes(88, 0);
    bytes[0] = 0x64;
    bytes[1] = 0x9F;
    /* -45 degrees -> raw = -45 * 2^32/360 = -536870912 = 0xE0000000. */
    const uint32_t raw = 0xE0000000u;
    for (int i = 0; i < 4; i++)
        bytes[14 + i] = static_cast<uint8_t>((raw >> (24 - 8 * i)) & 0xFF);
    /* Altitude field: (value / 1000) - 48 metres. */
    const uint32_t alt_raw = 1048000u;  /* -> 1048 - 48 = 1000 m */
    for (int i = 0; i < 4; i++)
        bytes[22 + i] = static_cast<uint8_t>((alt_raw >> (24 - 8 * i)) & 0xFF);

    const Packet p{make_meteomodem_packet(bytes), Packet::Type::Meteomodem_unknown};
    CHECK(p.type() == Packet::Type::Meteomodem_M10);

    const GPS_data g = p.get_GPS_data();
    CHECK_NEAR(g.lat, -45.0, 1e-4);
    CHECK_EQ(g.alt, static_cast<uint32_t>(1000));
}

/* ===========================================================================
 * Decoder — bit level
 * ===========================================================================*/

TEST(sonde_decoder_assembles_an_rs41_frame_from_bits) {
    const auto clear = build_rs41_frame("R3140571", 0x0201, 27, 48.858370, 2.294481, 15000.0);
    const auto bits = rs41_bit_frame(clear, 96);

    Rs41Calibration cal;
    Decoder decoder;
    decoder.set_calibration(&cal);

    int seen = 0;
    std::string serial;
    uint32_t frame_number = 0;
    GPS_data gps{};
    bool crc = false;
    decoder.set_packet_handler([&](const Packet& p) {
        seen++;
        serial = p.serial_number();
        frame_number = p.frame();
        gps = p.get_GPS_data();
        crc = p.crc_ok();
    });

    decoder.feed_bits_rs41(bits.data(), bits.size());

    CHECK_EQ(seen, 1);
    CHECK(crc);
    CHECK_STR_EQ(serial, "R3140571");
    CHECK_EQ(frame_number, static_cast<uint32_t>(0x0201));
    CHECK_NEAR(gps.lat, 48.858370, 1e-4);
    CHECK_NEAR(gps.lon, 2.294481, 1e-4);
    CHECK_EQ(decoder.packets_rs41(), static_cast<uint64_t>(1));
    CHECK_EQ(decoder.packets_meteomodem(), static_cast<uint64_t>(0));
}

TEST(sonde_decoder_accepts_the_inverted_bit_sense) {
    /* The host does not know which way round the discriminator sits, so each
     * bit is offered to the builders in both senses. */
    const auto clear = build_rs41_frame("INVERTED", 7, 26, 10.0, 20.0, 500.0);
    auto bits = rs41_bit_frame(clear, 96);
    for (uint8_t& b : bits) b = static_cast<uint8_t>(b ^ 1);

    Decoder decoder;
    int seen = 0;
    std::string serial;
    decoder.set_packet_handler([&](const Packet& p) {
        seen++;
        serial = p.serial_number();
    });

    decoder.feed_bits_rs41(bits.data(), bits.size());

    CHECK_EQ(seen, 1);
    CHECK_STR_EQ(serial, "INVERTED");
}

TEST(sonde_decoder_tolerates_one_bit_of_sync_error) {
    /* proc_sonde allows a Hamming distance of 1 on both sync words. */
    const auto clear = build_rs41_frame("S0000009", 3, 26, 5.0, 6.0, 700.0);
    auto bits = rs41_bit_frame(clear, 96);
    bits[96 + 5] = static_cast<uint8_t>(bits[96 + 5] ^ 1);  /* inside the sync word */

    Decoder decoder;
    int seen = 0;
    decoder.set_packet_handler([&](const Packet&) { seen++; });
    decoder.feed_bits_rs41(bits.data(), bits.size());
    CHECK_EQ(seen, 1);
}

TEST(sonde_decoder_rejects_two_bits_of_sync_error) {
    const auto clear = build_rs41_frame("S0000009", 3, 26, 5.0, 6.0, 700.0);
    auto bits = rs41_bit_frame(clear, 96);
    bits[96 + 5] = static_cast<uint8_t>(bits[96 + 5] ^ 1);
    bits[96 + 9] = static_cast<uint8_t>(bits[96 + 9] ^ 1);

    Decoder decoder;
    int seen = 0;
    decoder.set_packet_handler([&](const Packet&) { seen++; });
    decoder.feed_bits_rs41(bits.data(), bits.size());
    CHECK_EQ(seen, 0);
}

TEST(sonde_decoder_assembles_a_meteomodem_frame_from_chips) {
    const auto frame = build_m20_frame(0x0186A0u, 0x11, 51.477900, -0.001500, 8000.0, 190);
    const auto chips = meteomodem_chip_frame(frame, 96);

    Decoder decoder;
    int seen = 0;
    std::string serial;
    GPS_data gps{};
    Packet::Type type = Packet::Type::Unknown;
    decoder.set_packet_handler([&](const Packet& p) {
        seen++;
        type = p.type();
        serial = p.serial_number();
        gps = p.get_GPS_data();
    });

    decoder.feed_bits_meteomodem(chips.data(), chips.size());

    CHECK_EQ(seen, 1);
    CHECK(type == Packet::Type::Meteomodem_M20);
    CHECK_STR_EQ(serial, "100000");
    CHECK_NEAR(gps.lat, 51.477900, 1e-5);
    CHECK_NEAR(gps.lon, -0.001500, 1e-5);
    CHECK_EQ(gps.alt, static_cast<uint32_t>(8000));
    CHECK_EQ(decoder.packets_meteomodem(), static_cast<uint64_t>(1));
}

TEST(sonde_decoder_ignores_a_stream_with_no_sync) {
    /* A deterministic pseudo-random bit stream longer than two frames. */
    uint32_t state = 0x13579BDFu;
    std::vector<uint8_t> bits;
    bits.reserve(8000);
    for (size_t i = 0; i < 8000; i++) {
        state = state * 1664525u + 1013904223u;
        bits.push_back(static_cast<uint8_t>((state >> 24) & 1));
    }

    Decoder decoder;
    int seen = 0;
    decoder.set_packet_handler([&](const Packet&) { seen++; });
    decoder.feed_bits_rs41(bits.data(), bits.size());
    decoder.feed_bits_meteomodem(bits.data(), bits.size());

    CHECK_EQ(seen, 0);
    CHECK_EQ(decoder.packets_total(), static_cast<uint64_t>(0));
}

TEST(sonde_decoder_survives_a_truncated_frame_and_recovers_on_the_next) {
    const auto clear = build_rs41_frame("S0000042", 42, 26, 30.0, 40.0, 900.0);
    auto bits = rs41_bit_frame(clear, 96);
    /* Cut the first frame short, then send a complete one. */
    std::vector<uint8_t> stream(bits.begin(), bits.begin() + 1200);
    stream.insert(stream.end(), bits.begin(), bits.end());

    Decoder decoder;
    int seen = 0;
    uint32_t frame_number = 0;
    decoder.set_packet_handler([&](const Packet& p) {
        seen++;
        frame_number = p.frame();
    });
    decoder.feed_bits_rs41(stream.data(), stream.size());

    CHECK_EQ(seen, 1);
    CHECK_EQ(frame_number, static_cast<uint32_t>(42));
}

/* ===========================================================================
 * Decoder — signal level
 * ===========================================================================*/

TEST(sonde_decoder_recovers_an_rs41_frame_from_a_modulated_signal) {
    /* proc_sonde's post-decimation channel rate, and the RS41's real 4800 bit/s
     * with a 2.4 kHz deviation. */
    constexpr float fs = 38400.0f;

    const auto clear = build_rs41_frame("R3140571", 0x0301, 27, 48.858370, 2.294481, 15000.0);
    const auto bits = rs41_bit_frame(clear, 128, 64);
    auto signal = dsp::fsk_modulate(bits, fs, Decoder::rs41_baud, Decoder::rs41_deviation, 0.0f);

    Decoder decoder;
    decoder.configure_channel(fs);

    int seen = 0;
    std::string serial;
    bool crc = false;
    GPS_data gps{};
    decoder.set_packet_handler([&](const Packet& p) {
        seen++;
        serial = p.serial_number();
        crc = p.crc_ok();
        gps = p.get_GPS_data();
    });

    decoder.feed(signal.data(), signal.size());

    CHECK_EQ(seen, 1);
    CHECK(crc);
    CHECK_STR_EQ(serial, "R3140571");
    CHECK_NEAR(gps.lat, 48.858370, 1e-4);
    CHECK_NEAR(gps.lon, 2.294481, 1e-4);
    CHECK_EQ(decoder.channel_rate(), 38400.0);
    CHECK(!decoder.front_end_enabled());
}

TEST(sonde_decoder_recovers_a_meteomodem_frame_from_a_modulated_signal) {
    /* 9600 chips/s on air (4800 bit/s bi-phase mark) at proc_sonde's own
     * 38.4 kHz channel rate, i.e. 4 samples per chip. */
    constexpr float fs = 38400.0f;

    const auto frame = build_m20_frame(0x0186A0u, 0x33, 51.477900, -0.001500, 8000.0, 190);
    const auto chips = meteomodem_chip_frame(frame, 128, 64);
    auto signal = dsp::fsk_modulate(chips, fs, Decoder::meteomodem_chip_rate,
                                    Decoder::meteomodem_deviation, 0.0f);

    Decoder decoder;
    decoder.configure_channel(fs);

    int seen = 0;
    Packet::Type type = Packet::Type::Unknown;
    GPS_data gps{};
    decoder.set_packet_handler([&](const Packet& p) {
        seen++;
        type = p.type();
        gps = p.get_GPS_data();
    });

    decoder.feed(signal.data(), signal.size());

    CHECK_EQ(seen, 1);
    CHECK(type == Packet::Type::Meteomodem_M20);
    CHECK_EQ(gps.alt, static_cast<uint32_t>(8000));
}

TEST(sonde_decoder_front_end_mixes_and_filters_an_offset_channel) {
    /* The whole receive path: a frame sitting 48 kHz above the centre of a
     * 384 kHz capture, mixed down and decimated to the channel rate before the
     * discriminator sees it. */
    constexpr double input_rate = 384000.0;
    constexpr double offset = 48000.0;

    const auto clear = build_rs41_frame("OFFSET01", 0x0401, 26, -22.906800, -43.172900, 2500.0);
    const auto bits = rs41_bit_frame(clear, 128, 64);
    auto baseband = dsp::fsk_modulate(bits, static_cast<float>(input_rate), Decoder::rs41_baud,
                                      Decoder::rs41_deviation, 0.0f);

    /* Shift the frame up to +offset, where the app's NCO expects to find it. */
    std::vector<dsp::cfloat> wideband(baseband.size());
    for (size_t i = 0; i < baseband.size(); i++) {
        const double phase = 2.0 * kPi * offset * static_cast<double>(i) / input_rate;
        wideband[i] = baseband[i] * dsp::cfloat{static_cast<float>(std::cos(phase)),
                                                static_cast<float>(std::sin(phase))};
    }

    Decoder decoder;
    decoder.configure(input_rate, offset);

    CHECK(decoder.front_end_enabled());
    CHECK_EQ(decoder.decimation(), size_t{10});
    CHECK_EQ(decoder.channel_rate(), 38400.0);

    int seen = 0;
    std::string serial;
    bool crc = false;
    decoder.set_packet_handler([&](const Packet& p) {
        seen++;
        serial = p.serial_number();
        crc = p.crc_ok();
    });

    decoder.feed(wideband.data(), wideband.size());

    CHECK_EQ(seen, 1);
    CHECK(crc);
    CHECK_STR_EQ(serial, "OFFSET01");
}

TEST(sonde_decoder_finds_nothing_in_an_empty_band) {
    constexpr double input_rate = 384000.0;
    Decoder decoder;
    decoder.configure(input_rate, 0.0);

    int seen = 0;
    decoder.set_packet_handler([&](const Packet&) { seen++; });

    /* Deterministic noise, roughly the amplitude of the modulated frames. */
    uint32_t state = 0x2468ACE0u;
    std::vector<dsp::cfloat> noise(200000);
    for (auto& s : noise) {
        state = state * 1664525u + 1013904223u;
        const float i_part = (static_cast<float>((state >> 16) & 0xFFFF) / 32768.0f) - 1.0f;
        state = state * 1664525u + 1013904223u;
        const float q_part = (static_cast<float>((state >> 16) & 0xFFFF) / 32768.0f) - 1.0f;
        s = dsp::cfloat{i_part, q_part};
    }

    decoder.feed(noise.data(), noise.size());
    CHECK_EQ(seen, 0);
}

TEST(sonde_decoder_null_and_empty_input_are_no_ops) {
    Decoder decoder;
    decoder.configure_channel(38400.0);
    int seen = 0;
    decoder.set_packet_handler([&](const Packet&) { seen++; });

    decoder.feed(nullptr, 16);
    decoder.feed_channel(nullptr, 16);
    decoder.feed_bits_rs41(nullptr, 16);
    decoder.feed_bits_meteomodem(nullptr, 16);

    const dsp::cfloat one{1.0f, 0.0f};
    decoder.feed(&one, 0);

    CHECK_EQ(seen, 0);
    CHECK_EQ(decoder.samples_fed(), static_cast<uint64_t>(0));
}

/* ===========================================================================
 * Presentation helpers
 * ===========================================================================*/

TEST(sonde_geo_uri_matches_the_upstream_qr_payload) {
    CHECK_STR_EQ(app::sonde::geo_uri(48.85837f, 2.29448f), "geo:48.85837,2.29448");
    /* Exactly representable in a float, so the five decimals are not at the
     * mercy of the binary32 rounding of the literal. */
    CHECK_STR_EQ(app::sonde::geo_uri(-22.5f, -43.25f), "geo:-22.50000,-43.25000");
    CHECK_STR_EQ(app::sonde::geo_uri(0.0f, 0.0f), "geo:0.00000,0.00000");
}

TEST(sonde_symbols_formatted_hexdumps_the_descrambled_rs41_frame) {
    const std::vector<uint8_t> raw_payload = {0x22, 0x96, 0x12, 0xF8};
    const Packet p{packet_from_raw_lsb_first(raw_payload), Packet::Type::Vaisala_RS41_SG};
    CHECK_STR_EQ(p.symbols_formatted().data, "93DF1A60");
}
