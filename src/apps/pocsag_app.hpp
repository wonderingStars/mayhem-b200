/*
 * mayhem-b200 — POCSAG receiver.
 *
 * Ported from the PortaPack firmware:
 *
 *   application/apps/pocsag_app.*  -> the view, the settings page, the console
 *   baseband/proc_pocsag2.*        -> AudioNormalizer, BitQueue, BitExtractor,
 *                                     CodewordExtractor (the whole M4 half)
 *   common/pocsag.*                -> EccContainer (BCH(31,21)+parity), the
 *                                     batch decoder, the alpha/numeric
 *                                     heuristic, pocsag_encode
 *   common/pocsag_packet.hpp       -> POCSAGPacket
 *
 * POCSAG (CCIR Radiopaging Code No. 1) on the air:
 *
 *   - 2FSK, +/-4.5 kHz, at 512, 1200 or 2400 bps.
 *   - A preamble of at least 576 alternating bits (0xAAAA...), which is what
 *     the receiver locks its bit clock to.
 *   - Then batches. Each batch is the 32-bit frame synchronisation codeword
 *     0x7CD215D8 followed by 16 codewords, i.e. 8 frames of 2.
 *   - Every codeword is 32 bits: 1 flag bit (0 = address, 1 = message),
 *     21 - 1 = 20 payload bits, 10 BCH(31,21) parity bits and 1 even parity
 *     bit over the whole word. The BCH code corrects up to 2 bit errors.
 *   - An address codeword carries bits 21..3 of the RIC plus a 2-bit function;
 *     the low 3 bits of the RIC are the *frame number*, i.e. which of the 8
 *     frames the codeword arrived in, so they come from the codeword's
 *     position in the batch, not from the codeword itself.
 *   - Message codewords carry 20 bits of payload each: 7-bit ASCII, each
 *     character transmitted least-significant-bit first, packed across
 *     codeword boundaries; or 4-bit BCD digits for numeric pages.
 *
 * Host pipeline, replacing the M4 baseband processor. The stages are the same
 * and in the same order as proc_pocsag2's execute():
 *
 *   USRP IQ -> NCO (LO offset) -> 2-stage decimating FIR -> FM discriminator
 *           -> 1800 Hz Butterworth LPF -> AudioNormalizer (slice to +/-1)
 *           -> BitExtractor (2x-oversampled bit clock, 512/1200/2400)
 *           -> CodewordExtractor (sync search, 16-codeword batches)
 *           -> EccContainer (BCH correct) -> pocsag_decode_batch -> console
 *
 * Two host-specific departures, both marked at the point of use:
 *
 *   1. Upstream's audio rate is a fixed 24 kHz and its 1800 Hz lowpass is a
 *      hard-coded biquad for that rate. The B200's capture rate is whatever
 *      the user picked, so the decimation is chosen at configure() time and
 *      the biquad is designed for the rate that actually results.
 *      design_butterworth_lowpass(1800, 24000) reproduces upstream's
 *      coefficients to six digits; tests/test_pocsag.cpp asserts that.
 *   2. Upstream inserts BCH parity with common/bch_code.cpp's polynomial
 *      encoder. EccContainer::encode() here derives the same 10 parity bits
 *      from the syndrome table the decoder already builds, which is smaller
 *      and provably consistent with the decoder. It reproduces both the sync
 *      codeword and the idle codeword exactly; tests assert that too.
 *
 * Copyright (C) 1996 Thomas Sailer (multimon lineage of the ECC tables)
 * Copyright (C) 2012-2014 Elias Oenal (multimon-ng)
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2023 Kyle Reed
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_POCSAG_APP_H__
#define __MB200_POCSAG_APP_H__

#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace radio {
class ReceiverModel;
}

namespace pocsag {

/* ===========================================================================
 * Protocol constants (common/pocsag.hpp)
 * ===========================================================================*/

constexpr size_t preamble_length = 576;
constexpr uint32_t syncword = 0x7CD215D8u;
constexpr uint32_t idleword = 0x7A89C197u;
constexpr uint8_t batch_size = 16;

using batch_t = std::array<uint32_t, batch_size>;
using Timestamp = std::chrono::system_clock::time_point;

enum BitRate : uint32_t {
    RATE_UNKNOWN = 0,
    FSK512 = 512,
    FSK1200 = 1200,
    FSK2400 = 2400
};

enum PacketFlag : uint32_t {
    FLAG_NORMAL,
    FLAG_TIMED_OUT,
    FLAG_TOO_LONG
};

enum Mode : uint32_t {
    STATE_CLEAR,
    STATE_HAVE_ADDRESS,
    STATE_GETTING_MSG
};

enum OutputType : uint32_t {
    OUT_EMPTY,
    OUT_IDLE,
    OUT_ADDRESS,
    OUT_MESSAGE
};

enum MessageType : uint32_t {
    ADDRESS_ONLY,
    NUMERIC_ONLY,
    ALPHANUMERIC
};

/* Detected message type from the heuristic scoring below. */
enum DetectedType : uint8_t {
    DET_UNKNOWN,
    DET_TONE,
    DET_ALPHA,
    DET_NUMERIC
};

/* Max numeric nibbles per batch: 16 codewords * 5 nibbles. */
constexpr uint8_t max_batch_nibbles = 80;

inline std::string bitrate_str(uint16_t bitrate) {
    switch (bitrate) {
        case FSK512:
            return "512bps ";
        case FSK1200:
            return "1200bps";
        case FSK2400:
            return "2400bps";
        default:
            return "????";
    }
}

inline std::string flag_str(PacketFlag packetflag) {
    switch (packetflag) {
        case FLAG_NORMAL:
            return "OK";
        case FLAG_TIMED_OUT:
            return "TIMED OUT";
        default:
            return "";
    }
}

/* ===========================================================================
 * POCSAGPacket (common/pocsag_packet.hpp)
 * ===========================================================================*/

class POCSAGPacket {
   public:
    void set_timestamp(const Timestamp& value) { timestamp_ = value; }
    Timestamp timestamp() const { return timestamp_; }

    void set(size_t index, uint32_t data) {
        if (index < batch_size) codewords_[index] = data;
    }

    void set(const batch_t& batch) { codewords_ = batch; }

    uint32_t operator[](size_t index) const {
        return (index < batch_size) ? codewords_[index] : 0u;
    }

    void set_bitrate(uint16_t bitrate) { bitrate_ = bitrate; }
    uint16_t bitrate() const { return bitrate_; }

    void set_flag(PacketFlag flag) { flag_ = flag; }
    PacketFlag flag() const { return flag_; }

    void set_inverted(bool inverted) { inverted_ = inverted; }
    bool inverted() const { return inverted_; }

    void clear() {
        codewords_.fill(0);
        bitrate_ = 0u;
        flag_ = FLAG_NORMAL;
        inverted_ = false;
    }

   private:
    uint16_t bitrate_{0};
    PacketFlag flag_{FLAG_NORMAL};
    bool inverted_{false};
    batch_t codewords_{};
    Timestamp timestamp_{};
};

/* ===========================================================================
 * BCH(31,21) + even parity error correction
 *
 * Verbatim from common/pocsag.cpp (itself from multimon). `ecs[n]` is the
 * syndrome contribution of data bit n, where data bit n sits at codeword bit
 * 31-n; `bch[]` maps a syndrome to the one or two bit positions to flip.
 *
 * Note two upstream behaviours that are deliberately preserved:
 *   - Only the 21 *data* bits (31..11) are ever corrected. An error confined
 *     to the parity bits is reported (errl != 0) but not repaired, because
 *     repairing it would not change the decoded payload.
 *   - The final even-parity bit is computed but discarded; a codeword whose
 *     only corruption is bit 0 is reported as clean.
 * ===========================================================================*/

class EccContainer {
   public:
    EccContainer() { setup_ecc(); }

    EccContainer(const EccContainer&) = delete;
    EccContainer& operator=(const EccContainer&) = delete;

    /* Corrects `val` in place. Returns 0 (clean), 1 or 2 (that many bits
     * corrected) or 3 (uncorrectable — `val` is left as received). */
    int error_correct(uint32_t& val) const {
        int i, synd, errl, acc, pari, ecc, b1, b2;

        errl = 0;
        pari = 0;

        ecc = 0;
        for (i = 31; i >= 11; --i) {
            if (val & (1u << i)) {
                ecc = ecc ^ static_cast<int>(ecs_[31 - i]);
                pari = pari ^ 0x01;
            }
        }

        acc = 0;
        for (i = 10; i >= 1; --i) {
            acc = acc << 1;
            if (val & (1u << i)) acc = acc ^ 0x01;
        }

        synd = ecc ^ acc;
        errl = 0;

        if (synd != 0) { /* nonzero syndrome means an error is present */
            if (bch_[synd] != 0) { /* correctable? */
                b1 = static_cast<int>(bch_[synd] & 0x1f);
                b2 = static_cast<int>((bch_[synd] >> 5) & 0x1f);

                if (b2 != 0x1f) {
                    val ^= 0x01u << (31 - b2);
                    ecc = ecc ^ static_cast<int>(ecs_[b2]);
                }
                if (b1 != 0x1f) {
                    val ^= 0x01u << (31 - b1);
                    ecc = ecc ^ static_cast<int>(ecs_[b1]);
                }

                errl = static_cast<int>(bch_[synd] >> 12);
            } else {
                errl = 3;
            }

            if (errl == 1) pari = pari ^ 0x01;
        }

        if (errl == 4) errl = 3;

        return errl;
    }

    /* The transmit side of the same code: takes a codeword whose top 21 bits
     * are the payload, fills bits 10..1 with the BCH parity that makes the
     * syndrome zero, and bit 0 with even parity over the whole word.
     *
     * This is the departure noted in the file header — upstream's insert_BCH()
     * runs common/bch_code.cpp's polynomial encoder. Both produce the same 10
     * bits because the syndrome of a valid codeword is zero by definition, so
     * the parity field simply *is* the syndrome of the data field. */
    uint32_t encode(uint32_t codeword) const {
        codeword &= 0xFFFFF800u;

        uint32_t ecc = 0;
        for (int i = 31; i >= 11; --i)
            if (codeword & (1u << i)) ecc ^= ecs_[31 - i];
        codeword |= (ecc & 0x3FFu) << 1;

        uint32_t parity = 0;
        for (int i = 31; i >= 1; --i)
            if (codeword & (1u << i)) parity ^= 1u;
        codeword |= parity;

        return codeword;
    }

   private:
    void setup_ecc() {
        unsigned int srr = 0x3b4;
        unsigned int i, n, j, k;

        /* Only for the (31,21) code used by POCSAG and FLEX. */
        for (i = 0; i <= 20; i++) {
            ecs_[i] = srr;
            if ((srr & 0x01) != 0)
                srr = (srr >> 1) ^ 0x3B4;
            else
                srr = srr >> 1;
        }

        for (i = 0; i < 1024; i++) bch_[i] = 0;

        /* two errors in data */
        for (n = 0; n <= 20; n++) {
            for (i = 0; i <= 20; i++) {
                j = (i << 5) + n;
                k = ecs_[n] ^ ecs_[i];
                bch_[k] = j + 0x2000;
            }
        }

        /* one error in data */
        for (n = 0; n <= 20; n++) {
            k = ecs_[n];
            j = n + (0x1f << 5);
            bch_[k] = j + 0x1000;
        }

        /* one error in data and one in the ecc portion */
        for (n = 0; n <= 20; n++) {
            for (i = 0; i < 10; i++) {
                k = ecs_[n] ^ (1u << i);
                j = n + (0x1f << 5);
                bch_[k] = j + 0x2000;
            }
        }

        /* one error in ecc */
        for (n = 0; n < 10; n++) {
            k = 1u << n;
            bch_[k] = 0x3ff + 0x1000;
        }

        /* two errors in ecc */
        for (n = 0; n < 10; n++) {
            for (i = 0; i < 10; i++) {
                if (i != n) {
                    k = (1u << n) ^ (1u << i);
                    bch_[k] = 0x3ff + 0x2000;
                }
            }
        }
    }

    uint32_t ecs_[32]{};
    uint32_t bch_[1025]{};
};

/* ===========================================================================
 * Batch decoder state (common/pocsag.hpp)
 * ===========================================================================*/

struct POCSAGState {
    POCSAGState() = default;
    explicit POCSAGState(const EccContainer* e) : ecc{e} {}

    const EccContainer* ecc = nullptr;
    uint8_t codeword_index = 0;
    uint32_t function = 0;
    uint32_t address = 0;
    Mode mode = STATE_CLEAR;
    OutputType out_type = OUT_EMPTY;
    uint64_t ascii_data = 0;
    uint32_t ascii_idx = 0;
    uint32_t errors = 0;
    uint8_t prev_cw_err = 0;    /* error level of the previous codeword */
    uint8_t cur_cw_err = 0;     /* error level of the current codeword */
    bool new_message = false;   /* true when the decoder starts a new address */
    bool type_decided = false;  /* true after the first-batch heuristic runs */
    DetectedType detected = DET_UNKNOWN;
    uint8_t msg_codewords = 0;
    std::string output{};       /* alpha decode, always populated */
    char numeric_buf[80]{};
    uint8_t numeric_len = 0;
};

/* 4-bit BCD -> ASCII, from common/pocsag.cpp. */
inline const char* numeric_chars() {
    static const char table[17] = "0123456789RU -][";
    return table;
}

/* Extract and bit-reverse a 4-bit nibble from a message codeword. POCSAG
 * numeric digits are transmitted LSB first, so bit 30 (the first transmitted
 * bit of the payload) is the LSB of the digit value. */
inline uint8_t decode_nibble(uint32_t codeword, int nibble_idx) {
    const int bit_pos = 30 - nibble_idx * 4;
    uint8_t n = 0;
    n |= static_cast<uint8_t>(((codeword >> (bit_pos - 3)) & 1u) << 3);
    n |= static_cast<uint8_t>(((codeword >> (bit_pos - 2)) & 1u) << 2);
    n |= static_cast<uint8_t>(((codeword >> (bit_pos - 1)) & 1u) << 1);
    n |= static_cast<uint8_t>(((codeword >> (bit_pos - 0)) & 1u) << 0);
    return n;
}

/* --- heuristic message-type detection (first batch only) ------------------ */

inline int count_alpha_fill(const std::string& data) {
    if (data.empty()) return 0;
    int fill = 0;
    for (int i = static_cast<int>(data.size()) - 1; i >= 0; --i) {
        const char c = data[static_cast<size_t>(i)];
        if (c == '\0' || c == ' ')
            fill++;
        else
            break;
    }
    return fill;
}

inline int count_numeric_fill(const uint8_t* nibbles, int count) {
    int fill = 0;
    for (int i = count - 1; i >= 0; --i) {
        if (nibbles[i] == 0x0C)
            fill++;
        else
            break;
    }
    return fill;
}

inline int score_alpha(const std::string& data, int fill) {
    int score = 0;
    int content = 0;
    const int len = static_cast<int>(data.size());

    for (int i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[static_cast<size_t>(i)]);

        if (i >= len - fill && (c == 0 || c == ' ')) continue;
        if (c == 0) continue;

        content++;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == ' ')
            score += 3;
        else if (c >= 0x20 && c <= 0x7E)
            score -= 2;
        else if (c == '\n' || c == '\r' || c == '\t' || c == 0x04)
            score += 0;
        else
            score -= 5;
    }

    if (content > 0) score += fill * fill * 3 + fill * 5;

    return score;
}

inline int score_numeric(const uint8_t* nibbles, int count, int fill) {
    int raw_score = 0;
    int scored = 0;
    int digits = 0;
    int u_count = 0;

    for (int i = 0; i < count; ++i) {
        if (nibbles[i] == 0x0C && i >= count - fill) continue;
        if (nibbles[i] == 0x0B) u_count++;
    }

    const bool urgent_prefix = (u_count == 1);

    for (int i = 0; i < count; ++i) {
        const uint8_t n = nibbles[i];
        if (n == 0x0C && i >= count - fill) continue;

        scored++;
        if (n <= 0x09) {
            raw_score += 3;
            digits++;
        } else if (n == 0x0B) {
            raw_score += urgent_prefix ? -1 : -15;
        } else if (n == 0x0A) {
            raw_score -= 5;
        } else {
            raw_score -= 2;
        }
    }

    int score = scored > 0 ? raw_score * 4 / 7 : 0;

    if (digits > 15) score -= (digits - 15) * 5;
    score += fill * fill;

    return score;
}

inline DetectedType detect_message_type(const std::string& alpha,
                                        const uint8_t* nibbles,
                                        uint8_t nibble_count,
                                        uint8_t msg_codewords) {
    if (alpha.empty() && nibble_count == 0) return DET_TONE;

    /* Long messages cannot be numeric — phone numbers are short. */
    if (msg_codewords >= 8) return DET_ALPHA;

    const int alpha_fill = count_alpha_fill(alpha);
    const int numeric_fill = count_numeric_fill(nibbles, nibble_count);

    int sa = score_alpha(alpha, alpha_fill) + 2; /* alpha prior bias */
    const int sn = score_numeric(nibbles, nibble_count, numeric_fill);

    if (msg_codewords <= 3) sa += 3; /* short-message boost for alpha */

    return (sn > sa) ? DET_NUMERIC : DET_ALPHA;
}

/* --- the batch decoder ---------------------------------------------------- */

/* Decodes codewords from `state.codeword_index` to the end of the batch.
 * Returns true if the batch has more to process (the caller should display
 * what it has and call again). Direct port of pocsag_decode_batch. */
inline bool pocsag_decode_batch(const POCSAGPacket& batch, POCSAGState& state) {
    constexpr uint8_t codeword_max = 16;
    state.output.clear();

    /* Only reset the numeric accumulator when starting a new message, not on
     * continuation batches — numeric_buf is cumulative across batches. */
    const bool continuing_numeric =
        (state.mode != STATE_HAVE_ADDRESS) &&
        (state.out_type == OUT_MESSAGE) &&
        state.type_decided &&
        (state.detected == DET_NUMERIC);
    if (!continuing_numeric) state.numeric_len = 0;

    /* Preserve new_message across a batch boundary when STATE_HAVE_ADDRESS
     * persists: the address was at the end of the previous batch and has not
     * been displayed yet. */
    if (state.mode != STATE_HAVE_ADDRESS) state.new_message = false;

    uint8_t nibbles[max_batch_nibbles];
    uint8_t nibble_count = 0;
    uint8_t msg_codewords = 0;
    std::string raw_alpha{};
    /* If any character came from an uncorrectable codeword the heuristic is
     * unreliable, so it is skipped and alpha assumed. */
    bool has_bad_chars = false;

    const char* const numchars = numeric_chars();

    while (state.codeword_index < codeword_max) {
        uint32_t codeword = batch[state.codeword_index];
        const bool is_address = (codeword & 0x80000000u) == 0;

        const int error_count = state.ecc ? state.ecc->error_correct(codeword) : 0;

        switch (state.mode) {
            case STATE_CLEAR:
                if (is_address && codeword != idleword) {
                    state.function = (codeword >> 11) & 3u;
                    state.address = (codeword >> 10) & 0x1FFFF8u;
                    /* Frame number = low 3 bits of the RIC, taken from the
                     * address codeword's position in the batch. */
                    state.address |= (state.codeword_index >> 1);
                    state.mode = STATE_HAVE_ADDRESS;
                    state.out_type = OUT_ADDRESS;
                    state.errors = static_cast<uint32_t>(error_count);
                    state.new_message = true;
                    state.type_decided = false;
                    state.detected = DET_UNKNOWN;

                    state.ascii_idx = 0;
                    state.ascii_data = 0;
                    state.prev_cw_err = 0;
                    state.cur_cw_err = 0;
                    nibble_count = 0;
                    msg_codewords = 0;
                    raw_alpha.clear();
                } else if (codeword == idleword) {
                    state.out_type = OUT_IDLE;
                }
                break;

            case STATE_HAVE_ADDRESS:
                if (is_address) {
                    if (!state.type_decided && msg_codewords > 0) {
                        state.detected = has_bad_chars
                                             ? DET_ALPHA
                                             : detect_message_type(raw_alpha, nibbles,
                                                                   nibble_count, msg_codewords);
                        state.type_decided = true;
                        state.msg_codewords = msg_codewords;
                        if (state.detected == DET_NUMERIC) {
                            state.numeric_len = 0;
                            for (uint8_t ni = 0;
                                 ni < nibble_count && state.numeric_len < sizeof(state.numeric_buf);
                                 ++ni)
                                state.numeric_buf[state.numeric_len++] = numchars[nibbles[ni] & 0x0F];
                        }
                    }
                    state.mode = STATE_CLEAR;
                    return true;
                }

                state.mode = STATE_GETTING_MSG;
                [[fallthrough]];

            case STATE_GETTING_MSG: {
                if (is_address) {
                    if (!state.type_decided && msg_codewords > 0) {
                        state.detected = has_bad_chars
                                             ? DET_ALPHA
                                             : detect_message_type(raw_alpha, nibbles,
                                                                   nibble_count, msg_codewords);
                        state.type_decided = true;
                        state.msg_codewords = msg_codewords;
                        if (state.detected == DET_NUMERIC) {
                            state.numeric_len = 0;
                            for (uint8_t ni = 0;
                                 ni < nibble_count && state.numeric_len < sizeof(state.numeric_buf);
                                 ++ni)
                                state.numeric_buf[state.numeric_len++] = numchars[nibbles[ni] & 0x0F];
                        }
                    }
                    state.mode = STATE_CLEAR;
                    return true;
                }

                state.out_type = OUT_MESSAGE;
                state.errors += static_cast<uint32_t>(error_count);
                msg_codewords++;

                /* Per-codeword error level for character colouring:
                 * 0 = clean, 1-2 = corrected, 3 = uncorrectable. */
                state.prev_cw_err = state.cur_cw_err;
                state.cur_cw_err = static_cast<uint8_t>((error_count >= 3) ? 3 : error_count);

                /* Bits left over from the previous codeword inherit its error
                 * level; a character spanning the boundary gets the worse. */
                uint32_t bits_from_prev = state.ascii_idx;

                state.ascii_data |= static_cast<uint64_t>((codeword >> 11) & 0xFFFFFu)
                                    << (44 - state.ascii_idx);
                state.ascii_idx += 20;

                while (state.ascii_idx >= 7) {
                    uint8_t char_err;
                    if (bits_from_prev >= 7) {
                        char_err = state.prev_cw_err;
                        bits_from_prev -= 7;
                    } else if (bits_from_prev > 0) {
                        char_err = std::max(state.prev_cw_err, state.cur_cw_err);
                        bits_from_prev = 0;
                    } else {
                        char_err = state.cur_cw_err;
                    }

                    char ascii_char = static_cast<char>((state.ascii_data >> 57) & 0x7Fu);
                    state.ascii_data <<= 7;
                    state.ascii_idx -= 7;

                    /* 7-bit characters go out LSB first, so reverse. */
                    ascii_char = static_cast<char>(((ascii_char & 0xF0) >> 4) | ((ascii_char & 0x0F) << 4));
                    ascii_char = static_cast<char>(((ascii_char & 0xCC) >> 2) | ((ascii_char & 0x33) << 2));
                    ascii_char = static_cast<char>(((ascii_char & 0xAA) >> 2) | (ascii_char & 0x55));

                    if (!state.type_decided) raw_alpha += ascii_char;

                    if (char_err >= 3) {
                        state.output += "?";
                        has_bad_chars = true;
                    } else if (ascii_char < 32 || ascii_char > 126)
                        state.output += ".";
                    else
                        state.output += ascii_char;
                }

                /* Numeric decode. First batch: accumulate nibbles locally for
                 * the heuristic. Later batches: append straight to the buffer
                 * once numeric has been decided. */
                if (!state.type_decided && nibble_count + 5 <= max_batch_nibbles) {
                    for (int n = 0; n < 5; ++n)
                        nibbles[nibble_count++] = decode_nibble(codeword, n);
                } else if (state.type_decided && state.detected == DET_NUMERIC &&
                           state.numeric_len + 5 <= static_cast<uint8_t>(sizeof(state.numeric_buf))) {
                    for (int n = 0; n < 5; ++n) {
                        const uint8_t nib = decode_nibble(codeword, n);
                        state.numeric_buf[state.numeric_len++] = numchars[nib & 0x0F];
                    }
                }

                break;
            }
        }

        state.codeword_index++;
    }

    /* End of batch: if there is message data and the type is still open, run
     * the heuristic now. */
    if (state.out_type == OUT_MESSAGE && !state.type_decided && msg_codewords > 0) {
        state.detected = has_bad_chars
                             ? DET_ALPHA
                             : detect_message_type(raw_alpha, nibbles, nibble_count, msg_codewords);
        state.type_decided = true;
        state.msg_codewords = msg_codewords;
        if (state.detected == DET_NUMERIC) {
            state.numeric_len = 0;
            for (uint8_t ni = 0;
                 ni < nibble_count && state.numeric_len < sizeof(state.numeric_buf); ++ni)
                state.numeric_buf[state.numeric_len++] = numchars[nibbles[ni] & 0x0F];
        }
    }

    return false;
}

/* --- encoder (common/pocsag.cpp), used by the tests and by any TX app ----- */

inline uint32_t get_digit_code(char code) {
    if ((code >= '0') && (code <= '9')) {
        code = static_cast<char>(code - '0');
    } else {
        if (code == 'S')
            code = 10;
        else if (code == 'U')
            code = 11;
        else if (code == ' ')
            code = 12;
        else if (code == '-')
            code = 13;
        else if (code == ']')
            code = 14;
        else if (code == '[')
            code = 15;
        else
            code = 12;
    }

    code = static_cast<char>(((code & 0x0C) >> 2) | ((code & 0x03) << 2)); /* ----3210 -> ----1032 */
    code = static_cast<char>(((code & 0x0A) >> 1) | ((code & 0x05) << 1)); /* ----1032 -> ----0123 */

    return static_cast<uint32_t>(static_cast<uint8_t>(code));
}

/* Builds preamble + batches for one page, exactly as common/pocsag.cpp does. */
inline void pocsag_encode(MessageType type,
                          const EccContainer& ecc,
                          uint32_t function,
                          const std::string& message,
                          uint32_t address,
                          std::vector<uint32_t>& codewords) {
    size_t b, c, address_slot;
    size_t bit_idx, char_idx = 0;
    uint32_t codeword, digit_code;
    char ascii_char = 0;

    const size_t message_size = message.size();

    for (b = 0; b < (preamble_length / 32); b++) codewords.push_back(0xAAAAAAAAu);

    codeword = (address & 0x1FFFF8u) << 10;
    address_slot = (address & 7u) * 2;
    codeword |= (function << 11);
    codeword = ecc.encode(codeword);

    codewords.push_back(syncword);
    for (c = 0; c < 16; c++) {
        if (c == address_slot) {
            codewords.push_back(codeword);
            if (type != ADDRESS_ONLY) break;
        } else {
            codewords.push_back(idleword);
        }
    }

    if (type == ADDRESS_ONLY) return;

    c++;
    codeword = 0;
    bit_idx = 20 + 11;

    do {
        if (c == 0) codewords.push_back(syncword);

        for (; c < 16; c++) {
            if (type == ALPHANUMERIC) {
                if ((char_idx < message_size) || (ascii_char)) {
                    do {
                        bit_idx -= 7;

                        if (char_idx < message_size)
                            ascii_char = static_cast<char>(message[char_idx] & 0x7F);
                        else
                            ascii_char = 0;

                        ascii_char = static_cast<char>(((ascii_char & 0xF0) >> 4) | ((ascii_char & 0x0F) << 4));
                        ascii_char = static_cast<char>(((ascii_char & 0xCC) >> 2) | ((ascii_char & 0x33) << 2));
                        ascii_char = static_cast<char>(((ascii_char & 0xAA) >> 2) | (ascii_char & 0x55));

                        codeword |= (static_cast<uint32_t>(static_cast<uint8_t>(ascii_char)) << bit_idx);
                        char_idx++;
                    } while (bit_idx > 11);

                    codeword &= 0x7FFFF800u;
                    codeword |= 0x80000000u;
                    codeword = ecc.encode(codeword);
                    codewords.push_back(codeword);

                    if (bit_idx != 11) {
                        bit_idx = 20 + bit_idx;
                        codeword = static_cast<uint32_t>(static_cast<uint8_t>(ascii_char)) << bit_idx;
                    } else {
                        bit_idx = 20 + 11;
                        codeword = 0;
                    }
                } else {
                    codewords.push_back(idleword);
                }
            } else if (type == NUMERIC_ONLY) {
                if (char_idx < message_size) {
                    do {
                        bit_idx -= 4;

                        if (char_idx < message_size)
                            digit_code = get_digit_code(message[char_idx]);
                        else
                            digit_code = 3; /* space */

                        codeword |= (digit_code << bit_idx);
                        char_idx++;
                    } while (bit_idx > 11);

                    codeword |= 0x80000000u;
                    codeword = ecc.encode(codeword);
                    codewords.push_back(codeword);

                    bit_idx = 20 + 11;
                    codeword = 0;
                } else {
                    codewords.push_back(idleword);
                }
            }
        }

        c = 0;
    } while (char_idx < message_size);
}

/* ===========================================================================
 * Host baseband (baseband/proc_pocsag2.*)
 * ===========================================================================*/

/* Second-order IIR, direct form 1 — the host stand-in for the firmware's
 * IIRBiquadFilter. Coefficients are {b0,b1,b2} and {1,a1,a2}. */
struct BiquadCoefficients {
    float b[3]{1.0f, 0.0f, 0.0f};
    float a[3]{1.0f, 0.0f, 0.0f};
};

/* Butterworth (Q = 1/sqrt(2)) lowpass by the bilinear transform, i.e. exactly
 * what scipy.signal.butter(2, fc, "lowpass", fs=fs) returns — the call
 * upstream used to produce its hard-coded 1800 Hz / 24 kHz coefficients. */
inline BiquadCoefficients design_butterworth_lowpass(double cutoff_hz, double sample_rate_hz) {
    BiquadCoefficients c{};
    if (!(sample_rate_hz > 0.0) || !(cutoff_hz > 0.0)) return c;

    double fc = cutoff_hz;
    const double nyquist = sample_rate_hz * 0.5;
    if (fc >= nyquist * 0.999) fc = nyquist * 0.999;

    const double w0 = 2.0 * 3.14159265358979323846 * fc / sample_rate_hz;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double q = 0.70710678118654752440;
    const double alpha = sw / (2.0 * q);

    const double a0 = 1.0 + alpha;
    c.b[0] = static_cast<float>(((1.0 - cw) * 0.5) / a0);
    c.b[1] = static_cast<float>((1.0 - cw) / a0);
    c.b[2] = c.b[0];
    c.a[0] = 1.0f;
    c.a[1] = static_cast<float>((-2.0 * cw) / a0);
    c.a[2] = static_cast<float>((1.0 - alpha) / a0);
    return c;
}

class Biquad {
   public:
    void configure(const BiquadCoefficients& c) {
        c_ = c;
        reset();
    }
    void reset() { x1_ = x2_ = y1_ = y2_ = 0.0f; }

    float process(float x) {
        const float y = c_.b[0] * x + c_.b[1] * x1_ + c_.b[2] * x2_ - c_.a[1] * y1_ - c_.a[2] * y2_;
        x2_ = x1_;
        x1_ = x;
        y2_ = y1_;
        y1_ = y;
        return y;
    }

    void execute_in_place(float* p, size_t count) {
        for (size_t i = 0; i < count; ++i) p[i] = process(p[i]);
    }

    const BiquadCoefficients& coefficients() const { return c_; }

   private:
    BiquadCoefficients c_{};
    float x1_{0.0f}, x2_{0.0f}, y1_{0.0f}, y2_{0.0f};
};

/* Normalises the audio stream to +/-1.0 with a dead zone in the middle.
 * Upstream decays min/max once a second at its fixed 24 kHz; here the decay
 * interval is derived from the configured rate so it stays one second. */
class AudioNormalizer {
   public:
    void configure(float sample_rate_hz) {
        decay_interval_ = (sample_rate_hz > 0.0f) ? static_cast<uint32_t>(sample_rate_hz) : 24000u;
        reset();
    }

    void reset() {
        counter_ = 0;
        min_ = 99.0f;
        max_ = -99.0f;
        t_hi_ = 1.0f;
        t_lo_ = 1.0f;
    }

    void execute_in_place(float* p, size_t count) {
        if (counter_ >= decay_interval_) {
            /* A 90% decay keeps large transients from wrecking the filter. */
            max_ *= 0.9f;
            min_ *= 0.9f;
            counter_ = 0;
            calculate_thresholds();
        }

        counter_ += static_cast<uint32_t>(count);

        for (size_t i = 0; i < count; ++i) {
            float& val = p[i];

            if (val > max_) {
                max_ = val;
                calculate_thresholds();
            }
            if (val < min_) {
                min_ = val;
                calculate_thresholds();
            }

            if (val >= t_hi_)
                val = 1.0f;
            else if (val <= t_lo_)
                val = -1.0f;
            else
                val = 0.0f;
        }
    }

   private:
    void calculate_thresholds() {
        const float center = (max_ + min_) / 2.0f;
        const float range = (max_ - min_) / 2.0f;
        /* 10% off centre forces +/-1. Higher = larger dead zone, lower = more
         * false positives. */
        const float threshold = range * 0.1f;
        t_hi_ = center + threshold;
        t_lo_ = center - threshold;
    }

    uint32_t decay_interval_{24000};
    uint32_t counter_{0};
    float min_{99.0f};
    float max_{-99.0f};
    float t_hi_{1.0f};
    float t_lo_{1.0f};
};

/* FIFO over a uint32_t's bits. */
class BitQueue {
   public:
    void push(bool bit) {
        data_ = (data_ << 1) | (bit ? 1u : 0u);
        if (count_ < max_size_) ++count_;
    }

    bool pop() {
        if (count_ == 0) return false;
        --count_;
        return (data_ & (1u << count_)) != 0;
    }

    void reset() {
        data_ = 0;
        count_ = 0;
    }

    uint8_t size() const { return count_; }
    uint32_t data() const { return data_; }

   private:
    uint32_t data_{0};
    uint8_t count_{0};
    static constexpr uint8_t max_size_ = 32;
};

inline uint8_t diff_bit_count(uint32_t left, uint32_t right) {
    uint32_t v = left ^ right;
    uint8_t n = 0;
    while (v) {
        v &= (v - 1u);
        ++n;
    }
    return n;
}

/* Extracts bits and the bit rate from the normalised audio stream. Samples at
 * twice the baud rate so it can synchronise to bit transitions without knowing
 * where they are. */
class BitExtractor {
   public:
    explicit BitExtractor(BitQueue& bits) : bits_{bits} {}

    void extract_bits(const float* audio, size_t count) {
        /* Assumes the input has been normalised to +/-1. Positive is 0,
         * negative is 1. */
        for (size_t i = 0; i < count; ++i) {
            const float sample = audio[i];

            if (current_rate_) {
                if (current_rate_->handle_sample(sample)) {
                    const bool value = (current_rate_->bits.data() & 1u) == 1u;
                    bits_.push(value);
                }
            } else {
                /* Feed the sample to every known rate for clock detection. */
                for (auto& rate : known_rates_) {
                    if (rate.handle_sample(sample) &&
                        diff_bit_count(rate.bits.data(), clock_magic_number) <= 3) {
                        rate.is_stable = true;
                        current_rate_ = &rate;
                    }
                }
            }
        }
    }

    void configure(float sample_rate_hz) {
        sample_rate_ = sample_rate_hz;

        for (auto& rate : known_rates_)
            rate.samples_per_bit = sample_rate_hz / static_cast<float>(rate.baud_rate);

        select_configured_rate();
    }

    void reset() {
        for (auto& rate : known_rates_) rate.reset();
        select_configured_rate();
    }

    void set_baud_config(int8_t baud_config) { baud_config_ = baud_config; }
    int8_t baud_config() const { return baud_config_; }

    uint16_t baud_rate() const {
        return current_rate_ ? static_cast<uint16_t>(current_rate_->baud_rate) : uint16_t{0};
    }

   private:
    /* Clock-signal detection magic number: the 0xAAAA... preamble. */
    static constexpr uint32_t clock_magic_number = 0xAAAAAAAAu;

    /* Per-rate bit clock.
     *
     * DEPARTURE FROM UPSTREAM, and the one substantive change in this port.
     * proc_pocsag2 samples at twice the baud rate and emits a bit when two
     * consecutive samples agree, nudging the sampling instant by an eighth of
     * a half-bit whenever they do not — and it stops nudging entirely once the
     * clock is declared stable, after which the sampler free-runs.
     *
     * That only holds together when audio_rate/(2*baud) is an exact integer
     * and the transmitter's clock is exactly ours. Upstream gets away with it
     * because its audio rate is hard-wired to 24 kHz, where 1200 and 2400 give
     * intervals of exactly 10 and 5. Measured with upstream's algorithm on a
     * clean, noise-free bit stream (scratch probe, 1120 bits, no channel
     * impairment at all — preamble + sync + one batch):
     *
     *     24000 Hz / 1200 baud (interval 10.0)     1118/1120 bits correct
     *     24000 Hz / 2400 baud (interval 5.0)      1118/1120 bits correct
     *     24000 Hz /  512 baud (interval 23.4375)   687/1120 bits correct
     *     25000 Hz / 1200 baud (interval 10.4167)   531/1120 bits correct
     *
     * i.e. 512 bps never decodes even upstream's own way, and any host capture
     * rate that is not a tidy multiple of the baud rate loses the signal
     * outright. The B200's decimation cannot always land on 24 kHz and a real
     * transmitter's clock is never exactly ours, so the sampling instant here
     * is re-derived from the signal: one decision per bit, at the bit centre,
     * snapped to half a bit after every observed edge — the classic
     * edge-triggered resync a UART receiver uses. With that, all three rates
     * decode the same stream 1120/1120 at every audio rate tried (18k, 20k,
     * 22.05k, 24k, 24.024k, 25k, 30k, 32k, 36k, 48k, 50k, 96k) and tolerate
     * +/-1% transmitter clock error, forced or auto-detected.
     *
     * Two supporting details:
     *   - Samples that AudioNormalizer flattened to exactly 0 (its dead zone)
     *     carry no level information and are skipped, so noise in the middle
     *     of the swing cannot fake an edge, and the residual bias of taking
     *     signbit(0) as a '0' disappears.
     *   - The decision fires on the sample *nearest* the wanted instant.
     *   - 2400 bps at a 24 kHz audio rate is only 10 samples per bit and
     *     still loses a slow transmitter; the app therefore asks its front end
     *     for 48 kHz, which upstream could not do.
     *
     * Everything around this — the 32-bit history used for 0xAAAA.. clock
     * detection, the BitQueue contract, the auto-rate search — is unchanged. */
    struct RateInfo {
        int16_t baud_rate = 0;
        float samples_per_bit = 0.0f;

        float samples_until_next = 0.0f;
        bool prev_value = false;
        bool primed = false;
        /* Set once this rate's 32-bit history has matched the 0xAAAA.. clock
         * pattern. Upstream also used it to gate its sampling nudge; the
         * edge-snap above needs no such gate, so here it is only the "this is
         * the rate on the air" flag the auto-detect search sets. */
        bool is_stable = false;
        BitQueue bits{};

        /* Returns true if this rate has a new bit in its queue. */
        bool handle_sample(float sample) {
            if (!(samples_per_bit > 0.0f)) return false;

            const float half = samples_per_bit * 0.5f;

            /* AudioNormalizer forces the middle of the swing to exactly 0,
             * and signbit(0) is false — so a dead-zone sample looks like a
             * '0'. Taking that at face value moves every 1->0 edge earlier
             * and every 0->1 edge later by the width of the transition, a
             * systematic bias that shows up as an asymmetric tolerance to
             * transmitter clock error (measured: +0.5% fine, -0.05% lost).
             * A zero sample carries no level information, so it is skipped
             * for both edge detection and the bit decision. */
            if (sample != 0.0f) {
                const bool value = std::signbit(sample); /* negative is '1' */

                if (!primed) {
                    primed = true;
                    prev_value = value;
                    samples_until_next = half;
                } else if (value != prev_value) {
                    /* An edge is a bit boundary, so the next decision point
                     * is half a bit away. */
                    samples_until_next = half;
                }
                prev_value = value;
            }

            samples_until_next -= 1.0f;
            /* Decide on the sample nearest the wanted instant, not the first
             * one past it: with a threshold of 0 the countdown lands exactly
             * on zero whenever samples_per_bit is a whole number, and any
             * positive phase correction then costs a full sample. */
            if (samples_until_next > 0.5f) return false;

            samples_until_next += samples_per_bit;
            bits.push(prev_value);
            return true;
        }

        void reset() {
            samples_until_next = 0.0f;
            prev_value = false;
            primed = false;
            is_stable = false;
            bits.reset();
        }
    };

    void select_configured_rate() {
        if (baud_config_ >= 0 && baud_config_ < static_cast<int8_t>(known_rates_.size()))
            current_rate_ = &known_rates_[static_cast<size_t>(baud_config_)];
        else
            current_rate_ = nullptr;
    }

    std::array<RateInfo, 3> known_rates_{
        RateInfo{512},
        RateInfo{1200},
        RateInfo{2400}};

    BitQueue& bits_;
    int8_t baud_config_ = -1;
    float sample_rate_ = 0.0f;
    RateInfo* current_rate_ = nullptr;
};

/* Extracts 16-codeword batches from the BitQueue. */
class CodewordExtractor {
   public:
    using batch_handler_t = std::function<void(CodewordExtractor&)>;

    CodewordExtractor(BitQueue& bits, batch_handler_t on_batch)
        : bits_{bits}, on_batch_{std::move(on_batch)} {}

    void set_batch_handler(batch_handler_t h) { on_batch_ = std::move(h); }

    void process_bits() {
        while (bits_.size() > 0) {
            take_one_bit();

            if (bit_count_ < data_bit_count) continue;

            /* Wait for the frame synchronisation codeword. Two bit errors are
             * tolerated, and the complement is accepted so a receiver with
             * inverted FM polarity still locks. */
            if (!has_sync_) {
                if (diff_bit_count(data_, sync_codeword) <= 2)
                    handle_sync(/*inverted=*/false);
                else if (diff_bit_count(data_, ~sync_codeword) <= 2)
                    handle_sync(/*inverted=*/true);
                continue;
            }

            save_current_codeword();

            if (word_count_ == batch_size) handle_batch_complete();
        }
    }

    /* Pad with idle codewords and emit whatever is pending. */
    void flush() {
        if (word_count_ == 0) return;
        pad_idle();
        handle_batch_complete();
    }

    void reset() {
        clear_data_bits();
        has_sync_ = false;
        inverted_ = false;
        word_count_ = 0;
    }

    const batch_t& batch() const { return batch_; }
    uint32_t current() const { return data_; }
    uint8_t count() const { return word_count_; }
    bool has_sync() const { return has_sync_; }
    bool inverted() const { return inverted_; }

    static constexpr uint32_t sync_codeword = syncword;
    static constexpr uint32_t idle_codeword = idleword;

   private:
    static constexpr uint8_t data_bit_count = 32;

    void clear_data_bits() {
        data_ = 0;
        bit_count_ = 0;
    }

    void take_one_bit() {
        data_ = (data_ << 1) | (bits_.pop() ? 1u : 0u);
        if (bit_count_ < data_bit_count) ++bit_count_;
    }

    void handle_sync(bool inverted) {
        clear_data_bits();
        has_sync_ = true;
        inverted_ = inverted;
        word_count_ = 0;
    }

    void save_current_codeword() {
        batch_[word_count_++] = inverted_ ? ~data_ : data_;
        clear_data_bits();
    }

    void handle_batch_complete() {
        if (on_batch_) on_batch_(*this);
        has_sync_ = false;
        word_count_ = 0;
    }

    void pad_idle() {
        while (word_count_ < batch_size) batch_[word_count_++] = idle_codeword;
    }

    BitQueue& bits_;
    batch_handler_t on_batch_{};

    bool has_sync_ = false;
    bool inverted_ = false;

    uint32_t data_ = 0;
    uint8_t bit_count_ = 0;
    uint8_t word_count_ = 0;
    batch_t batch_{};
};

/* The whole audio-to-packet half of proc_pocsag2, as one object. Feed it
 * FM-demodulated audio; it calls the handler with a filled POCSAGPacket. */
class AudioDecoder {
   public:
    using PacketHandler = std::function<void(const POCSAGPacket&)>;

    AudioDecoder() {
        word_extractor_.set_batch_handler([this](CodewordExtractor&) { emit_packet(); });
    }

    void configure(float audio_rate_hz, int8_t baud_config) {
        audio_rate_ = audio_rate_hz;
        lpf_.configure(design_butterworth_lowpass(lowpass_hz_, audio_rate_hz));
        normalizer_.configure(audio_rate_hz);
        bit_extractor_.set_baud_config(baud_config);
        bit_extractor_.configure(audio_rate_hz);
        /* Roughly a tenth of a second of silence before the decoder gives up
         * on a partial batch — the same intent as upstream's squelch history
         * over its 16-sample audio blocks. */
        silence_limit_ = static_cast<uint32_t>(audio_rate_hz * 0.1f);
        reset();
        configured_ = true;
    }

    void set_packet_handler(PacketHandler h) { packet_handler_ = std::move(h); }

    /* Squelch: below this mean-square level the input counts as silence.
     * Upstream uses the M4's FMSquelch on the same audio for the same job. */
    void set_squelch_power(float p) { squelch_power_ = p; }

    /* Post-demod lowpass corner. Upstream hard-codes 1800 Hz and notes that
     * 2400 baud "falls nicely into the transition band" of it — measured here,
     * that filter smears a 2400 bps bit over more than one bit period and the
     * batch stops syncing as soon as the transmitter clock runs slow. Call
     * this before configure(). */
    void set_lowpass_hz(float hz) { lowpass_hz_ = hz; }
    float lowpass_hz() const { return lowpass_hz_; }

    void process(const float* audio, size_t count) {
        if (!configured_ || count == 0) return;

        scratch_.assign(audio, audio + count);

        /* Any signal at all in this block? */
        float power = 0.0f;
        for (size_t i = 0; i < count; ++i) power += scratch_[i] * scratch_[i];
        power /= static_cast<float>(count);

        if (power <= squelch_power_) {
            silence_samples_ += static_cast<uint32_t>(count);
            if (silence_samples_ >= silence_limit_) {
                if (word_extractor_.current() != 0 || word_extractor_.count() > 0) {
                    flush();
                    reset_locked_state();
                }
                silence_samples_ = 0;
            }
            return;
        }
        silence_samples_ = 0;

        lpf_.execute_in_place(scratch_.data(), scratch_.size());
        normalizer_.execute_in_place(scratch_.data(), scratch_.size());

        /* BitQueue is 32 bits deep and BitExtractor has no back-pressure, so
         * process_bits() has to drain it before it can overflow. Upstream gets
         * that for free because the M4 hands it one 16-sample audio block at a
         * time; the host arrives with milliseconds of audio at once, so the
         * same 16-sample cadence is imposed here. Feeding a whole block and
         * draining once silently loses every bit but the last 32. */
        constexpr size_t chunk = 16;
        for (size_t off = 0; off < scratch_.size(); off += chunk) {
            const size_t n = std::min(chunk, scratch_.size() - off);
            bit_extractor_.extract_bits(scratch_.data() + off, n);
            word_extractor_.process_bits();
        }
    }

    void flush() { word_extractor_.flush(); }

    void reset() {
        bits_.reset();
        lpf_.reset();
        normalizer_.reset();
        reset_locked_state();
        silence_samples_ = 0;
    }

    uint16_t baud_rate() const { return bit_extractor_.baud_rate(); }
    uint32_t current_bits() const { return word_extractor_.current(); }
    uint8_t current_frames() const { return word_extractor_.count(); }
    bool has_sync() const { return word_extractor_.has_sync(); }
    float audio_rate() const { return audio_rate_; }

    /* Test seams — the bit and codeword layers are useful on their own. */
    BitQueue& bit_queue() { return bits_; }
    BitExtractor& bit_extractor() { return bit_extractor_; }
    CodewordExtractor& codeword_extractor() { return word_extractor_; }

   private:
    void reset_locked_state() {
        bit_extractor_.reset();
        word_extractor_.reset();
    }

    void emit_packet() {
        POCSAGPacket packet{};
        packet.set_flag(FLAG_NORMAL);
        packet.set_timestamp(std::chrono::system_clock::now());
        packet.set_bitrate(bit_extractor_.baud_rate());
        packet.set_inverted(word_extractor_.inverted());
        packet.set(word_extractor_.batch());
        if (packet_handler_) packet_handler_(packet);
    }

    PacketHandler packet_handler_{};
    Biquad lpf_{};
    AudioNormalizer normalizer_{};
    BitQueue bits_{};
    BitExtractor bit_extractor_{bits_};
    CodewordExtractor word_extractor_{bits_, nullptr};
    std::vector<float> scratch_{};
    float audio_rate_{0.0f};
    float lowpass_hz_{1800.0f};
    float squelch_power_{1.0e-6f};
    uint32_t silence_samples_{0};
    uint32_t silence_limit_{2400};
    bool configured_{false};
};

/* ===========================================================================
 * RF front end
 *
 * The B200's captured band down to a ~24 kHz FM-demodulated audio stream.
 * This is the host's stand-in for proc_pocsag2's decim_0 / decim_1 /
 * channel_filter / demod chain, generalised so it works at whatever capture
 * rate the receiver is running.
 * ===========================================================================*/

class ChannelFrontEnd {
   public:
    /* `channel_cutoff_hz` is the half-bandwidth of the wanted channel. */
    void configure(double source_rate_hz, double target_audio_rate_hz, double channel_cutoff_hz) {
        source_rate_ = source_rate_hz;
        if (!(source_rate_hz > 0.0)) return;

        size_t total = static_cast<size_t>(std::lround(source_rate_hz / target_audio_rate_hz));
        if (total < 1) total = 1;

        /* Split the decimation so the first (cheap, wide) stage does most of
         * it and the second (sharp) stage runs at a low rate. */
        size_t d1 = total, d2 = 1;
        for (size_t k = 2; k * k <= total; ++k)
            if (total % k == 0) {
                d2 = k;
                d1 = total / k;
            }

        const double rate1 = source_rate_hz / static_cast<double>(d1);
        audio_rate_ = rate1 / static_cast<double>(d2);

        /* Stage 1: anything that would fold into the final band must go. */
        if (d1 > 1) {
            const double cut1 = std::min(rate1 * 0.1, std::max(channel_cutoff_hz * 1.5, 12000.0));
            const double trans1 = std::max(rate1 * 0.5 - cut1, rate1 * 0.05);
            stage1_.configure(dsp::design_lowpass(cut1, trans1, source_rate_hz, 60.0, 601), d1);
        } else {
            stage1_.configure({1.0f}, 1);
        }

        /* Stage 2: the actual channel filter. */
        const double cut2 = std::min(channel_cutoff_hz, audio_rate_ * 0.45);
        const double trans2 = std::max(audio_rate_ * 0.5 - cut2, audio_rate_ * 0.05);
        stage2_.configure(dsp::design_lowpass(cut2, trans2, rate1, 60.0, 601), d2);

        demod_.configure(static_cast<float>(audio_rate_), static_cast<float>(deviation_hz_));
        nco_.set_frequency(0.0, source_rate_hz);
        decim_total_ = total;
    }

    void set_deviation(double hz) { deviation_hz_ = hz; }

    /* Mix the wanted channel to DC: `offset_hz` is (tuned frequency - LO). */
    void set_offset(double offset_hz) { nco_.set_frequency(-offset_hz, source_rate_); }

    void process(const dsp::cfloat* in, size_t count, std::vector<float>& audio_out) {
        audio_out.clear();
        if (count == 0 || !(source_rate_ > 0.0)) return;

        mixed_.resize(count);
        nco_.mix(in, mixed_.data(), count);

        inter_.clear();
        stage1_.process(mixed_.data(), count, inter_);
        if (inter_.empty()) return;

        chan_.clear();
        stage2_.process(inter_.data(), inter_.size(), chan_);
        if (chan_.empty()) return;

        demod_.process(chan_.data(), chan_.size(), audio_out);
    }

    double audio_rate() const { return audio_rate_; }
    size_t decimation() const { return decim_total_; }

   private:
    dsp::Nco nco_{};
    dsp::FirDecimateC stage1_{};
    dsp::FirDecimateC stage2_{};
    dsp::FmDemod demod_{};
    std::vector<dsp::cfloat> mixed_{};
    std::vector<dsp::cfloat> inter_{};
    std::vector<dsp::cfloat> chan_{};
    double source_rate_{0.0};
    double audio_rate_{0.0};
    double deviation_hz_{4500.0};
    size_t decim_total_{1};
};

}  // namespace pocsag

/* ===========================================================================
 * UI
 * ===========================================================================*/

namespace app {

enum PocsagFilter : uint8_t {
    FILTER_NONE,
    FILTER_DROP,
    FILTER_KEEP
};

struct PocsagSettings {
    bool enable_small_font = false;
    bool hide_bad_data = false;
    bool hide_addr_only = false;
    bool enable_numeric_detect = true;
    uint8_t filter_mode = FILTER_NONE;
    int32_t baud_rate = -1; /* -1 = auto, else index into 512/1200/2400 */
    uint32_t filter_address = 0;
};

class PocsagSettingsView : public ui::View {
   public:
    explicit PocsagSettingsView(PocsagSettings& settings);

    std::string title() const override { return "POCSAG Config"; }
    void on_show() override;

   private:
    PocsagSettings& settings_;

    ui::Labels labels_{
        {{16, 0}, "Baud:", ui::Color::light_grey()},
        {{16, 176}, "Filter mode:", ui::Color::light_grey()},
        {{16, 194}, "Filter addr:", ui::Color::light_grey()},
    };

    ui::OptionsField opt_baud_rate_{
        {80, 0},
        4,
        {{"Auto", -1}, {" 512", 0}, {"1200", 1}, {"2400", 2}}};

    ui::Checkbox check_small_font_{{16, 32}, 14, "Use small font"};
    ui::Checkbox check_hide_bad_{{16, 64}, 14, "Hide bad data"};
    ui::Checkbox check_hide_addr_only_{{16, 96}, 14, "Hide addr only"};
    ui::Checkbox check_numeric_detect_{{16, 128}, 14, "Detect numeric"};

    ui::OptionsField opt_filter_mode_{
        {128, 176},
        4,
        {{"None", FILTER_NONE}, {"Drop", FILTER_DROP}, {"Keep", FILTER_KEEP}}};

    ui::NumberField field_filter_address_{{128, 194}, 7, {0, 2097151}, 1, '0'};

    ui::Button button_save_{{72, 240, 96, 32}, "Save"};
};

class PocsagAppView : public ui::View {
   public:
    PocsagAppView();
    ~PocsagAppView() override;

    PocsagAppView(const PocsagAppView&) = delete;
    PocsagAppView& operator=(const PocsagAppView&) = delete;

    std::string title() const override { return "POCSAG RX"; }

    void on_show() override;
    void on_frame_sync() override;

    /* Exposed so tests can drive the display path without a radio. */
    void handle_packet(const pocsag::POCSAGPacket& packet);
    bool ignore_address(uint32_t address) const;

   private:
    void refresh_ui();
    void reconfigure_dsp();
    void handle_decoded(const std::string& prefix);
    std::string format_decoded(const std::string& prefix) const;

    radio::ReceiverModel* receiver_{nullptr};

    PocsagSettings settings_{};

    pocsag::EccContainer ecc_{};
    pocsag::POCSAGState state_{&ecc_};
    pocsag::AudioDecoder decoder_{};
    pocsag::ChannelFrontEnd front_end_{};

    std::vector<dsp::cfloat> iq_{};
    std::vector<float> audio_{};

    uint32_t last_address_{0};
    uint16_t packet_count_{0};
    uint16_t current_bitrate_{0};
    bool current_inverted_{false};
    uint8_t frame_counter_{0};
    double configured_rate_{0.0};

    ui::FrequencyField field_frequency_{{0, 0}};
    ui::NumberField field_gain_{{104, 0}, 3, {0, 76}, 1, ' '};
    ui::NumberField field_volume_{{144, 0}, 2, {0, 99}, 1, ' '};
    ui::Text text_status_{{176, 0, 64, 16}, ""};

    ui::Text text_counters_{{0, 18, 240, 16}, "0 pkts"};

    ui::Button button_filter_last_{{0, 36, 112, 20}, "Filter Last"};
    ui::Button button_config_{{120, 36, 112, 20}, "Config"};

    ui::Console console_{{0, 58, 240, 246}};
};

}  // namespace app

#endif /*__MB200_POCSAG_APP_H__*/
