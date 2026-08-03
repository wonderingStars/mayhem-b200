/*
 * mayhem-b200 — Signal generator TX implementation.
 *
 * See ui_siggen.hpp for the port notes. The sweep math lives in the header's
 * encoder namespace so it can be tested without the UI; this file is the View
 * and its transmitter wiring.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (original)
 * Copyright (C) 2016 Furrtek (original)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_siggen.hpp"

#include "../core/string_format.hpp"
#include "../radio/transmitter_model.hpp"
#include "app_context.hpp"

#include <algorithm>

namespace app {

/* --- Visibility / summary -------------------------------------------------- */

void SigGenView::update_visibility() {
    const bool is_cw = (options_mod_.selected_index_value() == ModCW);
    const bool is_noise = (options_shape_.selected_index() == 5);

    /* CW is an unmodulated carrier: no waveform, no tone, no sweep. */
    options_shape_.hidden(is_cw);
    field_tone_.hidden(is_cw || is_noise || check_sweep_.value());

    const bool sweep_hidden = is_cw || is_noise;
    check_sweep_.hidden(sweep_hidden);
    const bool sweep_fields_hidden = sweep_hidden || !check_sweep_.value();
    field_start_.hidden(sweep_fields_hidden);
    field_end_.hidden(sweep_fields_hidden);
    field_step_.hidden(sweep_fields_hidden);
    field_dwell_.hidden(sweep_fields_hidden);

    set_dirty();
}

void SigGenView::update_summary() {
    const int32_t mod = options_mod_.selected_index_value();
    std::string s = "Out: ";
    if (mod == ModCW) {
        s += "CW carrier";
    } else {
        s += options_mod_.selected_index_name() + " ";
        if (options_shape_.selected_index() == 5) {
            s += "noise";
        } else if (check_sweep_.value()) {
            const uint32_t n = siggen::sweep_step_count(
                static_cast<uint32_t>(field_start_.value()),
                static_cast<uint32_t>(field_end_.value()),
                static_cast<uint32_t>(field_step_.value()));
            s += "sweep " + to_string_dec_uint(static_cast<uint32_t>(field_start_.value())) +
                 "-" + to_string_dec_uint(static_cast<uint32_t>(field_end_.value())) +
                 "Hz/" + to_string_dec_uint(n) + "st";
        } else {
            s += to_string_dec_uint(static_cast<uint32_t>(field_tone_.value())) + "Hz " +
                 options_shape_.selected_index_name();
        }
    }
    text_summary_.set(s);
}

/* --- Audio render (DSP thread) --------------------------------------------- */

size_t SigGenView::fill_audio(float* out, size_t count) {
    if (sweep_on_ && sweep_count_ > 1 && dwell_samples_ > 0) {
        const uint32_t idx = static_cast<uint32_t>((elapsed_ / dwell_samples_) % sweep_count_);
        const float f = static_cast<float>(
            siggen::sweep_frequency_at(sweep_start_, sweep_end_, sweep_step_, idx));
        if (f != cur_freq_hz_) {
            cur_freq_hz_ = f;
            tone_.set_frequency(f);
        }
    }
    tone_.process(out, count);
    elapsed_ += count;
    return count;
}

/* --- Transmit -------------------------------------------------------------- */

bool SigGenView::start_tx() {
    auto* tx = globals().transmitter;
    if (!tx) {
        text_status_.set("No transmitter (needs B200).");
        return false;
    }

    const int32_t mod = options_mod_.selected_index_value();
    shape_ = static_cast<dsp::ToneGen::Shape>(options_shape_.selected_index());
    base_freq_hz_ = static_cast<float>(field_tone_.value());
    sweep_on_ = check_sweep_.value() && (options_shape_.selected_index() != 5) && (mod != ModCW);
    sweep_start_ = static_cast<uint32_t>(field_start_.value());
    sweep_end_ = static_cast<uint32_t>(field_end_.value());
    sweep_step_ = static_cast<uint32_t>(field_step_.value());
    sweep_count_ = siggen::sweep_step_count(sweep_start_, sweep_end_, sweep_step_);
    dwell_samples_ = static_cast<uint32_t>(field_dwell_.value()) * kAudioRate / 1000;
    if (dwell_samples_ == 0) dwell_samples_ = 1;
    elapsed_ = 0;

    const float start_freq = sweep_on_ ? static_cast<float>(sweep_start_) : base_freq_hz_;
    cur_freq_hz_ = start_freq;
    tone_.configure(start_freq, static_cast<float>(kAudioRate), shape_, 1.0f);

    tx->set_target_frequency(field_freq_.value());
    tx->set_gain(static_cast<double>(field_gain_.value()));

    switch (mod) {
        case ModCW:
            tx->set_mode(radio::TransmitterModel::Mode::CW);
            tx->set_audio_source(nullptr);
            break;
        case ModFM:
            tx->set_mode(radio::TransmitterModel::Mode::NarrowbandFM);
            tx->set_nfm_configuration(radio::TransmitterModel::NfmConfig::Wide16k);
            tx->set_audio_source([this](float* o, size_t n) { return fill_audio(o, n); });
            break;
        case ModDSB:
            tx->set_mode(radio::TransmitterModel::Mode::DSB);
            tx->set_audio_source([this](float* o, size_t n) { return fill_audio(o, n); });
            break;
        case ModAM100:
            tx->set_mode(radio::TransmitterModel::Mode::AM);
            tx->set_am_depth(1.0f);
            tx->set_audio_source([this](float* o, size_t n) { return fill_audio(o, n); });
            break;
        case ModAM50:
            tx->set_mode(radio::TransmitterModel::Mode::AM);
            tx->set_am_depth(0.5f);
            tx->set_audio_source([this](float* o, size_t n) { return fill_audio(o, n); });
            break;
        default:
            break;
    }

    if (!tx->start()) {
        tx->set_audio_source(nullptr);
        text_status_.set("TX start failed (needs B200).");
        return false;
    }

    stop_after_s_ = check_stop_.value() ? static_cast<uint32_t>(field_stop_.value()) : 0;
    tx_start_ = std::chrono::steady_clock::now();
    transmitting_ = true;
    text_status_.set(STR_COLOR_RED "Transmitting...");
    return true;
}

void SigGenView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_audio_source(nullptr);
    }
    transmitting_ = false;
}

/* --- View lifecycle -------------------------------------------------------- */

void SigGenView::focus() {
    options_mod_.focus();
}

void SigGenView::on_hide() {
    if (transmitting_) stop_tx();
    View::on_hide();
}

void SigGenView::on_frame_sync() {
    View::on_frame_sync();
    if (!transmitting_) return;

    if (stop_after_s_ > 0) {
        const auto elapsed = std::chrono::steady_clock::now() - tx_start_;
        if (elapsed >= std::chrono::seconds(stop_after_s_)) {
            stop_tx();
            text_status_.set("Done.");
        }
    }
}

SigGenView::~SigGenView() {
    if (transmitting_) stop_tx();
}

SigGenView::SigGenView() {
    add_children({&labels_,
                  &options_mod_, &options_shape_, &field_tone_,
                  &check_sweep_, &field_start_, &field_end_, &field_step_, &field_dwell_,
                  &field_freq_, &field_gain_, &check_stop_, &field_stop_,
                  &text_summary_, &text_status_, &text_warning_,
                  &button_tx_, &button_stop_tx_});

    options_mod_.set_selected_index(0, false);   /* CW */
    options_shape_.set_selected_index(0, false); /* Sine */
    field_tone_.set_value(1000, false);
    field_start_.set_value(1000, false);
    field_end_.set_value(2000, false);
    field_step_.set_value(100, false);
    field_dwell_.set_value(100, false);
    field_gain_.set_value(40, false);
    field_stop_.set_value(5, false);
    field_freq_.set_value(433'920'000);  /* a benign ISM default; user retunes */

    options_mod_.on_change = [this](size_t, int32_t) {
        update_visibility();
        update_summary();
    };
    options_shape_.on_change = [this](size_t, int32_t) {
        update_visibility();
        update_summary();
    };
    field_tone_.on_change = [this](int32_t) { update_summary(); };
    check_sweep_.on_select = [this](ui::Checkbox&, bool) {
        update_visibility();
        update_summary();
    };
    field_start_.on_change = [this](int32_t) { update_summary(); };
    field_end_.on_change = [this](int32_t) { update_summary(); };
    field_step_.on_change = [this](int32_t) { update_summary(); };

    button_tx_.on_select = [this](ui::Button&) {
        if (!transmitting_) start_tx();
    };
    button_stop_tx_.on_select = [this](ui::Button&) {
        if (transmitting_) {
            stop_tx();
            text_status_.set("Stopped.");
        }
    };

    text_warning_.set(STR_COLOR_YELLOW "Radiates RF-check licence first");
    update_visibility();
    update_summary();
}

}  // namespace app

/* --- Registration ---------------------------------------------------------- */

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_siggen{{
    "siggen", "Signal gen", app::Category::Transmit,
    ui::Color::green(), nullptr,
    [] { return std::make_unique<app::SigGenView>(); }}};
}  // namespace
