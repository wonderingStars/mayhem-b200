/*
 * mayhem-b200 — AFSK receive terminal (generic Bell 202 / audio-FSK modem).
 *
 * Ported from the PortaPack firmware:
 *
 *   application/external/afsk_rx/ui_afsk_rx.{hpp,cpp}
 *       -> the console terminal, the 8-bit reverse-and-mask that turns a
 *          received word into a character, the "message split" on a DEL run,
 *          the per-message colour cycle, the LOG file
 *   baseband/proc_afskrx.cpp
 *       -> the word framer: the RS232-like WAIT_START / RECEIVE / WAIT_STOP
 *          state machine and the continuous value-triggered (AX.25) mode
 *   application/protocols/modems.{hpp,cpp}
 *       -> the seven modem presets and deframe_word()'s bit-order handling
 *   application/apps/ui_modemsetup.hpp
 *       -> the baudrate / mark / space / serial-format controls, which live in
 *          this view rather than a separate app: ModemSetupView is a shared
 *          firmware app that does not exist in this tree, and the porting
 *          contract forbids creating shared files.
 *
 * Departures from upstream, each marked where it appears:
 *
 *   1. Bit recovery uses the Phase A dsp::AfskDemod (delay-and-multiply
 *      correlator, designed lowpass, tracked-level slicer, the same
 *      phase-accumulator bit clock with the clean-transition nudge) instead of
 *      proc_afskrx's inline fixed-point version. That class is upstream's
 *      algorithm with the two fixes documented in demod_digital.hpp; both
 *      produce one 0/1 per bit period, which is all the framer consumes.
 *
 *   2. afsk_deframe_word() generalises the character transform to the selected
 *      word length, parity and bit order. For upstream's own configuration
 *      (7 data bits, parity, LSB first) it is bit-for-bit identical to the
 *      hard-coded 8-bit reversal in AFSKRxView::on_data — asserted in the
 *      tests over all 256 inputs. afsk_display_value() is that hard-coded
 *      transform, kept verbatim as the reference.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2023 Bernd Herzog
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_AFSK_RX_H__
#define __MB200_UI_AFSK_RX_H__

#include "../dsp/demod.hpp"
#include "../dsp/demod_digital.hpp"
#include "../dsp/fir.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace app {

/* ===========================================================================
 * Modem presets — modems.hpp, modem_defs[]
 * ===========================================================================*/

enum class AfskModulation : uint8_t { Afsk = 0, Fsk, Psk, Am };

struct AfskModemDef {
    const char* name;
    AfskModulation modulation;
    uint16_t mark_freq;
    uint16_t space_freq;
    uint16_t baudrate;
};

inline constexpr size_t kAfskModemDefCount = 7;

inline constexpr std::array<AfskModemDef, kAfskModemDefCount> kAfskModemDefs{{
    {"Bell202", AfskModulation::Afsk, 1200, 2200, 1200},
    {"Bell103", AfskModulation::Afsk, 1270, 1070, 300},
    {"V21", AfskModulation::Afsk, 980, 1180, 300},
    {"V23 M1", AfskModulation::Afsk, 1300, 1700, 600},
    {"V23 M2", AfskModulation::Afsk, 1300, 2100, 1200},
    {"RTTY US", AfskModulation::Am, 2295, 2125, 45},
    {"RTTY EU", AfskModulation::Am, 2125, 1955, 45},
}};

/* ===========================================================================
 * Serial format — serializer.hpp's serial_format_t
 * ===========================================================================*/

enum class AfskSerialParity : uint8_t { None = 0, Even = 1, Odd = 2 };
enum class AfskSerialBitOrder : uint8_t { MsbFirst = 0, LsbFirst = 1 };

struct AfskSerialFormat {
    uint8_t data_bits{7};
    AfskSerialParity parity{AfskSerialParity::Even};
    uint8_t stop_bits{1};
    AfskSerialBitOrder bit_order{AfskSerialBitOrder::LsbFirst};

    bool has_parity() const { return parity != AfskSerialParity::None; }
    /* Bits the framer counts between the start bit and the stop bit. Upstream's
     * afsk_rx hard-codes 8, which is what 7 data bits plus parity gives. */
    uint8_t word_length() const {
        return static_cast<uint8_t>(data_bits + (has_parity() ? 1 : 0));
    }
};

/* ===========================================================================
 * Word -> character
 * ===========================================================================*/

inline uint32_t afsk_reverse_bits(uint32_t value, uint8_t count) {
    uint32_t reversed = 0;
    for (uint8_t i = 0; i < count; i++)
        reversed = (reversed << 1) | ((value >> i) & 1u);
    return reversed;
}

/* Verbatim AFSKRxView::on_data(): mask to eight bits, reverse them (nibbles,
 * then pairs, then neighbours), then drop the parity bit that reversal has just
 * moved into the MSB. Kept as the reference the general form is checked
 * against. */
inline uint32_t afsk_display_value(uint32_t value) {
    value &= 0xFF;                                          // ABCDEFGH
    value = ((value & 0xF0) >> 4) | ((value & 0x0F) << 4);  // EFGHABCD
    value = ((value & 0xCC) >> 2) | ((value & 0x33) << 2);  // GHEFCDAB
    value = ((value & 0xAA) >> 1) | ((value & 0x55) << 1);  // HGFEDCBA
    value &= 0x7F;                                          // Ignore parity, now the MSB
    return value;
}

/* The general form, following modems.cpp deframe_word(): the framer shifts bits
 * in most-significant-first, so an LSB-first line needs the whole word
 * reversed, while an MSB-first line only needs the trailing parity bit shifted
 * off. */
inline uint32_t afsk_deframe_word(uint32_t word,
                                  uint8_t data_bits,
                                  bool has_parity,
                                  AfskSerialBitOrder bit_order) {
    const uint8_t word_length = static_cast<uint8_t>(data_bits + (has_parity ? 1 : 0));
    const uint32_t word_mask =
        (word_length >= 32) ? 0xFFFFFFFFu : ((1u << word_length) - 1u);
    const uint32_t data_mask = (data_bits >= 32) ? 0xFFFFFFFFu : ((1u << data_bits) - 1u);

    uint32_t value = word & word_mask;
    if (bit_order == AfskSerialBitOrder::LsbFirst)
        value = afsk_reverse_bits(value, word_length);
    else if (has_parity)
        value >>= 1;

    return value & data_mask;
}

/* Printable characters as themselves, everything else as "[hh]" — upstream's
 * console formatting. */
inline std::string afsk_format_byte(uint32_t value) {
    if ((value >= 32) && (value < 127)) return std::string(1, static_cast<char>(value));

    static const char* kHex = "0123456789ABCDEF";
    std::string out = "[";
    out.push_back(kHex[(value >> 4) & 0xF]);
    out.push_back(kHex[value & 0xF]);
    out.push_back(']');
    return out;
}

/* ===========================================================================
 * Word framer — proc_afskrx.cpp
 *
 * Fed one recovered bit per bit period. Two modes, both upstream's:
 *   - RS232-like: wait for a start bit (0), take `word_length` bits, then wait
 *     for a stop bit (1).
 *   - Value-triggered: a continuous stream that starts emitting once a sync
 *     value appears (upstream's untested AX.25 path).
 *
 * word_bits_ is deliberately never cleared between words, as upstream's is; the
 * low `word_length` bits are always the most recent word, and the consumer
 * masks.
 * ===========================================================================*/

class AfskWordFramer {
   public:
    enum class State : uint8_t { WaitStart = 0, WaitStop, Receive };

    void configure(uint32_t word_length, bool trigger_word, uint32_t trigger_value) {
        word_length_ = word_length;
        trigger_word_ = trigger_word;
        trigger_value_ = trigger_value;
        word_mask_ = (word_length >= 32) ? 0xFFFFFFFFu : ((1u << word_length) - 1u);
        reset();
    }

    void reset() {
        state_ = State::WaitStart;
        bit_counter_ = 0;
        word_bits_ = 0;
        triggered_ = false;
    }

    void feed_bit(uint8_t bit);

    void feed_bits(const std::vector<uint8_t>& bits) {
        for (const uint8_t b : bits) feed_bit(b);
    }

    /* Upstream pushes AFSKDataMessage{is_data=true, value}; the value is the
     * raw accumulator, unmasked, exactly as sent. */
    std::function<void(uint32_t)> on_word{};

    State state() const { return state_; }
    uint32_t word_length() const { return word_length_; }
    uint32_t word_mask() const { return word_mask_; }
    bool triggered() const { return triggered_; }
    size_t words_emitted() const { return words_emitted_; }

   private:
    uint32_t word_length_{8};
    uint32_t word_mask_{0xFFu};
    uint32_t trigger_value_{0};
    uint32_t word_bits_{0};
    uint32_t bit_counter_{0};
    size_t words_emitted_{0};
    State state_{State::WaitStart};
    bool trigger_word_{false};
    bool triggered_{false};
};

/* Port of the word-assembly half of AFSKRxProcessor::execute(). Upstream keeps
 * a slicer history in `sample_bits` and reads `sample_bits & 1` as the current
 * bit; the slicer and the bit clock live in dsp::AfskDemod here, so the current
 * bit arrives directly and that shift register is redundant. Inline so the
 * framer can be linked and tested without the view. */
inline void AfskWordFramer::feed_bit(uint8_t bit) {
    const uint32_t b = bit & 1u;

    if (trigger_word_) {
        /* Continuous-stream value-triggered mode (AX.25). Upstream marks this
         * path UNTESTED; it stays untested against real signals here too. */
        word_bits_ = (word_bits_ << 1) | b;
        bit_counter_++;

        if (triggered_) {
            if (bit_counter_ == word_length_) {
                bit_counter_ = 0;
                words_emitted_++;
                if (on_word) on_word(word_bits_ & word_mask_);
            }
        } else {
            if ((word_bits_ & word_mask_) == trigger_value_) {
                triggered_ = !triggered_;
                bit_counter_ = 0;
                words_emitted_++;
                if (on_word) on_word(trigger_value_);
            }
        }
        return;
    }

    /* RS232-like modem mode. */
    if (state_ == State::WaitStart) {
        if (b == 0) {
            /* Got start bit. It is not part of the word. */
            state_ = State::Receive;
            bit_counter_ = 0;
        }
    } else if (state_ == State::WaitStop) {
        if (b != 0) {
            /* Got stop bit. */
            state_ = State::WaitStart;
        }
    } else {
        word_bits_ = (word_bits_ << 1) | b;
        bit_counter_++;
    }

    if (bit_counter_ == word_length_) {
        bit_counter_ = 0;
        state_ = State::WaitStop;
        words_emitted_++;
        if (on_word) on_word(word_bits_);
    }
}

/* ===========================================================================
 * Audio / baseband front end
 * ===========================================================================*/

class AfskAudioDecoder {
   public:
    void configure(float audio_rate_hz,
                   float mark_hz,
                   float space_hz,
                   float baud,
                   uint32_t word_length,
                   bool trigger_word = false,
                   uint32_t trigger_value = 0) {
        audio_rate_hz_ = audio_rate_hz;
        demod_.configure(audio_rate_hz, mark_hz, space_hz, baud);
        framer_.configure(word_length, trigger_word, trigger_value);
        reset();
    }

    void reset() {
        demod_.reset();
        framer_.reset();
        bits_.clear();
    }

    /* Real audio — a sound card, or the output of an FM discriminator. */
    void process_audio(const float* in, size_t count) {
        bits_.clear();
        demod_.process_audio(in, count, bits_);
        framer_.feed_bits(bits_);
    }

    /* Complex baseband on an FM carrier, which is how AFSK arrives from the
     * air; AfskDemod discriminates it internally. */
    void process_baseband(const dsp::cfloat* in, size_t count) {
        bits_.clear();
        demod_.process(in, count, bits_);
        framer_.feed_bits(bits_);
    }

    AfskWordFramer& framer() { return framer_; }
    const AfskWordFramer& framer() const { return framer_; }
    dsp::AfskDemod& demod() { return demod_; }
    float audio_rate_hz() const { return audio_rate_hz_; }
    size_t last_bit_count() const { return bits_.size(); }

   private:
    dsp::AfskDemod demod_{};
    AfskWordFramer framer_{};
    std::vector<uint8_t> bits_{};
    float audio_rate_hz_{24000.0f};
};

/* ===========================================================================
 * View
 * ===========================================================================*/

class AfskRxView : public ui::View {
   public:
    AfskRxView();
    ~AfskRxView() override;

    AfskRxView(const AfskRxView&) = delete;
    AfskRxView& operator=(const AfskRxView&) = delete;

    std::string title() const override { return "AFSK RX"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void apply_preset(size_t index);
    void reconfigure_decoder();
    void rebuild_chain();
    void pump_samples();
    void on_word(uint32_t value);
    void update_status();

    radio::ReceiverModel& receiver_;

    dsp::Nco nco_{};
    dsp::FirDecimateC decim1_{};
    dsp::FirDecimateC decim2_{};
    std::vector<dsp::cfloat> raw_{};
    std::vector<dsp::cfloat> stage1_{};
    std::vector<dsp::cfloat> channel_{};

    AfskAudioDecoder decoder_{};
    AfskSerialFormat format_{};

    double configured_rate_{0.0};
    double configured_lo_{0.0};
    double channel_rate_{0.0};
    uint32_t frame_counter_{0};
    uint64_t samples_seen_{0};
    size_t words_{0};

    /* Upstream AFSKRxView state: the running log line, the previous character
     * (a DEL run is what splits messages) and the colour cycle. */
    std::string str_log_{};
    uint32_t prev_value_{0};
    uint8_t console_color_{0};

    std::ofstream log_{};
    bool logging_{false};

    ui::Labels labels_{
        {{0, 2}, "Freq", ui::Color::light_grey()},
        {{152, 2}, "Gain", ui::Color::light_grey()},
        {{0, 22}, "Modem", ui::Color::light_grey()},
        {{0, 42}, "Baud", ui::Color::light_grey()},
        {{88, 42}, "Mk", ui::Color::light_grey()},
        {{168, 42}, "Sp", ui::Color::light_grey()},
        {{0, 62}, "Fmt", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 2}};
    ui::NumberField field_gain_{{188, 2}, 3, {0, 76}, 1, ' '};

    ui::OptionsField options_preset_{{48, 22}, 7, {}};
    ui::Checkbox check_log_{{136, 18}, 3, "LOG", true};

    ui::NumberField field_baudrate_{{40, 42}, 5, {50, 9600}, 25, ' '};
    ui::NumberField field_mark_{{112, 42}, 5, {100, 15000}, 25, ' '};
    ui::NumberField field_space_{{192, 42}, 5, {100, 15000}, 25, ' '};

    ui::OptionsField options_data_bits_{{32, 62}, 1, {{"6", 6}, {"7", 7}, {"8", 8}, {"9", 9}}};
    ui::OptionsField options_parity_{{48, 62},
                                     1,
                                     {{"N", static_cast<int32_t>(AfskSerialParity::None)},
                                      {"E", static_cast<int32_t>(AfskSerialParity::Even)},
                                      {"O", static_cast<int32_t>(AfskSerialParity::Odd)}}};
    ui::OptionsField options_stop_bits_{{64, 62}, 1, {{"0", 0}, {"1", 1}, {"2", 2}}};
    ui::OptionsField options_bit_order_{
        {80, 62},
        1,
        {{"M", static_cast<int32_t>(AfskSerialBitOrder::MsbFirst)},
         {"L", static_cast<int32_t>(AfskSerialBitOrder::LsbFirst)}}};

    ui::Text text_status_{{104, 62, 136, 16}, ""};
    ui::Text text_tap_{{0, 80, 240, 16}, ""};

    ui::Console console_{{0, 100, 240, 200}};
};

}  // namespace app

#endif /*__MB200_UI_AFSK_RX_H__*/
