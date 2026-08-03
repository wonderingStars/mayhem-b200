/*
 * mayhem-b200 — RDS (Radio Data System) transmitter.
 *
 * Ported from the firmware's application/apps/ui_rds.*, its group builders and
 * checkword generator application/protocols/rds.* , and its baseband transmit
 * processor baseband/proc_rds.* . RDS is the low-bit-rate data channel carried
 * on a 57 kHz subcarrier of an FM broadcast signal:
 *
 *   payload (PI/PTY/flags + PSN | RadioText | ClockTime)
 *     -> group builders          (make_0B / make_2A / make_4A, app::rds)
 *     -> 26-bit blocks           16 data bits + 10-bit checkword+offset
 *     -> serial bit stream       1187.5 bps, MSB first
 *     -> differential coding     out = prev_out XOR bit
 *     -> biphase pulse shaping    the 576-tap waveform_biphase filter
 *     -> 57 kHz subcarrier (DSB)  the 0,+1,0,-1 quarter-rate switch
 *     -> FM onto the carrier      -> complex baseband handed to the B200
 *
 * WHAT IS FAITHFUL TO UPSTREAM
 *   - The checkword/offset generator (make_block) is byte-for-byte upstream: it
 *     is the RDS (26,16) shortened cyclic code, remainder of message(x)*x^10 mod
 *     g(x) with g(x)=x^10+x^8+x^7+x^5+x^4+x^3+1 (0x5B9), the offset word XORed in.
 *   - The 0B / 2A / 4A group builders, the PSN/RadioText/ClockTime frame
 *     assembly and the Modified-Julian-Day date arithmetic are ported exactly.
 *   - The modulator reproduces proc_rds.cpp's pipeline: the same 576-tap
 *     biphase waveform table, SAMPLES_PER_BIT=192 at a 228 kHz inner rate
 *     (228000/192 = 1187.5 bps), the same differential coding, the same 57 kHz
 *     quarter-rate subcarrier switch, and a x10 hold up to 2.28 Msps.
 *
 * DOCUMENTED DEVIATIONS (host, all noted where they occur)
 *   - The final FM stage is computed in float (cos/sin of an accumulated phase)
 *     rather than through the M4's int8 256-entry sine table and >>16 deviation
 *     truncation. The deviation scale is kept equal to upstream's
 *     (386760 / 65536 phase-units per baseband unit, one NCO turn = 2^26 units);
 *     only the sine-table and truncation quantisation — a cost of the hardware,
 *     not of RDS — are dropped, per the modulate.hpp philosophy.
 *   - The group builders pad a short PSN half-group / RadioText quarter-group
 *     with spaces instead of reading past the end of the std::string as upstream
 *     does (that is a segfault on a host, undefined behaviour on the M4). For a
 *     full 8-char PSN and a 4-aligned RadioText — the tested paths — the output
 *     is identical to upstream.
 *   - Upstream's RDSThread re-sends one frame's ~4 groups then sleeps 1 s; the
 *     host concatenates every enabled frame and repeats it continuously, which
 *     is the higher-duty behaviour a real RDS encoder uses.
 *
 * LEGALITY: transmitting RDS means transmitting on the FM broadcast band, which
 * is licensed spectrum almost everywhere. The view shows a warning and never
 * transmits until the user presses Start. No hardware is attached here, so the
 * radiated signal is unverified; the encoder and modulator logic are tested.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (original)
 * Copyright (C) 2016 Furrtek (original)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_RDS_H__
#define __MB200_UI_RDS_H__

#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace app {
namespace rds {

/* ===========================================================================
 * Encoder — application/protocols/rds.* ported to a self-contained namespace.
 * ===========================================================================*/

/* Offset words (application/protocols/rds.hpp). Added (XOR) to the checkword so
 * a receiver can identify which of the four blocks in a group it is looking at.
 * A' (Cp) is the block-C offset used by version-B groups. */
constexpr uint16_t RDS_OFFSET_A = 0b0011111100;   /* 0x0FC */
constexpr uint16_t RDS_OFFSET_B = 0b0110011000;   /* 0x198 */
constexpr uint16_t RDS_OFFSET_C = 0b0101101000;   /* 0x168 */
constexpr uint16_t RDS_OFFSET_Cp = 0b1101010000;  /* 0x350, block C in B-groups */
constexpr uint16_t RDS_OFFSET_D = 0b0110110100;   /* 0x1B4 */

struct RDS_flags {
    uint16_t PI_code{0};
    uint8_t PTY{0};
    uint8_t DI{0};
    bool TP{false};
    bool TA{false};
    bool MS{false};
};

/* One group is four 26-bit blocks; each block[] value already carries its
 * 16 data bits in [25:10] and its 10-bit checkword+offset in [9:0]. */
struct RDSGroup {
    uint32_t block[4];
};

inline uint8_t b2b(const bool in) {
    return in ? 1 : 0;
}

/* make_block — application/protocols/rds.cpp, byte-for-byte.
 *
 * Computes the 10-bit checkword: the remainder of (data << 10) divided by the
 * RDS generator polynomial g(x)=x^10+x^8+x^7+x^5+x^4+x^3+1 (0x5B9), then XORs
 * in the offset word. Returns the full 26-bit block (data in the high 16 bits,
 * checkword+offset in the low 10). */
inline uint32_t make_block(uint32_t data, uint16_t offset) {
    uint16_t CRC = 0;
    uint8_t bit;

    for (uint8_t i = 0; i < 16; i++) {
        bit = (((data << i) & 0x8000) >> 15) ^ (CRC >> 9);
        if (bit) CRC ^= 0b0011011100;
        CRC = ((CRC << 1) | bit) & 0x3FF;
    }

    return (data << 10) | (CRC ^ offset);
}

/* Type 0B group: program-service name, without the alternative-frequency data
 * that a 0A group carries. `chars` supplies the two PSN characters for segment
 * `C`; a short string is space-padded (host safety — upstream indexes past the
 * end). */
inline RDSGroup make_0B_group(const uint16_t PI_code, const bool TP, const uint8_t PTY,
                              const bool TA, const bool MS, const bool DI, const uint8_t C,
                              const std::string& chars) {
    RDSGroup group;
    const uint8_t c0 = static_cast<uint8_t>(chars.size() > 0 ? chars[0] : ' ');
    const uint8_t c1 = static_cast<uint8_t>(chars.size() > 1 ? chars[1] : ' ');

    group.block[0] = PI_code;
    group.block[1] = (0x0 << 12) | (1 << 11) | (b2b(TP) << 10) | ((PTY & 0x1F) << 5) |
                     (b2b(TA) << 4) | (b2b(MS) << 3) | (b2b(DI) << 2) | (C & 3);
    group.block[2] = PI_code;
    group.block[3] = (c0 << 8) | c1;

    return group;
}

/* Type 2A group: RadioText, four characters per group (up to 64 characters of
 * text across 16 groups). A short quarter-group is space-padded. */
inline RDSGroup make_2A_group(const uint16_t PI_code, const bool TP, const uint8_t PTY,
                              const bool AB, const uint8_t segment, const std::string& chars) {
    RDSGroup group;
    const uint8_t c0 = static_cast<uint8_t>(chars.size() > 0 ? chars[0] : ' ');
    const uint8_t c1 = static_cast<uint8_t>(chars.size() > 1 ? chars[1] : ' ');
    const uint8_t c2 = static_cast<uint8_t>(chars.size() > 2 ? chars[2] : ' ');
    const uint8_t c3 = static_cast<uint8_t>(chars.size() > 3 ? chars[3] : ' ');

    group.block[0] = PI_code;
    group.block[1] = (0x2 << 12) | (0 << 11) | (b2b(TP) << 10) | ((PTY & 0x1F) << 5) |
                     (b2b(AB) << 4) | (segment & 15);
    group.block[2] = (c0 << 8) | c1;
    group.block[3] = (c2 << 8) | c3;

    return group;
}

/* Type 4A group: clock-time and date. Month 1..12, day 1..31, hour/minute
 * 0..59, local_offset in half-hours from UTC (-12..+12). The date is carried as
 * a Modified Julian Day; the arithmetic is upstream's, verified to give
 * MJD 57723 for 2016-12-01. */
inline RDSGroup make_4A_group(const uint16_t PI_code, const bool TP, const uint8_t PTY,
                              const uint16_t year, const uint8_t month, const uint8_t day,
                              const uint8_t hour, const uint8_t minute, const int8_t local_offset) {
    RDSGroup group;
    uint32_t L = 0;
    uint32_t day_code;

    if ((month == 1) || (month == 2)) L = 1;

    day_code = 14956 + day + (uint32_t)((float)(year - 1900 - L) * 365.25) +
               uint16_t((float)((month + 1) + L * 12) * 30.6001);

    group.block[0] = PI_code;
    group.block[1] = (0x4 << 12) | (0 << 11) | (b2b(TP) << 10) | ((PTY & 0x1F) << 5) |
                     ((day_code & 0x18000) >> 15);
    group.block[2] = ((day_code & 0x7FFF) << 1) | (hour >> 4);
    group.block[3] = ((hour & 15) << 12) | ((minute & 0x3F) << 6) | (local_offset & 0x3F);

    return group;
}

/* Program Service Name: four 0B groups carrying two characters each (8-char
 * PSN). Each block gets its checkword; block C uses the C' offset (B-group). */
inline void gen_PSN(std::vector<RDSGroup>& frame, const std::string& psname,
                    const RDS_flags* rds_flags) {
    RDSGroup group;
    frame.clear();

    /* PSN is 8 characters (four 0B groups of two). A shorter name is space-padded
     * so substr() never runs past the end (host safety — upstream assumes 8). A
     * full 8-char name is left byte-identical to upstream. */
    std::string name = psname;
    if (name.size() < 8) name.resize(8, ' ');

    for (uint8_t c = 0; c < 4; c++) {
        group = make_0B_group(rds_flags->PI_code, rds_flags->TP, rds_flags->PTY, rds_flags->TA,
                              rds_flags->MS, rds_flags->DI, c, name.substr(c * 2, 2));
        group.block[0] = make_block(group.block[0], RDS_OFFSET_A);
        group.block[1] = make_block(group.block[1], RDS_OFFSET_B);
        group.block[2] = make_block(group.block[2], RDS_OFFSET_Cp);  /* C' */
        group.block[3] = make_block(group.block[3], RDS_OFFSET_D);
        frame.emplace_back(group);
    }
}

/* RadioText: the text plus a 0x0D terminator, segmented four characters per 2A
 * group. The group count rounds the length up to a multiple of four. */
inline void gen_RadioText(std::vector<RDSGroup>& frame, const std::string& text, const bool AB,
                          const RDS_flags* rds_flags) {
    std::string radiotext_buffer = text;
    radiotext_buffer += 0x0D;

    size_t rt_length = radiotext_buffer.length();
    rt_length = (rt_length + 3) & 0xFC;
    const size_t group_count = rt_length >> 2;  /* 4 characters per group */

    frame.clear();

    for (size_t c = 0; c < group_count; c++) {
        RDSGroup group = make_2A_group(rds_flags->PI_code, rds_flags->TP, rds_flags->PTY, AB,
                                       static_cast<uint8_t>(c), radiotext_buffer.substr(c * 4, 4));
        group.block[0] = make_block(group.block[0], RDS_OFFSET_A);
        group.block[1] = make_block(group.block[1], RDS_OFFSET_B);
        group.block[2] = make_block(group.block[2], RDS_OFFSET_C);
        group.block[3] = make_block(group.block[3], RDS_OFFSET_D);
        frame.emplace_back(group);
    }
}

/* Clock-time: a single 4A group. */
inline void gen_ClockTime(std::vector<RDSGroup>& frame, const RDS_flags* rds_flags,
                          const uint16_t year, const uint8_t month, const uint8_t day,
                          const uint8_t hour, const uint8_t minute, const int8_t local_offset) {
    RDSGroup group = make_4A_group(rds_flags->PI_code, rds_flags->TP, rds_flags->PTY, year, month,
                                   day, hour, minute, local_offset);

    group.block[0] = make_block(group.block[0], RDS_OFFSET_A);
    group.block[1] = make_block(group.block[1], RDS_OFFSET_B);
    group.block[2] = make_block(group.block[2], RDS_OFFSET_C);
    group.block[3] = make_block(group.block[3], RDS_OFFSET_D);

    frame.clear();
    frame.emplace_back(group);
}

/* ===========================================================================
 * Modulator — baseband/proc_rds.cpp ported to a streaming IqSource.
 * ===========================================================================*/

class RdsModulator {
   public:
    /* proc_rds constants. 228000/192 = 1187.5 bps; the biphase filter spans
     * three bit periods (3 * 192 = 576). */
    static constexpr int kSamplesPerBit = 192;
    static constexpr int kFilterSize = 576;
    static constexpr int kSampleBufferSize = kSamplesPerBit + kFilterSize;  /* 768 */
    /* The B200 stream rate; upstream's BasebandThread runs proc_rds at 2.28 Msps
     * and gates the inner biphase generator to one-tenth of it (228 kHz). */
    static constexpr double kSampleRate = 2'280'000.0;

    RdsModulator() { reset(); }

    /* Replaces the transmitted data with the flattened 26-bit blocks of every
     * enabled frame. Each value carries a full block in its low 26 bits, MSB is
     * bit 25 — the order proc_rds reads shared_memory.bb_data. */
    void set_blocks(std::vector<uint32_t> blocks) {
        blocks_ = std::move(blocks);
        reset();
    }

    const std::vector<uint32_t>& blocks() const { return blocks_; }
    size_t message_length_bits() const { return blocks_.size() * 26; }

    void reset() {
        for (int i = 0; i < kSampleBufferSize; i++) sample_buffer_[i] = 0;
        mphase_ = 0;
        s_ = 0;
        bit_pos_ = 0;
        prev_output_ = 0;
        cur_output_ = 0;
        cur_bit_ = 0;
        sample_count_ = kSamplesPerBit;  /* force a new bit on the first tick */
        in_sample_index_ = 0;
        out_sample_index_ = kSampleBufferSize - 1;
        sample_ = 0;
        phase_ = 0.0;
    }

    /* The raw serial bit stream (before differential coding), MSB first, exactly
     * as proc_rds slices the blocks. Deterministic — this is the symbol-level
     * output the tests pin. */
    std::vector<uint8_t> bit_stream() const {
        std::vector<uint8_t> bits;
        bits.reserve(blocks_.size() * 26);
        for (uint32_t block : blocks_)
            for (int k = 25; k >= 0; k--) bits.push_back(static_cast<uint8_t>((block >> k) & 1));
        return bits;
    }

    /* IqSource: fills `count` complex samples at kSampleRate and returns how many
     * were written (always `count`; an empty message transmits a bare carrier).
     * Reproduces proc_rds.cpp::execute() with the final FM stage in float. */
    size_t generate(std::complex<float>* out, size_t count) {
        const size_t message_length = message_length_bits();

        for (size_t i = 0; i < count; i++) {
            if (s_ >= 9) {
                s_ = 0;
                if (sample_count_ >= kSamplesPerBit) {
                    if (message_length == 0) {
                        cur_output_ = 0;
                        sample_count_ = 0;
                    } else {
                        if (bit_pos_ >= message_length) {
                            bit_pos_ = 0;
                            cur_output_ = 0;
                        }

                        cur_bit_ = static_cast<uint8_t>(
                            (blocks_[(bit_pos_ / 26) & 127] >> (25 - (bit_pos_ % 26))) & 1);
                        prev_output_ = cur_output_;
                        cur_output_ = prev_output_ ^ cur_bit_;

                        int idx = in_sample_index_;
                        for (int j = 0; j < kFilterSize; j++) {
                            int32_t val = waveform_biphase[j];
                            if (cur_output_) val = -val;
                            sample_buffer_[idx++] += val;
                            if (idx >= kSampleBufferSize) idx = 0;
                        }

                        in_sample_index_ += kSamplesPerBit;
                        if (in_sample_index_ >= kSampleBufferSize)
                            in_sample_index_ -= kSampleBufferSize;

                        bit_pos_++;
                        sample_count_ = 0;
                    }
                }

                sample_ = sample_buffer_[out_sample_index_];
                sample_buffer_[out_sample_index_] = 0;
                out_sample_index_++;
                if (out_sample_index_ >= kSampleBufferSize) out_sample_index_ = 0;

                /* 57 kHz DSB subcarrier: 228k/4 quarter-rate 0, +s, 0, -s. */
                switch (mphase_ & 3) {
                    case 0:
                    case 2:
                        sample_ = 0;
                        break;
                    case 1:
                        break;
                    case 3:
                        sample_ = -sample_;
                        break;
                }
                mphase_++;
                sample_count_++;
            } else {
                s_++;
            }

            /* FM. Upstream: delta = (sample>>16)*386760, phase (uint32) += delta,
             * one NCO turn = 2^26 phase units. Kept as a float phase to drop the
             * sine-table and >>16 quantisation while preserving the deviation. */
            phase_ += static_cast<double>(sample_) * kFmRadPerUnit;
            if (phase_ >= k2pi) phase_ -= k2pi;
            else if (phase_ < 0.0) phase_ += k2pi;

            out[i] = std::complex<float>(static_cast<float>(std::cos(phase_)),
                                         static_cast<float>(std::sin(phase_)));
        }
        return count;
    }

   private:
    static constexpr double k2pi = 6.283185307179586476925286766559;
    /* 2*pi * (386760 / 65536) / 2^26 — radians of phase per unit of baseband
     * sample, equal to upstream's integer NCO scaling. */
    static constexpr double kFmRadPerUnit = k2pi * 386760.0 / 65536.0 / 67108864.0;

    std::vector<uint32_t> blocks_{};

    int32_t sample_buffer_[kSampleBufferSize]{};
    uint8_t mphase_{0};
    uint8_t s_{0};
    uint32_t bit_pos_{0};
    uint8_t prev_output_{0};
    uint8_t cur_output_{0};
    uint8_t cur_bit_{0};
    int sample_count_{kSamplesPerBit};
    int in_sample_index_{0};
    int out_sample_index_{kSampleBufferSize - 1};
    int32_t sample_{0};
    double phase_{0.0};

    /* The 576-tap biphase waveform table, verbatim from proc_rds.hpp. */
    static constexpr int32_t waveform_biphase[576] = {
        165, 167, 168, 168, 167, 166, 163, 160, 157, 152, 147, 141, 134, 126, 118, 109,
        99, 88, 77, 66, 53, 41, 27, 14, 0, -14, -29, -44, -59, -74, -89, -105,
        -120, -135, -150, -165, -179, -193, -206, -218, -231, -242, -252, -262, -271, -279, -286, -291,
        -296, -299, -301, -302, -302, -300, -297, -292, -286, -278, -269, -259, -247, -233, -219, -202,
        -185, -166, -145, -124, -101, -77, -52, -26, 0, 27, 56, 85, 114, 144, 175, 205,
        236, 266, 296, 326, 356, 384, 412, 439, 465, 490, 513, 535, 555, 574, 590, 604,
        616, 626, 633, 637, 639, 638, 633, 626, 616, 602, 586, 565, 542, 515, 485, 451,
        414, 373, 329, 282, 232, 178, 121, 62, 0, -65, -132, -202, -274, -347, -423, -500,
        -578, -656, -736, -815, -894, -973, -1051, -1128, -1203, -1276, -1347, -1415, -1479, -1540, -1596, -1648,
        -1695, -1736, -1771, -1799, -1820, -1833, -1838, -1835, -1822, -1800, -1767, -1724, -1670, -1605, -1527, -1437,
        -1334, -1217, -1087, -943, -785, -611, -423, -219, 0, 235, 487, 755, 1040, 1341, 1659, 1994,
        2346, 2715, 3101, 3504, 3923, 4359, 4811, 5280, 5764, 6264, 6780, 7310, 7856, 8415, 8987, 9573,
        10172, 10782, 11404, 12036, 12678, 13329, 13989, 14656, 15330, 16009, 16694, 17382, 18074, 18767, 19461, 20155,
        20848, 21539, 22226, 22909, 23586, 24256, 24918, 25571, 26214, 26845, 27464, 28068, 28658, 29231, 29787, 30325,
        30842, 31339, 31814, 32266, 32694, 33097, 33473, 33823, 34144, 34437, 34699, 34931, 35131, 35299, 35434, 35535,
        35602, 35634, 35630, 35591, 35515, 35402, 35252, 35065, 34841, 34579, 34279, 33941, 33566, 33153, 32702, 32214,
        31689, 31128, 30530, 29897, 29228, 28525, 27788, 27017, 26214, 25379, 24513, 23617, 22693, 21740, 20761, 19755,
        18725, 17672, 16597, 15501, 14385, 13251, 12101, 10935, 9755, 8563, 7360, 6148, 4927, 3701, 2470, 1235,
        0, -1235, -2470, -3701, -4927, -6148, -7360, -8563, -9755, -10935, -12101, -13251, -14385, -15501, -16597, -17672,
        -18725, -19755, -20761, -21740, -22693, -23617, -24513, -25379, -26214, -27017, -27788, -28525, -29228, -29897, -30530, -31128,
        -31689, -32214, -32702, -33153, -33566, -33941, -34279, -34579, -34841, -35065, -35252, -35402, -35515, -35591, -35630, -35634,
        -35602, -35535, -35434, -35299, -35131, -34931, -34699, -34437, -34144, -33823, -33473, -33097, -32694, -32266, -31814, -31339,
        -30842, -30325, -29787, -29231, -28658, -28068, -27464, -26845, -26214, -25571, -24918, -24256, -23586, -22909, -22226, -21539,
        -20848, -20155, -19461, -18767, -18074, -17382, -16694, -16009, -15330, -14656, -13989, -13329, -12678, -12036, -11404, -10782,
        -10172, -9573, -8987, -8415, -7856, -7310, -6780, -6264, -5764, -5280, -4811, -4359, -3923, -3504, -3101, -2715,
        -2346, -1994, -1659, -1341, -1040, -755, -487, -235, 0, 219, 423, 611, 785, 943, 1087, 1217,
        1334, 1437, 1527, 1605, 1670, 1724, 1767, 1800, 1822, 1835, 1838, 1833, 1820, 1799, 1771, 1736,
        1695, 1648, 1596, 1540, 1479, 1415, 1347, 1276, 1203, 1128, 1051, 973, 894, 815, 736, 656,
        578, 500, 423, 347, 274, 202, 132, 65, 0, -62, -121, -178, -232, -282, -329, -373,
        -414, -451, -485, -515, -542, -565, -586, -602, -616, -626, -633, -638, -639, -637, -633, -626,
        -616, -604, -590, -574, -555, -535, -513, -490, -465, -439, -412, -384, -356, -326, -296, -266,
        -236, -205, -175, -144, -114, -85, -56, -27, 0, 26, 52, 77, 101, 124, 145, 166,
        185, 202, 219, 233, 247, 259, 269, 278, 286, 292, 297, 300, 302, 302, 301, 299,
        296, 291, 286, 279, 271, 262, 252, 242, 231, 218, 206, 193, 179, 165, 150, 135,
        120, 105, 89, 74, 59, 44, 29, 14, 0, -14, -27, -41, -53, -66, -77, -88,
        -99, -109, -118, -126, -134, -141, -147, -152, -157, -160, -163, -166, -167, -168, -168, -167};
};

}  // namespace rds

/* ===========================================================================
 * View
 * ===========================================================================*/

class RdsView : public ui::View {
   public:
    RdsView();
    ~RdsView() override;

    RdsView(const RdsView&) = delete;
    RdsView& operator=(const RdsView&) = delete;

    std::string title() const override { return "RDS TX"; }

    void focus() override;
    void on_hide() override;

   private:
    void start_tx();
    void stop_tx();
    void set_transmitting(bool on);
    void rebuild_frames();
    std::vector<uint32_t> flatten_enabled_frames();
    void set_status(std::string_view text);

    rds::RDS_flags flags_{};
    rds::RdsModulator modulator_{};

    std::vector<rds::RDSGroup> frame_psn_{};
    std::vector<rds::RDSGroup> frame_radiotext_{};
    std::vector<rds::RDSGroup> frame_datetime_{};

    std::string psn_{"TEST1234"};
    std::string radiotext_{"Radiotext test ABCD1234"};
    std::string name_buffer_{};

    bool transmitting_{false};

    /* --- Widgets --- */
    ui::Labels labels_{
        {{0, 0}, "Freq", ui::Color::light_grey()},
        {{18 * 8, 0}, "Gain", ui::Color::light_grey()},
        {{0, 7 * 8}, "PI", ui::Color::light_grey()},
        {{12 * 8, 7 * 8}, "PTY", ui::Color::light_grey()},
    };

    ui::Labels warning_{
        {{0, 18}, "Licensed FM band - illegal to", ui::Color::red()},
        {{0, 32}, "transmit without a licence.", ui::Color::red()},
    };

    ui::FrequencyField field_frequency_{{5 * 8, 0}};
    ui::NumberField field_gain_{{23 * 8, 0}, 3, {0, 89}, 1, ' '};

    ui::SymField sym_pi_code_{{3 * 8, 7 * 8}, 4, ui::SymField::Type::Hex};
    ui::OptionsField options_pty_{
        {16 * 8, 7 * 8},
        8,
        {{"None", 0},     {"News", 1},     {"Affairs", 2},  {"Info", 3},     {"Sport", 4},
         {"Educate", 5},  {"Drama", 6},    {"Culture", 7},  {"Science", 8},  {"Varied", 9},
         {"Pop", 10},     {"Rock", 11},    {"Easy", 12},    {"Light", 13},   {"Classics", 14},
         {"Other", 15},   {"Weather", 16}, {"Finance", 17}, {"Children", 18},{"Social", 19},
         {"Religion", 20},{"PhoneIn", 21}, {"Travel", 22},  {"Leisure", 23}, {"Jazz", 24},
         {"Country", 25}, {"National", 26},{"Oldies", 27},  {"Folk", 28},    {"Docs", 29},
         {"AlarmTst", 30},{"Alarm", 31}}};

    ui::Checkbox check_tp_{{0, 10 * 8}, 2, "TP"};
    ui::Checkbox check_ta_{{7 * 8, 10 * 8}, 2, "TA"};
    ui::Checkbox check_ms_{{14 * 8, 10 * 8}, 3, "Mus"};
    ui::Checkbox check_stereo_{{21 * 8, 10 * 8}, 2, "St"};

    ui::Checkbox check_psn_{{0, 13 * 8}, 4, "Name"};
    ui::Text text_psn_{{7 * 8, 13 * 8, 8 * 8, 16}, ""};
    ui::Button button_psn_{{18 * 8, 13 * 8 - 4, 80, 24}, "Set"};

    ui::Checkbox check_rt_{{0, 17 * 8}, 4, "Text"};
    ui::Button button_rt_{{18 * 8, 17 * 8 - 4, 80, 24}, "Set"};
    ui::Text text_rt_{{0, 20 * 8, 30 * 8, 16}, ""};

    ui::Checkbox check_ct_{{0, 23 * 8}, 20, "Time (system clock)"};

    ui::Button button_tx_{{0, 26 * 8, 15 * 8, 32}, "Start TX"};
    ui::Text text_status_{{0, 31 * 8, 30 * 8, 16}, ""};
};

}  // namespace app

#endif /*__MB200_UI_RDS_H__*/
