/*
 * mayhem-b200 — OOK / Encoders TX (preset remote-control encoders + de Bruijn).
 *
 * Port of the firmware's application/apps/ui_encoders.* together with the parts
 * of baseband/proc_ook.cpp and application/protocols/encoders.* that the app
 * depends on. Two functions live in this file:
 *
 *   Config tab   — pick a preset remote-encoder chip (PT2262, PT2260, HT12E,
 *                  MC145026, …), type the address/data word symbol-by-symbol,
 *                  choose the oscillator clock, and emit the framed OOK burst.
 *                  The preset table (`encoder_defs`) is copied verbatim from
 *                  upstream so the framing, the per-symbol fragment patterns,
 *                  the sync word and the timing are bit-for-bit what a real
 *                  chip produces.
 *   de Bruijn tab — brute a whole N-bit code space with a de Bruijn sequence,
 *                  so every N-bit code appears exactly once in one continuous
 *                  transmission. The sequence is generated with Duval's
 *                  algorithm, ported from proc_ook.cpp's duval_algo_step().
 *
 * How it differs from the firmware, and why:
 *
 *  - The M4 baseband image (proc_ook) is replaced by dsp::OokKeyer streaming
 *    complex baseband into radio::TransmitterModel in Raw mode. proc_ook counts
 *    whole samples-per-bit against a fixed 2.28 MHz; OokKeyer takes an exact
 *    symbol rate against the streamed rate, and where the two divide evenly the
 *    sample counts are identical (see samples_per_bit() below and the tests).
 *  - make_bitstream() wrote into shared_memory.bb_data; here pack_fragments()
 *    writes into a caller-owned vector with the same MSB-first packing, so the
 *    exact same bytes reach the keyer.
 *  - The de Bruijn order is capped at 20 on the host (upstream allows 24). At
 *    order 20 the packed fragment stream is already 512 KiB; the cap keeps a
 *    misfire from allocating gigabytes. The pure generator is not capped and is
 *    tested at small orders.
 *
 * The pure encoder logic (the preset table, frame generation, bit packing and
 * the de Bruijn generator) is in namespace app::ook with no UI or radio
 * dependency, so tests/test_encoders.cpp can assert its output against values
 * taken from the upstream sources.
 *
 * LEGALITY: these are the codes of real garage doors, gates and car remotes.
 * Radiating them can be illegal where you are and is your responsibility. The
 * app never transmits on its own — only when you press Start — and shows a
 * warning on screen.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_ENCODERS_H__
#define __MB200_UI_ENCODERS_H__

#include "../dsp/modulate.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace app {

/* ======================================================================== *
 *  Pure encoder logic — no UI, no radio. Ported from                        *
 *  application/protocols/encoders.* and baseband/proc_ook.cpp.              *
 * ======================================================================== */
namespace ook {

/* The firmware's fixed baseband rate for the OOK image. Kept as the host's TX
 * streamed rate so samples-per-symbol match upstream exactly. */
constexpr uint32_t kOokSampleRate = 2'280'000U;

constexpr size_t kEncTypesCount = 14;

/* The index of the UM3750 entry, guarded so a reorder is caught. Matches
 * upstream's ENCODER_UM3750. */
constexpr size_t kEncoderUm3750 = 8;

/* Verbatim copy of encoders::encoder_def_t. */
struct encoder_def_t {
    char name[16];              // Encoder chip ref/name
    char address_symbols[8];    // List of possible symbols like "01", "01F"...
    char data_symbols[8];       // Same
    uint16_t clk_per_symbol;    // Oscillator periods per symbol
    uint16_t clk_per_fragment;  // Oscillator periods per symbol fragment (state)
    char bit_format[4][20];     // Fragments per symbol, in *_symbols order
    uint8_t word_length;        // Total # of symbols (not counting sync)
    char word_format[32];       // A for Address, D for Data, S for sync
    char sync[64];              // Like bit_format
    uint32_t default_speed;     // Default encoder clk frequency
    uint8_t repeat_min;         // Minimum repeat count
    uint16_t pause_symbols;     // Length of pause between repeats in symbols
};

/* The preset table, copied byte-for-byte from encoders.hpp. */
extern const encoder_def_t encoder_defs[kEncTypesCount];

/* Port of encoders::make_bitstream(): pack a string of '0'/'1' fragments into
 * bytes, MSB-first, the last partial byte left-shifted into the high bits.
 * Returns the number of fragment bits written. `out` is sized and overwritten. */
size_t pack_fragments(const std::string& fragments, std::vector<uint8_t>& out);

/* Port of EncodersConfigView::generate_frame(): walk the encoder's word_format,
 * appending the sync word for each 'S' and, for every other slot, the fragment
 * pattern bit_format[offset] where `offset` is that slot's chosen symbol index
 * within its symbol list. `offsets` holds one index per non-sync slot, in order
 * (its length must be encoder_def.word_length). */
std::string generate_frame(const encoder_def_t& def, const std::vector<size_t>& offsets);

/* Upstream EncodersConfigView::samples_per_bit(): OOK_SAMPLERATE divided by the
 * fragment rate, with the same integer arithmetic. `clk_khz` is the oscillator
 * clock in kHz; `clk_per_fragment` is encoder_def_t::clk_per_fragment. */
uint32_t samples_per_bit(uint32_t clk_khz, uint16_t clk_per_fragment);

/* Duval's algorithm for a de Bruijn sequence B(k, n): the concatenation of all
 * Lyndon words over a k-symbol alphabet whose length divides n, in lexical
 * order. The result has length k^n and, read cyclically, contains every n-tuple
 * over the alphabet exactly once. Ported from proc_ook.cpp::duval_algo_step()
 * (which streams the same sequence one Lyndon word at a time). */
std::vector<uint8_t> de_bruijn_sequence(unsigned k, unsigned n);

/* The binary (k=2) de Bruijn sequence for order n expanded to OOK fragments,
 * using proc_ook's symbol encoding: 0 -> "1000", 1 -> "1110". */
std::string de_bruijn_fragments(unsigned n);

}  // namespace ook

/* ======================================================================== *
 *  Views                                                                    *
 * ======================================================================== */

/* Config tab: preset chip + symbol-by-symbol word entry. */
class OokEncodersConfigView : public ui::View {
   public:
    explicit OokEncodersConfigView(ui::Rect parent_rect);

    OokEncodersConfigView(const OokEncodersConfigView&) = delete;
    OokEncodersConfigView& operator=(const OokEncodersConfigView&) = delete;

    void focus() override;
    void on_show() override;

    const ook::encoder_def_t& encoder() const { return *encoder_def_; }
    uint8_t repeat_min() const;
    uint32_t clk_khz() const;
    /* Symbol rate (fragments per second) for the keyer. */
    double symbol_rate() const;
    /* Regenerate and return the current framed fragment string. */
    const std::string& frame_fragments();

   private:
    void on_type_change(size_t index);
    void regenerate();
    void draw_waveform();

    const ook::encoder_def_t* encoder_def_{&ook::encoder_defs[0]};
    std::string frame_fragments_{};

    static constexpr size_t kWaveformBufferSize = 550;
    int16_t waveform_buffer_[kWaveformBufferSize]{};

    ui::Labels labels_{
        {{1 * 8, 0}, "Type:", ui::Color::light_grey()},
        {{17 * 8, 0}, "Repeat:", ui::Color::light_grey()},
        {{1 * 8, 2 * 8}, "Clk:", ui::Color::light_grey()},
        {{10 * 8, 2 * 8}, "kHz", ui::Color::light_grey()},
        {{1 * 8, 4 * 8}, "Symbols:", ui::Color::light_grey()},
        {{1 * 8, 11 * 8}, "Waveform:", ui::Color::light_grey()}};

    ui::OptionsField options_enctype_{{6 * 8, 0}, 7, {}};

    ui::NumberField field_repeat_min_{{24 * 8, 0}, 2, {1, 99}, 1, ' '};

    ui::NumberField field_clk_{{5 * 8, 2 * 8}, 4, {1, 1000}, 1, ' '};
    ui::OptionsField field_clk_step_{{22 * 8, 2 * 8}, 5, {{"1", 1}, {"10", 10}, {"100", 100}}};

    std::vector<std::unique_ptr<ui::SymField>> symfields_word_{};

    ui::Text text_format_{{2 * 8, 8 * 8, 26 * 8, 16}, ""};

    ui::Waveform waveform_{
        {0, 13 * 8, ui::screen_width, 32},
        waveform_buffer_,
        0,
        0,
        true,
        ui::Color::yellow()};
};

/* de Bruijn tab: brute a code space. */
class OokEncodersScanView : public ui::View {
   public:
    explicit OokEncodersScanView(ui::Rect parent_rect);

    void focus() override;

    unsigned order() const { return static_cast<unsigned>(field_length_.value()); }
    /* Per-fragment duration in microseconds. */
    double bit_length_us() const {
        return static_cast<double>(bit_length_10_.value()) * 10.0 +
               static_cast<double>(bit_length_.value());
    }

   private:
    ui::Labels labels_{
        {{1 * 8, 0 * 8}, "Order:", ui::Color::light_grey()},
        {{1 * 8, 2 * 8}, "Bit length:", ui::Color::light_grey()},
        {{16 * 8, 2 * 8}, "us", ui::Color::light_grey()},
        {{1 * 8, 5 * 8}, "Covers every N-bit code once.", ui::Color::light_grey()}};

    /* Upstream allows 3..24; the host caps at 20 (see the header note). */
    ui::NumberField field_length_{{8 * 8, 0}, 2, {3, 20}, 1, ' '};
    ui::NumberField bit_length_10_{{12 * 8, 2 * 8}, 2, {1, 88}, 1, ' '};
    ui::NumberField bit_length_{{14 * 8, 2 * 8}, 1, {0, 9}, 1, ' '};
};

class OokTxView : public ui::View {
   public:
    OokTxView();
    ~OokTxView() override;

    OokTxView(const OokTxView&) = delete;
    OokTxView& operator=(const OokTxView&) = delete;

    std::string title() const override { return "OOK TX"; }

    void focus() override;
    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void toggle_tx();
    void start_tx(bool scan);
    void stop_tx();

    bool transmitting_{false};
    std::vector<uint8_t> bits_{};
    size_t bit_count_{0};
    std::shared_ptr<dsp::OokKeyer> keyer_{};

    ui::Rect view_rect_{0, 3 * 8, ui::screen_width, 176};

    OokEncodersConfigView config_view_{view_rect_};
    OokEncodersScanView scan_view_{view_rect_};

    ui::TabView tab_view_{
        {"Config", ui::Color::cyan(), &config_view_},
        {"de Bruijn", ui::Color::green(), &scan_view_}};

    ui::FrequencyField field_frequency_{{0, 26 * 8}};

    ui::Text text_status_{{0, 28 * 8, 15 * 8, 16}, "Ready"};
    ui::ProgressBar progressbar_{{0, 30 * 8, ui::screen_width, 16}};

    ui::Text text_warning_{{0, 32 * 8, ui::screen_width, 16},
                           "TX may be illegal - you are responsible"};

    ui::Button button_start_{{4 * 8, 34 * 8, 16 * 8, 28}, "Start TX"};
};

}  // namespace app

#endif /*__MB200_UI_ENCODERS_H__*/
