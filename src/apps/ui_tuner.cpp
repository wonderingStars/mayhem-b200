/*
 * mayhem-b200 — instrument tuner (reference-tone generator).
 *
 * Copyright (C) 2024 zxkmm (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_tuner.hpp"

#include "../audio/audio_out.hpp"
#include "../core/string_format.hpp"
#include "../radio/receiver_model.hpp"
#include "app_context.hpp"

#include <cmath>
#include <iterator>

namespace app {

TunerView::TunerView() {
    add_children({&labels_,
                  &options_instrument_,
                  &options_note_,
                  &text_played_frequency_,
                  &text_stored_frequency_,
                  &text_octave_shift_,
                  &field_volume_,
                  &button_play_stop_,
                  &text_status_,
                  &host_note_});

    options_instrument_.on_change = [this](size_t, int32_t value) {
        if (const Instrument* instrument = tuner_instrument_by_index(value))
            update_notes_for_instrument(*instrument);
        update_current_note();
    };

    options_note_.on_change = [this](size_t, int32_t) { update_current_note(); };

    button_play_stop_.on_select = [this](ui::Button&) {
        if (playing_)
            stop_tone();
        else
            start_tone();
    };

    field_volume_.on_change = [this](int32_t v) {
        if (auto* ao = globals().audio_out) ao->set_volume(static_cast<uint8_t>(v));
    };

    options_instrument_.set_selected_index(0, false);
    update_notes_for_instrument(tuner_guitar());
    update_current_note();

    /* A clearly audible default that the user can turn down. */
    field_volume_.set_value(60);
}

TunerView::~TunerView() {
    stop_tone();
}

void TunerView::on_show() {
    View::on_show();
    options_instrument_.focus();

    /* The reference tone and the demodulator both write to the one shared audio
     * output, so silence the receiver while the tuner owns it — this mirrors
     * upstream disabling receiver_model in the Tuner. */
    if (auto* r = globals().receiver) {
        if (r->running()) r->stop();
    }

    auto* ao = globals().audio_out;
    if (ao == nullptr || !ao->running()) {
        text_status_.set(STR_COLOR_YELLOW "Audio output unavailable");
    } else {
        text_status_.set("");
    }
}

void TunerView::on_hide() {
    stop_tone();
    View::on_hide();
}

void TunerView::update_notes_for_instrument(const Instrument& instrument) {
    ui::OptionsField::options_t note_options;
    int32_t index = 0;
    for (const auto& note_pair : instrument.notes)
        note_options.emplace_back(note_pair.first, index++);

    options_note_.set_options(std::move(note_options));
    if (!instrument.notes.empty())
        options_note_.set_selected_index(0, false);
}

void TunerView::update_current_note() {
    const Instrument* instrument =
        tuner_instrument_by_index(static_cast<int>(options_instrument_.selected_index_value()));
    if (instrument == nullptr) return;

    const std::string note_name = options_note_.selected_index_name();
    const auto it = instrument->notes.find(note_name);
    if (it == instrument->notes.end()) return;

    stored_frequency_ = it->second.frequency;
    octave_shift_ = it->second.octave_shift;
    played_frequency_ = real_note_frequency(stored_frequency_, octave_shift_);

    text_played_frequency_.set(std::to_string(played_frequency_));
    text_stored_frequency_.set(std::to_string(stored_frequency_));
    text_octave_shift_.set(to_string_dec_int(octave_shift_) + " * 8ve");

    /* If a tone is already sounding, glide it to the new note without a click:
     * only the phase increment changes, phase_ carries over. */
    set_dirty();
}

void TunerView::start_tone() {
    playing_ = true;
    button_play_stop_.set_text("Stop");
    set_dirty();
}

void TunerView::stop_tone() {
    playing_ = false;
    button_play_stop_.set_text("Play");
    set_dirty();
}

void TunerView::on_frame_sync() {
    View::on_frame_sync();

    if (!playing_) return;

    auto* ao = globals().audio_out;
    if (ao == nullptr || !ao->running()) return;
    if (played_frequency_ <= 0) return;

    /* Top the output ring back up to full each frame. space() never reports
     * more than is free, so this cannot overflow; the cap bounds the very first
     * write into a freshly opened (empty) ring. */
    size_t n = ao->space();
    if (n == 0) return;
    constexpr size_t kMaxPerFrame = 4096;
    if (n > kMaxPerFrame) n = kMaxPerFrame;

    const double increment = static_cast<double>(played_frequency_) /
                             static_cast<double>(audio::sample_rate);
    constexpr float kAmplitude = 0.6f;

    scratch_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        scratch_[i] = kAmplitude * static_cast<float>(std::sin(2.0 * M_PI * phase_));
        phase_ += increment;
        if (phase_ >= 1.0) phase_ -= 1.0;
    }
    ao->write(scratch_.data(), n);
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_tuner{{"tuner", "Tuner", app::Category::Utilities,
                                ui::Color::cyan(), &ui::bitmap_icon_speaker,
                                [] { return std::make_unique<app::TunerView>(); }}};
}  // namespace
