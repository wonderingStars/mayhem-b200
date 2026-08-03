/*
 * mayhem-b200 — RDS TX encoder + modulator tests.
 *
 * The load-bearing, hardware-free parts of the RDS transmitter are:
 *   1. the checkword / offset generator (make_block) — the RDS (26,16) shortened
 *      cyclic code, which a receiver relies on to align and error-check blocks;
 *   2. the 0B / 2A / 4A group builders and PSN / RadioText / ClockTime frame
 *      assembly — including the Modified-Julian-Day date arithmetic;
 *   3. the modulator's serial bit stream and the constant-envelope IQ it emits.
 *
 * make_block is checked against an INDEPENDENT polynomial-division reference
 * (remainder of data*x^10 mod g, g = 0x5B9 = x^10+x^8+x^7+x^5+x^4+x^3+1, the
 * documented RDS generator), not against the port's own output, plus pinned
 * numeric vectors. The actual radiated RF needs a USRP B200 and is not covered.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_rds.hpp"  /* app::rds:: encoder + modulator */

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

using namespace mb200test;
namespace rds = app::rds;

namespace {

/* Independent RDS checkword: textbook modulo-2 long division of (data << 10) by
 * the RDS generator polynomial g(x)=0x5B9 (degree 10), then XOR the offset. This
 * shares no code with make_block. */
uint16_t ref_checkword(uint16_t data, uint16_t offset) {
    uint32_t reg = static_cast<uint32_t>(data) << 10;  /* 26-bit dividend */
    for (int p = 25; p >= 10; --p) {
        if (reg & (1u << p)) reg ^= (0x5B9u << (p - 10));
    }
    return static_cast<uint16_t>((reg & 0x3FF) ^ offset);
}

uint32_t ref_block(uint16_t data, uint16_t offset) {
    return (static_cast<uint32_t>(data) << 10) | ref_checkword(data, offset);
}

/* Data field (upper 16 bits) of an assembled block. */
uint16_t data_of(uint32_t block) {
    return static_cast<uint16_t>((block >> 10) & 0xFFFF);
}

}  // namespace

/* ===========================================================================
 * 1. Checkword / offset
 * ===========================================================================*/

TEST(rds_offset_constants_match_standard) {
    /* The five offset words, EN 50067 / IEC 62106. (Copied into locals so the
     * comparison is not a compile-time constant, which MSVC /W4 flags.) */
    uint16_t a = rds::RDS_OFFSET_A, b = rds::RDS_OFFSET_B, c = rds::RDS_OFFSET_C,
             cp = rds::RDS_OFFSET_Cp, d = rds::RDS_OFFSET_D;
    CHECK_EQ(a, 0x0FCu);
    CHECK_EQ(b, 0x198u);
    CHECK_EQ(c, 0x168u);
    CHECK_EQ(cp, 0x350u);
    CHECK_EQ(d, 0x1B4u);
}

TEST(rds_make_block_known_vectors) {
    /* All-zero data: the checkword is just the offset word. */
    CHECK_EQ(rds::make_block(0x0000, rds::RDS_OFFSET_A), 0x0000000FCu);
    CHECK_EQ(rds::make_block(0x0000, rds::RDS_OFFSET_B), 0x000000198u);

    /* data = 1, offset 0: remainder of x^10 mod g == x^8+x^7+x^5+x^4+x^3+1
     * = 0b0110111001 = 0x1B9; block = (1<<10)|0x1B9 = 0x5B9. */
    CHECK_EQ(rds::make_block(0x0001, 0x000), 0x0000005B9u);
    CHECK_EQ(rds::make_block(0x0001, 0x000) & 0x3FFu, 0x1B9u);
}

TEST(rds_make_block_matches_independent_division) {
    const uint16_t offsets[] = {rds::RDS_OFFSET_A, rds::RDS_OFFSET_B, rds::RDS_OFFSET_C,
                                rds::RDS_OFFSET_Cp, rds::RDS_OFFSET_D};
    /* Sweep a wide, structured set of data words against every offset. */
    for (uint32_t d = 0; d < 0x10000u; d += 37) {
        for (uint16_t off : offsets) {
            const uint32_t got = rds::make_block(static_cast<uint16_t>(d), off);
            const uint32_t want = ref_block(static_cast<uint16_t>(d), off);
            CHECK_EQ(got, want);
            /* The data field must survive untouched in the top 16 bits. */
            CHECK_EQ(data_of(got), static_cast<uint16_t>(d));
        }
    }
    /* And the corner cases exactly. */
    CHECK_EQ(rds::make_block(0xFFFF, rds::RDS_OFFSET_D), ref_block(0xFFFF, rds::RDS_OFFSET_D));
    CHECK_EQ(rds::make_block(0x8000, rds::RDS_OFFSET_A), ref_block(0x8000, rds::RDS_OFFSET_A));
}

/* ===========================================================================
 * 2. PSN group assembly (0B groups)
 * ===========================================================================*/

TEST(rds_psn_assembly) {
    rds::RDS_flags flags{};
    flags.PI_code = 0x1234;
    flags.PTY = 5;
    flags.TP = true;
    flags.TA = false;
    flags.MS = true;
    flags.DI = 1;  /* stereo */

    std::vector<rds::RDSGroup> frame;
    rds::gen_PSN(frame, "TEST1234", &flags);

    /* Four 0B groups, two PSN characters per group. */
    CHECK_EQ(frame.size(), 4u);

    const char* chars[4] = {"TE", "ST", "12", "34"};
    for (uint8_t c = 0; c < 4; c++) {
        const auto& g = frame[c];

        /* Block A: PI code, offset A. */
        CHECK_EQ(g.block[0], rds::make_block(0x1234, rds::RDS_OFFSET_A));
        CHECK_EQ(data_of(g.block[0]), 0x1234u);

        /* Block B: 0B type/version + flags + segment. */
        const uint16_t b1 = static_cast<uint16_t>((0x0 << 12) | (1 << 11) | (1 << 10) |
                                                  ((5 & 0x1F) << 5) | (0 << 4) | (1 << 3) |
                                                  (1 << 2) | (c & 3));
        CHECK_EQ(data_of(g.block[1]), b1);
        CHECK_EQ(g.block[1], rds::make_block(b1, rds::RDS_OFFSET_B));

        /* Block C: PI code again, but with the C' (B-group) offset. */
        CHECK_EQ(g.block[2], rds::make_block(0x1234, rds::RDS_OFFSET_Cp));
        CHECK_EQ(data_of(g.block[2]), 0x1234u);

        /* Block D: the two PSN characters, offset D. */
        const uint16_t d = static_cast<uint16_t>((static_cast<uint8_t>(chars[c][0]) << 8) |
                                                 static_cast<uint8_t>(chars[c][1]));
        CHECK_EQ(data_of(g.block[3]), d);
        CHECK_EQ(g.block[3], rds::make_block(d, rds::RDS_OFFSET_D));
    }
}

TEST(rds_psn_short_name_is_space_padded) {
    rds::RDS_flags flags{};
    flags.PI_code = 0xBEEF;
    std::vector<rds::RDSGroup> frame;
    /* Two characters given; the view pads to 8 before calling gen_PSN, but the
     * builder must be safe if handed a short string directly. */
    rds::gen_PSN(frame, "Hi", &flags);
    CHECK_EQ(frame.size(), 4u);
    /* Group 0 carries "Hi"; groups 1..3 carry padding spaces. */
    CHECK_EQ(data_of(frame[0].block[3]),
             static_cast<uint16_t>(('H' << 8) | 'i'));
    CHECK_EQ(data_of(frame[1].block[3]),
             static_cast<uint16_t>((' ' << 8) | ' '));
}

/* ===========================================================================
 * 3. RadioText segmentation (2A groups)
 * ===========================================================================*/

TEST(rds_radiotext_group_counts) {
    rds::RDS_flags flags{};
    flags.PI_code = 0x0F00;

    auto count = [&](const std::string& text) {
        std::vector<rds::RDSGroup> frame;
        rds::gen_RadioText(frame, text, 0, &flags);
        return frame.size();
    };

    /* length rounds up to a multiple of 4 (after the 0x0D terminator), /4 groups. */
    CHECK_EQ(count(""), 1u);       /* "\r"        -> pad 4  -> 1 group  */
    CHECK_EQ(count("ABC"), 1u);    /* "ABC\r"     -> 4      -> 1 group  */
    CHECK_EQ(count("ABCD"), 2u);   /* "ABCD\r"    -> pad 8  -> 2 groups */
    CHECK_EQ(count("ABCDEFG"), 2u);/* "ABCDEFG\r" -> 8      -> 2 groups */
    /* Default demo text: 23 chars + CR = 24 -> 6 groups. */
    CHECK_EQ(count("Radiotext test ABCD1234"), 6u);
}

TEST(rds_radiotext_segments_and_chars) {
    rds::RDS_flags flags{};
    flags.PI_code = 0x0F00;
    flags.PTY = 10;
    flags.TP = true;

    std::vector<rds::RDSGroup> frame;
    rds::gen_RadioText(frame, "ABCDEFGH", 1 /* AB */, &flags);
    /* "ABCDEFGH\r" = 9 -> pad to 12 -> 3 groups. */
    CHECK_EQ(frame.size(), 3u);

    for (uint8_t seg = 0; seg < frame.size(); seg++) {
        const auto& g = frame[seg];
        /* Block A: PI, offset A. */
        CHECK_EQ(g.block[0], rds::make_block(0x0F00, rds::RDS_OFFSET_A));

        /* Block B: 2A type + AB + segment counter. */
        const uint16_t b1 = static_cast<uint16_t>((0x2 << 12) | (0 << 11) | (1 << 10) |
                                                  ((10 & 0x1F) << 5) | (1 << 4) | (seg & 15));
        CHECK_EQ(data_of(g.block[1]), b1);
        CHECK_EQ(static_cast<uint16_t>(data_of(g.block[1]) & 0x0F), seg);
        /* 2A uses the C offset (not C'). */
        CHECK_EQ(g.block[2], rds::make_block(data_of(g.block[2]), rds::RDS_OFFSET_C));
        CHECK_EQ(g.block[3], rds::make_block(data_of(g.block[3]), rds::RDS_OFFSET_D));
    }

    /* Group 0 carries the first four characters, two per block. */
    CHECK_EQ(data_of(frame[0].block[2]), static_cast<uint16_t>(('A' << 8) | 'B'));
    CHECK_EQ(data_of(frame[0].block[3]), static_cast<uint16_t>(('C' << 8) | 'D'));
    CHECK_EQ(data_of(frame[1].block[2]), static_cast<uint16_t>(('E' << 8) | 'F'));
    CHECK_EQ(data_of(frame[1].block[3]), static_cast<uint16_t>(('G' << 8) | 'H'));
    /* Group 2 begins with the 0x0D terminator then space padding. */
    CHECK_EQ(data_of(frame[2].block[2]), static_cast<uint16_t>((0x0D << 8) | ' '));
    CHECK_EQ(data_of(frame[2].block[3]), static_cast<uint16_t>((' ' << 8) | ' '));
}

/* ===========================================================================
 * 4. Clock-time (4A group) + Modified Julian Day
 * ===========================================================================*/

TEST(rds_clocktime_mjd_and_fields) {
    rds::RDS_flags flags{};
    flags.PI_code = 0x2222;
    flags.PTY = 3;
    flags.TP = false;

    std::vector<rds::RDSGroup> frame;
    /* 2016-12-01 09:23, +1h (offset in half-hours = 2). */
    rds::gen_ClockTime(frame, &flags, 2016, 12, 1, 9, 23, 2);
    CHECK_EQ(frame.size(), 1u);
    const auto& g = frame[0];

    /* Block A: PI, offset A; 4A uses A/B/C/D offsets. */
    CHECK_EQ(g.block[0], rds::make_block(0x2222, rds::RDS_OFFSET_A));

    const uint16_t b1 = data_of(g.block[1]);
    const uint16_t b2 = data_of(g.block[2]);
    const uint16_t b3 = data_of(g.block[3]);

    /* Reconstruct the 17-bit Modified Julian Day: 2 high bits in block B[1:0],
     * 15 bits in block C[15:1]. True MJD of 2016-12-01 is 57723. */
    const uint32_t mjd = (static_cast<uint32_t>(b1 & 0x3) << 15) |
                         (static_cast<uint32_t>(b2 >> 1) & 0x7FFF);
    CHECK_EQ(mjd, 57723u);

    /* 4A block B carries type 0x4, version A (bit11=0). */
    CHECK_EQ((b1 >> 12) & 0xF, 0x4u);
    CHECK_EQ((b1 >> 11) & 0x1, 0x0u);

    /* Hour spans block C bit0 (top) + block D[15:12]; minute D[11:6]; offset D[5:0]. */
    const uint32_t hour = ((static_cast<uint32_t>(b2 & 0x1) << 4) | ((b3 >> 12) & 0xF));
    const uint32_t minute = (b3 >> 6) & 0x3F;
    const uint32_t offset = b3 & 0x3F;
    CHECK_EQ(hour, 9u);
    CHECK_EQ(minute, 23u);
    CHECK_EQ(offset, 2u);

    /* Every block checkword/offset is consistent. */
    CHECK_EQ(g.block[1], rds::make_block(b1, rds::RDS_OFFSET_B));
    CHECK_EQ(g.block[2], rds::make_block(b2, rds::RDS_OFFSET_C));
    CHECK_EQ(g.block[3], rds::make_block(b3, rds::RDS_OFFSET_D));
}

TEST(rds_clocktime_jan_feb_leap_adjust) {
    /* January/February shift the year back one for the MJD formula (L=1). Verify
     * against a known MJD: 2000-01-01 = 51544. */
    rds::RDS_flags flags{};
    flags.PI_code = 0x0001;
    std::vector<rds::RDSGroup> frame;
    rds::gen_ClockTime(frame, &flags, 2000, 1, 1, 0, 0, 0);
    const auto& g = frame[0];
    const uint16_t b1 = data_of(g.block[1]);
    const uint16_t b2 = data_of(g.block[2]);
    const uint32_t mjd = (static_cast<uint32_t>(b1 & 0x3) << 15) |
                         (static_cast<uint32_t>(b2 >> 1) & 0x7FFF);
    CHECK_EQ(mjd, 51544u);
}

/* ===========================================================================
 * 5. Modulator — serial bit stream and constant-envelope IQ
 * ===========================================================================*/

TEST(rds_modulator_bit_stream_matches_blocks) {
    rds::RDS_flags flags{};
    flags.PI_code = 0x3E20;
    std::vector<rds::RDSGroup> frame;
    rds::gen_ClockTime(frame, &flags, 2016, 12, 1, 9, 23, 0);

    std::vector<uint32_t> blocks;
    for (const auto& g : frame)
        for (int i = 0; i < 4; i++) blocks.push_back(g.block[i]);

    rds::RdsModulator mod;
    mod.set_blocks(blocks);

    /* 1 group -> 4 blocks -> 104 bits. */
    CHECK_EQ(mod.message_length_bits(), blocks.size() * 26);
    const auto bits = mod.bit_stream();
    CHECK_EQ(bits.size(), blocks.size() * 26u);

    /* Bits are emitted MSB first, 26 per block. */
    size_t idx = 0;
    for (uint32_t block : blocks) {
        for (int k = 25; k >= 0; k--) {
            CHECK_EQ(bits[idx], static_cast<uint8_t>((block >> k) & 1));
            idx++;
        }
    }
    CHECK_EQ(idx, bits.size());
}

TEST(rds_modulator_emits_constant_envelope) {
    int spb = rds::RdsModulator::kSamplesPerBit;
    CHECK_EQ(spb, 192);
    /* 228 kHz inner rate / 192 samples per bit == 1187.5 bps. */
    CHECK_NEAR((rds::RdsModulator::kSampleRate / 10.0) / rds::RdsModulator::kSamplesPerBit,
               1187.5, 1e-6);

    rds::RDS_flags flags{};
    flags.PI_code = 0xF3E0;
    flags.TP = true;
    std::vector<rds::RDSGroup> frame;
    rds::gen_PSN(frame, "TEST1234", &flags);

    std::vector<uint32_t> blocks;
    for (const auto& g : frame)
        for (int i = 0; i < 4; i++) blocks.push_back(g.block[i]);

    rds::RdsModulator mod;
    mod.set_blocks(blocks);

    const size_t n = 8192;
    std::vector<std::complex<float>> iq(n);
    const size_t produced = mod.generate(iq.data(), n);
    CHECK_EQ(produced, n);

    /* FM is constant envelope: every sample sits on the unit circle. */
    bool phase_moved = false;
    for (size_t i = 0; i < n; i++) {
        const float mag = std::abs(iq[i]);
        CHECK_NEAR(mag, 1.0f, 1e-4f);
        if (std::abs(iq[i] - iq[0]) > 1e-3f) phase_moved = true;
    }
    /* A real message modulates the carrier, so the phase must move. */
    CHECK(phase_moved);
}

TEST(rds_modulator_empty_message_is_bare_carrier) {
    rds::RdsModulator mod;
    mod.set_blocks({});
    CHECK_EQ(mod.message_length_bits(), 0u);

    const size_t n = 1024;
    std::vector<std::complex<float>> iq(n);
    CHECK_EQ(mod.generate(iq.data(), n), n);

    /* No data -> the baseband stays zero -> an unmodulated carrier at (1, 0). */
    for (size_t i = 0; i < n; i++) {
        CHECK_NEAR(iq[i].real(), 1.0f, 1e-4f);
        CHECK_NEAR(iq[i].imag(), 0.0f, 1e-4f);
    }
}
