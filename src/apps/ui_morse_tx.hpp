/*
 * mayhem-b200 — Morse TX (CW / MCW keyer).
 *
 * Host port of PortaPack Mayhem's external/morse_tx app (copyright 2015 Jared
 * Boone / 2016 Furrtek), whose encoder lives in firmware/common/morse.* .
 *
 * The upstream encoder (morse_encode) turns text into a stream of five symbol
 * kinds — dot, dash, symbol-space, letter-space, word-space — using the ITU code
 * table, where every letter's entry packs the dot/dash pattern in the high bits
 * and its length in the low three bits. That table and the encode algorithm are
 * ported here byte-for-byte. The PARIS timing standard is upstream's too:
 * time_unit_ms = 1200 / wpm, and each symbol lasts morse_symbols[kind] units
 * (dot 1, dash 3, symbol-space 1, letter-space 3, word-space 7).
 *
 * The firmware transmits CW by keying the HackRF's TX enable pin on/off per
 * symbol (external/morse_tx keys GPIO), and MCW/FM by playing a gated tone. Here
 * the symbol stream is expanded to one on/off flag per time unit and rendered to
 * complex baseband: CW uses dsp::OokKeyer (carrier keyed on/off, A1A), FM/MCW
 * gates a sidetone and runs it through dsp::FmModulator (carrier continuous).
 *
 * The pure encoder and unit expansion live inline in this header (no radio/UHD
 * deps) so they can be unit-tested against upstream's exact output.
 *
 * Copyright (C) 2015 Jared Boone / 2016 Furrtek (original app + encoder)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_MORSE_TX_H__
#define __MB200_UI_MORSE_TX_H__

#include "ui.hpp"
#include "ui_widget.hpp"
#include "ui_freq_field.hpp"

#include "../dsp/demod.hpp"  /* cfloat */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace app {
namespace morse_tx {

/* Symbol kinds, matching morse.hpp's morse_message encoding. */
enum MorseSymbol : uint8_t {
    DOT = 0,
    DASH = 1,
    SYMBOL_SPACE = 2,
    LETTER_SPACE = 3,
    WORD_SPACE = 4,
};

/* Duration of each symbol in time units (morse.hpp morse_symbols). */
constexpr uint32_t morse_symbols[5] = {1, 3, 1, 3, 7};

/* 0=dot, 1=dash; the low three bits are the code length. Byte-for-byte from
 * firmware/common/morse.hpp morse_ITU, indexed by (char - '!'). */
constexpr uint16_t morse_ITU[63] = {
    0b1010110000000110,  // !
    0b0100100000000110,  // "
    0,                   // #
    0b0001001000000111,  // $
    0,                   // %
    0b0100000000000101,  // &
    0b0111100000000110,  // '
    0b1011000000000101,  // (
    0b1011010000000110,  // )
    0,                   // *
    0b0101000000000101,  // +
    0b1100110000000110,  // ,
    0b1000010000000110,  // -
    0b0101010000000110,  // .
    0b1001000000000101,  // /
    0b1111100000000101,  // 0
    0b0111100000000101,  // 1
    0b0011100000000101,  // 2
    0b0001100000000101,  // 3
    0b0000100000000101,  // 4
    0b0000000000000101,  // 5
    0b1000000000000101,  // 6
    0b1100000000000101,  // 7
    0b1110000000000101,  // 8
    0b1111000000000101,  // 9
    0b1110000000000110,  // :
    0b1010100000000110,  // ;
    0,                   // <
    0b1000100000000101,  // =
    0,                   // >
    0b0011000000000110,  // ?
    0b0110100000000110,  // @
    0b0100000000000010,  // A
    0b1000000000000100,  // B
    0b1010000000000100,  // C
    0b1000000000000011,  // D
    0b0000000000000001,  // E
    0b0010000000000100,  // F
    0b1100000000000011,  // G
    0b0000000000000100,  // H
    0b0000000000000010,  // I
    0b0111000000000100,  // J
    0b1010000000000011,  // K
    0b0100000000000100,  // L
    0b1100000000000010,  // M
    0b1000000000000010,  // N
    0b1110000000000011,  // O
    0b0110000000000100,  // P
    0b1101000000000100,  // Q
    0b0100000000000011,  // R
    0b0000000000000011,  // S
    0b1000000000000001,  // T
    0b0010000000000011,  // U
    0b0001000000000100,  // V
    0b0110000000000011,  // W
    0b1001000000000100,  // X
    0b1011000000000100,  // Y
    0b1100000000000100,  // Z
    0,                   // [
    0,                   // backslash
    0,                   // ]
    0,                   // ^
    0b0011010000000110   // _
};

/* Port of morse_encode (firmware/common/morse.cpp). Appends the symbol stream to
 * `out` and returns the symbol count, or 0 if the message is empty or would
 * exceed the 256-symbol buffer (upstream's "message too long"). Any character
 * outside '!'..'_' (after upper-casing) is a word gap. */
inline size_t morse_encode(const std::string& message, std::vector<uint8_t>& out) {
    out.clear();
    uint8_t mm[256];
    size_t i = 0;

    for (char ch0 : message) {
        if (i >= 256) return 0;  // too long

        char ch = ch0;
        if (ch >= 'a' && ch <= 'z') ch -= 32;  // upper-case

        uint16_t code = (ch >= '!' && ch <= '_') ? morse_ITU[ch - '!'] : 0;

        if (!code) {
            if (i) mm[i - 1] = WORD_SPACE;  // gap between words
        } else {
            const uint16_t code_size = code & 7;
            for (uint16_t c = 0; c < code_size; c++) {
                if (i >= 256) return 0;
                mm[i++] = ((code << c) & 0x8000) ? DASH : DOT;
                if (i >= 256) return 0;
                mm[i++] = SYMBOL_SPACE;
            }
            mm[i - 1] = LETTER_SPACE;
        }
    }

    if (i == 0) return 0;
    out.assign(mm, mm + i);
    return i;
}

/* Total transmit length in time units (sum of morse_symbols). */
inline uint32_t morse_time_units(const std::vector<uint8_t>& symbols) {
    uint32_t units = 0;
    for (uint8_t s : symbols)
        if (s < 5) units += morse_symbols[s];
    return units;
}

/* PARIS-standard unit length in ms: 1200 / wpm (integer, as upstream). */
inline uint32_t morse_time_unit_ms(uint32_t wpm) {
    return wpm ? 1200u / wpm : 0u;
}

/* Expands the symbol stream to one on/off flag per time unit: on (1) during a
 * dot or dash, off (0) during any space. This is the keying envelope. */
inline std::vector<uint8_t> morse_expand_units(const std::vector<uint8_t>& symbols) {
    std::vector<uint8_t> onoff;
    onoff.reserve(morse_time_units(symbols));
    for (uint8_t s : symbols) {
        if (s >= 5) continue;
        const bool on = (s < 2);  // dot or dash
        for (uint32_t k = 0; k < morse_symbols[s]; ++k)
            onoff.push_back(on ? 1 : 0);
    }
    return onoff;
}

/* Packs a 0/1 stream MSB-first for dsp::bit_at()/OokKeyer. */
inline std::vector<uint8_t> pack_bits_msb(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> bytes((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i)
        if (bits[i])
            bytes[i >> 3] |= static_cast<uint8_t>(0x80 >> (i & 7));
    return bytes;
}

}  // namespace morse_tx

/* --- View ------------------------------------------------------------------ */

class MorseTxView : public ui::View {
   public:
    MorseTxView();
    ~MorseTxView() override;

    MorseTxView(const MorseTxView&) = delete;
    MorseTxView& operator=(const MorseTxView&) = delete;

    std::string title() const override { return "Morse TX"; }

    void focus() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void update_duration();
    void build_waveform();
    void start_tx();
    void stop_tx();

    static constexpr double kSampleRate = 48000.0;
    static constexpr double kFmDeviation = 2500.0;  /* NFM MCW deviation */

    std::string message_{"SOS"};

    std::vector<dsp::cfloat> waveform_{};
    std::atomic<size_t> play_pos_{0};
    std::atomic<bool> transmitting_{false};
    bool loop_{false};

    ui::Labels labels_{
        {{0, 8}, "Morse CW / MCW TX", ui::Color::light_grey()},
        {{0, 48}, "Speed wpm", ui::Color::light_grey()},
        {{0, 72}, "Tone Hz", ui::Color::light_grey()},
        {{0, 96}, "Mode", ui::Color::light_grey()},
        {{0, 136}, "Frequency", ui::Color::light_grey()},
        {{0, 168}, "Message", ui::Color::light_grey()},
    };

    ui::Text text_warning_{{0, 24, 240, 16}, ""};

    ui::NumberField field_speed_{{120, 48}, 3, {5, 60}, 1, ' '};
    ui::NumberField field_tone_{{120, 72}, 4, {100, 9999}, 20, ' '};

    ui::OptionsField options_mode_{
        {120, 96},
        3,
        {{"CW", 0}, {"FM", 1}}};

    ui::Checkbox check_loop_{{0, 116}, 6, "Loop"};

    ui::Text text_duration_{{120, 116, 120, 16}, ""};

    ui::FrequencyField field_frequency_{{0, 152}};

    ui::Button button_message_{{0, 184, 240, 28}, "Set message"};
    ui::Text text_message_{{0, 216, 240, 16}, ""};

    ui::Button button_tx_{{0, 240, 240, 32}, "Start TX"};

    ui::Text text_status_{{0, 280, 240, 16}, ""};
};

}  // namespace app

#endif /*__MB200_UI_MORSE_TX_H__*/
