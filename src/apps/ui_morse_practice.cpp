/*
 * mayhem-b200 — Morse Practice (Games).
 *
 * Copyright (C) 2025 Pezsma (decoder + Morse table)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_morse_practice.hpp"

#include "../audio/audio_out.hpp"
#include "app_context.hpp"
#include "input.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <cmath>

namespace app {

namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double sidetone_hz = 700.0;   /* classic CW sidetone pitch */
constexpr float sidetone_amp = 0.5f;

/* The four console colours, indexed by morse::score_color_id(). */
const char* const color_escape[4] = {
    STR_COLOR_WHITE, STR_COLOR_RED, STR_COLOR_YELLOW, STR_COLOR_GREEN};

}  // namespace

MorsePracticeView::MorsePracticeView() {
    tone_buf_.resize(2048);

    add_children({&labels_, &button_key_, &text_last_, &console_,
                  &button_clear_, &button_back_});

    /* Keying is driven by polling the physical Enter/Select key in
     * on_frame_sync(), not by the button callback, because a single on_select
     * click cannot carry a hold duration. The button is the focus target and
     * the visual affordance; its callback stays empty on purpose. */

    button_clear_.on_select = [this](ui::Button&) {
        console_.clear(true);
        text_last_.set("");
    };

    button_back_.on_select = [this](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void MorsePracticeView::focus() {
    button_key_.focus();
}

void MorsePracticeView::on_show() {
    View::on_show();
    console_.clear(true);
    console_.write(STR_COLOR_LIGHT_GREY "Morse Practice ready\n");
    start_time_ = 0;
    end_time_ = 0;
    timing_gap_ = false;
    key_down_ = false;
    last_sync_ms_ = input::now_ms();
    pending_samples_ = 0.0;
}

void MorsePracticeView::on_press(uint32_t now) {
    start_time_ = now;
    if (end_time_ != 0) {
        int64_t gap_delta = static_cast<int64_t>(now) - static_cast<int64_t>(end_time_);
        auto result = decoder_.handleInput(-static_cast<int32_t>(gap_delta));
        if (result.isValid()) write_result(result);
    }
    end_time_ = 0;
    timing_gap_ = false;
}

void MorsePracticeView::on_release(uint32_t now) {
    end_time_ = now;
    if (start_time_ != 0) {
        int32_t press_delta = static_cast<int32_t>(now - start_time_);
        auto result = decoder_.handleInput(press_delta);
        if (result.isValid()) write_result(result);
    }
    start_time_ = 0;
    timing_gap_ = true;
}

void MorsePracticeView::write_result(const morse::MorseDecoder::DecodeResult& r) {
    /* A decoded result may carry a trailing space (inter-word gap); upstream
     * writes the whole token in one colour. */
    write_char(r.text, r.confidence);
}

void MorsePracticeView::write_char(const std::string& ch, double confidence) {
    if (ch.empty()) return;
    text_last_.set(decoder_.getLastSequence());
    const uint8_t color_id = morse::score_color_id(ch, confidence);
    console_.write(std::string{color_escape[color_id]} + ch);
}

void MorsePracticeView::update_sidetone(bool keyed, uint32_t now) {
    auto* ao = globals().audio_out;

    const uint32_t rate = audio::sample_rate;
    const double dt =
        (last_sync_ms_ != 0) ? static_cast<double>(now - last_sync_ms_) / 1000.0 : 0.0;
    last_sync_ms_ = now;

    /* Drain estimate, matching the metronome's bounded top-up scheme. */
    pending_samples_ -= dt * rate;
    if (pending_samples_ < 0.0) pending_samples_ = 0.0;

    if (ao == nullptr || !ao->running() || !keyed) return;

    const double target = 0.03 * rate;  /* keep ~30 ms queued while keying */
    int want = static_cast<int>(target - pending_samples_);
    int room = static_cast<int>(ao->space());
    int n = std::min(want, room);

    while (n > 0) {
        const int chunk = std::min<int>(n, static_cast<int>(tone_buf_.size()));
        const double step = 2.0 * pi * sidetone_hz / rate;
        for (int i = 0; i < chunk; ++i) {
            tone_buf_[static_cast<size_t>(i)] =
                sidetone_amp * static_cast<float>(std::sin(phase_));
            phase_ += step;
            if (phase_ >= 2.0 * pi) phase_ -= 2.0 * pi;
        }
        const size_t written =
            ao->write(tone_buf_.data(), static_cast<size_t>(chunk));
        pending_samples_ += static_cast<double>(written);
        n -= chunk;
        if (written < static_cast<size_t>(chunk)) break;  /* ring full */
    }
}

void MorsePracticeView::on_frame_sync() {
    View::on_frame_sync();

    const uint32_t now = input::now_ms();
    const bool down = input::key_is_down(ui::KeyEvent::Select);

    /* Edge-detect key transitions to time each dit/dah and gap. */
    if (down && !key_down_) {
        key_down_ = true;
        on_press(now);
    } else if (!down && key_down_) {
        key_down_ = false;
        on_release(now);
    }

    /* Segment characters and words on silence, as the firmware did. */
    if (end_time_ != 0 && timing_gap_) {
        int64_t gap_delta = static_cast<int64_t>(now) - static_cast<int64_t>(end_time_);
        if (gap_delta >= decoder_.getInterCharThreshold()) {
            auto result = decoder_.handleInput(-static_cast<int32_t>(gap_delta));
            if (result.isValid()) write_result(result);
        }
        if (gap_delta >= decoder_.getInterWordThreshold()) {
            write_char(" ", 1.0);
            end_time_ = 0;
            timing_gap_ = false;
        }
    }

    update_sidetone(down, now);
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_morse_practice{{
    "morse_practice",
    "Morse Practice",
    app::Category::Games,
    ui::Color::yellow(),  /* upstream icon_color */
    &ui::bitmap_icon_games,
    [] { return std::make_unique<app::MorsePracticeView>(); },
    false  /* fully functional on the host */
}};
}  // namespace
