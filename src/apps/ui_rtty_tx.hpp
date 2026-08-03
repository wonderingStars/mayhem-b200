/*
 * mayhem-b200 — RTTY TX (Baudot/ITA2 2FSK teletype transmitter).
 *
 * Host port of PortaPack Mayhem's external/rtty_tx app (copyright 2026 HTotoo).
 *
 * The upstream chain is split across three files, all ported here faithfully:
 *
 *   external/rtty_tx/baudot.*     -> the ITA2 (Baudot) letters/figures coder,
 *                                    which turns text into 5-bit codes with
 *                                    LTRS/FIGS shift codes inserted on demand.
 *   baseband/proc_rtty_tx.cpp     -> the on-wire framing: a 15x LTRS + CR + LF
 *                                    preamble, then each 5-bit code sent LSB
 *                                    first as [start=space][d0..d4][stop=mark],
 *                                    then a trailing CR + LF.
 *
 * On the firmware the M4 baseband walks a ring buffer of 5-bit codes and keys a
 * phase accumulator between the mark and space deltas. Here the same 5-bit codes
 * are expanded into a mark(1)/space(0) bit stream and handed to dsp::FskKeyer
 * (src/dsp/modulate.hpp), which is the direct host equivalent of proc_fsk. The
 * two tones are placed symmetrically about the carrier at +/- shift/2, so the
 * B200's LO/NCO tunes to the carrier and FskKeyer's +deviation/-deviation for a
 * 1/0 bit reproduces mark/space exactly. "Inverted" swaps them by negating the
 * deviation, matching upstream's mark/space swap.
 *
 * The pure encoder and framing live inline in this header (no radio/UHD deps) so
 * they can be unit-tested against upstream's exact output; the View and the
 * transmit plumbing live in the .cpp.
 *
 * Copyright (C) 2026 HTotoo (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_RTTY_TX_H__
#define __MB200_UI_RTTY_TX_H__

#include "ui.hpp"
#include "ui_widget.hpp"
#include "ui_freq_field.hpp"

#include "../dsp/demod.hpp"  /* cfloat */

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace app {
namespace rtty_tx {

/* --- Baudot / ITA2 coder ----------------------------------------------------
 *
 * Byte-for-byte port of external/rtty_tx/baudot.*. The two 32-entry tables are
 * indexed by the 5-bit code; index 0 and the two shift codes (0x1B FIGS,
 * 0x1F LTRS) are non-printing. encode() emits the shift code only when the
 * required shift differs from the current one, exactly as upstream. */

constexpr uint8_t CODE_FIGS = 0x1B;   /* shift to FIGURES */
constexpr uint8_t CODE_LTRS = 0x1F;   /* shift to LETTERS */
constexpr uint8_t CODE_SPACE = 0x04;  /* space is the same code in both shifts */

inline char baudot_char(bool is_figures, uint8_t index) {
    if (index >= 32) return 0;
    if (is_figures) {
        static const char figures[32] = {
            0, '3', '\n', '-', ' ', '\'', '8', '7',
            '\r', '$', '4', '\a', ',', '!', ':', '(',
            '5', '+', ')', '2', '#', '6', '0', '1',
            '9', '?', '&', 0, '.', '/', '=', 0};
        return figures[index];
    }
    static const char letters[32] = {
        0, 'E', '\n', 'A', ' ', 'S', 'I', 'U',
        '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K',
        'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q',
        'O', 'B', 'G', 0, 'M', 'X', 'V', 0};
    return letters[index];
}

class BaudotCoder {
   public:
    enum ShiftState { LETTERS, FIGURES };

    BaudotCoder() : shiftState(LETTERS) {}

    void set_usos(bool enable) { usos_enabled = enable; }
    void reset_shift() { shiftState = LETTERS; }

    char decode(uint8_t baudotCode) {
        uint8_t code = baudotCode & 0x1F;
        if (code == CODE_FIGS) {
            shiftState = FIGURES;
            return 0;
        }
        if (code == CODE_LTRS) {
            shiftState = LETTERS;
            return 0;
        }
        if (usos_enabled && shiftState == FIGURES && code == CODE_SPACE) {
            shiftState = LETTERS;
            return ' ';
        }
        return baudot_char(shiftState == FIGURES, code);
    }

    /* Upstream signature, byte-for-byte from baudot.cpp. Writes at most
     * dest_max_size codes into dest and reports the count in *dest_length. */
    void encode(const std::string& src, uint8_t* dest, uint16_t* dest_length,
                uint16_t dest_max_size) {
        uint16_t idx = 0;
        for (char c : src) {
            if (idx >= dest_max_size) break;

            char upper_c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            uint8_t code = 0;
            bool found = false;
            bool target_is_figures = false;

            if (upper_c == ' ') {
                code = CODE_SPACE;
                found = true;
            } else {
                for (int i = 1; i < 32; i++) {
                    if (baudot_char(false, static_cast<uint8_t>(i)) == upper_c) {
                        code = static_cast<uint8_t>(i);
                        found = true;
                        target_is_figures = false;
                        break;
                    }
                }
                if (!found) {
                    for (int i = 1; i < 32; i++) {
                        if (baudot_char(true, static_cast<uint8_t>(i)) == upper_c) {
                            code = static_cast<uint8_t>(i);
                            found = true;
                            target_is_figures = true;
                            break;
                        }
                    }
                }
            }

            if (found) {
                if (target_is_figures && shiftState != FIGURES) {
                    if (idx + 1 >= dest_max_size) break;
                    dest[idx++] = CODE_FIGS;
                    shiftState = FIGURES;
                } else if (!target_is_figures && shiftState != LETTERS &&
                           code != CODE_SPACE) {
                    if (idx + 1 >= dest_max_size) break;
                    dest[idx++] = CODE_LTRS;
                    shiftState = LETTERS;
                }
                dest[idx++] = code;
            }
        }
        if (dest_length) *dest_length = idx;
    }

    ShiftState shift() const { return shiftState; }

   private:
    ShiftState shiftState;
    bool usos_enabled = true;
};

/* Convenience wrapper: encode `text` into a vector of 5-bit codes. `usos` toggles
 * Unshift-On-Space; upstream's TX path calls set_usos(false) for the widest
 * compatibility, which is the default here. */
inline std::vector<uint8_t> baudot_encode(const std::string& text, bool usos = false) {
    std::vector<uint8_t> out(text.size() * 2 + 2, 0);  /* worst case: shift per char */
    BaudotCoder coder;
    coder.set_usos(usos);
    uint16_t len = 0;
    coder.encode(text, out.data(), &len, static_cast<uint16_t>(out.size()));
    out.resize(len);
    return out;
}

/* --- On-wire framing (port of proc_rtty_tx.cpp) -----------------------------
 *
 * Builds the mark(1)/space(0) bit stream the FSK keyer transmits. It reproduces
 * proc_rtty_tx::on_message exactly: 15 LTRS then CR then LF as a preamble, the
 * data codes, then a trailing CR + LF. Each 5-bit code is framed as
 *
 *     start bit (space=0), d0, d1, d2, d3, d4 (LSB first), stop bit(s) (mark=1)
 *
 * The firmware's baseband holds each character's stop state for exactly one baud
 * period regardless of the "1.0/1.5/2.0" selector (the selector is stored but
 * not used to lengthen the stop). Here `stop_bits` is an integer count of whole
 * stop bits so the FSK symbol stream stays sample-exact; the app maps the
 * selector to {1, 2} (1.5 rounds up to 2). */

constexpr uint8_t RTTY_CR = 0x08;  /* carriage return (ITA2) */
constexpr uint8_t RTTY_LF = 0x02;  /* line feed (ITA2) */

/* Appends one framed character (start + 5 data LSB-first + stop_bits) to `bits`,
 * each element 0 (space) or 1 (mark). */
inline void rtty_frame_char(std::vector<uint8_t>& bits, uint8_t code, int stop_bits) {
    bits.push_back(0);  /* start bit = space */
    for (int b = 0; b < 5; ++b)
        bits.push_back(static_cast<uint8_t>((code >> b) & 1));  /* data, LSB first, 1=mark */
    for (int s = 0; s < stop_bits; ++s)
        bits.push_back(1);  /* stop bit(s) = mark */
}

/* Full frame: preamble (15 LTRS, CR, LF) + data + trailing (CR, LF). */
inline std::vector<uint8_t> rtty_build_frame_bits(const std::vector<uint8_t>& data_codes,
                                                  int stop_bits) {
    if (stop_bits < 1) stop_bits = 1;
    std::vector<uint8_t> bits;
    bits.reserve((data_codes.size() + 19) * (6 + stop_bits + 1));

    for (int i = 0; i < 15; ++i)
        rtty_frame_char(bits, CODE_LTRS, stop_bits);
    rtty_frame_char(bits, RTTY_CR, stop_bits);
    rtty_frame_char(bits, RTTY_LF, stop_bits);

    for (uint8_t code : data_codes)
        rtty_frame_char(bits, code, stop_bits);

    rtty_frame_char(bits, RTTY_CR, stop_bits);
    rtty_frame_char(bits, RTTY_LF, stop_bits);
    return bits;
}

/* Packs a 0/1 bit stream into bytes MSB-first, the order dsp::bit_at()/FskKeyer
 * read. Returns the packed bytes; the bit count is the input size. */
inline std::vector<uint8_t> pack_bits_msb(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> bytes((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i)
        if (bits[i])
            bytes[i >> 3] |= static_cast<uint8_t>(0x80 >> (i & 7));
    return bytes;
}

}  // namespace rtty_tx

/* --- View ------------------------------------------------------------------ */

class RttyTxView : public ui::View {
   public:
    RttyTxView();
    ~RttyTxView() override;

    RttyTxView(const RttyTxView&) = delete;
    RttyTxView& operator=(const RttyTxView&) = delete;

    std::string title() const override { return "RTTY TX"; }

    void focus() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void refresh_tones();
    void build_waveform();
    void start_tx();
    void stop_tx();

    static constexpr double kSampleRate = 48000.0;

    std::string message_{"CQ CQ DE MAYHEM"};

    /* Pre-built complex baseband, streamed to the transmitter in Raw mode. */
    std::vector<dsp::cfloat> waveform_{};
    std::atomic<size_t> play_pos_{0};
    std::atomic<bool> transmitting_{false};

    ui::Labels labels_{
        {{0, 8}, "RTTY teletype TX", ui::Color::light_grey()},
        {{0, 40}, "Baud", ui::Color::light_grey()},
        {{0, 64}, "Shift Hz", ui::Color::light_grey()},
        {{0, 88}, "Stop bits", ui::Color::light_grey()},
        {{0, 136}, "Frequency", ui::Color::light_grey()},
        {{0, 168}, "Message", ui::Color::light_grey()},
    };

    ui::Text text_warning_{
        {0, 24, 240, 16},
        ""};

    ui::OptionsField options_baud_{
        {120, 40},
        6,
        {{"45.45", 4545},
         {"45", 4500},
         {"50", 5000},
         {"75", 7500},
         {"100", 10000},
         {"110", 11000},
         {"150", 15000},
         {"200", 20000}}};

    ui::OptionsField options_shift_{
        {120, 64},
        4,
        {{"170", 170},
         {"85", 85},
         {"200", 200},
         {"425", 425},
         {"850", 850}}};

    ui::OptionsField options_stop_{
        {120, 88},
        4,
        {{"1.0", 1}, {"1.5", 2}, {"2.0", 2}}};

    ui::Checkbox check_inverted_{{0, 110}, 10, "Inverted"};

    ui::Text text_tones_{{0, 112, 240, 16}, ""};

    ui::FrequencyField field_frequency_{{0, 152}};

    ui::Button button_message_{{0, 184, 240, 28}, "Set message"};
    ui::Text text_message_{{0, 216, 240, 16}, ""};

    ui::Button button_tx_{{0, 240, 240, 32}, "Start TX"};

    ui::Text text_status_{{0, 280, 240, 16}, ""};
};

}  // namespace app

#endif /*__MB200_UI_RTTY_TX_H__*/
