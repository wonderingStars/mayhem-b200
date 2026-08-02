/*
 * mayhem-b200 — wideband spectrum and waterfall.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "spectrum_app.hpp"

#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "theme.hpp"

#include <cmath>

namespace app {

SpectrumAnalysisView::SpectrumAnalysisView()
    : receiver_{*globals().receiver} {
    add_children({&labels_, &field_frequency_, &options_span_, &field_gain_,
                  &check_peak_, &trace_, &freq_scale_, &waterfall_});

    window_ = dsp::make_window(dsp::WindowType::BlackmanHarris, fft_.size());

    waterfall_.set_range(-110.0f, -20.0f);
    trace_.set_range(-110.0f, -20.0f);

    waterfall_.on_touch_select = [this](int x) { tune_to_column(x); };
    trace_.on_touch_select = [this](int x) { tune_to_column(x); };

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));

        /* Offer only spans the device can actually sample. */
        ui::OptionsField::options_t spans;
        for (const auto& option : options_span_.options()) {
            if (static_cast<double>(option.second) <= caps.rx_rate.max)
                spans.push_back(option);
        }
        if (!spans.empty()) options_span_.set_options(std::move(spans));
    }

    field_frequency_.set_value(receiver_.target_frequency(), false);
    field_frequency_.set_step_index(9);  /* 100 kHz suits a wide span */
    field_frequency_.on_change = [this](uint64_t hz) { receiver_.set_target_frequency(hz); };

    options_span_.set_by_nearest_value(static_cast<int32_t>(receiver_.sampling_rate()), false);
    options_span_.on_change = [this](size_t, int32_t value) {
        receiver_.set_sampling_rate(static_cast<double>(value));
    };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    check_peak_.on_select = [this](ui::Checkbox&, bool value) {
        trace_.set_peak_hold(value);
    };
}

SpectrumAnalysisView::~SpectrumAnalysisView() {
    /* Put the demodulator back the way we found it. */
    receiver_.set_mode(previous_mode_);
}

void SpectrumAnalysisView::on_show() {
    View::on_show();
    field_frequency_.focus();

    /* Spectrum-only mode skips demodulation entirely, so the whole CPU budget
     * goes to the FFT. */
    previous_mode_ = receiver_.mode();
    receiver_.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);

    if (!receiver_.running()) receiver_.start();
}

void SpectrumAnalysisView::on_hide() {
    View::on_hide();
}

void SpectrumAnalysisView::tune_to_column(int x) {
    const double span = receiver_.sampling_rate();
    if (span <= 0.0) return;

    const double hz_per_px = span / 240.0;
    const int64_t target = static_cast<int64_t>(receiver_.target_frequency()) +
                           static_cast<int64_t>(std::llround((x - 120) * hz_per_px));
    if (target <= 0) return;

    field_frequency_.set_value(static_cast<uint64_t>(target));
}

void SpectrumAnalysisView::on_frame_sync() {
    View::on_frame_sync();

    /* Update the scale first: it describes the tuning, not the data, so it must
     * still draw when no samples are arriving (no device, or a stalled stream). */
    freq_scale_.set_spectrum(receiver_.target_frequency(), receiver_.sampling_rate());

    if (!receiver_.take_spectrum_samples(samples_, fft_.size())) return;
    if (samples_.size() < fft_.size()) return;

    fft_.spectrum_db(samples_.data(), window_, spectrum_db_);

    trace_.feed(spectrum_db_);
    waterfall_.feed(spectrum_db_);
}

}  // namespace app
