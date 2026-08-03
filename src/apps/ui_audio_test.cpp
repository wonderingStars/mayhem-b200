/*
 * mayhem-b200 — Audio Test (tone generator + output level meter).
 *
 * Copyright (C) 2024 Mark Thompson (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_audio_test.hpp"

#include "../audio/audio_out.hpp"
#include "../core/string_format.hpp"
#include "app_context.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <cmath>

namespace app {

namespace {
constexpr double pi = 3.14159265358979323846;
}  // namespace

/* ---- AudioTestTone -------------------------------------------------------- */

void AudioTestTone::configure(uint32_t freq, uint32_t rate, float amplitude) {
    freq_ = freq;
    rate_ = rate == 0 ? 48000 : rate;
    amplitude_ = amplitude;
    /* freq == 0 gives a zero increment -> a flat, silent signal, matching the
     * firmware's divide-by-zero guard in beep_start(). */
    increment_ = static_cast<double>(freq_) / static_cast<double>(rate_);
}

void AudioTestTone::render(float* out, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = amplitude_ * static_cast<float>(std::sin(2.0 * pi * phase_));
        phase_ += increment_;
        if (phase_ >= 1.0) phase_ -= 1.0;
    }
}

/* ---- AudioTestView -------------------------------------------------------- */

AudioTestView::AudioTestView() {
    render_buf_.resize(4096);

    add_children({&labels_, &options_sample_rate_, &field_frequency_,
                  &options_step_, &field_duration_, &field_volume_,
                  &check_speaker_, &text_level_, &meter_, &text_status_,
                  &button_back_});

    options_sample_rate_.on_change = [this](size_t, int32_t v) {
        apply_sample_rate(v);
        update_beep();
    };

    field_frequency_.on_change = [this](int32_t) { update_beep(); };

    options_step_.on_change = [this](size_t, int32_t v) {
        field_frequency_.set_step(v);
    };

    field_duration_.on_change = [this](int32_t) { update_beep(); };

    field_volume_.on_change = [this](int32_t v) {
        if (auto* ao = globals().audio_out)
            ao->set_volume(static_cast<uint8_t>(std::clamp(v, 0, 99)));
    };

    check_speaker_.on_select = [this](ui::Checkbox&, bool v) {
        beep_ = v;
        update_beep();
    };

    /* Defaults follow upstream: 24 kHz band, 1 kHz tone, 100 ms, volume 80. */
    options_sample_rate_.set_selected_index(0, false);
    apply_sample_rate(24000);
    options_step_.set_by_value(100, false);
    field_frequency_.set_step(100);
    field_frequency_.set_value(1000, false);
    field_duration_.set_value(100, false);
    field_volume_.set_value(80, false);
}

AudioTestView::~AudioTestView() {
    beep_ = false;
}

void AudioTestView::focus() {
    check_speaker_.focus();
}

void AudioTestView::on_show() {
    View::on_show();
    check_speaker_.focus();

    if (auto* ao = globals().audio_out) {
        ao->set_volume(static_cast<uint8_t>(field_volume_.value()));
        if (!ao->running())
            text_status_.set(STR_COLOR_YELLOW "Audio output unavailable");
        else
            text_status_.set("");
    } else {
        text_status_.set(STR_COLOR_YELLOW "Audio output unavailable");
    }
}

void AudioTestView::on_hide() {
    beep_ = false;
    View::on_hide();
}

void AudioTestView::apply_sample_rate(int32_t rate) {
    const uint32_t r = rate <= 0 ? 24000u : static_cast<uint32_t>(rate);
    const int32_t lo = static_cast<int32_t>(audio_test_dsp::freq_min_for_rate(r));
    const int32_t hi = static_cast<int32_t>(audio_test_dsp::freq_max_for_rate(r));
    field_frequency_.set_range(lo < 1 ? 1 : lo, hi);
}

void AudioTestView::update_beep() {
    if (!beep_) {
        remaining_samples_ = -1;
        return;
    }

    const uint32_t freq = static_cast<uint32_t>(field_frequency_.value());
    const uint32_t rate = audio::sample_rate;  /* host output is fixed 48 kHz */
    tone_.configure(freq, rate, kAmplitude);
    tone_.reset();

    const uint32_t dur_ms = static_cast<uint32_t>(field_duration_.value());
    if (dur_ms == 0)
        remaining_samples_ = -1;  /* continuous until the speaker is turned off */
    else
        remaining_samples_ =
            static_cast<int64_t>(audio_test_dsp::duration_samples(dur_ms, rate));
}

void AudioTestView::on_frame_sync() {
    View::on_frame_sync();

    auto* ao = globals().audio_out;

    /* Output level meter: take_peak() is sampled after the volume/mute step, so
     * this is the true level going to the sound card. Drains to the floor when
     * nothing is playing. */
    if (ao != nullptr) {
        const float peak = ao->take_peak();
        const float db = audio_test_dsp::peak_to_dbfs(peak, kMeterFloorDb);
        meter_.set_value(audio_test_dsp::dbfs_to_bar255(db, kMeterFloorDb));
        text_level_.set("Level  " + to_string_decimal(db, 1) + " dBFS");
    }

    if (!beep_ || ao == nullptr || !ao->running()) return;
    if (remaining_samples_ == 0) return;  /* one-shot beep already finished */

    size_t room = ao->space();
    if (room == 0) return;
    if (room > render_buf_.size()) room = render_buf_.size();

    size_t n = room;
    if (remaining_samples_ > 0 && static_cast<int64_t>(n) > remaining_samples_)
        n = static_cast<size_t>(remaining_samples_);

    tone_.render(render_buf_.data(), static_cast<uint32_t>(n));
    const size_t written = ao->write(render_buf_.data(), n);
    if (remaining_samples_ > 0)
        remaining_samples_ -= static_cast<int64_t>(written);
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream menu_location DEBUG. Genuinely functional on the host — it drives the
 * real audio output — so hardware_limited stays false. */
const app::Registrar reg_audio_test{{"audio_test", "Audio Test",
                                     app::Category::Debug, ui::Color::cyan(),
                                     &ui::bitmap_icon_speaker,
                                     [] { return std::make_unique<app::AudioTestView>(); }}};
}  // namespace
