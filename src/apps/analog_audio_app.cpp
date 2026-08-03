/*
 * mayhem-b200 — analog audio receiver.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "analog_audio_app.hpp"

#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>

namespace app {

using Mode = radio::ReceiverModel::Mode;
using AmConfig = radio::ReceiverModel::AmConfig;
using NfmConfig = radio::ReceiverModel::NfmConfig;
using WfmConfig = radio::ReceiverModel::WfmConfig;

namespace {

/* The waterfall's display range. -100 dBFS is roughly the noise floor of a
 * B200 at moderate gain, so this keeps the floor dark and signals bright. */
constexpr float kWaterfallMinDb = -100.0f;
constexpr float kWaterfallMaxDb = -20.0f;

}  // namespace

AnalogAudioView::AnalogAudioView()
    : receiver_{*globals().receiver} {
    add_children({&big_frequency_,
                  &labels_,
                  &field_frequency_,
                  &step_view_,
                  &options_mode_,
                  &options_config_,
                  &field_gain_,
                  &check_agc_,
                  &field_squelch_,
                  &field_volume_,
                  &text_level_,
                  &freq_scale_,
                  &waterfall_});

    window_ = dsp::make_window(dsp::WindowType::BlackmanHarris, fft_.size());

    waterfall_.set_range(kWaterfallMinDb, kWaterfallMaxDb);
    waterfall_.on_touch_select = [this](int x) { tune_to_column(x); };

    /* Gain range comes from the device, not from a constant: a B210 or a
     * B200mini reports a different maximum. */
    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    field_frequency_.set_value(receiver_.target_frequency(), false);
    field_frequency_.on_change = [this](uint64_t hz) {
        receiver_.set_target_frequency(hz);
        big_frequency_.set(hz);
    };
    field_frequency_.on_edit = [this] { step_view_.set_dirty(); };

    options_mode_.set_by_value(static_cast<int32_t>(receiver_.mode()), false);
    options_mode_.on_change = [this](size_t, int32_t value) {
        receiver_.set_mode(static_cast<Mode>(value));
        update_config_options();
        apply_mode();
    };

    options_config_.on_change = [this](size_t index, int32_t) {
        switch (receiver_.mode()) {
            case Mode::AMAudio:
                receiver_.set_am_configuration(static_cast<AmConfig>(index));
                break;
            case Mode::NarrowbandFMAudio:
                receiver_.set_nfm_configuration(static_cast<NfmConfig>(index));
                break;
            case Mode::WidebandFMAudio:
                receiver_.set_wfm_configuration(static_cast<WfmConfig>(index));
                break;
            default:
                break;
        }
        apply_mode();
    };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    check_agc_.set_value(receiver_.agc());
    check_agc_.on_select = [this](ui::Checkbox&, bool value) { receiver_.set_agc(value); };

    field_squelch_.set_value(receiver_.squelch_level(), false);
    field_squelch_.on_change = [this](int32_t v) {
        receiver_.set_squelch_level(static_cast<uint8_t>(v));
    };

    field_volume_.set_value(receiver_.volume(), false);
    field_volume_.on_change = [this](int32_t v) {
        receiver_.set_volume(static_cast<uint8_t>(v));
    };

    big_frequency_.set(receiver_.target_frequency());
    update_config_options();
    apply_mode();
}

AnalogAudioView::~AnalogAudioView() {
    /* Leave the radio streaming: the user is usually just stepping back to the
     * menu, and tearing the stream down makes returning feel slow. Audio is
     * muted by the navigation layer instead. */
}

void AnalogAudioView::on_show() {
    View::on_show();
    field_frequency_.focus();

    if (!receiver_.running()) receiver_.start();
}

void AnalogAudioView::on_hide() {
    View::on_hide();
}

void AnalogAudioView::update_config_options() {
    ui::OptionsField::options_t options;
    size_t selected = 0;

    switch (receiver_.mode()) {
        case Mode::AMAudio:
            options = {{"DSB 9k", 0}, {"DSB 6k", 1}, {"USB", 2}, {"LSB", 3}, {"CW", 4}};
            selected = static_cast<size_t>(receiver_.am_configuration());
            break;

        case Mode::NarrowbandFMAudio:
            options = {{"8k5", 0}, {"11k", 1}, {"16k", 2}};
            selected = static_cast<size_t>(receiver_.nfm_configuration());
            break;

        case Mode::WidebandFMAudio:
            options = {{"200k", 0}, {"180k", 1}};
            selected = static_cast<size_t>(receiver_.wfm_configuration());
            break;

        case Mode::SpectrumAnalysis:
            options = {{"-", 0}};
            selected = 0;
            break;
    }

    options_config_.set_options(std::move(options));
    options_config_.set_selected_index(selected, false);
}

void AnalogAudioView::apply_mode() {
    freq_scale_.set_channel_bandwidth(receiver_.channel_bandwidth());
    set_dirty();
}

void AnalogAudioView::tune_to_column(int x) {
    const double span = receiver_.sampling_rate();
    if (span <= 0.0) return;

    /* Column 120 is the centre of the 240-pixel waterfall. */
    const double hz_per_px = span / 240.0;
    const double offset = (x - 120) * hz_per_px;

    const int64_t target = static_cast<int64_t>(receiver_.target_frequency()) +
                           static_cast<int64_t>(std::llround(offset));
    if (target <= 0) return;

    field_frequency_.set_value(static_cast<uint64_t>(target));
}

void AnalogAudioView::update_readouts() {
    const float channel_db = receiver_.channel_level_db();
    const float rf_db = receiver_.rf_level_db();

    std::string line = "RF" + to_string_dec_int(static_cast<int64_t>(std::lround(rf_db)), 5) +
                       "dB  CH" +
                       to_string_dec_int(static_cast<int64_t>(std::lround(channel_db)), 5) +
                       "dB ";
    line += receiver_.squelch_open() ? STR_COLOR_GREEN "OPEN" : STR_COLOR_DARK_GREY "MUTE";

    text_level_.set(line);
}

void AnalogAudioView::update_spectrum() {
    if (!receiver_.take_spectrum_samples(samples_, fft_.size())) return;
    if (samples_.size() < fft_.size()) return;

    fft_.spectrum_db(samples_.data(), window_, spectrum_db_);
    waterfall_.feed(spectrum_db_);
}

void AnalogAudioView::on_frame_sync() {
    View::on_frame_sync();

    frame_counter_++;

    /* The scale describes the tuning, not the data, so it is updated whether or
     * not samples are flowing. */
    freq_scale_.set_spectrum(receiver_.target_frequency(), receiver_.sampling_rate());
    freq_scale_.set_channel_bandwidth(receiver_.channel_bandwidth());

    update_spectrum();

    /* Text readouts change slowly; refreshing them every frame just burns
     * redraws. Roughly 6 Hz at a 60 Hz UI loop. */
    if ((frame_counter_ % 10) == 0) update_readouts();
}

bool AnalogAudioView::on_key(const ui::KeyEvent key) {
    /* Left/Right step the frequency regardless of which control has focus —
     * the single most common action in this app. */
    if (key == ui::KeyEvent::Right) {
        field_frequency_.on_encoder(+1);
        return true;
    }
    if (key == ui::KeyEvent::Left) {
        field_frequency_.on_encoder(-1);
        return true;
    }
    return false;
}

bool AnalogAudioView::on_encoder(const ui::EncoderEvent delta) {
    /* Reaching here means no focused child consumed it, so treat it as tuning. */
    field_frequency_.on_encoder(delta);
    return true;
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_audio{{"audio", "Audio", app::Category::Receive,
                                ui::Color::green(), &ui::bitmap_icon_speaker,
                                [] { return std::make_unique<app::AnalogAudioView>(); }}};
}  // namespace
