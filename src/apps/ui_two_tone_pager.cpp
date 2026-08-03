/*
 * mayhem-b200 — Two-Tone pager TX implementation.
 *
 * See ui_two_tone_pager.hpp for the port notes. The tone tables, phase-delta
 * math and sequence builder live in the header's encoder namespace so they can
 * be tested without the UI; this file is the View and its transmitter wiring.
 *
 * Copyright (C) 2024 PortaPack Mayhem (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_two_tone_pager.hpp"

#include "../core/string_format.hpp"
#include "../radio/transmitter_model.hpp"
#include "app_context.hpp"

#include <algorithm>

namespace app {

namespace {

/* "288.5Hz", or "None" for a zero entry — upstream freq_name(). */
std::string freq_name_x10(uint32_t freq_x10) {
    if (freq_x10 == 0) return "None";
    return to_string_dec_uint(freq_x10 / 10) + "." +
           to_string_dec_uint(freq_x10 % 10) + "Hz";
}

}  // namespace

using two_tone_pager::build_sequence;
using two_tone_pager::CTCSS_FREQS;
using two_tone_pager::MOTO_FREQS;
using two_tone_pager::MOTO_TONE_COUNT;

/* --- Tone selection -------------------------------------------------------- */

float TwoTonePagerView::freq_a_hz() const {
    return MOTO_FREQS[options_tone_a_.selected_index()] / 10.0f;
}

float TwoTonePagerView::freq_b_hz() const {
    return MOTO_FREQS[options_tone_b_.selected_index()] / 10.0f;
}

float TwoTonePagerView::ctcss_hz() const {
    return CTCSS_FREQS[options_ctcss_.selected_index()] / 10.0f;
}

/* --- Summary --------------------------------------------------------------- */

void TwoTonePagerView::update_summary() {
    const uint32_t total_ms = static_cast<uint32_t>(field_dur_a_.value()) +
                              static_cast<uint32_t>(field_gap_.value()) +
                              static_cast<uint32_t>(field_dur_b_.value());
    std::string s = "Seq: A " +
                    freq_name_x10(MOTO_FREQS[options_tone_a_.selected_index()]);
    if (field_gap_.value() > 0)
        s += " gap";
    s += " B " + freq_name_x10(MOTO_FREQS[options_tone_b_.selected_index()]);
    s += "  " + to_string_dec_uint(total_ms / 1000) + "." +
         to_string_dec_uint((total_ms / 100) % 10) + "s";
    text_summary_.set(s);
}

/* --- Audio render (DSP thread) --------------------------------------------- */

size_t TwoTonePagerView::fill_audio(float* out, size_t count) {
    size_t written = 0;
    while (written < count) {
        if (seg_index_ >= sequence_.size()) {
            finished_.store(true);
            break;
        }

        const auto& seg = sequence_[seg_index_];

        /* Entering a new segment: retune the tone generators. */
        if (seg_pos_ == 0) {
            tone_main_.configure(seg.freq_hz, static_cast<float>(kAudioRate),
                                 dsp::ToneGen::Shape::Sine, 1.0f);
            if (seg.ctcss_hz > 0.0f)
                tone_ctcss_.configure(seg.ctcss_hz, static_cast<float>(kAudioRate),
                                      dsp::ToneGen::Shape::Sine, 1.0f);
            seg_progress_.store(static_cast<uint32_t>(seg_index_));
        }

        const uint32_t remain = seg.samples - seg_pos_;
        const size_t n = std::min(static_cast<size_t>(remain), count - written);

        if (seg.freq_hz <= 0.0f) {
            /* Gap: silent (an unmodulated carrier once FM'd). */
            for (size_t i = 0; i < n; i++) out[written + i] = 0.0f;
        } else if (seg.ctcss_hz > 0.0f) {
            /* Dual tone: half main + half CTCSS, upstream's (a>>1)+(sub>>1). */
            for (size_t i = 0; i < n; i++) out[written + i] = tone_main_.process_one();
            tone_ctcss_.mix(out + written, n, 0.5f);
        } else {
            for (size_t i = 0; i < n; i++) out[written + i] = tone_main_.process_one();
        }

        written += n;
        seg_pos_ += static_cast<uint32_t>(n);
        if (seg_pos_ >= seg.samples) {
            seg_pos_ = 0;
            seg_index_++;
        }
    }
    return written;
}

/* --- Transmit -------------------------------------------------------------- */

bool TwoTonePagerView::start_tx() {
    auto* tx = globals().transmitter;
    if (!tx) {
        text_status_.set("No transmitter (needs B200).");
        return false;
    }

    sequence_ = build_sequence(freq_a_hz(), freq_b_hz(), ctcss_hz(),
                               static_cast<uint32_t>(field_dur_a_.value()),
                               static_cast<uint32_t>(field_dur_b_.value()),
                               static_cast<uint32_t>(field_gap_.value()),
                               kAudioRate);
    seg_index_ = 0;
    seg_pos_ = 0;
    finished_.store(false);
    seg_progress_.store(0);

    progressbar_.set_max(static_cast<uint32_t>(sequence_.size()));
    progressbar_.set_value(0);

    /* NFM, 3.5 kHz deviation — the paging channel proc_tones targets. */
    tx->set_mode(radio::TransmitterModel::Mode::NarrowbandFM);
    tx->set_nfm_configuration(radio::TransmitterModel::NfmConfig::Medium11k);
    tx->set_target_frequency(field_freq_.value());
    tx->set_gain(static_cast<double>(field_gain_.value()));
    tx->set_audio_source([this](float* out, size_t n) { return fill_audio(out, n); });

    if (!tx->start()) {
        tx->set_audio_source(nullptr);
        text_status_.set("TX start failed (needs B200).");
        return false;
    }

    playing_.store(true);
    transmitting_ = true;
    text_status_.set(STR_COLOR_RED "Transmitting...");
    return true;
}

void TwoTonePagerView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_audio_source(nullptr);
    }
    playing_.store(false);
    transmitting_ = false;
    progressbar_.set_value(0);
}

/* --- View lifecycle -------------------------------------------------------- */

void TwoTonePagerView::focus() {
    options_tone_a_.focus();
}

void TwoTonePagerView::on_hide() {
    if (transmitting_) stop_tx();
    View::on_hide();
}

void TwoTonePagerView::on_frame_sync() {
    View::on_frame_sync();
    if (!transmitting_) return;

    progressbar_.set_value(seg_progress_.load());

    if (finished_.load()) {
        stop_tx();
        text_status_.set("Done.");
    }
}

TwoTonePagerView::~TwoTonePagerView() {
    if (transmitting_) stop_tx();
}

TwoTonePagerView::TwoTonePagerView() {
    add_children({&labels_,
                  &options_tone_a_, &options_tone_b_, &options_ctcss_,
                  &field_dur_a_, &field_dur_b_, &field_gap_,
                  &field_freq_, &field_gain_,
                  &text_summary_, &text_status_, &progressbar_,
                  &text_warning_,
                  &button_tx_, &button_stop_});

    /* Tone A/B share the Motorola pool. */
    {
        ui::OptionsField::options_t opts;
        opts.reserve(MOTO_TONE_COUNT);
        for (size_t i = 0; i < MOTO_TONE_COUNT; i++)
            opts.push_back({freq_name_x10(MOTO_FREQS[i]), static_cast<int32_t>(i)});
        options_tone_a_.set_options(opts);
        options_tone_b_.set_options(std::move(opts));
    }
    {
        ui::OptionsField::options_t opts;
        opts.reserve(two_tone_pager::CTCSS_COUNT);
        for (size_t i = 0; i < two_tone_pager::CTCSS_COUNT; i++)
            opts.push_back({freq_name_x10(CTCSS_FREQS[i]), static_cast<int32_t>(i)});
        options_ctcss_.set_options(std::move(opts));
    }

    /* Upstream default indices: A idx 8 (445.7 Hz), B idx 24 (1064.2 Hz),
     * 1000/3000 ms. (Upstream's source comment mislabels these as 405.3/813.9
     * Hz; the indices, and so the actual tones, are what is reproduced here.) */
    options_tone_a_.set_selected_index(8, false);
    options_tone_b_.set_selected_index(24, false);
    options_ctcss_.set_selected_index(0, false);
    field_dur_a_.set_value(1000, false);
    field_dur_b_.set_value(3000, false);
    field_gap_.set_value(0, false);
    field_gain_.set_value(40, false);
    field_freq_.set_value(154'280'000);  /* upstream default VHF paging freq */

    auto on_edit = [this]() { update_summary(); };
    options_tone_a_.on_change = [this](size_t, int32_t) { update_summary(); };
    options_tone_b_.on_change = [this](size_t, int32_t) { update_summary(); };
    field_dur_a_.on_change = [on_edit](int32_t) { on_edit(); };
    field_dur_b_.on_change = [on_edit](int32_t) { on_edit(); };
    field_gap_.on_change = [on_edit](int32_t) { on_edit(); };

    button_tx_.on_select = [this](ui::Button&) {
        if (!transmitting_) start_tx();
    };
    button_stop_.on_select = [this](ui::Button&) {
        if (transmitting_) {
            stop_tx();
            text_status_.set("Stopped.");
        }
    };

    text_warning_.set(STR_COLOR_YELLOW "Pager tones-may be illegal to TX");
    update_summary();
}

}  // namespace app

/* --- Registration ---------------------------------------------------------- */

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_two_tone_pager{{
    "two_tone_pager", "2-Tone TX", app::Category::Transmit,
    ui::Color::orange(), nullptr,
    [] { return std::make_unique<app::TwoTonePagerView>(); }}};
}  // namespace
