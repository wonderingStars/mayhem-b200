/*
 * mayhem-b200 — Metronome utility.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_metronome.hpp"

#include "../audio/audio_out.hpp"
#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace app {

namespace {

constexpr double pi = 3.14159265358979323846;

/* Monotonic clock in milliseconds, standing in for the firmware's chTimeNow(). */
uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace

/* ---- metronome_dsp -------------------------------------------------------- */

namespace metronome_dsp {

uint32_t beat_interval_ms(uint32_t bpm, uint32_t beat_unit) {
    if (bpm == 0) return 0;
    if (beat_unit == 0) beat_unit = 1;
    const uint32_t base = 60000u / bpm;  /* ms per quarter note */
    return (base * 4u) / beat_unit;
}

uint32_t samples_per_beat(uint32_t bpm, uint32_t beat_unit, uint32_t rate) {
    if (bpm == 0) return 0;
    if (beat_unit == 0) beat_unit = 1;
    return static_cast<uint32_t>((240ull * rate) / (static_cast<uint64_t>(bpm) * beat_unit));
}

uint32_t beep_samples(uint32_t beep_ms, uint32_t rate) {
    return static_cast<uint32_t>((static_cast<uint64_t>(beep_ms) * rate) / 1000ull);
}

bool is_accent(uint32_t beat_index, uint32_t beats_per_measure) {
    if (beats_per_measure == 0) beats_per_measure = 1;
    return (beat_index % beats_per_measure) == 0;
}

uint16_t bpm_from_interval_ms(uint32_t interval_ms) {
    if (interval_ms == 0) return 0;
    return static_cast<uint16_t>(60000u / interval_ms);
}

}  // namespace metronome_dsp

/* ---- MetronomeEngine ------------------------------------------------------ */

void MetronomeEngine::configure(uint32_t bpm,
                                uint32_t beats_per_measure,
                                uint32_t beat_unit,
                                uint32_t accent_hz,
                                uint32_t unaccent_hz,
                                uint32_t beep_ms,
                                uint32_t rate) {
    bpm_ = bpm == 0 ? 1 : bpm;
    beats_per_measure_ = beats_per_measure == 0 ? 1 : beats_per_measure;
    beat_unit_ = beat_unit == 0 ? 1 : beat_unit;
    accent_hz_ = accent_hz;
    unaccent_hz_ = unaccent_hz;
    beep_ms_ = beep_ms;
    rate_ = rate == 0 ? 48000 : rate;
}

void MetronomeEngine::reset() {
    pos_in_beat_ = 0;
    beat_index_ = 0;
    spb_ = 0;
    beep_len_ = 0;
    cur_freq_ = 0;
    accent_ = true;
}

uint32_t MetronomeEngine::beat_in_measure() const {
    const uint32_t bpm_measure = beats_per_measure_ == 0 ? 1 : beats_per_measure_;
    return beat_index_ % bpm_measure;
}

uint32_t MetronomeEngine::render(float* out, uint32_t n) {
    uint32_t beats_started = 0;
    for (uint32_t k = 0; k < n; ++k) {
        if (pos_in_beat_ == 0) {
            /* Latch this beat's parameters from the (possibly updated) config. */
            spb_ = metronome_dsp::samples_per_beat(bpm_, beat_unit_, rate_);
            if (spb_ == 0) spb_ = 1;
            beep_len_ = metronome_dsp::beep_samples(beep_ms_, rate_);
            if (beep_len_ > spb_) beep_len_ = spb_;
            accent_ = metronome_dsp::is_accent(beat_index_, beats_per_measure_);
            cur_freq_ = accent_ ? accent_hz_ : unaccent_hz_;
            ++beats_started;
        }

        if (pos_in_beat_ < beep_len_) {
            const double t = static_cast<double>(pos_in_beat_) / rate_;
            out[k] = amplitude_ *
                     static_cast<float>(std::sin(2.0 * pi * cur_freq_ * t));
        } else {
            out[k] = 0.0f;
        }

        if (++pos_in_beat_ >= spb_) {
            pos_in_beat_ = 0;
            ++beat_index_;
        }
    }
    return beats_started;
}

/* ---- MetronomeView -------------------------------------------------------- */

MetronomeView::MetronomeView() {
    render_buf_.resize(4096);

    add_children({&labels_, &field_bpm_, &button_tap_, &field_beats_,
                  &field_beat_unit_, &field_accent_hz_, &field_beat_hz_,
                  &field_beep_ms_, &field_volume_, &progressbar_,
                  &button_play_stop_, &button_back_});

    field_bpm_.set_value(120);
    field_beats_.set_value(4);
    field_beat_unit_.set_value(4);
    field_accent_hz_.set_value(880);
    field_beat_hz_.set_value(440);
    field_beep_ms_.set_value(100);
    field_volume_.set_value(80);

    progressbar_.set_max(4);
    progressbar_.set_value(0);
    progressbar_.set_style(ui::Theme::getInstance()->fg_light);

    field_beats_.on_change = [this](int32_t v) {
        progressbar_.set_max(v < 1 ? 1 : static_cast<uint32_t>(v));
    };

    field_volume_.on_change = [](int32_t v) {
        if (auto* ao = globals().audio_out)
            ao->set_volume(static_cast<uint8_t>(std::clamp(v, 0, 99)));
    };

    button_tap_.on_select = [this](ui::Button&) { register_tap(); };

    button_play_stop_.on_select = [this](ui::Button&) { toggle_play(); };

    button_back_.on_select = [this](ui::Button&) {
        stop_play();
        if (auto* nav = globals().nav) nav->pop();
    };
}

void MetronomeView::focus() {
    button_play_stop_.focus();
}

void MetronomeView::on_show() {
    View::on_show();
    if (auto* ao = globals().audio_out)
        ao->set_volume(static_cast<uint8_t>(field_volume_.value()));
}

void MetronomeView::toggle_play() {
    if (playing_)
        stop_play();
    else
        play();
}

void MetronomeView::play() {
    if (playing_) return;
    engine_.reset();
    pending_samples_ = 0.0;
    last_sync_ms_ = now_ms();
    playing_ = true;
    button_play_stop_.set_text("STOP");
}

void MetronomeView::stop_play() {
    if (!playing_) return;
    playing_ = false;
    button_play_stop_.set_text("PLAY");
    progressbar_.set_value(0);
    progressbar_.set_style(ui::Theme::getInstance()->fg_light);
    set_dirty();
}

void MetronomeView::register_tap() {
    const uint64_t t = now_ms();
    if (last_tap_ms_ != 0) {
        const uint32_t interval = static_cast<uint32_t>(t - last_tap_ms_);
        /* Ignore double-taps and taps slower than ~20 BPM (upstream window). */
        if (interval > 100 && interval < 3000) {
            tap_intervals_.push_back(interval);
            if (tap_intervals_.size() > 4) tap_intervals_.pop_front();
            uint64_t sum = 0;
            for (uint32_t i : tap_intervals_) sum += i;
            const uint32_t avg =
                static_cast<uint32_t>(sum / tap_intervals_.size());
            const uint16_t bpm = metronome_dsp::bpm_from_interval_ms(avg);
            if (bpm > 0) field_bpm_.set_value(bpm);
        }
    }
    last_tap_ms_ = t;
}

void MetronomeView::on_frame_sync() {
    View::on_frame_sync();

    if (!playing_) return;

    auto* ao = globals().audio_out;
    if (ao == nullptr) return;

    const uint32_t rate = audio::sample_rate;

    engine_.configure(static_cast<uint32_t>(field_bpm_.value()),
                      static_cast<uint32_t>(field_beats_.value()),
                      static_cast<uint32_t>(field_beat_unit_.value()),
                      static_cast<uint32_t>(field_accent_hz_.value()),
                      static_cast<uint32_t>(field_beat_hz_.value()),
                      static_cast<uint32_t>(field_beep_ms_.value()),
                      rate);

    /* Bound output latency to ~50 ms: estimate how much the sound card has
     * drained since the last frame from wall-clock time, then top the ring back
     * up to the target. 50 ms comfortably survives ~16 ms frame jitter without
     * underrunning, and keeps the on-screen beat within a frame of the audio. */
    const uint64_t now = now_ms();
    const double dt = static_cast<double>(now - last_sync_ms_) / 1000.0;
    last_sync_ms_ = now;
    pending_samples_ -= dt * rate;
    if (pending_samples_ < 0.0) pending_samples_ = 0.0;

    const double target = 0.05 * rate;
    int want = static_cast<int>(target - pending_samples_);
    int room = static_cast<int>(ao->space());
    int n = std::min(want, room);

    while (n > 0) {
        const int chunk = std::min<int>(n, static_cast<int>(render_buf_.size()));
        engine_.render(render_buf_.data(), static_cast<uint32_t>(chunk));
        const size_t written = ao->write(render_buf_.data(), static_cast<size_t>(chunk));
        pending_samples_ += static_cast<double>(written);
        n -= chunk;
        if (written < static_cast<size_t>(chunk)) break;  /* ring unexpectedly full */
    }

    /* Beat indicator. Runs a little ahead of the audible click by the buffer
     * depth (~50 ms), which is within one frame. */
    const uint32_t beats = std::max<uint32_t>(1, static_cast<uint32_t>(field_beats_.value()));
    progressbar_.set_max(beats);
    progressbar_.set_value(engine_.beat_in_measure() + 1);
    progressbar_.set_style(engine_.current_is_accent()
                               ? ui::Theme::getInstance()->fg_red
                               : ui::Theme::getInstance()->fg_green);
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_metronome{{"metronome", "Metronome",
                                    app::Category::Utilities, ui::Color::cyan(),
                                    &ui::bitmap_icon_speaker,
                                    [] { return std::make_unique<app::MetronomeView>(); }}};
}  // namespace
