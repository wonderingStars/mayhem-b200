/*
 * mayhem-b200 — FLEX pager receiver.
 *
 * Ported from the PortaPack firmware:
 *
 *   application/external/flex_rx/ui_flex_rx.*  -> the view and its formatting
 *   baseband/proc_flex.*                       -> the whole decoder: symbol
 *                                                 recovery, the SYNC1/FIW/
 *                                                 SYNC2/DATA state machine,
 *                                                 the four interleaved phase
 *                                                 buffers, BCH correction and
 *                                                 the BIW/address/vector
 *                                                 parsers (multimon-ng's
 *                                                 demod_flex.c lineage)
 *   common/flex_defs.hpp                       -> FlexPacket
 *
 * FLEX (TIA/EIA-STD-43A / Motorola FLEX) on the air:
 *
 *   - 2FSK or 4FSK, at 1600 or 3200 symbols/s, giving 1600, 3200 or 6400 bps.
 *   - Time is divided into 15 cycles of 128 frames; each frame is 1.875 s.
 *   - A frame starts with SYNC1: a bit-reversed 16-bit mode code A, the
 *     32-bit marker 0xA6C6AAAA, then the complement of A. The mode code says
 *     which speed and how many levels the frame's data uses.
 *   - Then the Frame Information Word (FIW), a BCH(31,21) word carrying the
 *     cycle and frame numbers with a 4-bit nibble checksum.
 *   - Then SYNC2 (25 ms), then 1.76 s of data.
 *   - Data is interleaved over up to four "phases" (A/B/C/D) of 88 BCH(31,21)
 *     words each. 2FSK/1600 uses phase A alone; 4FSK/3200 uses all four.
 *   - Each phase begins with Block Information Word 1, which gives the offset
 *     of the address field and of the vector field. Addresses are one or two
 *     words; each has a matching vector word saying what kind of page it is
 *     and where its message words live.
 *
 * Host pipeline, replacing the M4 baseband processor:
 *
 *   USRP IQ -> NCO -> 2-stage decimating FIR -> FM discriminator
 *           -> flex::Decoder (DC block, symbol clock, 4-level slicer,
 *              sync/FIW/data state machine, BCH, page parsers) -> console
 *
 * The one departure from upstream is that the decoder is parameterised by its
 * input sample rate instead of assuming 24 kHz, because the B200's decimation
 * cannot always land there. Everything protocol-level — the sync codes, the
 * BCH bit order, the checksum rules, the address ranges, the vector layouts,
 * the BCD table — is upstream's, bit for bit.
 *
 * Copyright (C) 1996 Thomas Sailer (BCH tables, multimon lineage)
 * Copyright (C) 2012-2014 Elias Oenal (multimon-ng demod_flex.c)
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_FLEX_RX_H__
#define __MB200_UI_FLEX_RX_H__

#include "../core/string_format.hpp"
#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace radio {
class ReceiverModel;
}

namespace flex {

/* ===========================================================================
 * Decoder constants (proc_flex.cpp)
 * ===========================================================================*/

constexpr double dc_offset_filter = 0.010;
constexpr double phase_locked_rate = 0.045;
constexpr double phase_unlocked_rate = 0.050;
constexpr int lock_len = 24;
constexpr int idle_threshold = 0;
constexpr int demod_timeout = 100;
constexpr uint32_t sync_marker = 0xA6C6AAAAu;
constexpr double slice_threshold = 0.667;

/* Words per phase in one frame's data field. */
constexpr int phase_words = 88;

/* BCD characters for FLEX numeric messages, index 0-15. */
inline const char* bcd_chars() {
    static const char table[17] = "0123456789.U -][";
    return table;
}

enum class PageType : uint8_t {
    SECURE,
    SHORT_INSTRUCTION,
    TONE,
    STANDARD_NUMERIC,
    SPECIAL_NUMERIC,
    ALPHANUMERIC,
    BINARY,
    NUMBERED_NUMERIC
};

enum class State : uint8_t { SYNC1,
                             FIW,
                             SYNC2,
                             DATA };

enum class AddrType : uint8_t {
    SHORT,      /* normal individual */
    LONG,       /* 9-10 digit */
    TEMPORARY,  /* 0x1F7800-0F, 16 group slots */
    OPERATOR,   /* 0x1F7810-1F, system messages */
    NETWORK,    /* 0x1F6800-77FF */
    INFO_SVC,   /* 0x1F2800-67FF */
    RESERVED,
    UNKNOWN
};

/* Packet handed to the app. Field-for-field common/flex_defs.hpp. */
struct FlexPacket {
    uint32_t bitrate = 0;   /* 1600, 3200 or 6400 */
    int64_t capcode = 0;    /* long addresses reach 4,297,068,542 */
    uint32_t function = 0;  /* 0-3, or the BIW word index when type == 9 */
    uint32_t type = 0;      /* 0=SEC 1=INS 2=TON 3=NUM 4=SNUM 5=ALN 6=HEX
                             * 7=NNUM 8=SHORT 9=BIW */
    char message[256] = {};
    uint32_t status = 0;
    uint8_t cycle = 0;
    uint8_t frame = 0;
    char phase = 'A';
    uint8_t is_inverted = 0;
    uint8_t addr_type = 0;

    /* Fragment flags (ALN/SEC/HEX) */
    uint8_t frag = 0;
    uint8_t more_frag = 0;
    uint8_t seq = 0;
    uint8_t is_new = 0;
    uint8_t maildrop = 0;
    uint8_t sig = 0;
    uint8_t has_flags = 0;
    uint8_t sec_enc = 0;
    uint8_t nnum_s = 0;
    uint8_t fiw_roaming = 0;
    uint8_t is_priority = 0;

    /* BIW raw values (type == 9). biw_field says what they mean:
     * 0=SSID1 1=DATE 2=TIME 5=SYSINFO 7=SSID2 */
    uint8_t biw_field = 0;
    uint16_t biw_v1 = 0;
    uint16_t biw_v2 = 0;
    uint16_t biw_v3 = 0;
    uint16_t biw_v4 = 0;
};

/* --- small helpers -------------------------------------------------------- */

inline unsigned int popcount32(uint32_t n) {
    unsigned int c = 0;
    while (n) {
        n &= (n - 1u);
        ++c;
    }
    return c;
}

inline uint32_t bit_reverse_32(uint32_t x) {
    x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
    x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
    x = ((x >> 4) & 0x0F0F0F0Fu) | ((x & 0x0F0F0F0Fu) << 4);
    x = ((x >> 8) & 0x00FF00FFu) | ((x & 0x00FF00FFu) << 8);
    x = (x >> 16) | (x << 16);
    return x;
}

/* ===========================================================================
 * BCH(31,21) + parity
 *
 * The same code POCSAG uses. proc_flex.cpp carries its own copy of the tables
 * rather than link pocsag.cpp, and this port does the same so the two apps
 * stay independent files (see doc/PORTING.md).
 *
 * FLEX transmits each word least-significant-bit first, so a FLEX word has
 * the first transmitted bit in bit 0; the tables expect it in bit 31. Hence
 * the bit reversal in fix_errors().
 * ===========================================================================*/

class BchEcc {
   public:
    BchEcc() { setup(); }

    BchEcc(const BchEcc&) = delete;
    BchEcc& operator=(const BchEcc&) = delete;

    /* Corrects a POCSAG-order word in place. 0/1/2 = errors repaired,
     * 3 = uncorrectable. */
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

        if (synd != 0) {
            if (bch_[synd] != 0) {
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

    /* Fills the parity of a POCSAG-order word whose top 21 bits are payload. */
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

    /* Corrects a FLEX-order (LSB-first) word in place. */
    int fix_errors(uint32_t& word) const {
        uint32_t reversed = bit_reverse_32(word);
        const int result = error_correct(reversed);
        if (result <= 2) word = bit_reverse_32(reversed);
        return result;
    }

    /* Builds a FLEX-order word from 21 information bits. */
    uint32_t make_word(uint32_t info21) const {
        return bit_reverse_32(encode(bit_reverse_32(info21 & 0x001FFFFFu)));
    }

   private:
    void setup() {
        unsigned int srr = 0x3b4;
        unsigned int i, n, j, k;

        for (i = 0; i <= 20; i++) {
            ecs_[i] = srr;
            if ((srr & 0x01) != 0)
                srr = (srr >> 1) ^ 0x3B4;
            else
                srr = srr >> 1;
        }

        for (i = 0; i < 1024; i++) bch_[i] = 0;

        for (n = 0; n <= 20; n++)
            for (i = 0; i <= 20; i++) {
                j = (i << 5) + n;
                k = ecs_[n] ^ ecs_[i];
                bch_[k] = j + 0x2000;
            }

        for (n = 0; n <= 20; n++) {
            k = ecs_[n];
            j = n + (0x1f << 5);
            bch_[k] = j + 0x1000;
        }

        for (n = 0; n <= 20; n++)
            for (i = 0; i < 10; i++) {
                k = ecs_[n] ^ (1u << i);
                j = n + (0x1f << 5);
                bch_[k] = j + 0x2000;
            }

        for (n = 0; n < 10; n++) {
            k = 1u << n;
            bch_[k] = 0x3ff + 0x1000;
        }

        for (n = 0; n < 10; n++)
            for (i = 0; i < 10; i++)
                if (i != n) {
                    k = (1u << n) ^ (1u << i);
                    bch_[k] = 0x3ff + 0x2000;
                }
    }

    uint32_t ecs_[32]{};
    uint32_t bch_[1025]{};
};

/* ===========================================================================
 * Decoder state (proc_flex.hpp)
 * ===========================================================================*/

struct DemodParams {
    unsigned int sample_freq = 24000;
    double sample_last = 0.0;
    int locked = 0;
    int phase = 0;
    unsigned int sample_count = 0;
    unsigned int symbol_count = 0;
    double envelope_sum = 0.0;
    int envelope_count = 0;
    uint64_t lock_buf = 0;
    int symcount[4] = {0, 0, 0, 0};
    int timeout = 0;
    int nonconsec = 0;
    unsigned int baud = 1600;
};

struct Modulation {
    double symbol_rate = 0.0;
    double envelope = 0.0;
    double zero = 0.0;
};

struct StateInfo {
    unsigned int sync2_count = 0;
    unsigned int data_count = 0;
    unsigned int fiwcount = 0;
    State current = State::SYNC1;

    uint16_t sync2_shiftreg = 0;
    int sync2_c_pos = -1;
    int sync2_cinv_pos = -1;
};

struct SyncInfo {
    unsigned int sync = 0;
    unsigned int baud = 0;
    unsigned int levels = 0;
    unsigned int polarity = 0;
    uint64_t syncbuf = 0;
};

struct FrameInfoWord {
    uint32_t rawdata = 0;
    unsigned int checksum = 0;
    unsigned int cycleno = 0;
    unsigned int frameno = 0;
    unsigned int roaming = 0; /* bit 15 */
    unsigned int repeat = 0;  /* bit 16 */
    unsigned int traffic = 0; /* bits 17-20 */
};

struct PhaseBuffer {
    uint32_t buf[phase_words] = {};
    int idle_count = 0;
};

struct DataState {
    int phase_toggle = 0;
    unsigned int data_bit_counter = 0;
    PhaseBuffer a, b, c, d;
};

struct DecodeState {
    PageType type = PageType::ALPHANUMERIC;
    int long_address = 0;
    int64_t capcode = 0;
    AddrType addr_type = AddrType::SHORT;
    int is_priority = 0;
};

/* ===========================================================================
 * The decoder
 *
 * Members are public: the app only calls configure()/process(), but the tests
 * drive individual stages (sync check, FIW, a whole phase buffer) directly,
 * which is the only way to test a decoder like this without a transmitter.
 * ===========================================================================*/

class Decoder {
   public:
    using PacketHandler = std::function<void(const FlexPacket&)>;

    void configure(float sample_rate_hz) {
        demod.sample_freq = static_cast<unsigned int>(sample_rate_hz);
        if (demod.sample_freq == 0) demod.sample_freq = 24000;
        dc_alpha_ = static_cast<double>(demod.sample_freq) * dc_offset_filter;
        reset();
    }

    void set_packet_handler(PacketHandler h) { handler_ = std::move(h); }

    void reset() {
        demod = DemodParams{};
        demod.sample_freq = dc_alpha_ > 0.0
                                ? static_cast<unsigned int>(dc_alpha_ / dc_offset_filter)
                                : 24000u;
        modulation = Modulation{};
        state = StateInfo{};
        sync = SyncInfo{};
        fiw = FrameInfoWord{};
        data = DataState{};
        decode = DecodeState{};
    }

    void process(const float* audio, size_t count) {
        for (size_t i = 0; i < count; ++i) demodulate(static_cast<double>(audio[i]));
    }

    /* --- symbol recovery -------------------------------------------------- */

    void demodulate(double sample) {
        if (build_symbol(sample) != 1) return;

        demod.nonconsec = 0;
        demod.symbol_count++;

        /* Modal symbol over the samples taken inside this symbol period. */
        int decmax = 0;
        int modal_symbol = 0;
        for (int j = 0; j < 4; j++) {
            if (demod.symcount[j] > decmax) {
                modal_symbol = j;
                decmax = demod.symcount[j];
            }
        }
        demod.symcount[0] = demod.symcount[1] = demod.symcount[2] = demod.symcount[3] = 0;

        if (demod.locked) {
            handle_symbol(static_cast<unsigned char>(modal_symbol));
        } else {
            /* Look for the lock pattern. Symbols are mapped so that the two
             * outer levels each become a single 1 bit. */
            demod.lock_buf = (demod.lock_buf << 2) | static_cast<uint64_t>(modal_symbol ^ 0x1);
            const uint64_t lock_pattern = demod.lock_buf ^ 0x6666666666666666ull;
            const uint64_t lock_mask = (1ull << (2 * lock_len)) - 1;

            if ((lock_pattern & lock_mask) == 0 || ((~lock_pattern) & lock_mask) == 0) {
                demod.locked = 1;
                demod.lock_buf = 0;
                demod.symbol_count = 0;
                demod.sample_count = 0;
            }
        }

        demod.timeout++;
        if (demod.timeout > demod_timeout) demod.locked = 0;
    }

    /* Returns 1 at the end of each symbol period. */
    int build_symbol(double sample) {
        const int64_t phase_max = 100 * static_cast<int64_t>(demod.sample_freq);
        const int64_t phase_rate =
            phase_max * static_cast<int64_t>(demod.baud) / static_cast<int64_t>(demod.sample_freq);
        const double phasepercent = 100.0 * demod.phase / static_cast<double>(phase_max);

        demod.sample_count++;

        /* DC offset removal, only while hunting for sync. */
        if (state.current == State::SYNC1)
            modulation.zero = (modulation.zero * dc_alpha_ + sample) / (dc_alpha_ + 1.0);
        sample -= modulation.zero;

        if (demod.locked) {
            if (state.current == State::SYNC1) {
                demod.envelope_sum += std::abs(sample);
                demod.envelope_count++;
                modulation.envelope =
                    demod.envelope_sum / static_cast<double>(demod.envelope_count);
            }
        } else {
            modulation.envelope = 0;
            demod.envelope_sum = 0;
            demod.envelope_count = 0;
            demod.baud = 1600;
            demod.timeout = 0;
            demod.nonconsec = 0;
            state.current = State::SYNC1;
        }

        /* Slice, but only over the middle 80% of the symbol period. */
        if (phasepercent > 10 && phasepercent < 90) {
            if (sample > 0) {
                if (sample > modulation.envelope * slice_threshold)
                    demod.symcount[3]++;
                else
                    demod.symcount[2]++;
            } else {
                if (sample < -modulation.envelope * slice_threshold)
                    demod.symcount[0]++;
                else
                    demod.symcount[1]++;
            }
        }

        /* Zero crossing drives the symbol clock. */
        if ((demod.sample_last < 0 && sample >= 0) || (demod.sample_last >= 0 && sample < 0)) {
            double phase_error;
            if (phasepercent < 50)
                phase_error = demod.phase;
            else
                phase_error = demod.phase - static_cast<double>(phase_max);

            if (demod.locked)
                demod.phase -= static_cast<int>(phase_error * phase_locked_rate);
            else
                demod.phase -= static_cast<int>(phase_error * phase_unlocked_rate);

            if (phasepercent > 10 && phasepercent < 90) {
                demod.nonconsec++;
                if (demod.nonconsec > 20 && demod.locked) demod.locked = 0;
            } else {
                demod.nonconsec = 0;
            }

            demod.timeout = 0;
        }
        demod.sample_last = sample;

        demod.phase += static_cast<int>(phase_rate);

        if (demod.phase > phase_max) {
            demod.phase -= static_cast<int>(phase_max);
            return 1;
        }
        return 0;
    }

    /* --- sync ------------------------------------------------------------- */

    /* Checks a 64-bit sync buffer: A (16) : marker (32) : ~A (16).
     * Returns the mode code A, or 0. */
    unsigned int sync_check(uint64_t buf) const {
        const uint32_t marker = static_cast<uint32_t>((buf & 0x0000FFFFFFFF0000ull) >> 16);
        const uint16_t codehigh = static_cast<uint16_t>((buf & 0xFFFF000000000000ull) >> 48);
        const uint16_t codelow = static_cast<uint16_t>(~(buf & 0x000000000000FFFFull));

        const unsigned int diff_marker = popcount32(marker ^ sync_marker);
        const unsigned int diff_code = popcount32(static_cast<uint32_t>(codelow ^ codehigh));

        if (diff_marker < 4 && diff_code < 4) return codehigh;
        return 0;
    }

    /* Shifts one symbol into the sync buffer and tests both polarities. */
    unsigned int feed_sync(unsigned char sym) {
        sync.syncbuf = (sync.syncbuf << 1) | ((sym < 2) ? 1ull : 0ull);

        unsigned int retval = sync_check(sync.syncbuf);
        if (retval != 0) {
            sync.polarity = 0;
        } else {
            retval = sync_check(~sync.syncbuf);
            if (retval != 0) sync.polarity = 1;
        }
        return retval;
    }

    /* Mode code -> symbol rate and level count. */
    void decode_mode(unsigned int sync_code) {
        struct ModeDef {
            unsigned int sync;
            unsigned int baud;
            unsigned int levels;
        };
        static const ModeDef modes[] = {
            {0x870C, 1600, 2},
            {0xB068, 1600, 4},
            {0x7B18, 3200, 2},
            {0xDEA0, 3200, 4},
            {0x4C7C, 3200, 4},
            {0, 0, 0}};

        for (int i = 0; modes[i].sync != 0; i++) {
            if (popcount32(modes[i].sync ^ sync_code) < 4) {
                sync.sync = sync_code;
                sync.baud = modes[i].baud;
                sync.levels = modes[i].levels;
                return;
            }
        }

        sync.baud = 1600;
        sync.levels = 2;
    }

    /* --- FIW -------------------------------------------------------------- */

    static void read_2fsk(unsigned int sym, uint32_t* dat) {
        *dat = (*dat >> 1) | ((sym > 1) ? 0x80000000u : 0u);
    }

    /* Returns 0 on success. */
    int decode_fiw() {
        uint32_t fiw_val = fiw.rawdata;
        if (bch.fix_errors(fiw_val) > 2) return 1;

        fiw.checksum = fiw_val & 0xF;
        fiw.cycleno = (fiw_val >> 4) & 0xF;
        fiw.frameno = (fiw_val >> 8) & 0x7F;
        fiw.roaming = (fiw_val >> 15) & 0x01;
        fiw.repeat = (fiw_val >> 16) & 0x01;
        fiw.traffic = (fiw_val >> 17) & 0x0F;

        unsigned int checksum = (fiw_val & 0xF);
        checksum += ((fiw_val >> 4) & 0xF);
        checksum += ((fiw_val >> 8) & 0xF);
        checksum += ((fiw_val >> 12) & 0xF);
        checksum += ((fiw_val >> 16) & 0xF);
        checksum += ((fiw_val >> 20) & 0x01);
        checksum &= 0xF;

        return (checksum == 0xF) ? 0 : 1;
    }

    /* --- data ------------------------------------------------------------- */

    /* De-interleaves one symbol into the phase buffers. Returns non-zero once
     * every active phase has filled with idle. */
    int read_data(unsigned char sym) {
        const int bit_a = (sym > 1) ? 1 : 0;
        int bit_b = 0;
        if (sync.levels == 4) bit_b = (sym == 1) || (sym == 2);

        if (sync.baud == 1600) data.phase_toggle = 0;

        const unsigned int idx =
            ((data.data_bit_counter >> 5) & 0xFFF8u) | (data.data_bit_counter & 0x0007u);
        if (idx >= static_cast<unsigned int>(phase_words)) return 0;

        if (data.phase_toggle == 0) {
            data.a.buf[idx] = (data.a.buf[idx] >> 1) | (bit_a ? 0x80000000u : 0u);
            data.b.buf[idx] = (data.b.buf[idx] >> 1) | (bit_b ? 0x80000000u : 0u);
            data.phase_toggle = 1;

            if ((data.data_bit_counter & 0xFF) == 0xFF) {
                if (data.a.buf[idx] == 0x00000000u || data.a.buf[idx] == 0xffffffffu)
                    data.a.idle_count++;
                if (data.b.buf[idx] == 0x00000000u || data.b.buf[idx] == 0xffffffffu)
                    data.b.idle_count++;
            }
        } else {
            data.c.buf[idx] = (data.c.buf[idx] >> 1) | (bit_a ? 0x80000000u : 0u);
            data.d.buf[idx] = (data.d.buf[idx] >> 1) | (bit_b ? 0x80000000u : 0u);
            data.phase_toggle = 0;

            if ((data.data_bit_counter & 0xFF) == 0xFF) {
                if (data.c.buf[idx] == 0x00000000u || data.c.buf[idx] == 0xffffffffu)
                    data.c.idle_count++;
                if (data.d.buf[idx] == 0x00000000u || data.d.buf[idx] == 0xffffffffu)
                    data.d.idle_count++;
            }
        }

        if (sync.baud == 1600 || data.phase_toggle == 0) data.data_bit_counter++;

        int idle = 0;
        if (sync.baud == 1600) {
            if (sync.levels == 2)
                idle = (data.a.idle_count > idle_threshold);
            else
                idle = ((data.a.idle_count > idle_threshold) && (data.b.idle_count > idle_threshold));
        } else {
            if (sync.levels == 2)
                idle = ((data.a.idle_count > idle_threshold) && (data.c.idle_count > idle_threshold));
            else
                idle = ((data.a.idle_count > idle_threshold) && (data.b.idle_count > idle_threshold) &&
                        (data.c.idle_count > idle_threshold) && (data.d.idle_count > idle_threshold));
        }
        return idle;
    }

    /* The SYNC1/FIW/SYNC2/DATA state machine. */
    void handle_symbol(unsigned char sym) {
        const unsigned char sym_rectified =
            sync.polarity ? static_cast<unsigned char>(3 - sym) : sym;

        switch (state.current) {
            case State::SYNC1: {
                const unsigned int sync_code = feed_sync(sym);
                if (sync_code != 0) {
                    decode_mode(sync_code);
                    if (sync.baud != 0 && sync.levels != 0)
                        state.current = State::FIW;
                    else
                        state.current = State::SYNC1;
                } else {
                    state.current = State::SYNC1;
                }
                state.fiwcount = 0;
                fiw.rawdata = 0;
                break;
            }

            case State::FIW: {
                state.fiwcount++;
                if (state.fiwcount >= 16) read_2fsk(sym_rectified, &fiw.rawdata);
                if (state.fiwcount == 48) {
                    if (decode_fiw() == 0) {
                        state.sync2_count = 0;
                        state.sync2_shiftreg = 0;
                        state.sync2_c_pos = -1;
                        state.sync2_cinv_pos = -1;
                        demod.baud = sync.baud;
                        state.current = State::SYNC2;
                    } else {
                        state.current = State::SYNC1;
                    }
                }
                break;
            }

            case State::SYNC2: {
                /* S2 is BS2 + C(16) + inv.BS2 + inv.C(16), 25 ms in total.
                 * Scan for the C pattern; if inv.C turns up where the nominal
                 * duration says it should, use it as the data boundary. */
                const unsigned char s2_sym =
                    sync.polarity ? static_cast<unsigned char>(3 - sym) : sym;
                const int bit_a = (s2_sym > 1) ? 1 : 0;
                state.sync2_shiftreg =
                    static_cast<uint16_t>((state.sync2_shiftreg << 1) | static_cast<uint16_t>(bit_a));
                state.sync2_count++;

                if (state.sync2_count >= 16) {
                    const unsigned int errs_c =
                        popcount32(static_cast<uint32_t>(state.sync2_shiftreg ^ 0xED84u));
                    const unsigned int errs_cinv =
                        popcount32(static_cast<uint32_t>(state.sync2_shiftreg ^ 0x127Bu));

                    if (errs_c <= 2 && state.sync2_c_pos < 0)
                        state.sync2_c_pos = static_cast<int>(state.sync2_count);
                    if (errs_cinv <= 2 && state.sync2_cinv_pos < 0)
                        state.sync2_cinv_pos = static_cast<int>(state.sync2_count);
                }

                const unsigned int s2_nominal = sync.baud * 25 / 1000;

                unsigned int s2_end = s2_nominal;
                if (state.sync2_cinv_pos > 0) {
                    const unsigned int cinv_end = static_cast<unsigned int>(state.sync2_cinv_pos);
                    const int diff = static_cast<int>(cinv_end) - static_cast<int>(s2_nominal);
                    if (diff >= -1 && diff <= 1) s2_end = cinv_end;
                }

                if (state.sync2_count == s2_end) {
                    data = DataState{};
                    state.data_count = 0;
                    state.sync2_shiftreg = 0;
                    state.sync2_c_pos = -1;
                    state.sync2_cinv_pos = -1;
                    state.current = State::DATA;
                }

                if (state.sync2_count > s2_nominal + 1) {
                    state.sync2_shiftreg = 0;
                    state.sync2_c_pos = -1;
                    state.sync2_cinv_pos = -1;
                    state.current = State::SYNC1;
                }
                break;
            }

            case State::DATA: {
                const int idle = read_data(sym_rectified);
                if (++state.data_count == sync.baud * 1760 / 1000 || idle) {
                    decode_data();
                    demod.baud = 1600;
                    state.current = State::SYNC1;
                    state.data_count = 0;
                }
                break;
            }
        }
    }

    /* --- page parsing ----------------------------------------------------- */

    void decode_data() {
        if (sync.baud == 1600) {
            if (sync.levels == 2) {
                decode_phase('A');
            } else {
                decode_phase('A');
                decode_phase('B');
            }
        } else {
            if (sync.levels == 2) {
                decode_phase('A');
                decode_phase('C');
            } else {
                decode_phase('A');
                decode_phase('B');
                decode_phase('C');
                decode_phase('D');
            }
        }
    }

    void decode_phase(char phase_no) {
        uint32_t* phaseptr = nullptr;
        switch (phase_no) {
            case 'A':
                phaseptr = data.a.buf;
                break;
            case 'B':
                phaseptr = data.b.buf;
                break;
            case 'C':
                phaseptr = data.c.buf;
                break;
            case 'D':
                phaseptr = data.d.buf;
                break;
            default:
                return;
        }
        decode_phase_buffer(phaseptr, phase_no);
    }

    /* Runs BCH over a whole phase buffer and emits its pages. Split out from
     * decode_phase() so tests can hand it a hand-built frame. */
    void decode_phase_buffer(uint32_t* phaseptr, char phase_no) {
        /* Idle fill alternates 0xFFFFFFFF and 0x00000000. If every word is one
         * of those, there is no data — do not let BCH "correct" fill into
         * plausible-looking garbage. */
        {
            bool all_idle = true;
            for (int i = 0; i < phase_words; i++) {
                if (phaseptr[i] != 0xFFFFFFFFu && phaseptr[i] != 0x00000000u) {
                    all_idle = false;
                    break;
                }
            }
            if (all_idle) return;
        }

        uint8_t word_bad[phase_words] = {};
        for (int i = 0; i < phase_words; i++) {
            if (bch.fix_errors(phaseptr[i]) > 2) {
                word_bad[i] = 1;
                phaseptr[i] = 0;
            }
            phaseptr[i] &= 0x001FFFFFu;
        }

        if (word_bad[0]) return;

        const uint32_t biw = phaseptr[0];
        if (biw == 0 || biw == 0x001FFFFFu) return;

        const int voffset = static_cast<int>((biw >> 10) & 0x3f);
        const int aoffset = static_cast<int>(((biw >> 8) & 0x03) + 1);
        const int prio_count = static_cast<int>((biw >> 4) & 0x0F);

        if (voffset < aoffset || voffset >= phase_words) return;

        /* Always announce the frame, even an idle one, so the app can show
         * that decoding is alive. */
        {
            FlexPacket bpkt{};
            bpkt.type = 9;
            bpkt.bitrate = sync.baud * (sync.levels == 4 ? 2 : 1);
            bpkt.cycle = static_cast<uint8_t>(fiw.cycleno);
            bpkt.frame = static_cast<uint8_t>(fiw.frameno);
            bpkt.phase = phase_no;
            bpkt.is_inverted = static_cast<uint8_t>(sync.polarity);
            bpkt.fiw_roaming = static_cast<uint8_t>(fiw.roaming);
            bpkt.function = 0;
            bpkt.biw_field = 0xFF;
            send(bpkt);
        }

        /* BIW words 1..aoffset-1: a 3-bit type in bits 4-6 says what each
         * carries. */
        for (int bw = 1; bw < aoffset && bw < phase_words; bw++) {
            if (word_bad[bw]) continue;
            const uint32_t bword = phaseptr[bw];
            const uint32_t btype = (bword >> 4) & 0x07u;

            if (btype == 3 || btype == 4 || btype == 6) continue; /* reserved */

            FlexPacket bpkt{};
            bpkt.type = 9;
            bpkt.bitrate = sync.baud * (sync.levels == 4 ? 2 : 1);
            bpkt.cycle = static_cast<uint8_t>(fiw.cycleno);
            bpkt.frame = static_cast<uint8_t>(fiw.frameno);
            bpkt.phase = phase_no;
            bpkt.is_inverted = static_cast<uint8_t>(sync.polarity);
            bpkt.fiw_roaming = static_cast<uint8_t>(fiw.roaming);
            bpkt.function = static_cast<uint32_t>(bw);
            bpkt.biw_field = static_cast<uint8_t>(btype);

            switch (btype) {
                case 0: /* SSID1 */
                    bpkt.biw_v1 = static_cast<uint16_t>((bword >> 12) & 0x01FFu);
                    bpkt.biw_v2 = static_cast<uint16_t>((bword >> 7) & 0x1Fu);
                    break;
                case 1: /* date */
                    bpkt.biw_v1 = static_cast<uint16_t>(((bword >> 7) & 0x1Fu) + 1994u);
                    bpkt.biw_v2 = static_cast<uint16_t>((bword >> 17) & 0x0Fu);
                    bpkt.biw_v3 = static_cast<uint16_t>((bword >> 12) & 0x1Fu);
                    break;
                case 2: /* time */
                    bpkt.biw_v1 = static_cast<uint16_t>((bword >> 7) & 0x1Fu);
                    bpkt.biw_v2 = static_cast<uint16_t>((bword >> 12) & 0x3Fu);
                    bpkt.biw_v3 = static_cast<uint16_t>((bword >> 18) & 0x07u);
                    break;
                case 5: /* system info */
                    bpkt.biw_v1 = static_cast<uint16_t>((bword >> 7) & 0x0Fu);
                    bpkt.biw_v2 = static_cast<uint16_t>((bword >> 11) & 0x03FFu);
                    break;
                case 7: /* SSID2 */
                    bpkt.biw_v1 = static_cast<uint16_t>((bword >> 11) & 0x03FFu);
                    bpkt.biw_v2 = static_cast<uint16_t>((bword >> 7) & 0x0Fu);
                    break;
                default:
                    continue;
            }
            send(bpkt);
        }

        /* Count vector words that pass the 4-bit nibble checksum. Tone-only
         * addresses sit at the end of the address field with no vector, so the
         * last passing vector marks where they begin. */
        int n_valid_vecs = 0;
        for (int vi = 0; vi < (voffset - aoffset); vi++) {
            const int wi = voffset + vi;
            if (wi >= phase_words) break;
            if (nibble_checksum_ok(phaseptr[wi])) n_valid_vecs = vi + 1;
        }

        if (voffset <= aoffset) return;

        int vec_count = 0;
        int addr_count = 0;
        for (int i = aoffset; i < voffset; i++) {
            int j = voffset + vec_count;
            if (j >= phase_words) break;
            if (phaseptr[i] == 0x00000000u || phaseptr[i] == 0x001FFFFFu) continue;

            const int is_priority = (addr_count < prio_count) ? 1 : 0;

            parse_capcode(phaseptr[i]);
            decode.is_priority = is_priority;
            addr_count++;

            if (decode.long_address) {
                if (i + 1 >= voffset) break; /* truncated */
                const uint32_t aw1 = phaseptr[i];
                const uint32_t aw2 = phaseptr[i + 1];
                if (aw2 == 0x00000000u || aw2 == 0x001FFFFFu) {
                    i++;
                    addr_count++;
                    vec_count += 2;
                    continue;
                }

                int64_t cap = 0;
                if (aw1 >= 0x000001u && aw1 <= 0x008000u && aw2 >= 0x1F7FFFu && aw2 <= 0x1FFFFEu) {
                    cap = static_cast<int64_t>(aw1) +
                          static_cast<int64_t>(0x1FFFFF - aw2) * 32768LL + 2068480LL;
                } else if (aw1 >= 0x000001u && aw1 <= 0x008000u && aw2 >= 0x1E0001u &&
                           aw2 <= 0x1F0000u) {
                    cap = static_cast<int64_t>(aw1) +
                          (static_cast<int64_t>(aw2) - 1933312LL) * 32768LL + 2068480LL;
                } else if (aw1 >= 0x1F7FFFu && aw1 <= 0x1FFFFEu && aw2 >= 0x1E0001u &&
                           aw2 <= 0x1F0000u) {
                    cap = (static_cast<int64_t>(aw1) - 2064383LL) +
                          (static_cast<int64_t>(aw2) - 1867776LL) * 32768LL + 2068479LL;
                } else {
                    i++;
                    addr_count++;
                    vec_count += 2;
                    continue;
                }

                decode.capcode = cap;
                i++;
                addr_count++;

                /* Long addresses always carry vectors; the second vector word
                 * is the first message word, not a checksummed vector. */
                vec_count += 2;
                j = voffset + vec_count - 2;
            } else {
                if (decode.capcode > 4297068542ll || decode.capcode <= 0) continue;

                if (vec_count >= n_valid_vecs) {
                    parse_tone_only(phase_no);
                    continue;
                }
                vec_count++;
            }

            const uint32_t viw = phaseptr[j];
            const int type_val = static_cast<int>((viw >> 4) & 0x07u);

            switch (type_val) {
                case 0:
                    decode.type = PageType::SECURE;
                    break;
                case 1:
                    decode.type = PageType::SHORT_INSTRUCTION;
                    break;
                case 2:
                    decode.type = PageType::TONE;
                    break;
                case 3:
                    decode.type = PageType::STANDARD_NUMERIC;
                    break;
                case 4:
                    decode.type = PageType::SPECIAL_NUMERIC;
                    break;
                case 5:
                    decode.type = PageType::ALPHANUMERIC;
                    break;
                case 6:
                    decode.type = PageType::BINARY;
                    break;
                default:
                    decode.type = PageType::NUMBERED_NUMERIC;
                    break;
            }

            int mw1 = static_cast<int>((viw >> 7) & 0x7Fu);
            int len;
            /* Numeric vectors (3, 4, 7) hold word_count-1 in a 3-bit n field
             * at bits 14-16 and the K checksum at 17-20; alpha/hex/secure use
             * all seven bits. */
            if (type_val == 3 || type_val == 4 || type_val == 7)
                len = static_cast<int>((viw >> 14) & 0x07u) + 1;
            else
                len = static_cast<int>((viw >> 14) & 0x7Fu);
            int mw2 = mw1 + (len - 1);

            if (mw1 == 0 && mw2 == 0) continue;
            if (decode.type == PageType::TONE) mw1 = mw2 = 0;

            if (decode.type == PageType::ALPHANUMERIC || decode.type == PageType::SECURE) {
                if (mw1 > 87 || mw2 > 87) continue;
                if (decode.long_address)
                    parse_alphanumeric(phaseptr, word_bad, phase_no, mw1 - 1, mw2 - 1);
                else
                    parse_alphanumeric(phaseptr, word_bad, phase_no, mw1, mw2);
            } else if (decode.type == PageType::STANDARD_NUMERIC ||
                       decode.type == PageType::SPECIAL_NUMERIC ||
                       decode.type == PageType::NUMBERED_NUMERIC) {
                parse_numeric(phaseptr, word_bad, phase_no, j);
            } else if (decode.type == PageType::TONE) {
                parse_short(phaseptr, phase_no, viw, j);
            } else if (decode.type == PageType::BINARY) {
                parse_binary(phaseptr, word_bad, phase_no, mw1, mw2);
            } else if (decode.type == PageType::SHORT_INSTRUCTION) {
                parse_instruction(phase_no, viw);
            }
        }
    }

    static bool nibble_checksum_ok(uint32_t w) {
        const uint32_t csum = (w & 0xFu) + ((w >> 4) & 0xFu) + ((w >> 8) & 0xFu) +
                              ((w >> 12) & 0xFu) + ((w >> 16) & 0xFu) + ((w >> 20) & 0x1u);
        return (csum & 0xFu) == 0xFu;
    }

    /* Classifies an address word by value range, per Table 3.8.1-1. */
    void parse_capcode(uint32_t aw1) {
        decode.long_address = 0;
        decode.addr_type = AddrType::SHORT;

        if ((aw1 >= 0x000001u && aw1 <= 0x008000u) || /* LA1 */
            (aw1 >= 0x1E0001u && aw1 <= 0x1E8000u) || /* LA3 */
            (aw1 >= 0x1E8001u && aw1 <= 0x1F0000u) || /* LA4 */
            (aw1 >= 0x1F7FFFu && aw1 <= 0x1FFFFEu)) { /* LA2 */
            decode.long_address = 1;
            decode.addr_type = AddrType::LONG;
        } else if (aw1 >= 0x1F7800u && aw1 <= 0x1F780Fu) {
            decode.addr_type = AddrType::TEMPORARY;
        } else if (aw1 >= 0x1F7810u && aw1 <= 0x1F781Fu) {
            decode.addr_type = AddrType::OPERATOR;
        } else if (aw1 >= 0x1F6800u && aw1 <= 0x1F77FFu) {
            decode.addr_type = AddrType::NETWORK;
        } else if (aw1 >= 0x1F2800u && aw1 <= 0x1F67FFu) {
            decode.addr_type = AddrType::INFO_SVC;
        } else if ((aw1 >= 0x1F0001u && aw1 <= 0x1F27FFu) ||
                   (aw1 >= 0x1F7820u && aw1 <= 0x1F7FFEu)) {
            decode.addr_type = AddrType::RESERVED;
        } else if (aw1 >= 0x008001u && aw1 <= 0x1E0000u) {
            decode.addr_type = AddrType::SHORT;
        } else {
            decode.addr_type = AddrType::UNKNOWN;
        }

        decode.capcode = static_cast<int64_t>(aw1) - 0x8000;
    }

    void parse_alphanumeric(const uint32_t* phaseptr, const uint8_t* word_bad, char phase_no,
                            int mw1, int mw2) {
        char message[256] = {};
        int current_char = 0;

        /* The first message word is a header (K, C, F, N, R, M). */
        uint8_t hdr_c = 0, hdr_f = 0, hdr_n = 0, hdr_r = 0, hdr_m = 0, hdr_sig = 0;
        int hdr_valid = 0;
        if (mw1 >= 0 && mw1 < phase_words && !word_bad[mw1]) {
            const uint32_t hdr = phaseptr[mw1];
            hdr_c = static_cast<uint8_t>((hdr >> 10) & 0x01u);
            hdr_f = static_cast<uint8_t>((hdr >> 11) & 0x03u);
            hdr_n = static_cast<uint8_t>((hdr >> 13) & 0x3Fu);
            hdr_r = static_cast<uint8_t>((hdr >> 19) & 0x01u);
            hdr_m = static_cast<uint8_t>((hdr >> 20) & 0x01u);
            hdr_valid = 1;
        }
        mw1++;

        /* The first data word's low 7 bits are the signature, not a
         * character. */
        if (mw1 >= 0 && mw1 < phase_words && !word_bad[mw1])
            hdr_sig = static_cast<uint8_t>(phaseptr[mw1] & 0x7Fu);

        for (int i = mw1; i <= mw2 && i < phase_words; i++) {
            if (i < 0) continue;
            const uint32_t dw = phaseptr[i];
            const int bad = word_bad[i];

            if (i > mw1) append_char(message, current_char, dw & 0x7Fu, bad);
            append_char(message, current_char, (dw >> 7) & 0x7Fu, bad);
            append_char(message, current_char, (dw >> 14) & 0x7Fu, bad);
        }

        /* Trim trailing ETX/NUL padding; an ETX that has printable text after
         * it is a real error, so show it as '?'. */
        {
            int last_printable = -1;
            for (int k = 0; k < current_char; k++)
                if (message[k] != '\x03') last_printable = k;

            int out = 0;
            for (int k = 0; k <= last_printable && out < 255; k++)
                message[out++] = (message[k] == '\x03') ? '?' : message[k];
            current_char = out;
        }
        message[current_char] = '\0';

        FlexPacket packet{};
        fill_common(packet, phase_no);
        packet.type = (decode.type == PageType::SECURE) ? 0u : 5u;
        if (hdr_valid) {
            packet.frag = hdr_f;
            packet.more_frag = hdr_c;
            packet.seq = hdr_n;
            packet.is_new = hdr_r;
            packet.maildrop = hdr_m;
            packet.sig = hdr_sig;
            packet.has_flags = 1;
            if (decode.type == PageType::SECURE) {
                packet.sec_enc = static_cast<uint8_t>(hdr_r | (hdr_m << 1));
                packet.is_new = 0;
                packet.maildrop = 0;
            }
        }
        std::memcpy(packet.message, message, static_cast<size_t>(current_char) + 1);
        send(packet);
    }

    void parse_numeric(const uint32_t* phaseptr, const uint8_t* word_bad, char phase_no, int j) {
        char message[256] = {};

        uint8_t nnum_n = 0, nnum_r = 0, nnum_s = 0;
        const int is_nnum = (decode.type == PageType::NUMBERED_NUMERIC);

        int w1 = static_cast<int>(phaseptr[j] >> 7);
        int w2 = w1 >> 7;
        w1 = w1 & 0x7f;
        const int n_field = w2 & 0x07;
        w2 = n_field + w1;

        if (w1 > 87) return;
        if (w2 > 87) w2 = 87;

        /* Long addresses put body[0] in the second vector word. */
        int body0_idx;
        if (decode.long_address && n_field > 0)
            body0_idx = j + 1;
        else
            body0_idx = w1;
        if (body0_idx < 0 || body0_idx >= phase_words) return;

        int dw = static_cast<int>(phaseptr[body0_idx]);

        if (is_nnum) {
            nnum_n = static_cast<uint8_t>((dw >> 2) & 0x3F);
            nnum_r = static_cast<uint8_t>((dw >> 8) & 0x01);
            nnum_s = static_cast<uint8_t>((dw >> 9) & 0x01);
        }

        unsigned char digit = 0;
        int count = 4;
        if (is_nnum)
            count += 10; /* K5K4(2) + N(6) + R(1) + S(1) */
        else
            count += 2; /* K5K4(2) */

        int idx = 0;

        if (word_bad[body0_idx]) {
            const int data_bits = 21 - (count - 4);
            int lost = data_bits / 4;
            while (lost-- > 0 && idx < 255) message[idx++] = '?';
            count = 4;
            digit = 0;
        } else {
            for (int k = 0; k < 21; k++) {
                digit = static_cast<unsigned char>((digit >> 1) & 0x0F);
                if (dw & 0x01) digit ^= 0x08;
                dw >>= 1;
                if (--count == 0) {
                    if (idx < 255) message[idx++] = bcd_chars()[digit];
                    count = 4;
                }
            }
        }

        int start, end;
        if (decode.long_address) {
            start = w1;
            end = w1 + n_field - 1;
        } else {
            start = w1 + 1;
            end = w2;
        }
        for (int i = start; i <= end && i < phase_words; i++) {
            if (i < 0) continue;
            if (word_bad[i]) {
                int lost = 21 / 4;
                while (lost-- > 0 && idx < 255) message[idx++] = '?';
                count = 4;
                digit = 0;
                continue;
            }
            dw = static_cast<int>(phaseptr[i]);
            for (int k = 0; k < 21; k++) {
                digit = static_cast<unsigned char>((digit >> 1) & 0x0F);
                if (dw & 0x01) digit ^= 0x08;
                dw >>= 1;
                if (--count == 0) {
                    if (idx < 255) message[idx++] = bcd_chars()[digit];
                    count = 4;
                }
            }
        }

        /* The encoder pads unused nibbles with 0x0C, which is a space. */
        while (idx > 0 && message[idx - 1] == ' ') idx--;
        message[idx] = '\0';

        FlexPacket packet{};
        fill_common(packet, phase_no);
        if (decode.type == PageType::SPECIAL_NUMERIC)
            packet.type = 4;
        else if (is_nnum)
            packet.type = 7;
        else
            packet.type = 3;
        if (is_nnum) {
            packet.seq = nnum_n;
            packet.is_new = nnum_r;
            packet.nnum_s = nnum_s;
            packet.has_flags = 1;
        }
        std::memcpy(packet.message, message, static_cast<size_t>(idx) + 1);
        send(packet);
    }

    /* Vector type 2: short message (3.9.2). */
    void parse_short(const uint32_t* phaseptr, char phase_no, uint32_t viw, int j) {
        const uint32_t t = (viw >> 7) & 0x03u;
        const uint32_t d = (viw >> 9) & 0x0FFFu;

        FlexPacket packet{};
        fill_common(packet, phase_no);
        packet.function = t;
        packet.type = 8;

        bool numeric = false;
        if (t == 0 && d == 0xCCC) {
            /* All digits are the space code: tone-only. */
            bool tone = true;
            if (decode.long_address && j + 1 < phase_words) {
                if ((phaseptr[j + 1] & 0xFFFFFu) != 0xCCCCCu) tone = false;
            }
            if (tone)
                std::strcpy(packet.message, "TONE");
            else
                numeric = true;
        } else if (t == 0) {
            numeric = true;
        } else if (t == 1) {
            std::string s = "SRC " + std::to_string(d & 0x07u);
            copy_message(packet, s);
        } else if (t == 2) {
            const uint32_t src = d & 0x07u;
            const uint32_t n = (d >> 3) & 0x3Fu;
            const uint32_t r = (d >> 9) & 0x01u;
            const std::string s = "SRC " + std::to_string(src) + " N=" + std::to_string(n) +
                                  " R=" + std::to_string(r);
            copy_message(packet, s);
        } else {
            std::string s = "RESERVED ";
            for (int k = 2; k >= 0; --k) s += "0123456789ABCDEF"[(d >> (k * 4)) & 0xF];
            copy_message(packet, s);
        }

        if (numeric) {
            std::string s = "NUM ";
            s += bcd_chars()[(d >> 0) & 0xF];
            s += bcd_chars()[(d >> 4) & 0xF];
            s += bcd_chars()[(d >> 8) & 0xF];
            if (decode.long_address && j + 1 < phase_words) {
                const uint32_t vy = phaseptr[j + 1];
                for (int k = 0; k < 5; k++) s += bcd_chars()[(vy >> (k * 4)) & 0xF];
            }
            copy_message(packet, s);
        }

        send(packet);
    }

    void parse_binary(const uint32_t* phaseptr, const uint8_t* word_bad, char phase_no, int mw1,
                      int mw2) {
        if (mw1 > 87 || mw2 > 87 || mw1 < 0) return;

        uint8_t hex_c = 0, hex_f = 0, hex_n = 0;
        int hex_hdr_valid = 0;
        if (!word_bad[mw1]) {
            const uint32_t hw1 = phaseptr[mw1];
            hex_c = static_cast<uint8_t>((hw1 >> 12) & 0x01u);
            hex_f = static_cast<uint8_t>((hw1 >> 13) & 0x03u);
            hex_n = static_cast<uint8_t>((hw1 >> 15) & 0x3Fu);
            hex_hdr_valid = 1;
        }

        uint8_t hex_r = 0, hex_m = 0, hex_d = 0, hex_b = 0;
        int data_start = mw1 + 1;
        if (hex_f == 3 && (mw1 + 1) <= mw2 && !word_bad[mw1 + 1]) {
            const uint32_t hw2 = phaseptr[mw1 + 1];
            hex_r = static_cast<uint8_t>((hw2 >> 0) & 0x01u);
            hex_m = static_cast<uint8_t>((hw2 >> 1) & 0x01u);
            hex_d = static_cast<uint8_t>((hw2 >> 2) & 0x01u);
            hex_b = static_cast<uint8_t>((hw2 >> 4) & 0x0Fu);
            data_start = mw1 + 2;
        }

        std::string message;
        for (int w = data_start; w <= mw2 && w < phase_words; w++) {
            if (w < 0) continue;
            if (word_bad[w]) {
                message += "?????? ";
            } else {
                const uint32_t v = phaseptr[w] & 0x1FFFFFu;
                for (int k = 4; k >= 0; --k) message += "0123456789ABCDEF"[(v >> (k * 4)) & 0xF];
                message += ' ';
            }
            if (message.size() > 240) break;
        }
        if (!message.empty() && message.back() == ' ') message.pop_back();

        FlexPacket packet{};
        fill_common(packet, phase_no);
        packet.function = 0;
        packet.type = 6;
        if (hex_hdr_valid) {
            packet.frag = hex_f;
            packet.more_frag = hex_c;
            packet.seq = hex_n;
            packet.has_flags = 1;
            if (hex_f == 3) {
                packet.is_new = hex_r;
                packet.maildrop = hex_m;
                packet.function = static_cast<uint32_t>((hex_d << 4) | hex_b);
            }
        }
        copy_message(packet, message);
        send(packet);
    }

    void parse_instruction(char phase_no, uint32_t viw) {
        const uint32_t instr_data = (viw >> 7) & 0x3FFFu;
        const uint32_t itype = instr_data & 0x07u;

        FlexPacket packet{};
        fill_common(packet, phase_no);
        packet.function = 0;
        packet.type = 1;

        if (itype == 0) {
            const uint32_t tgt_frame = (instr_data >> 3) & 0x7Fu;
            const uint32_t slot = (instr_data >> 10) & 0x0Fu;
            packet.biw_v1 = static_cast<uint16_t>(slot);
            packet.biw_v2 = static_cast<uint16_t>(tgt_frame);
            copy_message(packet, "i=temp|slot=" + std::to_string(slot) + "|target=" +
                                     std::to_string(tgt_frame));
        } else if (itype == 1) {
            const uint32_t flags = (instr_data >> 3) & 0x7FFu;
            std::string s = "i=event|flags=";
            for (int k = 2; k >= 0; --k) s += "0123456789ABCDEF"[(flags >> (k * 4)) & 0xF];
            copy_message(packet, s);
        } else {
            std::string s = "i=rsvd|type=" + std::to_string(itype) + "|raw=";
            for (int k = 3; k >= 0; --k) s += "0123456789ABCDEF"[(instr_data >> (k * 4)) & 0xF];
            copy_message(packet, s);
        }
        send(packet);
    }

    void parse_tone_only(char phase_no) {
        if (decode.capcode == 1) return; /* idle artefact */
        FlexPacket packet{};
        fill_common(packet, phase_no);
        packet.function = 0;
        packet.type = 2;
        send(packet);
    }

    /* --- state ------------------------------------------------------------ */

    BchEcc bch{};
    DemodParams demod{};
    Modulation modulation{};
    StateInfo state{};
    SyncInfo sync{};
    FrameInfoWord fiw{};
    DataState data{};
    DecodeState decode{};

   private:
    void fill_common(FlexPacket& p, char phase_no) const {
        p.bitrate = sync.baud * (sync.levels == 4 ? 2 : 1);
        p.capcode = decode.capcode;
        p.cycle = static_cast<uint8_t>(fiw.cycleno);
        p.frame = static_cast<uint8_t>(fiw.frameno);
        p.phase = phase_no;
        p.is_inverted = static_cast<uint8_t>(sync.polarity);
        p.fiw_roaming = static_cast<uint8_t>(fiw.roaming);
        p.addr_type = static_cast<uint8_t>(decode.addr_type);
        p.is_priority = static_cast<uint8_t>(decode.is_priority);
    }

    static void copy_message(FlexPacket& p, const std::string& s) {
        const size_t n = std::min(s.size(), sizeof(p.message) - 1);
        std::memcpy(p.message, s.data(), n);
        p.message[n] = '\0';
    }

    static void append_char(char* message, int& current_char, uint32_t raw, int bad) {
        const unsigned char ch = static_cast<unsigned char>(raw & 0x7Fu);
        if (bad) {
            if (current_char < 255) message[current_char++] = '?';
        } else if (ch >= 0x20 || ch == 0x0A || ch == 0x0D) {
            if (current_char < 255) message[current_char++] = static_cast<char>(ch);
        } else if (ch == 0x03 || ch == 0x00) {
            if (current_char < 255) message[current_char++] = '\x03';
        }
    }

    void send(const FlexPacket& p) const {
        if (handler_) handler_(p);
    }

    PacketHandler handler_{};
    double dc_alpha_ = 24000.0 * dc_offset_filter;
};

/* ===========================================================================
 * RF front end — the same shape as the POCSAG app's, with FLEX's deviation
 * and channel width. Each app owns its own copy (see doc/PORTING.md).
 * ===========================================================================*/

class ChannelFrontEnd {
   public:
    void configure(double source_rate_hz, double target_audio_rate_hz, double channel_cutoff_hz) {
        source_rate_ = source_rate_hz;
        if (!(source_rate_hz > 0.0)) return;

        size_t total = static_cast<size_t>(std::lround(source_rate_hz / target_audio_rate_hz));
        if (total < 1) total = 1;

        size_t d1 = total, d2 = 1;
        for (size_t k = 2; k * k <= total; ++k)
            if (total % k == 0) {
                d2 = k;
                d1 = total / k;
            }

        const double rate1 = source_rate_hz / static_cast<double>(d1);
        audio_rate_ = rate1 / static_cast<double>(d2);

        if (d1 > 1) {
            const double cut1 = std::min(rate1 * 0.1, std::max(channel_cutoff_hz * 1.5, 12000.0));
            const double trans1 = std::max(rate1 * 0.5 - cut1, rate1 * 0.05);
            stage1_.configure(dsp::design_lowpass(cut1, trans1, source_rate_hz, 60.0, 601), d1);
        } else {
            stage1_.configure({1.0f}, 1);
        }

        const double cut2 = std::min(channel_cutoff_hz, audio_rate_ * 0.45);
        const double trans2 = std::max(audio_rate_ * 0.5 - cut2, audio_rate_ * 0.05);
        stage2_.configure(dsp::design_lowpass(cut2, trans2, rate1, 60.0, 601), d2);

        demod_.configure(static_cast<float>(audio_rate_), static_cast<float>(deviation_hz_));
        nco_.set_frequency(0.0, source_rate_hz);
        decim_total_ = total;
    }

    void set_deviation(double hz) { deviation_hz_ = hz; }
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
    double deviation_hz_{4800.0};
    size_t decim_total_{1};
};

/* Type tag for a packet's `type` field, as the upstream view prints it. */
inline const char* type_tag(uint32_t type) {
    switch (type) {
        case 0:
            return "SEC";
        case 1:
            return "INS";
        case 2:
            return "TON";
        case 3:
            return "NUM";
        case 4:
            return "SNUM";
        case 5:
            return "ALN";
        case 6:
            return "HEX";
        case 7:
            return "NNUM";
        case 8:
            return "SHORT";
        case 9:
            return "BIW";
        default:
            return "UNK";
    }
}

/* Timezone offsets in minutes, indexed by the 5-bit zone code in a SysInfo
 * BIW. Straight from ui_flex_rx.cpp. */
inline int timezone_offset_minutes(unsigned int zone) {
    static const int table[32] = {
        0, 60, 120, 180, 240, 300, 360, 420, 480, 540, 600, 660, 720,
        210, 270, 330, 0, 345, 390, 570, -210, -660, -600, -540, -480,
        -420, -360, -300, -240, -180, -120, -60};
    return (zone < 32) ? table[zone] : 0;
}

/* One console line for a decoded packet, in upstream's format. A free
 * function so it can be tested without building a view. */
inline std::string format_packet_line(const FlexPacket& pkt) {
    const char* type = type_tag(pkt.type);
    const char* pol = pkt.is_inverted ? "I" : "N";

    std::string line = to_string_dec_uint(pkt.cycle) + "/" + to_string_dec_uint(pkt.frame) + " " +
                       to_string_dec_uint(pkt.bitrate) + " " + pol + " " +
                       std::string(1, pkt.phase) + " ";

    if (pkt.type == 1 && pkt.message[0] == 'i' && pkt.message[2] == 't') {
        /* Short instruction assigning a temporary group. */
        line += to_string_dec_uint(static_cast<uint64_t>(pkt.capcode));
        line += " +TG" + to_string_dec_uint(pkt.biw_v1);
        line += "@F" + to_string_dec_uint(pkt.biw_v2);
    } else if (pkt.type == 1) {
        line += to_string_dec_uint(static_cast<uint64_t>(pkt.capcode));
        line += " INS ";
        line += pkt.message;
    } else if (pkt.addr_type == 2) {
        /* Temporary address: show the group slot, not the raw capcode. */
        const uint32_t slot = static_cast<uint32_t>(pkt.capcode + 0x8000 - 0x1F7800) & 0x0Fu;
        line += "TG" + to_string_dec_uint(slot);
        line += " ";
        line += type;
        if (pkt.message[0]) {
            line += " ";
            line += pkt.message;
        }
    } else {
        line += to_string_dec_uint(static_cast<uint64_t>(pkt.capcode));
        if (pkt.is_priority) line += " P";
        line += " ";
        line += type;
        if (pkt.message[0]) {
            line += " ";
            line += pkt.message;
        }
    }
    return line;
}

}  // namespace flex

/* ===========================================================================
 * UI
 * ===========================================================================*/

namespace app {

class FlexRxView : public ui::View {
   public:
    FlexRxView();
    ~FlexRxView() override;

    FlexRxView(const FlexRxView&) = delete;
    FlexRxView& operator=(const FlexRxView&) = delete;

    std::string title() const override { return "FLEX RX"; }

    void on_show() override;
    void on_frame_sync() override;

    /* Exposed so the display path can be tested without a radio. */
    void handle_packet(const flex::FlexPacket& packet);

    /* Read-only, for src/remote/provider_flex.cpp — the same idiom as
     * AprsTableView::entries() (ui_aprs_rx.hpp). The decoded pages exist only
     * in this console, and a panel provider handed a ui::View& cannot reach a
     * private member. */
    const ui::Console& console() const { return console_; }

   private:
    void reconfigure_dsp();

    radio::ReceiverModel* receiver_{nullptr};

    flex::Decoder decoder_{};
    flex::ChannelFrontEnd front_end_{};

    std::vector<dsp::cfloat> iq_{};
    std::vector<float> audio_{};

    std::string status_time_{};
    std::string status_tz_{};
    uint16_t status_lid_{0};
    uint16_t status_cz_{0};
    uint16_t status_cc_{0};
    uint32_t packet_count_{0};
    uint8_t frame_counter_{0};
    double configured_rate_{0.0};

    ui::FrequencyField field_frequency_{{0, 0}};
    ui::NumberField field_gain_{{104, 0}, 3, {0, 76}, 1, ' '};

    ui::Text text_status1_{{0, 18, 240, 16}, "No signal"};
    ui::Text text_status2_{{0, 36, 240, 16}, ""};

    ui::Console console_{{0, 56, 240, 248}};
};

}  // namespace app

#endif /*__MB200_UI_FLEX_RX_H__*/
