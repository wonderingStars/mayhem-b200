/*
 * mayhem-b200 — FLEX pager transmitter.
 *
 * Host port of the PortaPack firmware app
 *   application/external/flex_tx/ui_flex_tx.*
 * whose encoder is self-contained in that one .cpp (there is no common/flex_*
 * encoder). The whole encoder is ported here, function for function, into
 * namespace app::flex_tx so tests can exercise it directly. Nothing about the
 * protocol — the BCH(31,21) generator 0x769, the bit-reversed FLEX word order,
 * the nibble checksum, the sync patterns, the block interleave, the address
 * ranges, the alpha/numeric packing — is changed from upstream.
 *
 * FLEX (TIA/EIA-STD-43A / Motorola FLEX) on the air (see ui_flex_rx.hpp for the
 * receive-side description):
 *   - This app transmits 2FSK at 1600 sym/s, +/-4.5 kHz, exactly as upstream:
 *     upstream's speed selector only ever offered "1600/2FSK".
 *   - A frame is SYNC1 (bit-reversed mode code, marker 0xA6C6AAAA, complement),
 *     the Frame Information Word (BCH, nibble-checksummed), SYNC2, then 88
 *     BCH(31,21) words carrying BIW1, optional extra BIWs, the address, the
 *     vector and the message, block-interleaved 8 words at a time.
 *   - Each transmission is an optional run of ERS (empty) cycles, then the new
 *     frame, then a duplicate frame, matching upstream's TX sequence.
 *
 * The only host difference is where the byte stream goes: instead of
 * shared_memory.bb_data + baseband::set_fsk_data(), it is built into a
 * std::vector<uint8_t> and 2FSK-modulated by the Phase A dsp::FskKeyer, which
 * reads it MSB-first and maps a 1 bit to positive deviation — the same
 * convention as the firmware's proc_fsk.cpp.
 *
 * Nothing transmits until the user presses Start. A B200 radiating a spoofed
 * FLEX page on a real paging channel is illegal almost everywhere; the view
 * shows a short warning.
 *
 * Copyright (C) 2023-2024 PortaPack Mayhem contributors (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_FLEX_TX_H__
#define __MB200_UI_FLEX_TX_H__

#include "ui.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"
#include "ui_freq_field.hpp"

#include "../dsp/modulate.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace app::flex_tx {

/* ===========================================================================
 * BIW parameters (upstream FlexBIWParams)
 * ===========================================================================*/

struct FlexBIWParams {
    int32_t send_date{1};
    int32_t send_time{1};
    int32_t send_tz{1};
    int32_t send_dst{0};
    int32_t send_ssid1{0};
    int32_t send_ssid2{0};
    int32_t roaming{0};
    int32_t year{2015};
    int32_t month{1};
    int32_t day{1};
    int32_t hour{0};
    int32_t minute{0};
    int32_t second{0};
    int32_t tz_code{0};
    int32_t local_id{0};
    int32_t coverage_zone{0};
    int32_t country_code{0};
    int32_t msg_number{0};
    int32_t send_ers{1};
    int32_t ers_count{1};
};

/* ===========================================================================
 * Encoder — ported verbatim from firmware/application/external/flex_tx.
 * All tested in tests/test_flex_tx.cpp.
 * ===========================================================================*/

constexpr uint32_t kBchPoly = 0x769u;    // BCH(31,21) generator polynomial
constexpr uint32_t kDataMask = 0x1FFFFFu;

/* --- bit reversal --- */
inline uint32_t reverse_bits32(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    v = (v >> 16) | (v << 16);
    return v;
}

/* --- BCH(31,21) + even parity encoder --- */
inline uint32_t flex_encode_word(uint32_t dw) {
    uint32_t data = dw >> 11;
    uint32_t dividend = data << 10;
    for (int i = 30; i >= 10; i--) {
        if ((dividend >> i) & 1u)
            dividend ^= kBchPoly << (i - 10);
    }
    uint32_t ecc = dividend & 0x3FFu;
    uint32_t code31 = (data << 10) | ecc;
    uint32_t p = 0, tmp = code31;
    while (tmp) {
        p ^= (tmp & 1u);
        tmp >>= 1;
    }
    return (code31 << 1) | p;
}

/* FLEX transmits least-significant-bit first, so the 21-bit info field is
 * bit-reversed before the systematic BCH encode. */
inline uint32_t flex_enc(uint32_t data21) {
    return flex_encode_word(reverse_bits32(data21));
}

/* --- 4-bit nibble checksum --- */
inline uint32_t flex_checksum(uint32_t d) {
    uint32_t s = (d & 0xF) + ((d >> 4) & 0xF) + ((d >> 8) & 0xF) +
                 ((d >> 12) & 0xF) + ((d >> 16) & 0xF) + ((d >> 20) & 0x1);
    return (d & ~0xFu) | ((0xF - (s & 0xF)) & 0xF);
}

/* --- year equivalence (FLEX only carries 1994-2025) --- */
inline int flex_is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

inline int flex_jan1_dow(int y) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    return (y + y / 4 - y / 100 + y / 400 + t[0] + 1) % 7;
}

inline int flex_equiv_year(int year) {
    if (year >= 1994 && year <= 2025)
        return year;
    int target_leap = flex_is_leap(year);
    int target_dow = flex_jan1_dow(year);
    for (int y = 2025; y >= 1994; y--) {
        if (flex_is_leap(y) == target_leap && flex_jan1_dow(y) == target_dow)
            return y;
    }
    return 1994 + ((year - 1994) & 0x1F);
}

/* --- Frame Information Word --- */
inline uint32_t flex_fiw(uint32_t cycle, uint32_t frame, uint32_t roaming) {
    uint32_t d = 0;
    d |= (cycle & 0xF) << 4;
    d |= (frame & 0x7F) << 8;
    d |= (roaming & 1u) << 15;
    return flex_enc(flex_checksum(d));
}

/* --- Block Information Words --- */
inline uint32_t flex_biw1(uint32_t astart, uint32_t vstart, uint32_t collapse) {
    uint32_t d = 0;
    d |= ((astart - 1) & 0x03) << 8;
    d |= (vstart & 0x3F) << 10;
    d |= (collapse & 0x07) << 18;
    return flex_enc(flex_checksum(d));
}

inline uint32_t flex_biw_date(uint32_t year_field, uint32_t month, uint32_t day) {
    uint32_t d = 0;
    d |= (1u) << 4;  // type = 001
    d |= (year_field & 0x1F) << 7;
    d |= (day & 0x1F) << 12;
    d |= (month & 0x0F) << 17;
    return flex_enc(flex_checksum(d));
}

inline uint32_t flex_biw_time(uint32_t hour, uint32_t minute, uint32_t second_step) {
    uint32_t d = 0;
    d |= (2u) << 4;  // type = 010
    d |= (hour & 0x1F) << 7;
    d |= (minute & 0x3F) << 12;
    d |= (second_step & 0x07) << 18;
    return flex_enc(flex_checksum(d));
}

inline uint32_t flex_biw_sysinfo(uint32_t a_type, uint32_t info) {
    uint32_t d = 0;
    d |= (5u) << 4;  // type = 101
    d |= (a_type & 0x0F) << 7;
    d |= (info & 0x03FF) << 11;
    return flex_enc(flex_checksum(d));
}

inline uint32_t flex_biw_ssid1(uint32_t local_id, uint32_t coverage_zone) {
    uint32_t d = 0;
    // type = 000 (no bits to set)
    d |= (coverage_zone & 0x1F) << 7;
    d |= (local_id & 0x01FF) << 12;
    return flex_enc(flex_checksum(d));
}

inline uint32_t flex_biw_ssid2(uint32_t country_code, uint32_t tmf) {
    uint32_t d = 0;
    d |= (7u) << 4;  // type = 111
    d |= (tmf & 0x0F) << 7;
    d |= (country_code & 0x03FF) << 11;
    return flex_enc(flex_checksum(d));
}

/* --- addresses --- */
inline uint32_t flex_short_addr(uint64_t capcode) {
    return flex_enc((uint32_t)(capcode + 0x8000) & kDataMask);
}

inline int flex_long_addr(uint64_t capcode, uint32_t out[2]) {
    uint64_t result;
    uint32_t w1, w2;
    if (capcode >= 2101249ULL && capcode <= 1075843072ULL) {
        result = capcode - 2068481ULL;
        w1 = (result % 32768) + 1;
        w2 = 2097151U - (uint32_t)(result / 32768);
    } else if (capcode >= 1075843073ULL && capcode <= 3223326720ULL) {
        result = capcode - 2068481ULL;
        w1 = (result % 32768) + 1;
        w2 = (uint32_t)(result / 32768) + 1933312U;
    } else if (capcode >= 3223326721ULL && capcode <= 4297068542ULL) {
        result = capcode - 2068479ULL;
        w1 = (result % 32768) + 2064383U;
        w2 = (uint32_t)(result / 32768) + 1867776U;
    } else {
        return -1;
    }
    out[0] = flex_enc(w1 & kDataMask);
    out[1] = flex_enc(w2 & kDataMask);
    return 0;
}

/* --- vector words --- */
inline uint32_t flex_alpha_vector(uint32_t mw_start, uint32_t mw_count) {
    uint32_t d = 0;
    d |= (5u & 0x07) << 4;
    d |= (mw_start & 0x7F) << 7;
    d |= (mw_count & 0x7F) << 14;
    return flex_enc(flex_checksum(d));
}

inline uint32_t flex_numeric_vector(uint32_t type, uint32_t mw_start, uint32_t mw_count, uint32_t kbit) {
    uint32_t n_field = (mw_count > 0) ? mw_count - 1 : 0;
    uint32_t d = 0;
    d |= (type & 0x07) << 4;
    d |= (mw_start & 0x7F) << 7;
    d |= (n_field & 0x07) << 14;
    d |= (kbit & 0x0F) << 17;
    return flex_enc(flex_checksum(d));
}

/* Short vector (type 2): BCD digits packed into the vector word (and a second
 * word for long addresses). Empty msg => tone-only (all spaces). */
inline uint32_t flex_short_vector(int is_long, const std::string& msg, uint32_t* vy_out) {
    static const char bcd_chars[20] = "0123456789.U -][";
    auto to_bcd = [&](char c) -> uint8_t {
        for (int k = 0; k < 16; k++)
            if (bcd_chars[k] == c) return (uint8_t)k;
        return 0xC;  // space
    };

    int max_digits = is_long ? 8 : 3;
    uint8_t digits[8];
    for (int i = 0; i < 8; i++)
        digits[i] = (i < (int)msg.size() && i < max_digits) ? to_bcd(msg[i]) : 0xC;

    uint32_t vw = 0;
    vw |= (2u & 0x07) << 4;  // type = 010
    vw |= ((uint32_t)digits[0] & 0xF) << 9;
    vw |= ((uint32_t)digits[1] & 0xF) << 13;
    vw |= ((uint32_t)digits[2] & 0xF) << 17;

    if (is_long && vy_out) {
        uint32_t d2 = 0;
        for (int i = 0; i < 5 && (i + 3) < max_digits; i++)
            d2 |= ((uint32_t)digits[i + 3] & 0xF) << (i * 4);
        *vy_out = flex_enc(d2);
    }
    return flex_enc(flex_checksum(vw));
}

/* --- block interleave: 8 words -> 32 bytes, transmitted MSB(word0) first --- */
inline void flex_interleave_block_to_bytes(const uint32_t* words8, std::vector<uint8_t>& out) {
    for (uint32_t i = 0; i < 32; i++) {
        uint8_t b = (uint8_t)((((words8[0] >> (31 - i)) & 1u) << 7) |
                              (((words8[1] >> (31 - i)) & 1u) << 6) |
                              (((words8[2] >> (31 - i)) & 1u) << 5) |
                              (((words8[3] >> (31 - i)) & 1u) << 4) |
                              (((words8[4] >> (31 - i)) & 1u) << 3) |
                              (((words8[5] >> (31 - i)) & 1u) << 2) |
                              (((words8[6] >> (31 - i)) & 1u) << 1) |
                              (((words8[7] >> (31 - i)) & 1u) << 0));
        out.push_back(b);
    }
}

/* --- alpha encoding (returns word count, fills words[]) --- */
inline int flex_encode_alpha(const std::string& msg, uint32_t* words, int max_words, uint32_t seq, uint32_t msg_r) {
    int wc = 0;
    uint32_t hdr = 0;
    hdr |= (3u << 11);
    hdr |= ((seq & 0x3F) << 13);
    hdr |= ((msg_r & 1u) << 19);
    words[wc++] = hdr;

    size_t ci = 0;
    uint32_t dw = 0;
    uint8_t ch;
    ch = (ci < msg.size()) ? (uint8_t)msg[ci++] : 0x03;
    dw |= ((uint32_t)(ch & 0x7F)) << 7;
    ch = (ci < msg.size()) ? (uint8_t)msg[ci++] : 0x03;
    dw |= ((uint32_t)(ch & 0x7F)) << 14;
    words[wc++] = dw;

    while (ci < msg.size() && wc < max_words) {
        dw = 0;
        for (int s = 0; s < 3; s++) {
            ch = (ci < msg.size()) ? (uint8_t)msg[ci++] : 0x03;
            dw |= ((uint32_t)(ch & 0x7F)) << (s * 7);
        }
        words[wc++] = dw;
    }

    // Signature
    {
        uint32_t sig_sum = 0;
        for (int i = 1; i < wc; i++) {
            uint32_t c0 = (words[i] >> 0) & 0x7F;
            uint32_t c1 = (words[i] >> 7) & 0x7F;
            uint32_t c2 = (words[i] >> 14) & 0x7F;
            if (c0 != 0x03) sig_sum += c0;
            if (c1 != 0x03) sig_sum += c1;
            if (c2 != 0x03) sig_sum += c2;
        }
        words[1] = (words[1] & ~0x7Fu) | ((~sig_sum) & 0x7F);
    }

    // K checksum
    {
        uint32_t k_sum = 0;
        for (int i = 0; i < wc; i++) {
            k_sum += words[i] & 0xFF;
            k_sum += (words[i] >> 8) & 0xFF;
            k_sum += (words[i] >> 16) & 0x1F;
        }
        words[0] |= ((~k_sum) & 0x3FF);
    }

    for (int i = 0; i < wc; i++)
        words[i] = flex_enc(words[i]);

    return wc;
}

/* --- numeric BCD encoding --- */
inline int flex_encode_numeric(const std::string& msg, uint32_t* words, int max_words, uint32_t* k_out) {
    static const char bcd[20] = "0123456789.U -][";
    uint32_t mw[8] = {0};
    int bit = 2;
    int word_idx = 0;
    for (size_t i = 0; i < msg.size(); i++) {
        uint8_t nib = 0;
        for (int k = 0; k < 16; k++)
            if (bcd[k] == msg[i]) {
                nib = (uint8_t)k;
                break;
            }
        for (int b = 0; b < 4; b++) {
            word_idx = bit / 21;
            if (word_idx >= 8) break;
            if (nib & (1 << b))
                mw[word_idx] |= (1u << (bit % 21));
            bit++;
        }
    }
    word_idx = (bit > 0) ? (bit - 1) / 21 : 0;
    {
        int end_bit = (word_idx + 1) * 21;
        while (bit + 4 <= end_bit) {
            for (int b = 0; b < 4; b++) {
                if (0x0C & (1 << b))
                    mw[bit / 21] |= (1u << (bit % 21));
                bit++;
            }
        }
    }
    uint32_t kb = 0;
    for (int i = 0; i <= word_idx; i++) {
        kb += mw[i] & 0xFF;
        kb += (mw[i] >> 8) & 0xFF;
        kb += (mw[i] >> 16) & 0x1F;
    }
    kb &= 0xFF;
    kb = (kb & 0x3F) + (kb >> 6);
    kb = ~kb;
    mw[0] |= ((kb >> 4) & 0x03);
    *k_out = kb & 0x0F;
    int wc = word_idx + 1;
    for (int i = 0; i < wc && i < max_words; i++)
        words[i] = flex_enc(mw[i]);
    return wc;
}

/* --- sync patterns --- */
inline const uint8_t* flex_bs1() { static const uint8_t v[4] = {0xAA, 0xAA, 0xAA, 0xAA}; return v; }
inline const uint8_t* flex_a1() { static const uint8_t v[4] = {0x78, 0xF3, 0x59, 0x39}; return v; }
inline const uint8_t* flex_b_code() { static const uint8_t v[4] = {0x55, 0x55, 0x00, 0x00}; return v; }
inline const uint8_t* flex_ar() { static const uint8_t v[4] = {0xCB, 0x20, 0x59, 0x39}; return v; }

inline void flex_write_word(std::vector<uint8_t>& out, uint32_t w) {
    out.push_back((uint8_t)((w >> 24) & 0xFF));
    out.push_back((uint8_t)((w >> 16) & 0xFF));
    out.push_back((uint8_t)((w >> 8) & 0xFF));
    out.push_back((uint8_t)(w & 0xFF));
}

/* --- one complete frame appended to `out`; returns bytes appended, or 0 on a
 *     bad long address (matching upstream). --- */
inline size_t flex_build_frame(std::vector<uint8_t>& out, uint64_t capcode, int msg_type,
                               const std::string& msg, uint32_t cycle, uint32_t frame,
                               uint32_t msg_r, const FlexBIWParams& bp) {
    const size_t start = out.size();

    // S1 sync
    const uint8_t* a1 = flex_a1();
    out.insert(out.end(), flex_bs1(), flex_bs1() + 4);
    out.insert(out.end(), a1, a1 + 4);
    out.insert(out.end(), flex_b_code(), flex_b_code() + 2);
    for (int i = 0; i < 4; i++) out.push_back((uint8_t)~a1[i]);

    // FIW
    flex_write_word(out, flex_fiw(cycle, frame, bp.roaming));

    // S2 (fixed 40-bit pattern)
    {
        uint64_t bits = 0;
        bits |= (uint64_t)0xA << 36;
        bits |= (uint64_t)0xED84 << 20;
        bits |= (uint64_t)0x5 << 16;
        bits |= (uint64_t)0x127B;
        out.push_back((uint8_t)((bits >> 32) & 0xFF));
        out.push_back((uint8_t)((bits >> 24) & 0xFF));
        out.push_back((uint8_t)((bits >> 16) & 0xFF));
        out.push_back((uint8_t)((bits >> 8) & 0xFF));
        out.push_back((uint8_t)(bits & 0xFF));
    }

    // Extra BIW words (max 3 slots)
    uint32_t extra_biw[4];
    int extra_count = 0;
    if (bp.send_ssid1 && extra_count < 3)
        extra_biw[extra_count++] = flex_biw_ssid1(bp.local_id, bp.coverage_zone);
    if (bp.send_time && extra_count < 3) {
        uint32_t sec_step = (uint32_t)(bp.second * 2 / 15);  // 7.5s steps
        if (sec_step > 7) sec_step = 7;
        extra_biw[extra_count++] = flex_biw_time(bp.hour, bp.minute, sec_step);
    }
    if (bp.send_date && extra_count < 3) {
        uint32_t year_field = (uint32_t)(bp.year - 1994);
        extra_biw[extra_count++] = flex_biw_date(year_field, bp.month, bp.day);
    }
    if (bp.send_tz && extra_count < 3) {
        uint32_t tz_info = (uint32_t)bp.tz_code & 0x1F;
        if (bp.send_dst)
            tz_info |= (0u << 5);  // L0=0 -> DST active
        else
            tz_info |= (1u << 5);  // L0=1 -> standard time
        extra_biw[extra_count++] = flex_biw_sysinfo(0x04, tz_info);
    }
    if (bp.send_ssid2 && extra_count < 3)
        extra_biw[extra_count++] = flex_biw_ssid2(bp.country_code, 0);

    uint32_t fw[88];
    uint32_t mw[84];
    int mwc = 0;
    uint32_t num_k = 0;

    if (msg_type == 0)
        mwc = flex_encode_alpha(msg, mw, 84, bp.msg_number, msg_r);
    else if (msg_type == 1)
        mwc = flex_encode_numeric(msg, mw, 84, &num_k);

    int is_long = (capcode >= 2101249ULL);
    int addr_words = is_long ? 2 : 1;
    int vec_words = is_long ? 2 : 1;
    int astart = 1 + extra_count;
    int vstart = astart + addr_words;
    int mstart = vstart + vec_words;

    fw[0] = flex_biw1(astart, vstart, 0);

    for (int i = 0; i < extra_count; i++)
        fw[1 + i] = extra_biw[i];

    if (is_long) {
        uint32_t la[2];
        if (flex_long_addr(capcode, la) < 0) {
            out.resize(start);  // undo partial frame
            return 0;
        }
        fw[astart] = la[0];
        fw[astart + 1] = la[1];
    } else {
        fw[astart] = flex_short_addr(capcode);
    }

    if (msg_type == 0) {
        fw[vstart] = flex_alpha_vector(mstart, mwc);
        if (is_long && mwc > 0) {
            fw[vstart + 1] = mw[0];
            for (int i = 0; i < mwc - 1 && (mstart + i) < 88; i++)
                fw[mstart + i] = mw[i + 1];
            mwc = (mwc > 0) ? mwc - 1 : 0;
        } else {
            for (int i = 0; i < mwc && (mstart + i) < 88; i++)
                fw[mstart + i] = mw[i];
        }
    } else if (msg_type == 1) {
        fw[vstart] = flex_numeric_vector(3, mstart, mwc, num_k);
        if (is_long) {
            fw[vstart + 1] = (mwc > 0) ? mw[0] : flex_enc(0);
            for (int i = 1; i < mwc && (mstart + i - 1) < 88; i++)
                fw[mstart + i - 1] = mw[i];
        } else {
            for (int i = 0; i < mwc && (mstart + i) < 88; i++)
                fw[mstart + i] = mw[i];
        }
    } else {
        // Type 2 = short/tone (empty msg), Type 3 = short numeric (msg digits)
        std::string short_msg = (msg_type == 3) ? msg : "";
        uint32_t short_vy = 0;
        fw[vstart] = flex_short_vector(is_long, short_msg, &short_vy);
        if (is_long)
            fw[vstart + 1] = short_vy;
    }

    // Idle fill
    int mf_words = mwc;
    if (is_long && mwc > 0 && msg_type == 1)
        mf_words = mwc - 1;
    for (int i = mstart + mf_words; i < 88; i++)
        fw[i] = (i & 1) ? 0x00000000u : 0xFFFFFFFFu;

    for (int blk = 0; blk < 11; blk++)
        flex_interleave_block_to_bytes(fw + blk * 8, out);

    // Trailing idle
    out.push_back(0xAA);
    out.push_back(0xAA);
    out.push_back(0xAA);
    out.push_back(0xAA);

    return out.size() - start;
}

/* --- ERS (empty/idle) cycles appended to `out` --- */
inline size_t flex_build_ers(std::vector<uint8_t>& out, int cycles) {
    const size_t start = out.size();
    const uint8_t* ar = flex_ar();
    uint8_t ar_inv[4];
    for (int i = 0; i < 4; i++) ar_inv[i] = (uint8_t)~ar[i];
    for (int c = 0; c < cycles; c++) {
        out.push_back(0xAA);
        out.push_back(0xAA);
        out.insert(out.end(), ar, ar + 4);
        out.push_back(0x55);
        out.push_back(0x55);
        out.insert(out.end(), ar_inv, ar_inv + 4);
    }
    return out.size() - start;
}

/* Complete transmission: optional ERS run, then the new frame, then a duplicate
 * frame (msg_r 1 then 0), each prefixed by 8 ERS cycles — upstream's TX
 * sequence. Returns false only when a long address is out of range. */
inline bool flex_build_transmission(std::vector<uint8_t>& out, uint64_t capcode,
                                    int msg_type, const std::string& msg,
                                    const FlexBIWParams& bp) {
    const int ers_n = (bp.send_ers && bp.ers_count > 0) ? bp.ers_count : 0;
    for (int i = 0; i < ers_n; i++)
        flex_build_ers(out, 42);

    flex_build_ers(out, 8);
    if (flex_build_frame(out, capcode, msg_type, msg, 0, 0, /*msg_r=*/1, bp) == 0)
        return false;

    flex_build_ers(out, 8);
    if (flex_build_frame(out, capcode, msg_type, msg, 0, 0, /*msg_r=*/0, bp) == 0)
        return false;

    return true;
}

/* Legal capcode range. */
constexpr uint64_t min_capcode = 1;
constexpr uint64_t max_capcode = 4297068542ULL;

/* Firmware's FLEX TX runs at 2.28 Msps, +/-4.5 kHz, 1600 sym/s 2FSK. */
constexpr double sample_rate_hz = 2'280'000.0;
constexpr float deviation_hz = 4500.0f;
constexpr uint32_t symbol_rate = 1600;

/* ===========================================================================
 * FLEX parameter sub-view (upstream FlexParamsView)
 * ===========================================================================*/

class FlexParamsView : public ui::View {
   public:
    explicit FlexParamsView(FlexBIWParams& params);

    std::string title() const override { return "FLEX Params"; }
    void focus() override;

   private:
    void on_roaming_changed(bool v);

    FlexBIWParams& params_;

    ui::Labels labels_{
        {{96, 24}, "-", ui::Color::light_grey()},
        {{120, 24}, "-", ui::Color::light_grey()},
        {{80, 48}, ":", ui::Color::light_grey()},
        {{104, 48}, ":", ui::Color::light_grey()},
    };

    ui::Checkbox check_date_{{0, 24}, 4, "Date", true};
    ui::NumberField field_year_{{64, 24}, 4, {1994, 2099}, 1, '0', true};
    ui::NumberField field_month_{{104, 24}, 2, {1, 12}, 1, '0', true};
    ui::NumberField field_day_{{128, 24}, 2, {1, 31}, 1, '0', true};

    ui::Checkbox check_time_{{0, 48}, 4, "Time", true};
    ui::NumberField field_hour_{{64, 48}, 2, {0, 23}, 1, '0', true};
    ui::NumberField field_minute_{{88, 48}, 2, {0, 59}, 1, '0', true};
    ui::NumberField field_second_{{112, 48}, 2, {0, 59}, 1, '0', true};

    ui::Checkbox check_tz_{{0, 72}, 2, "TZ", true};
    ui::OptionsField options_tz_{
        {48, 72},
        9,
        {{"UTC+0   ", 0}, {"UTC+1   ", 1}, {"UTC+2   ", 2}, {"UTC+3   ", 3},
         {"UTC+4   ", 4}, {"UTC+5   ", 5}, {"UTC+6   ", 6}, {"UTC+7   ", 7},
         {"UTC+8   ", 8}, {"UTC+9   ", 9}, {"UTC+10  ", 10}, {"UTC+11  ", 11},
         {"UTC+12  ", 12}, {"UTC-8   ", 24}, {"UTC-7   ", 25}, {"UTC-6   ", 26},
         {"UTC-5   ", 27}, {"UTC-4   ", 28}, {"UTC-3   ", 29}, {"UTC-2   ", 30},
         {"UTC-1   ", 31}}};
    ui::Checkbox check_dst_{{144, 72}, 3, "DST", true};

    ui::Checkbox check_ssid1_{{0, 104}, 5, "LocID", true};
    ui::NumberField field_local_id_{{72, 104}, 3, {0, 511}, 1, '0'};
    ui::Labels labels_cz_{
        {{104, 104}, "CovZ", ui::Color::light_grey()},
    };
    ui::NumberField field_coverage_{{144, 104}, 2, {0, 31}, 1, '0'};

    ui::Checkbox check_ssid2_{{0, 128}, 11, "CountryCode", true};
    ui::NumberField field_country_{{120, 128}, 4, {0, 1023}, 1, '0'};
    ui::Checkbox check_roaming_{{0, 152}, 4, "Roam", true};

    ui::Labels labels_msg_{
        {{0, 184}, "Msg#:", ui::Color::light_grey()},
    };
    ui::NumberField field_msg_number_{{48, 184}, 2, {0, 63}, 1, '0'};

    ui::Checkbox check_ers_{{0, 208}, 3, "ERS", true};
    ui::NumberField field_ers_count_{{72, 208}, 2, {1, 10}, 1, ' '};

    ui::Button button_save_{{8, 248, 96, 28}, "Save"};
    ui::Button button_cancel_{{128, 248, 96, 28}, "Cancel"};
};

/* ===========================================================================
 * FLEX TX main view
 * ===========================================================================*/

class FlexTXView : public ui::View {
   public:
    FlexTXView();
    ~FlexTXView() override;

    FlexTXView(const FlexTXView&) = delete;
    FlexTXView& operator=(const FlexTXView&) = delete;

    std::string title() const override { return "FLEX TX"; }

    void focus() override;
    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void set_message();
    void update_message_text();
    void update_capcode_info();
    bool start_tx();
    void stop_tx();

    FlexBIWParams biw_params_{};
    std::string message_{"FLEX TEST"};

    dsp::FskKeyer fsk_{};
    std::vector<uint8_t> tx_bytes_{};
    bool transmitting_{false};
    std::atomic<bool> tx_done_{false};
    std::atomic<uint64_t> produced_samples_{0};
    uint64_t total_samples_{0};

    ui::Labels labels_{
        {{4, 24}, "Freq", ui::Color::light_grey()},
        {{4, 48}, "Capcode", ui::Color::light_grey()},
        {{4, 72}, "Speed", ui::Color::light_grey()},
        {{4, 96}, "Type", ui::Color::light_grey()},
        {{4, 136}, "Message", ui::Color::light_grey()},
    };

    ui::FrequencyField field_freq_{{64, 24}};

    ui::SymField field_capcode_{{72, 48}, 10, ui::SymField::Type::Dec};

    ui::OptionsField options_speed_{
        {72, 72},
        10,
        {{"1600/2FSK ", 0}}};

    ui::OptionsField options_type_{
        {72, 96},
        13,
        {{"Alphanumeric ", 0},
         {"Numeric      ", 1},
         {"Short/tone   ", 2},
         {"Short numeric", 3}}};

    ui::Text text_capinfo_{{4, 116, 232, 16}, ""};

    ui::Text text_message_{{4, 152, 232, 16}, ""};
    ui::Text text_message_l2_{{4, 168, 232, 16}, ""};

    ui::Button button_message_{{4, 188, 112, 28}, "Set message"};
    ui::Button button_params_{{124, 188, 112, 28}, "Set params"};

    ui::Button button_tx_{{124, 222, 112, 28}, "Start TX"};

    ui::ProgressBar progressbar_{{4, 256, 232, 12}};

    ui::Text text_warning_{{0, 272, 240, 16}, ""};
    ui::Text text_status_{{0, 288, 240, 16}, ""};
};

}  // namespace app::flex_tx

#endif /*__MB200_UI_FLEX_TX_H__*/
