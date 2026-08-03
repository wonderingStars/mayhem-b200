/*
 * mayhem-b200 — TETRA downlink receiver.
 *
 * Ported from firmware/application/external/tetra_rx/ (ui_tetra_rx.*,
 * tetra_bits.hpp, tetra_crc.*, tetra_descrambler.*, tetra_interleave.*,
 * tetra_rcpc.*, tetra_viterbi.*) plus its baseband processor
 * firmware/baseband/proc_tetra.*.
 *
 * THE PIPELINE, and where each stage came from
 *
 *   RF -> mix + channel filter          host: dsp::Nco + dsp::FirDecimateC
 *                                       (upstream: two fixed decimators,
 *                                        3.072 Msps -> 48 kHz)
 *   -> carrier PLL derotation           proc_tetra.cpp execute()
 *   -> Gardner symbol timing @18 kBd    proc_tetra.cpp execute()
 *   -> differential (pi/4-DQPSK) demod  proc_tetra.cpp process_symbol()
 *   -> dibit slicing                    process_symbol(), the four-quadrant map
 *   -> sync correlation                 process_symbol(), Y/N/P training seqs
 *   -> burst extraction from a 1024-bit history buffer
 *   -> descramble (32-bit LFSR)         tetra_descrambler.cpp
 *   -> block de-interleave              tetra_interleave.cpp
 *   -> RCPC 2/3 depuncture              tetra_rcpc.cpp
 *   -> rate-1/4 K=5 Viterbi             tetra_viterbi.cpp
 *   -> CRC16 check (residue 0x1D0F)     tetra_crc.cpp
 *   -> SYNC-PDU / MAC-PDU parse         ui_tetra_rx.cpp
 *
 * SYNC DEPTH — what the burst detector actually looks for
 *   SB (synchronisation burst): the 38-bit frame-synchronisation sequence
 *   Y = 0x30673A7067, accepted at a Hamming distance of up to 4 in either
 *   polarity (the differential demodulator has no absolute phase reference, so
 *   an inverted match is normal and is flagged). The burst is taken as the 500
 *   bits starting 214 bits before the sync word.
 *
 *   DNB (normal downlink burst): the 22-bit normal (N = 0x343A74) or extended
 *   (P = 0x1E90DE) training sequence, accepted at distance <= 1 in either
 *   polarity. Block 1 is the 216 bits starting 230 bits before the training
 *   sequence; block 2 the 216 bits starting 38 bits after it.
 *
 *   All four constants, both distance limits and all three offsets are
 *   upstream's, unchanged.
 *
 * HOST DIFFERENCES, each marked at the point of use
 *   1. There is no M4 and no message queue. TetraDemodulator runs on the UI
 *      thread from on_frame_sync() and calls a handler directly.
 *   2. The loops are floating point. Upstream's fixed-point loop gains scale
 *      with the *signal power* (they are raw complex16 products shifted right);
 *      the host normalises the differential product to unit magnitude first, so
 *      the loop behaves the same at any input level. The gain constants below
 *      are upstream's, converted to that normalisation — see each constant.
 *   3. THE SAMPLE SOURCE IS NOT CONTINUOUS. ReceiverModel only exposes
 *      take_spectrum_samples(), a *snapshot* of the most recent wideband block;
 *      whatever arrived between two UI frames is dropped. A burst decoder needs
 *      an unbroken stream, so live decoding will only ever catch a burst that
 *      happens to fall inside one snapshot.
 *      IDEAL TAP: ReceiverModel::take_channel_samples(std::vector<cfloat>&),
 *      draining a gap-free ring of post-channel-filter samples at a stated
 *      rate. Everything below is written to work the moment such a tap exists;
 *      it is fed from the snapshot tap in the meantime and says so on screen.
 *
 * Copyright (C) 2026 PortaPack Mayhem contributors (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_TETRA_RX_H__
#define __MB200_UI_TETRA_RX_H__

#include "../dsp/fir.hpp"
#include "../dsp/protocol.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace app {
namespace tetra {

/* ===========================================================================
 * Packed-bit helpers (tetra_bits.hpp BitVector, as free functions)
 * ===========================================================================*/

inline uint8_t bits_get(const uint8_t* p, size_t bit) {
    return static_cast<uint8_t>((p[bit >> 3] >> (7 - (bit & 7))) & 1);
}

inline void bits_set(uint8_t* p, size_t bit, bool value) {
    if (value)
        p[bit >> 3] = static_cast<uint8_t>(p[bit >> 3] | (1u << (7 - (bit & 7))));
    else
        p[bit >> 3] = static_cast<uint8_t>(p[bit >> 3] & ~(1u << (7 - (bit & 7))));
}

inline void bits_xor(uint8_t* p, size_t bit, bool value) {
    if (value) p[bit >> 3] = static_cast<uint8_t>(p[bit >> 3] ^ (1u << (7 - (bit & 7))));
}

/* ===========================================================================
 * CRC (tetra_crc.cpp)
 *
 * CRC-16 with the CCITT polynomial 0x1021, MSB-first, initial value 0xFFFF and
 * no final XOR. ETSI EN 300 392-2 appends the *ones complement* of the
 * remainder, which makes the CRC over data+parity settle on the fixed residue
 * below — that is the whole of the "is this block good" test.
 * ===========================================================================*/

constexpr uint16_t kTetraCrcOk = 0x1D0F;

inline uint16_t crc16_itut_bits(const uint8_t* bits, uint32_t n_bits, uint16_t init = 0xFFFF) {
    uint16_t crc = init;
    for (uint32_t i = 0; i < n_bits; i++) {
        const uint8_t bit = bits_get(bits, i);
        crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(bit) << 15));
        if (crc & 0x8000)
            crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
        else
            crc = static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

/* ===========================================================================
 * Scrambler (tetra_descrambler.cpp)
 *
 * 32-bit Fibonacci LFSR, taps as in EN 300 392-2 8.2.5. The BSCH is scrambled
 * with the fixed "extended colour code 0" seed; every other channel uses a seed
 * built from MCC/MNC/BCC.
 * ===========================================================================*/

constexpr uint32_t kTetraScrambInitBsch = 0x00000003;

inline uint8_t lfsr_step(uint32_t& s) {
    uint8_t b = 0;
    b = static_cast<uint8_t>(b ^ ((s >> (32 - 32)) & 1));
    b = static_cast<uint8_t>(b ^ ((s >> (32 - 26)) & 1));
    b = static_cast<uint8_t>(b ^ ((s >> (32 - 23)) & 1));
    b = static_cast<uint8_t>(b ^ ((s >> (32 - 22)) & 1));
    b = static_cast<uint8_t>(b ^ ((s >> (32 - 16)) & 1));
    b = static_cast<uint8_t>(b ^ ((s >> (32 - 12)) & 1));
    b = static_cast<uint8_t>(b ^ ((s >> (32 - 11)) & 1));
    b = static_cast<uint8_t>(b ^ ((s >> (32 - 10)) & 1));
    b = static_cast<uint8_t>(b ^ ((s >> (32 - 8)) & 1));
    b = static_cast<uint8_t>(b ^ ((s >> (32 - 7)) & 1));
    b = static_cast<uint8_t>(b ^ ((s >> (32 - 5)) & 1));
    b = static_cast<uint8_t>(b ^ ((s >> (32 - 4)) & 1));
    b = static_cast<uint8_t>(b ^ ((s >> (32 - 2)) & 1));
    b = static_cast<uint8_t>(b ^ ((s >> (32 - 1)) & 1));
    s = (s >> 1) | (static_cast<uint32_t>(b) << 31);
    return b;
}

inline void descramble_generate(uint32_t init, uint8_t* seq, size_t bits) {
    uint32_t s = init;
    for (size_t i = 0; i < bits; i++) seq[i] = lfsr_step(s);
}

/* XOR in place; the scrambler is its own inverse. */
inline void descramble(uint8_t* packed_bits, size_t n_bits, uint32_t init = kTetraScrambInitBsch) {
    uint32_t s = init;
    for (size_t i = 0; i < n_bits; i++) bits_xor(packed_bits, i, lfsr_step(s) != 0);
}

/* Seed for a DMO channel, from the mobile network identity and source address. */
inline uint32_t dmo_scramb_init(uint32_t mni, uint32_t src) {
    uint32_t init = (src & 0xFFFFFF) | ((mni & 0x3F) << 24);
    init <<= 2;
    init |= 3;
    return init;
}

/* ===========================================================================
 * Block de-interleaver (tetra_interleave.cpp)
 * ===========================================================================*/

inline void block_deinterleave(const uint8_t* in, uint8_t* out, uint32_t K, uint32_t a) {
    for (uint32_t i = 1; i <= K; i++) {
        const uint32_t k = 1 + ((a * i) % K);
        bits_set(out, i - 1, bits_get(in, k - 1) != 0);
    }
}

/* The inverse, used by the tests to build a valid burst. Not called by the
 * receiver; upstream has no encoder. */
inline void block_interleave(const uint8_t* in, uint8_t* out, uint32_t K, uint32_t a) {
    for (uint32_t i = 1; i <= K; i++) {
        const uint32_t k = 1 + ((a * i) % K);
        bits_set(out, k - 1, bits_get(in, i - 1) != 0);
    }
}

/* ===========================================================================
 * RCPC depuncturing (tetra_rcpc.cpp)
 *
 * Rate 2/3 puncturing of the rate-1/4 mother code: of every eight mother bits
 * the transmitter keeps positions 1, 2 and 5 (1-based). Everything else is
 * marked erased so the Viterbi metric ignores it.
 * ===========================================================================*/

constexpr uint8_t kTetraErased = 0xFF;
constexpr uint8_t kTetraPRate23[3] = {1, 2, 5};
constexpr uint32_t kTetraTRate23 = 3;
constexpr uint32_t kTetraPuncturePeriod = 8;

inline uint32_t depuncture_2_3_mother_bits(uint32_t in_bits) {
    return ((in_bits + kTetraTRate23 - 1) / kTetraTRate23) * kTetraPuncturePeriod +
           kTetraPuncturePeriod;
}

inline void depuncture_2_3(const uint8_t* in_packed, uint8_t* out, uint32_t in_bits) {
    const uint32_t mother_bits = depuncture_2_3_mother_bits(in_bits);
    std::memset(out, kTetraErased, mother_bits);
    for (uint32_t j = 1; j <= in_bits; j++) {
        const uint32_t i = j;
        uint32_t k = kTetraPuncturePeriod * ((i - 1) / kTetraTRate23);
        k += kTetraPRate23[(i - 1) % kTetraTRate23];
        if (k >= 1 && k <= mother_bits) out[k - 1] = bits_get(in_packed, j - 1);
    }
}

/* The inverse, for tests: keep mother positions 1, 2 and 5 of each period. */
inline void puncture_2_3(const uint8_t* mother, uint8_t* out_packed, uint32_t out_bits) {
    for (uint32_t j = 1; j <= out_bits; j++) {
        uint32_t k = kTetraPuncturePeriod * ((j - 1) / kTetraTRate23);
        k += kTetraPRate23[(j - 1) % kTetraTRate23];
        bits_set(out_packed, j - 1, mother[k - 1] != 0);
    }
}

/* ===========================================================================
 * Rate-1/4, constraint-length-5 convolutional code (tetra_viterbi.cpp)
 * ===========================================================================*/

/* Upstream keeps these on the stack to save flash; on the host they are file
 * scope constants with the identical contents. */
inline constexpr uint8_t kTetraNextState[16][2] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7}, {8, 9}, {10, 11}, {12, 13}, {14, 15},
    {0, 1}, {2, 3}, {4, 5}, {6, 7}, {8, 9}, {10, 11}, {12, 13}, {14, 15}};

inline constexpr uint8_t kTetraNextOutput[16][2] = {
    {0, 15}, {11, 4}, {6, 9}, {13, 2}, {5, 10}, {14, 1}, {3, 12}, {8, 7},
    {15, 0}, {4, 11}, {9, 6}, {2, 13}, {10, 5}, {1, 14}, {12, 3}, {7, 8}};

/* Encoder, for tests: 4 output bits per input bit, MSB of the nibble first. */
inline void convolutional_encode(const uint8_t* in_packed, uint32_t n_info, uint8_t* mother_out) {
    uint8_t state = 0;
    for (uint32_t step = 0; step < n_info; step++) {
        const uint8_t bit = bits_get(in_packed, step);
        const uint8_t pattern = kTetraNextOutput[state][bit];
        for (int b = 0; b < 4; b++) {
            mother_out[step * 4 + static_cast<uint32_t>(b)] =
                static_cast<uint8_t>((pattern >> (3 - b)) & 1);
        }
        state = kTetraNextState[state][bit];
    }
}

/* Soft-ish Viterbi: each mother bit is 0, 1 or kTetraErased; erased bits
 * contribute no metric. Returns the surviving path cost. */
inline int viterbi_decode_cch(const uint8_t* mother,
                              uint32_t n_info,
                              uint8_t* bits_out,
                              uint8_t* trace_buffer) {
    constexpr uint16_t INF = 0x3FFF;
    uint16_t cost[16];
    uint16_t new_cost[16];

    for (int i = 0; i < 16; i++) cost[i] = INF;
    cost[0] = 0;

    for (uint32_t step = 0; step < n_info; step++) {
        for (int i = 0; i < 16; i++) new_cost[i] = INF;
        const uint8_t* rx = &mother[step * 4];

        for (int prev = 0; prev < 16; prev++) {
            if (cost[prev] >= INF) continue;

            for (int bit = 0; bit < 2; bit++) {
                const uint8_t pattern = kTetraNextOutput[prev][bit];
                const uint8_t next = kTetraNextState[prev][bit];

                uint16_t d = 0;
                for (int b = 0; b < 4; b++) {
                    if (rx[b] != kTetraErased && rx[b] != ((pattern >> (3 - b)) & 1)) d++;
                }

                const uint16_t total = static_cast<uint16_t>(cost[prev] + d);
                if (total < new_cost[next]) {
                    new_cost[next] = total;
                    trace_buffer[step * 16 + next] = static_cast<uint8_t>((prev << 1) | bit);
                }
            }
        }
        std::memcpy(cost, new_cost, sizeof(cost));
    }

    uint8_t best_state = 0;
    for (int i = 1; i < 16; i++) {
        if (cost[i] < cost[best_state]) best_state = static_cast<uint8_t>(i);
    }

    uint8_t state = best_state;
    std::memset(bits_out, 0, (n_info + 7) >> 3);

    for (int step = static_cast<int>(n_info) - 1; step >= 0; step--) {
        const uint8_t packed = trace_buffer[static_cast<size_t>(step) * 16 + state];
        const uint8_t prev = static_cast<uint8_t>((packed >> 1) & 0x0F);
        const uint8_t bit = static_cast<uint8_t>(packed & 0x01);

        if (bit) bits_out[step >> 3] = static_cast<uint8_t>(bits_out[step >> 3] | (1 << (7 - (step & 7))));
        state = prev;
    }

    return cost[best_state];
}

/* ===========================================================================
 * Bursts handed from the demodulator to the channel decoder
 * (upstream TetraBurstMessage / TetraDnbMessage)
 * ===========================================================================*/

struct SyncBurst {
    std::array<uint8_t, 63> payload{};  /* 500 bits, MSB-first */
    bool inverted{false};
    uint8_t sync_errors{0};
};

struct NormalBurst {
    std::array<uint8_t, 54> payload{};  /* two 216-bit blocks, MSB-first */
    bool inverted{false};
    uint8_t errors{0};
    bool p_train{false};
};

/* ===========================================================================
 * Channel decoder (ui_tetra_rx.cpp TetraChannelDecoder)
 * ===========================================================================*/

class ChannelDecoder {
   public:
    struct Result {
        enum class Type : uint8_t { None, Sync, SyncFull, Dnb };
        Type type{Type::None};

        bool is_ok{false};
        uint8_t sync_errors{0};
        bool inverted{false};

        uint16_t crc{0};
        int cost{0};

        uint8_t timeslot{0xFF};
        uint8_t frame_number{0xFF};
        uint16_t mcc{0xFFFF};
        uint16_t mnc{0xFFFF};
        uint8_t bcc{0xFF};

        uint8_t pdu_type{0xFF};
        uint16_t la{0xFFFF};
        uint8_t encryption{0xFF};

        uint8_t cmce_type{0xFF};
        uint16_t call_id{0xFFFF};
        uint32_t calling_ssi{0};

        std::array<uint8_t, 34> payload{};
    };

    /* Type-2 block geometry, all upstream's. type2 = type1 + 16 CRC + 4 tail. */
    static constexpr size_t BLK1_OFFSET_BITS = 94;
    static constexpr size_t BLK1_LEN_BITS = 120;
    static constexpr size_t TMO_BLK2_OFFSET_BITS = 282;
    static constexpr size_t BLK2_LEN_BITS = 216;
    static constexpr size_t SB1_TYPE1_BITS = 60;
    static constexpr size_t SB1_TYPE2_BITS = 80;
    static constexpr size_t SB1_INTERLEAVE_A = 11;
    static constexpr size_t SB2_TYPE1_BITS = 124;
    static constexpr size_t SB2_TYPE2_BITS = 144;
    static constexpr size_t SB2_INTERLEAVE_A = 101;
    static constexpr size_t SCH_F_LEN_BITS = 432;
    static constexpr size_t SCH_F_TYPE1_BITS = 268;
    static constexpr size_t SCH_F_TYPE2_BITS = 288;
    static constexpr size_t SCH_F_INTERLEAVE_A = 103;

    static std::string pdu_name(uint8_t pdu_type) {
        switch (pdu_type) {
            case 0: return "MAC-RESOURCE";
            case 1: return "MAC-FRAG";
            case 2: return "SYSINFO/BCAST";
            case 3: return "MAC-U-SIGNAL";
            default: return "UNK_" + std::to_string(pdu_type);
        }
    }

    static std::string cmce_name(uint8_t cmce_type) {
        switch (cmce_type) {
            case 0: return "ALERTING";
            case 1: return "CALL PROCEEDING";
            case 2: return "CONNECT";
            case 3: return "CONNECT ACK";
            case 4: return "DISCONNECT";
            case 5: return "INFO";
            case 6: return "RELEASE";
            case 7: return "SETUP";
            case 8: return "STATUS";
            case 9: return "TX CEASED";
            case 10: return "TX CONTINUE";
            case 11: return "TX GRANTED";
            case 12: return "TX INTERRUPT";
            case 13: return "TX WAIT";
            case 14: return "SDS DATA";
            case 15: return "FACILITY";
            case 16: return "CALL RESTORE";
            case 31: return "NOT SUPPORTED";
            default: return "CMCE:" + std::to_string(cmce_type);
        }
    }

    Result decode_burst(const SyncBurst& burst);
    Result decode_dnb(const NormalBurst& burst);

    /* One type-2 block: descramble, deinterleave, depuncture, Viterbi, CRC.
     * Public because the tests drive it directly with a block they built. */
    bool decode_block(const uint8_t* burst,
                      size_t offset,
                      size_t len_t5,
                      size_t type1_bits,
                      size_t type2_bits,
                      size_t interleave_a,
                      uint8_t* out,
                      size_t out_bytes,
                      uint16_t& crc,
                      int& cost,
                      uint32_t scramb_init);

    static void parse_sync_pdu(Result& result);
    static void parse_mac_pdu(Result& result);

    /* MSB-first field read, upstream TetraChannelDecoder::read_bits. */
    static uint32_t read_bits(const uint8_t* bytes, size_t bit, size_t count) {
        uint32_t value = 0;
        for (size_t i = 0; i < count; i++) {
            value <<= 1;
            value |= (bytes[(bit + i) >> 3] >> (7 - ((bit + i) & 7))) & 1;
        }
        return value;
    }

    bool network_synced() const { return network_synced_; }
    uint16_t last_mcc() const { return last_mcc_; }
    uint16_t last_mnc() const { return last_mnc_; }
    uint8_t last_bcc() const { return last_bcc_; }

    /* The traffic-channel scrambling seed upstream derives from the last
     * decoded SYNC PDU. */
    uint32_t tmo_dnb_seed() const {
        return (((static_cast<uint32_t>(last_mcc_) << 20) |
                 (static_cast<uint32_t>(last_mnc_) << 6) |
                 static_cast<uint32_t>(last_bcc_))
                << 2) |
               3;
    }

    void set_network(uint16_t mcc, uint16_t mnc, uint8_t bcc) {
        last_mcc_ = mcc;
        last_mnc_ = mnc;
        last_bcc_ = bcc;
        network_synced_ = true;
    }

   private:
    uint16_t last_mcc_{0};
    uint16_t last_mnc_{0};
    uint8_t last_bcc_{0};
    bool network_synced_{false};

    /* Viterbi traceback scratch; 16 states x up to 288 steps. Upstream sizes it
     * 9216 and keeps it as a member for the same reason. */
    uint8_t trace_buffer_[9216]{};
};

/* ===========================================================================
 * Demodulator (proc_tetra.cpp)
 * ===========================================================================*/

/* Upstream's Y (SB frame sync), N (normal training) and P (extended training)
 * sequences, and the offsets from each to the start of the data it delimits. */
constexpr uint64_t kYSync = 0x30673A7067ULL;
constexpr uint32_t kYSyncBits = 38;
constexpr uint32_t kSyncOffset = 214;
constexpr uint32_t kBurstBits = 500;
constexpr uint32_t kNSync = 0x343A74UL;
constexpr uint32_t kPSync = 0x1E90DEUL;
constexpr uint32_t kDnbTrainBits = 22;
constexpr uint32_t kDnbBlockBits = 216;
constexpr uint32_t kDnbBlock1Back = 230;
constexpr uint32_t kDnbBlock2Forward = 38;
constexpr uint32_t kYSyncMaxErrors = 4;
constexpr uint32_t kDnbTrainMaxErrors = 1;

constexpr uint32_t kTetraSymbolRate = 18000;
constexpr uint32_t kTetraChannelRate = 48000; /* upstream's channel_fs */

/* Upstream clamps the symbol-clock NCO increment to [1'600'000'000,
 * 1'620'000'000] around a nominal 1'610'612'736 (18 kBd at 48 kHz). Expressed
 * as fractions so the clamp still means the same thing at another channel
 * rate. */
constexpr double kTetraIncMinRatio = 1600000000.0 / 1610612736.0;
constexpr double kTetraIncMaxRatio = 1620000000.0 / 1610612736.0;

/* Timing-loop gain, in NCO increment counts per unit of normalised Gardner
 * error. Upstream computes (mid . (prompt - prev)) >> 16 on raw complex16
 * samples; at the ~2000-count amplitude its decimator chain delivers, that is
 * an effective gain near 2000^2 / 65536 = 61 on a unit-normalised error. */
constexpr float kTetraTimingGain = 64.0f;

/* Carrier-loop gains. Upstream: pll_freq += (err * 2) >> 8 and
 * pll_phase += ((err * 150) >> 8) + pll_freq, with both accumulators in
 * 2^-32 turn units and err a raw complex16 product. Converted to turns for a
 * unit-normalised err: 2^30/(128 * 2^32) = 1/512 and 150 * 2^30/(256 * 2^32). */
constexpr double kTetraPllBeta = 1.0 / 512.0;
constexpr double kTetraPllAlpha = 150.0 / 1024.0;
constexpr double kTetraPllFreqClamp = 400000000.0 / 4294967296.0;

/* Four-quadrant pi/4-DQPSK dibit map, verbatim from proc_tetra.cpp. `dot` is
 * the differential product current * conj(previous), so its argument is the
 * phase change carried by this symbol.
 *
 *      +pi/4  -> 00      +3pi/4 -> 01
 *      -3pi/4 -> 11      -pi/4  -> 10
 *
 * The bits are then emitted most significant first. */
inline uint8_t slice_dibit(float dot_i, float dot_q) {
    if (dot_i > 0 && dot_q > 0) return 0b00;
    if (dot_i < 0 && dot_q > 0) return 0b01;
    if (dot_i < 0 && dot_q < 0) return 0b11;
    return 0b10;
}

/* Upstream's carrier phase detector, same four-quadrant decision. Returns zero
 * for a symbol sitting exactly on a constellation point. */
inline float carrier_phase_error(float dot_i, float dot_q) {
    if (dot_i > 0 && dot_q > 0) return dot_q - dot_i;
    if (dot_i < 0 && dot_q > 0) return dot_q + dot_i;
    if (dot_i < 0 && dot_q < 0) return -dot_q + dot_i;
    return -dot_q - dot_i;
}

class Demodulator {
   public:
    using SyncHandler = std::function<void(const SyncBurst&)>;
    using DnbHandler = std::function<void(const NormalBurst&)>;

    void configure(float channel_rate_hz, float symbol_rate_hz = kTetraSymbolRate);
    void reset();

    void set_sync_handler(SyncHandler h) { on_sync_ = std::move(h); }
    void set_dnb_handler(DnbHandler h) { on_dnb_ = std::move(h); }
    void set_dnb_enabled(bool enabled) { dnb_enabled_ = enabled; }

    void process(const dsp::cfloat* in, size_t count);

    uint64_t bit_count() const { return bit_count_; }
    uint32_t symbol_phase_inc() const { return symbol_phase_inc_; }
    double carrier_frequency_turns() const { return pll_freq_; }

   private:
    void process_symbol(dsp::cfloat sample);
    void push_bit(uint8_t bit);
    uint8_t history_bit(uint64_t absolute_bit) const;
    void emit_sync_burst();
    void emit_dnb_burst();

    SyncHandler on_sync_{};
    DnbHandler on_dnb_{};
    bool dnb_enabled_{true};

    float channel_rate_{static_cast<float>(kTetraChannelRate)};
    float symbol_rate_{static_cast<float>(kTetraSymbolRate)};

    uint32_t symbol_phase_{0};
    uint32_t symbol_phase_inc_{0};
    uint32_t symbol_phase_inc_nominal_{0};
    uint32_t symbol_phase_inc_min_{0};
    uint32_t symbol_phase_inc_max_{0};

    dsp::cfloat prompt_{0.0f, 0.0f};
    dsp::cfloat prev_prompt_{0.0f, 0.0f};
    dsp::cfloat mid_{0.0f, 0.0f};
    dsp::cfloat delay_{0.0f, 0.0f};

    double pll_phase_{0.0}; /* turns, [0,1) */
    double pll_freq_{0.0};  /* turns per sample */

    uint64_t sync_register_{0};
    std::array<uint8_t, 128> history_{}; /* 1024 bits */
    size_t history_write_idx_{0};
    uint64_t bit_count_{0};
    bool configured_{false};

    struct PendingSync {
        bool valid{false};
        uint64_t burst_start{0};
        uint64_t ready_at{0};
        bool inverted{false};
        uint8_t errors{0};
    } pending_sync_{};

    struct PendingDnb {
        bool valid{false};
        uint64_t block1_start{0};
        uint64_t block2_start{0};
        uint64_t ready_at{0};
        bool inverted{false};
        bool p_train{false};
        uint8_t errors{0};
    } pending_dnb_{};
};

/* ===========================================================================
 * ChannelDecoder implementation
 *
 * Kept inline so the decoder is linkable on its own — the tests exercise it
 * without dragging in the view and the whole UI layer.
 * ===========================================================================*/

inline bool ChannelDecoder::decode_block(const uint8_t* burst,
                                         size_t offset,
                                         size_t len_t5,
                                         size_t type1_bits,
                                         size_t type2_bits,
                                         size_t interleave_a,
                                         uint8_t* out,
                                         size_t out_bytes,
                                         uint16_t& crc,
                                         int& cost,
                                         uint32_t scramb_init) {
    std::array<uint8_t, 64> t5{};
    std::array<uint8_t, 64> t3{};
    std::array<uint8_t, 1200> mother{};
    std::array<uint8_t, 64> type2{};

    std::memset(out, 0, out_bytes);

    for (size_t i = 0; i < len_t5; i++) {
        bits_set(t5.data(), i, bits_get(burst, offset + i) != 0);
    }

    descramble(t5.data(), len_t5, scramb_init);
    block_deinterleave(t5.data(), t3.data(), static_cast<uint32_t>(len_t5),
                       static_cast<uint32_t>(interleave_a));
    depuncture_2_3(t3.data(), mother.data(), static_cast<uint32_t>(len_t5));

    cost = viterbi_decode_cch(mother.data(), static_cast<uint32_t>(type2_bits), type2.data(),
                              trace_buffer_);
    crc = crc16_itut_bits(type2.data(), static_cast<uint32_t>(type1_bits + 16));

    for (size_t i = 0; i < type1_bits; i++) {
        bits_set(out, i, bits_get(type2.data(), i) != 0);
    }
    return crc == kTetraCrcOk;
}

inline ChannelDecoder::Result ChannelDecoder::decode_burst(const SyncBurst& burst) {
    Result result{};
    result.sync_errors = burst.sync_errors;
    result.inverted = burst.inverted;

    /* 1. SCH/S — the synchronisation block, always scrambled with the BSCH
     * seed because the receiver cannot know the colour code yet. */
    bool ok = decode_block(burst.payload.data(), BLK1_OFFSET_BITS, BLK1_LEN_BITS,
                           SB1_TYPE1_BITS, SB1_TYPE2_BITS, SB1_INTERLEAVE_A,
                           result.payload.data(), result.payload.size(),
                           result.crc, result.cost, kTetraScrambInitBsch);

    if (!ok) return result;

    result.type = Result::Type::Sync;
    result.is_ok = true;
    parse_sync_pdu(result);
    set_network(result.mcc, result.mnc, result.bcc);

    const uint16_t current_mcc = result.mcc;
    const uint16_t current_mnc = result.mnc;
    const uint8_t current_bcc = result.bcc;
    const uint8_t current_enc = result.encryption;

    /* 2. SCH/H — the half-slot block that shares the burst. */
    uint16_t h_crc = 0;
    int h_cost = 0;

    ok = decode_block(burst.payload.data(), TMO_BLK2_OFFSET_BITS, BLK2_LEN_BITS,
                      SB2_TYPE1_BITS, SB2_TYPE2_BITS, SB2_INTERLEAVE_A,
                      result.payload.data(), result.payload.size(),
                      h_crc, h_cost, kTetraScrambInitBsch);

    if (ok) {
        result.type = Result::Type::SyncFull;
        result.crc = h_crc;
        result.cost = h_cost;

        result.mcc = current_mcc;
        result.mnc = current_mnc;
        result.bcc = current_bcc;
        result.encryption = current_enc;

        parse_mac_pdu(result);
    } else {
        result.encryption = current_enc;
    }

    return result;
}

inline ChannelDecoder::Result ChannelDecoder::decode_dnb(const NormalBurst& burst) {
    Result result{};
    result.inverted = burst.inverted;

    /* Without a SYNC PDU there is no scrambling seed, so nothing can be read. */
    if (!network_synced_) return result;

    const uint32_t seed = tmo_dnb_seed();

    /* 1. Full-slot SCH/F across both blocks. */
    bool ok = decode_block(burst.payload.data(), 0, SCH_F_LEN_BITS, SCH_F_TYPE1_BITS,
                           SCH_F_TYPE2_BITS, SCH_F_INTERLEAVE_A, result.payload.data(),
                           result.payload.size(), result.crc, result.cost, seed);

    /* 2. SCH/HD in block 1. */
    if (!ok) {
        ok = decode_block(burst.payload.data(), 0, BLK2_LEN_BITS, SB2_TYPE1_BITS,
                          SB2_TYPE2_BITS, SB2_INTERLEAVE_A, result.payload.data(),
                          result.payload.size(), result.crc, result.cost, seed);
    }

    /* 3. SCH/HD in block 2. */
    if (!ok) {
        ok = decode_block(burst.payload.data(), 216, BLK2_LEN_BITS, SB2_TYPE1_BITS,
                          SB2_TYPE2_BITS, SB2_INTERLEAVE_A, result.payload.data(),
                          result.payload.size(), result.crc, result.cost, seed);
    }

    if (ok) {
        result.type = Result::Type::Dnb;
        result.is_ok = true;
        parse_mac_pdu(result);
    }

    return result;
}

inline void ChannelDecoder::parse_sync_pdu(Result& result) {
    /* SYNC PDU, EN 300 392-2 21.4.4.1: 4-bit system code, then colour code,
     * timeslot, frame number, ... */
    const uint8_t* p = result.payload.data();
    result.bcc = static_cast<uint8_t>(read_bits(p, 4, 6));
    result.timeslot = static_cast<uint8_t>(read_bits(p, 10, 2));
    result.frame_number = static_cast<uint8_t>(read_bits(p, 12, 5));
    result.encryption = static_cast<uint8_t>(read_bits(p, 30, 1));
    result.mcc = static_cast<uint16_t>(read_bits(p, 31, 10));
    result.mnc = static_cast<uint16_t>(read_bits(p, 41, 14));
}

inline void ChannelDecoder::parse_mac_pdu(Result& result) {
    const uint8_t* p = result.payload.data();
    result.pdu_type = static_cast<uint8_t>(read_bits(p, 0, 2));

    if (result.pdu_type == 0) {  /* MAC-RESOURCE */
        result.encryption = static_cast<uint8_t>(read_bits(p, 4, 2));
        const uint8_t length_ind = static_cast<uint8_t>(read_bits(p, 7, 6));

        if (result.encryption == 0 && length_ind > 0 && length_ind < 62) {
            size_t offset = 13;
            const uint8_t addr_type = static_cast<uint8_t>(read_bits(p, offset, 3));
            offset += 3;

            if (addr_type == 1) {
                result.calling_ssi = read_bits(p, offset, 24);
                offset += 24;
            } else if (addr_type == 2) {
                offset += 10;
            } else if (addr_type == 3 || addr_type == 4) {
                result.calling_ssi = read_bits(p, offset, 24);
                offset += 24;
            } else if (addr_type == 5) {
                offset += 34;
            } else if (addr_type == 6) {
                offset += 30;
            } else if (addr_type == 7) {
                offset += 34;
            }

            /* Optional fields, skipped by their presence flags. */
            if (read_bits(p, offset++, 1)) offset += 4;  /* power control */
            if (read_bits(p, offset++, 1)) offset += 8;  /* slot granting  */

            const uint8_t chan_alloc = static_cast<uint8_t>(read_bits(p, offset++, 1));

            if (chan_alloc == 0) {
                const uint8_t llc_type = static_cast<uint8_t>(read_bits(p, offset, 4));
                offset += 4;

                if (llc_type == 0 || llc_type == 1)
                    offset += 2;
                else if (llc_type == 2 || llc_type == 3 || llc_type == 6 || llc_type == 7)
                    offset += 1;

                const uint8_t mle_type = static_cast<uint8_t>(read_bits(p, offset, 3));
                offset += 3;

                if (mle_type == 1) {  /* CMCE */
                    result.cmce_type = static_cast<uint8_t>(read_bits(p, offset, 5));
                    offset += 5;
                    result.call_id = static_cast<uint16_t>(read_bits(p, offset, 14));
                }
            }
        }
    } else if (result.pdu_type == 2) {  /* SYSINFO / BCAST */
        const uint8_t bcast_type = static_cast<uint8_t>(read_bits(p, 2, 2));
        if (bcast_type == 0) {
            result.la = static_cast<uint16_t>(read_bits(p, 82, 14));
            result.encryption = static_cast<uint8_t>(read_bits(p, 122, 1));
        }
    }
}

/* ===========================================================================
 * Demodulator implementation
 * ===========================================================================*/

inline void Demodulator::configure(float channel_rate_hz, float symbol_rate_hz) {
    channel_rate_ = channel_rate_hz;
    symbol_rate_ = symbol_rate_hz;

    const double nominal = (channel_rate_hz > 0.0f)
                               ? (static_cast<double>(symbol_rate_hz) * 4294967296.0 /
                                  static_cast<double>(channel_rate_hz))
                               : 0.0;
    symbol_phase_inc_nominal_ = static_cast<uint32_t>(nominal);
    symbol_phase_inc_min_ = static_cast<uint32_t>(nominal * kTetraIncMinRatio);
    symbol_phase_inc_max_ = static_cast<uint32_t>(nominal * kTetraIncMaxRatio);
    configured_ = symbol_phase_inc_nominal_ != 0;
    reset();
}

inline void Demodulator::reset() {
    symbol_phase_ = 0;
    symbol_phase_inc_ = symbol_phase_inc_nominal_;
    prompt_ = prev_prompt_ = mid_ = delay_ = dsp::cfloat{0.0f, 0.0f};
    pll_phase_ = 0.0;
    pll_freq_ = 0.0;
    sync_register_ = 0;
    history_.fill(0);
    history_write_idx_ = 0;
    bit_count_ = 0;
    pending_sync_ = PendingSync{};
    pending_dnb_ = PendingDnb{};
}

inline void Demodulator::process(const dsp::cfloat* in, size_t count) {
    if (!configured_ || in == nullptr) return;

    for (size_t i = 0; i < count; i++) {
        /* Carrier PLL derotation. Upstream indexes a 256-entry sine table with
         * the top byte of a 32-bit phase; the host has the FPU for the real
         * thing, and the loop is otherwise identical. */
        const double angle = -2.0 * 3.14159265358979323846 * pll_phase_;
        const auto rot = dsp::cfloat{static_cast<float>(std::cos(angle)),
                                     static_cast<float>(std::sin(angle))};
        const dsp::cfloat sample = in[i] * rot;

        const uint32_t old_phase = symbol_phase_;
        symbol_phase_ += symbol_phase_inc_;

        /* Half-symbol instant, for Gardner. */
        if ((old_phase < 0x80000000u) && (symbol_phase_ >= 0x80000000u)) {
            mid_ = sample;
        }

        /* Full-symbol instant. */
        if (symbol_phase_ < old_phase) {
            prev_prompt_ = prompt_;
            prompt_ = sample;

            /* Gardner timing error on unit-normalised samples, so the loop
             * gain does not depend on the input level (see the header). */
            const float mag_mid = std::abs(mid_);
            const float mag_p = std::abs(prompt_);
            const float mag_pp = std::abs(prev_prompt_);
            if (mag_mid > 1e-12f && mag_p > 1e-12f && mag_pp > 1e-12f) {
                const dsp::cfloat mu = mid_ / mag_mid;
                const dsp::cfloat pu = prompt_ / mag_p;
                const dsp::cfloat ppu = prev_prompt_ / mag_pp;
                const float error = mu.real() * (pu.real() - ppu.real()) +
                                    mu.imag() * (pu.imag() - ppu.imag());

                double inc = static_cast<double>(symbol_phase_inc_) +
                             static_cast<double>(error) * kTetraTimingGain;
                if (inc < symbol_phase_inc_min_) inc = symbol_phase_inc_min_;
                if (inc > symbol_phase_inc_max_) inc = symbol_phase_inc_max_;
                symbol_phase_inc_ = static_cast<uint32_t>(inc);
            }

            process_symbol(prompt_);
        }
    }
}

inline void Demodulator::process_symbol(dsp::cfloat current) {
    /* Differential demodulation: dot = current * conj(previous). */
    float dot_i = current.real() * delay_.real() + current.imag() * delay_.imag();
    float dot_q = current.imag() * delay_.real() - current.real() * delay_.imag();
    delay_ = current;

    const float mag = std::sqrt(dot_i * dot_i + dot_q * dot_q);
    if (mag > 1e-12f) {
        dot_i /= mag;
        dot_q /= mag;
    }

    /* Carrier loop. The detector returns zero on an exact constellation
     * point, so a clean signal leaves the loop where it is. */
    const float phase_err = carrier_phase_error(dot_i, dot_q);

    pll_freq_ += static_cast<double>(phase_err) * kTetraPllBeta;
    if (pll_freq_ > kTetraPllFreqClamp) pll_freq_ = kTetraPllFreqClamp;
    if (pll_freq_ < -kTetraPllFreqClamp) pll_freq_ = -kTetraPllFreqClamp;

    pll_phase_ += static_cast<double>(phase_err) * kTetraPllAlpha + pll_freq_;
    pll_phase_ -= std::floor(pll_phase_);

    const uint8_t dibit = slice_dibit(dot_i, dot_q);

    for (int b = 1; b >= 0; b--) {
        push_bit(static_cast<uint8_t>((dibit >> b) & 0x01));
    }
}

inline void Demodulator::push_bit(uint8_t bit_val) {
    const size_t byte_idx = (history_write_idx_ / 8) % 128;
    if ((history_write_idx_ % 8) == 0) history_[byte_idx] = 0;
    history_[byte_idx] =
        static_cast<uint8_t>(history_[byte_idx] | (bit_val << (7 - (history_write_idx_ % 8))));
    history_write_idx_ = (history_write_idx_ + 1) % 1024;
    bit_count_++;

    sync_register_ = (sync_register_ << 1) | bit_val;

    /* --- SB frame synchronisation, 38 bits, distance <= 4 either polarity --- */
    const uint64_t sync = sync_register_ & 0x3FFFFFFFFFULL;
    const uint32_t err_pos = static_cast<uint32_t>(std::popcount(sync ^ kYSync));
    const uint32_t err_neg =
        static_cast<uint32_t>(std::popcount(sync ^ (~kYSync & 0x3FFFFFFFFFULL)));

    if (!pending_sync_.valid && bit_count_ >= kYSyncBits &&
        (err_pos <= kYSyncMaxErrors || err_neg <= kYSyncMaxErrors)) {
        const uint64_t sync_start = bit_count_ - kYSyncBits;
        if (sync_start >= kSyncOffset) {
            pending_sync_.valid = true;
            pending_sync_.burst_start = sync_start - kSyncOffset;
            pending_sync_.ready_at = pending_sync_.burst_start + kBurstBits;
            pending_sync_.inverted = err_neg < err_pos;
            pending_sync_.errors = static_cast<uint8_t>(std::min(err_pos, err_neg));
        }
    }

    /* --- DNB training sequences, 22 bits, distance <= 1 either polarity --- */
    if (dnb_enabled_) {
        const uint32_t train_mask = (1UL << kDnbTrainBits) - 1;
        const uint32_t train = static_cast<uint32_t>(sync_register_) & train_mask;
        const uint32_t n_err_pos = static_cast<uint32_t>(std::popcount(train ^ kNSync));
        const uint32_t n_err_neg =
            static_cast<uint32_t>(std::popcount(train ^ (~kNSync & train_mask)));
        const uint32_t p_err_pos = static_cast<uint32_t>(std::popcount(train ^ kPSync));
        const uint32_t p_err_neg =
            static_cast<uint32_t>(std::popcount(train ^ (~kPSync & train_mask)));

        if (!pending_dnb_.valid && bit_count_ >= kDnbTrainBits &&
            (n_err_pos <= kDnbTrainMaxErrors || n_err_neg <= kDnbTrainMaxErrors ||
             p_err_pos <= kDnbTrainMaxErrors || p_err_neg <= kDnbTrainMaxErrors)) {
            const bool p_train = std::min(p_err_pos, p_err_neg) < std::min(n_err_pos, n_err_neg);
            const uint32_t pos_err = p_train ? p_err_pos : n_err_pos;
            const uint32_t neg_err = p_train ? p_err_neg : n_err_neg;

            const uint64_t train_start = bit_count_ - kDnbTrainBits;
            if (train_start >= kDnbBlock1Back) {
                pending_dnb_.valid = true;
                pending_dnb_.block1_start = train_start - kDnbBlock1Back;
                pending_dnb_.block2_start = train_start + kDnbBlock2Forward;
                pending_dnb_.ready_at = pending_dnb_.block2_start + kDnbBlockBits;
                pending_dnb_.inverted = neg_err < pos_err;
                pending_dnb_.p_train = p_train;
                pending_dnb_.errors = static_cast<uint8_t>(std::min(pos_err, neg_err));
            }
        }

        if (pending_dnb_.valid && bit_count_ >= pending_dnb_.ready_at) emit_dnb_burst();
    }

    if (pending_sync_.valid && bit_count_ >= pending_sync_.ready_at) emit_sync_burst();
}

inline uint8_t Demodulator::history_bit(uint64_t absolute_bit) const {
    const size_t p = static_cast<size_t>(absolute_bit % 1024);
    return static_cast<uint8_t>((history_[p >> 3] >> (7 - (p & 7))) & 1);
}

inline void Demodulator::emit_sync_burst() {
    SyncBurst burst{};
    for (size_t i = 0; i < kBurstBits; i++) {
        uint8_t b = history_bit(pending_sync_.burst_start + i);
        if (pending_sync_.inverted) b ^= 1;
        burst.payload[i >> 3] = static_cast<uint8_t>(burst.payload[i >> 3] | (b << (7 - (i & 7))));
    }
    burst.inverted = pending_sync_.inverted;
    burst.sync_errors = pending_sync_.errors;
    pending_sync_.valid = false;
    if (on_sync_) on_sync_(burst);
}

inline void Demodulator::emit_dnb_burst() {
    NormalBurst burst{};
    for (size_t i = 0; i < kDnbBlockBits; i++) {
        uint8_t b = history_bit(pending_dnb_.block1_start + i);
        if (pending_dnb_.inverted) b ^= 1;
        burst.payload[i >> 3] = static_cast<uint8_t>(burst.payload[i >> 3] | (b << (7 - (i & 7))));
    }
    for (size_t i = 0; i < kDnbBlockBits; i++) {
        uint8_t b = history_bit(pending_dnb_.block2_start + i);
        if (pending_dnb_.inverted) b ^= 1;
        const size_t out_bit = kDnbBlockBits + i;
        burst.payload[out_bit >> 3] =
            static_cast<uint8_t>(burst.payload[out_bit >> 3] | (b << (7 - (out_bit & 7))));
    }
    burst.inverted = pending_dnb_.inverted;
    burst.errors = pending_dnb_.errors;
    burst.p_train = pending_dnb_.p_train;
    pending_dnb_.valid = false;
    if (on_dnb_) on_dnb_(burst);
}

}  // namespace tetra

/* ===========================================================================
 * View
 * ===========================================================================*/

class TetraRxView : public ui::View {
   public:
    TetraRxView();
    ~TetraRxView() override;

    TetraRxView(const TetraRxView&) = delete;
    TetraRxView& operator=(const TetraRxView&) = delete;

    std::string title() const override { return "Tetra RX"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void rebuild_channel_filter();
    void on_sync_burst(const tetra::SyncBurst& burst);
    void on_dnb_burst(const tetra::NormalBurst& burst);
    void update_counters();

    radio::ReceiverModel& receiver_;

    tetra::Demodulator demod_{};
    tetra::ChannelDecoder decoder_{};

    std::vector<dsp::cfloat> samples_{};
    std::vector<dsp::cfloat> mixed_{};
    std::vector<dsp::cfloat> channel_{};
    dsp::FirDecimateC channel_filter_{};
    double filter_input_rate_{0.0};
    double channel_rate_{static_cast<double>(tetra::kTetraChannelRate)};

    uint32_t sync_count_{0};
    uint32_t valid_count_{0};
    uint32_t h_valid_count_{0};
    uint32_t dnb_count_{0};
    uint8_t last_sync_errors_{0};
    bool counters_dirty_{true};

    ui::FrequencyField field_frequency_{{0, 0}};
    ui::FrequencyStepView step_view_{{84, 0}, field_frequency_};
    ui::NumberField field_gain_{{184, 0}, 3, {0, 76}, 1, ' '};

    ui::Text text_mcc_{{0, 18, 120, 16}, "MCC: ---"};
    ui::Text text_mnc_{{120, 18, 120, 16}, "MNC: ---"};
    ui::Text text_ts_{{0, 34, 120, 16}, "TS:  -"};
    ui::Text text_fn_{{120, 34, 120, 16}, "FN:  -"};
    ui::Text text_bcc_{{0, 50, 120, 16}, "BCC: -"};
    ui::Text text_enc_{{120, 50, 120, 16}, "ENC: -"};
    ui::Text text_la_{{0, 66, 120, 16}, "LA:  ----"};
    ui::Text text_pdu_{{120, 66, 120, 16}, "PDU: ----"};
    ui::Text text_debug_{{0, 82, 240, 16}, "Syn: 0, V: 0, H: 0, E:0"};

    ui::Text text_tap_note_{{0, 100, 240, 16}, ""};

    ui::Console console_{{0, 118, 240, 186}};
};

}  // namespace app

#endif /*__MB200_UI_TETRA_RX_H__*/
