/*
 * mayhem-b200 — OOK / Encoders TX (preset remote encoders + de Bruijn scan).
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_encoders.hpp"

#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"

#include <algorithm>

namespace app {
namespace ook {

/* ---------------------------------------------------------------------------
 * The preset table, copied byte-for-byte from
 * application/protocols/encoders.hpp. Do not "clean this up" — the fragment
 * patterns, sync words and clock divisors are what real chips emit.
 * ------------------------------------------------------------------------- */
const encoder_def_t encoder_defs[kEncTypesCount] = {
    // PT2260-R2
    {"2260-R2", "01F", "01", 1024, 128,
     {"10001000", "11101110", "10001110"},
     12, "AAAAAAAAAADDS", "10000000000000000000000000000000", 150000, 2, 0},

    // PT2260-R4
    {"2260-R4", "01F", "01", 1024, 128,
     {"10001000", "11101110", "10001110"},
     12, "AAAAAAAADDDDS", "10000000000000000000000000000000", 150000, 2, 0},

    // PT2262
    {"2262   ", "01F", "01F", 32, 4,
     {"10001000", "11101110", "10001110"},
     12, "AAAAAAAAAAAAS", "10000000000000000000000000000000", 20000, 4, 0},

    // 16-bit ?
    {"16-bit ", "01", "01", 32, 8,
     {"1110", "1000"},  // Opposite ?
     16, "AAAAAAAAAAAAAAAAS", "100000000000000000000", 25000, 50, 0},

    // RT1527
    {"1527   ", "01", "01", 128, 32,
     {"1000", "1110"},
     24, "SAAAAAAAAAAAAAAAAAAAADDDD", "10000000000000000000000000000000", 100000, 4, 0},

    // HK526E
    {"526E   ", "01", "01", 24, 8,
     {"110", "100"},
     12, "AAAAAAAAAAAA", "", 20000, 4, 10},

    // HT12E
    {"12E    ", "01", "01", 3, 1,
     {"011", "001"},
     12, "SAAAAAAAADDDD", "0000000000000000000000000000000000001", 3000, 4, 10},

    // VD5026 13 bits ?
    {"5026   ", "0123", "0123", 128, 8,
     {"1000000010000000", "1111111011111110", "1111111010000000", "1000000011111110"},
     12, "SAAAAAAAAAAAA", "000000000000000000000000000000000000000000000001", 100000, 4, 10},

    // UM3750
    {"UM3750 ", "01", "01", 96, 32,
     {"011", "001"},
     12, "SAAAAAAAAAAAA", "001", 100000, 4, (3 * 12) - 6},

    // UM3758
    {"UM3758 ", "01F", "01", 96, 16,
     {"011011", "001001", "011001"},
     18, "SAAAAAAAAAADDDDDDDD", "1", 160000, 4, 10},

    // BA5104
    {"BA5104 ", "01", "01", 3072, 768,
     {"1000", "1110"},
     9, "SDDAAAAAAA", "", 455000, 4, 10},

    // MC145026
    {"145026 ", "01F", "01", 16, 1,
     {"0111111101111111", "0100000001000000", "0111111101000000"},
     9, "SAAAAADDDD", "000000000000000000", 455000, 2, 2},

    // HT6*** TODO: Add individual variations
    {"HT6*** ", "01F", "01", 198, 33,
     {"011011", "001001", "001011"},
     18, "SAAAAAAAAAAAADDDDDD", "0000000000000000000000000000000000001011001011001", 80000, 3, 10},

    // TC9148
    {"TC9148 ", "01", "01", 48, 12,
     {"1000", "1110"},
     12, "AAAAAAAAAAAA", "", 455000, 3, 10}};

size_t pack_fragments(const std::string& fragments, std::vector<uint8_t>& out) {
    const size_t n = fragments.size();
    out.assign((n + 7) / 8, 0);

    uint8_t byte = 0;
    size_t len = 0;
    for (char c : fragments) {
        byte <<= 1;
        if (c != '0') byte |= 1;
        if ((len & 7) == 7) out[len >> 3] = byte;
        len++;
    }

    const size_t padding = 8 - (len & 7);
    if (padding != 8) {
        byte <<= padding;
        out[(len + padding - 1) >> 3] = byte;
    }

    return len;
}

std::string generate_frame(const encoder_def_t& def, const std::vector<size_t>& offsets) {
    std::string frame;
    size_t i = 0;
    for (char c : def.word_format) {
        if (c == '\0') break;
        if (c == 'S') {
            frame += def.sync;
        } else {
            size_t off = (i < offsets.size()) ? offsets[i] : 0;
            i++;
            if (off > 3) off = 0;
            frame += def.bit_format[off];
        }
    }
    return frame;
}

uint32_t samples_per_bit(uint32_t clk_khz, uint16_t clk_per_fragment) {
    if (clk_per_fragment == 0) return 0;
    const uint32_t fragment_rate = (clk_khz * 1000U) / clk_per_fragment;
    if (fragment_rate == 0) return 0;
    return kOokSampleRate / fragment_rate;
}

std::vector<uint8_t> de_bruijn_sequence(unsigned k, unsigned n) {
    std::vector<uint8_t> seq;
    if (k < 2 || n < 1) return seq;

    /* Duval's algorithm, the iterative form proc_ook.cpp streams. `v` holds the
     * running necklace; every time its current prefix length divides n we emit
     * that prefix (a Lyndon word whose length divides n). */
    std::vector<uint8_t> v(n, 0);
    unsigned idx = 1;
    const unsigned w = n;

    while (idx) {
        if (w % idx == 0) {
            for (unsigned j = 0; j < idx; j++)
                seq.push_back(v[j]);
        }
        for (unsigned j = 0; j < w - idx; j++)
            v[idx + j] = v[j];
        for (idx = w; (idx > 0) && (v[idx - 1] >= static_cast<uint8_t>(k - 1)); idx--) {
        }
        if (idx)
            v[idx - 1]++;
    }

    return seq;
}

std::string de_bruijn_fragments(unsigned n) {
    const auto seq = de_bruijn_sequence(2, n);
    std::string out;
    out.reserve(seq.size() * 4);
    for (uint8_t s : seq)
        out += (s == 0) ? "1000" : "1110";
    return out;
}

}  // namespace ook

/* ======================================================================== *
 *  Config tab                                                               *
 * ======================================================================== */

OokEncodersConfigView::OokEncodersConfigView(ui::Rect parent_rect) {
    set_parent_rect(parent_rect);
    hidden(true);

    add_children({&labels_, &options_enctype_, &field_repeat_min_, &field_clk_,
                  &field_clk_step_, &text_format_, &waveform_});

    std::vector<ui::OptionsField::option_t> enc_options;
    for (size_t i = 0; i < ook::kEncTypesCount; i++)
        enc_options.emplace_back(std::string{ook::encoder_defs[i].name},
                                 static_cast<int32_t>(i));

    field_clk_step_.on_change = [this](size_t, int32_t v) { field_clk_.set_step(v); };

    options_enctype_.on_change = [this](size_t index, int32_t) {
        on_type_change(index);
        set_dirty();
    };
    options_enctype_.set_options(enc_options);
    options_enctype_.set_selected_index(0);  // triggers on_type_change(0)
}

void OokEncodersConfigView::focus() {
    options_enctype_.focus();
}

void OokEncodersConfigView::on_show() {
    options_enctype_.set_selected_index(0);
}

void OokEncodersConfigView::on_type_change(size_t index) {
    if (index >= ook::kEncTypesCount) return;

    for (auto& sf : symfields_word_)
        remove_child(sf.get());
    symfields_word_.clear();

    encoder_def_ = &ook::encoder_defs[index];
    field_clk_.set_value(static_cast<int32_t>(encoder_def_->default_speed / 1000), false);
    field_repeat_min_.set_value(encoder_def_->repeat_min, false);

    /* One SymField per non-sync slot, laid out left to right. Unlike upstream
     * (which creates one per word_format[0..word_length-1] and mis-indexes the
     * S-first encoders), this keeps symfields_word_[i] aligned with the i-th
     * non-S slot, so the frame is correct for every encoder in the table. */
    ui::Point pos{2 * 8, 6 * 8};
    auto on_sym_change = [this](ui::SymField&) { regenerate(); };

    for (char c : encoder_def_->word_format) {
        if (c == '\0') break;
        if (c == 'S') continue;

        std::string list = (c == 'D') ? std::string{encoder_def_->data_symbols}
                                      : std::string{encoder_def_->address_symbols};
        if (list.empty()) list = "01";

        auto sf = std::make_unique<ui::SymField>(pos, size_t{1}, list);
        sf->on_change = on_sym_change;
        add_child(sf.get());
        symfields_word_.push_back(std::move(sf));
        pos += ui::Point{8, 0};
    }

    std::string fmt{encoder_def_->word_format};
    fmt.erase(std::remove(fmt.begin(), fmt.end(), 'S'), fmt.end());
    text_format_.set(fmt);

    regenerate();
}

void OokEncodersConfigView::regenerate() {
    std::vector<size_t> offsets;
    offsets.reserve(symfields_word_.size());
    for (auto& sf : symfields_word_)
        offsets.push_back(sf->get_offset(0));

    frame_fragments_ = ook::generate_frame(*encoder_def_, offsets);
    draw_waveform();
}

const std::string& OokEncodersConfigView::frame_fragments() {
    regenerate();
    return frame_fragments_;
}

void OokEncodersConfigView::draw_waveform() {
    constexpr size_t kPadLeft = 1;
    constexpr size_t kPadRight = 1;

    size_t length = frame_fragments_.length();
    if (length + (kPadLeft + kPadRight) >= kWaveformBufferSize)
        length = kWaveformBufferSize - (kPadLeft + kPadRight);

    for (size_t i = 0; i < kPadLeft; i++)
        waveform_buffer_[i] = 0;
    for (size_t n = 0; n < length; n++)
        waveform_buffer_[n + kPadLeft] = (frame_fragments_[n] == '0') ? 0 : 1;
    for (size_t i = length + kPadLeft; i < kWaveformBufferSize; i++)
        waveform_buffer_[i] = 0;

    waveform_.set_length(static_cast<uint32_t>(length + kPadLeft + kPadRight));
    waveform_.set_dirty();
}

uint8_t OokEncodersConfigView::repeat_min() const {
    return static_cast<uint8_t>(field_repeat_min_.value());
}

uint32_t OokEncodersConfigView::clk_khz() const {
    return static_cast<uint32_t>(field_clk_.value());
}

double OokEncodersConfigView::symbol_rate() const {
    if (encoder_def_->clk_per_fragment == 0) return 1.0;
    return (static_cast<double>(clk_khz()) * 1000.0) /
           static_cast<double>(encoder_def_->clk_per_fragment);
}

/* ======================================================================== *
 *  de Bruijn tab                                                            *
 * ======================================================================== */

OokEncodersScanView::OokEncodersScanView(ui::Rect parent_rect) {
    set_parent_rect(parent_rect);
    hidden(true);

    add_children({&labels_, &field_length_, &bit_length_10_, &bit_length_});

    field_length_.set_value(8);
    bit_length_10_.set_value(40);
    bit_length_.set_value(0);
}

void OokEncodersScanView::focus() {
    field_length_.focus();
}

/* ======================================================================== *
 *  Main view                                                                *
 * ======================================================================== */

OokTxView::OokTxView() {
    add_children({&tab_view_, &config_view_, &scan_view_, &field_frequency_,
                  &text_status_, &progressbar_, &text_warning_, &button_start_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_frequency_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                                   static_cast<uint64_t>(caps.tx_freq.max));
    }

    field_frequency_.set_step_index(9);  // 100 kHz
    field_frequency_.set_value(433'920'000, false);
    if (auto* tx = globals().transmitter)
        tx->set_target_frequency(433'920'000);

    field_frequency_.on_change = [this](uint64_t hz) {
        if (auto* tx = globals().transmitter) tx->set_target_frequency(hz);
    };

    button_start_.on_select = [this](ui::Button&) { toggle_tx(); };
}

OokTxView::~OokTxView() {
    stop_tx();
}

void OokTxView::focus() {
    tab_view_.focus();
}

void OokTxView::on_show() {
    View::on_show();
}

void OokTxView::on_hide() {
    stop_tx();
    View::on_hide();
}

void OokTxView::toggle_tx() {
    if (transmitting_)
        stop_tx();
    else
        start_tx(tab_view_.selected() == 1);
}

void OokTxView::start_tx(bool scan) {
    auto* tx = globals().transmitter;
    if (!tx) {
        text_status_.set("No TX (needs B200)");
        return;
    }

    auto keyer = std::make_shared<dsp::OokKeyer>();

    if (scan) {
        const unsigned n = scan_view_.order();
        const std::string frags = ook::de_bruijn_fragments(n);
        bit_count_ = ook::pack_fragments(frags, bits_);
        if (bit_count_ == 0) {
            text_status_.set("Empty sequence");
            return;
        }
        const double us = scan_view_.bit_length_us();
        const double symrate = (us > 0.0) ? (1'000'000.0 / us) : 1000.0;
        keyer->configure(static_cast<float>(ook::kOokSampleRate),
                         static_cast<float>(symrate));
        keyer->set_data(bits_.data(), bit_count_);
        keyer->set_repeat(1, 0);
        progressbar_.set_max(1);
        text_status_.set("Scanning...");
    } else {
        const auto& def = config_view_.encoder();
        const std::string frags = config_view_.frame_fragments();
        bit_count_ = ook::pack_fragments(frags, bits_);
        if (bit_count_ == 0) {
            text_status_.set("Empty frame");
            return;
        }
        keyer->configure(static_cast<float>(ook::kOokSampleRate),
                         static_cast<float>(config_view_.symbol_rate()));
        keyer->set_data(bits_.data(), bit_count_);
        keyer->set_repeat(config_view_.repeat_min(), def.pause_symbols);
        progressbar_.set_max(config_view_.repeat_min());
        text_status_.set("Sending...");
    }

    keyer_ = keyer;

    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(static_cast<double>(ook::kOokSampleRate));
    tx->set_target_frequency(field_frequency_.value());
    tx->set_iq_source([keyer](dsp::cfloat* out, size_t count) {
        return keyer->process(out, count);
    });

    if (!tx->start()) {
        text_status_.set("TX start failed (B200?)");
        stop_tx();
        return;
    }

    transmitting_ = true;
    button_start_.set_text("Stop TX");
    progressbar_.set_value(0);
}

void OokTxView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    keyer_.reset();
    transmitting_ = false;
    button_start_.set_text("Start TX");
    progressbar_.set_value(0);
}

void OokTxView::on_frame_sync() {
    View::on_frame_sync();
    if (!transmitting_) return;

    auto k = keyer_;
    if (!k) return;

    progressbar_.set_value(k->repeats_sent());
    if (k->done()) {
        stop_tx();
        text_status_.set("Done");
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_ooktx{{
    "ooktx", "OOK TX", app::Category::Transmit,
    ui::Color::orange(), &ui::bitmap_icon_remote,
    [] { return std::make_unique<app::OokTxView>(); }}};
}  // namespace
