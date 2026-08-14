/*
 * mayhem-b200 — NOAA APT receiver.
 *
 * Copyright (C) 2025 Brumi, HTotoo
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_noaaapt_rx.hpp"

#include "../core/file_path.hpp"
#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "display.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>

namespace app {

namespace {

/* Upstream's baseband rate. Keeping it makes every decimation ratio in the
 * chain below an exact integer, which the pixel clock's constants assume. */
constexpr double kCaptureRate = 3'072'000.0;

/* The receiver's wideband spectrum tap holds 4096 samples. */
constexpr size_t kTapSamples = 4096;

/* Complex channel rate feeding the FM discriminator, and the ~38 kHz channel
 * width upstream's taps_38k_wfmam_decim_1 gives it. */
constexpr double kChannelRate = 96'000.0;
constexpr double kChannelBandwidth = 38'000.0;
constexpr double kFmDeviation = 17'000.0;

/* Upstream's audio chain after the discriminator: 96k -> 24k -> 12k, the
 * second stage being a 64-tap bandpass centred on the 2.4 kHz subcarrier with
 * 2 kHz of bandwidth (taps_64_bpf_2k4_bw_2k). */
constexpr double kAudio24 = 24'000.0;

/* Cosine-modulated lowpass: the standard way to turn a prototype lowpass of
 * half the wanted bandwidth into a bandpass centred on `center_hz`. */
std::vector<float> design_bandpass(double center_hz, double bandwidth_hz, double fs, size_t taps) {
    auto h = dsp::design_lowpass_fixed(bandwidth_hz * 0.5, fs, taps);
    const size_t n = h.size();
    const double mid = static_cast<double>(n - 1) / 2.0;
    for (size_t i = 0; i < n; i++) {
        const double phase =
            2.0 * 3.14159265358979323846 * center_hz * (static_cast<double>(i) - mid) / fs;
        h[i] = static_cast<float>(h[i] * 2.0 * std::cos(phase));
    }
    return h;
}

}  // namespace

namespace noaaapt {

ScanCanvas::ScanCanvas(ui::Rect parent_rect)
    : ui::Widget{parent_rect} {
    rows_ = parent_rect.height();
    fb_.assign(static_cast<size_t>(240) * static_cast<size_t>(rows_), ui::Color::black());
}

void ScanCanvas::set_row(int row, const ui::Color* pixels) {
    if (row < 0 || row >= rows_) return;
    std::copy(pixels, pixels + 240, fb_.begin() + static_cast<ptrdiff_t>(row) * 240);
    set_dirty();
}

void ScanCanvas::clear() {
    std::fill(fb_.begin(), fb_.end(), ui::Color::black());
    set_dirty();
}

void ScanCanvas::paint(ui::Painter&) {
    /* Painter has no bulk-pixel primitive; the display does, and it is the same
     * call the firmware's render_line() ends up in. */
    const auto r = screen_rect();
    host::display.draw_pixels(r, fb_.data(), fb_.size());
}

}  // namespace noaaapt

NoaaAptRxView::NoaaAptRxView()
    : receiver_{*globals().receiver} {
    add_children({&field_frequency_,
                  &field_gain_,
                  &text_level_,
                  &text_status_,
                  &button_ss_,
                  &check_sync_,
                  &text_lines_,
                  &text_tap_,
                  &canvas_});

    line_.assign(noaaapt::px_per_line, 0);
    row_.assign(240, ui::Color::black());

    text_tap_.set_style(ui::Theme::getInstance()->fg_yellow);

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    /* NOAA APT downlinks: 137.100, 137.9125 and 137.620 MHz. Upstream leaves the
     * frequency to the operator and only sets a 100 Hz step. */
    field_frequency_.set_step_index(2);
    field_frequency_.set_value(receiver_.target_frequency(), false);
    field_frequency_.on_change = [this](uint64_t hz) {
        receiver_.set_target_frequency(hz);
        chain_valid_ = false;
    };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    check_sync_.set_value(true);
    check_sync_.on_select = [this](ui::Checkbox&, bool v) {
        use_sync_ = v;
        sync_.reset();
    };

    button_ss_.on_select = [this](ui::Button&) { toggle_capture(); };

    set_status("Waiting for signal.");
}

NoaaAptRxView::~NoaaAptRxView() {
    if (bmp_.is_loaded()) bmp_.close();
}

void NoaaAptRxView::focus() {
    field_frequency_.focus();
}

void NoaaAptRxView::set_status(const std::string& s) {
    if (status_ == s) return;
    status_ = s;
    text_status_.set(s);
}

void NoaaAptRxView::toggle_capture() {
    if (capturing_) {
        bmp_.close();
        capturing_ = false;
        button_ss_.set_text("START");
        set_status("Stopped. " + capture_path_);
        return;
    }

    const std::string dir = core::data_directory() + "/BMP";
    core::ensure_directory(dir);
    capture_path_ = dir + "/noaa_" + to_string_timestamp_now() + ".bmp";

    /* Upstream creates a one-row image and grows it a row per line, so a pass
     * that is cut short still leaves a valid BMP on disk. */
    if (!bmp_.create(capture_path_, noaaapt::px_per_line, 1)) {
        set_status("Cannot create BMP");
        return;
    }
    capturing_ = true;
    lines_done_ = 0;
    button_ss_.set_text("STOP");
    set_status("Recording.");
}

void NoaaAptRxView::rebuild_chain() {
    const double input_rate = receiver_.sampling_rate();
    if (input_rate <= 0.0) return;

    size_t decimation = static_cast<size_t>(std::lround(input_rate / kChannelRate));
    if (decimation < 1) decimation = 1;
    const double channel_rate = input_rate / static_cast<double>(decimation);

    const double cutoff = kChannelBandwidth * 0.5;
    const double transition = std::max(2000.0, channel_rate * 0.5 - cutoff);
    channel_filter_.configure(dsp::design_lowpass(cutoff, transition, input_rate, 50.0),
                              decimation);

    fm_.configure(static_cast<float>(channel_rate), static_cast<float>(kFmDeviation));

    /* 96k -> 24k, then the 64-tap subcarrier bandpass 24k -> 12k. */
    size_t d1 = static_cast<size_t>(std::lround(channel_rate / kAudio24));
    if (d1 < 1) d1 = 1;
    const double rate24 = channel_rate / static_cast<double>(d1);
    audio_decim_.configure(dsp::design_lowpass(9000.0, 3000.0, channel_rate, 50.0), d1);

    size_t d2 = static_cast<size_t>(std::lround(rate24 / noaaapt::audio_rate_hz));
    if (d2 < 1) d2 = 1;
    chain_audio_rate_ = rate24 / static_cast<double>(d2);
    subcarrier_bpf_.configure(design_bandpass(noaaapt::subcarrier_hz, 2000.0, rate24, 63), d2);

    envelope_.configure(noaaapt::subcarrier_hz, chain_audio_rate_);
    pixel_clock_.configure(chain_audio_rate_, noaaapt::pixel_rate_hz);
    sync_.reset();

    double offset = 0.0;
    if (auto* r = globals().radio) {
        const double lo = r->rx_frequency();
        if (lo > 0.0) offset = static_cast<double>(receiver_.target_frequency()) - lo;
    }
    nco_.set_frequency(-offset, input_rate);

    const double duty = (static_cast<double>(kTapSamples) * 60.0) / input_rate;
    const int pct = static_cast<int>(std::lround(std::min(1.0, duty) * 100.0));
    text_tap_.set("tap " + to_string_dec_uint(static_cast<uint64_t>(pct)) +
                  "% - no chan tap");

    chain_valid_ = true;
}

void NoaaAptRxView::flush_line() {
    /* Full-resolution row to the BMP, downscaled row to the preview. Upstream
     * interleaves the two per pixel; doing it per line here is the same output
     * and lets the sync realign a line before any of it is committed. */
    if (capturing_ && bmp_.is_loaded()) {
        for (size_t i = 0; i < line_.size(); i++) {
            const ui::Color c{line_[i], line_[i], line_[i]};
            bmp_.write_next_px(c);
        }
        bmp_.expand_y_delta(1);
    }

    std::fill(row_.begin(), row_.end(), ui::Color::black());
    for (uint16_t i = 0; i < noaaapt::px_per_line; i++) {
        const uint16_t x = noaaapt::preview_column(i);
        row_[x] = ui::Color{line_[i], line_[i], line_[i]};
    }
    canvas_.set_row(line_num_, row_.data());

    line_num_++;
    if (line_num_ >= canvas_.rows()) line_num_ = 0;
    lines_done_++;
}

void NoaaAptRxView::feed_pixel(uint8_t px) {
    if (use_sync_ && sync_.process(px)) {
        /* HOST ADDITION: realign the line onto the sync burst. The burst's own
         * 39 pixels open the new line, followed by the handful that arrived
         * while the detector was picking the correlation peak. Whatever was
         * part-way through is abandoned rather than written as a torn row. */
        const size_t n = sync_.pattern_size();
        std::fill(line_.begin(), line_.end(), static_cast<uint8_t>(0));
        sync_.copy_window(line_.data());
        line_pos_ = n + sync_.copy_since_sync(line_.data() + n, line_.size() - n);
        set_status("Synced.");
        return;
    }

    if (line_pos_ < line_.size()) line_[line_pos_++] = px;

    if (line_pos_ >= line_.size()) {
        flush_line();
        line_pos_ = 0;
        if (status_ != "Synced.") set_status("Image arriving.");
    }
}

void NoaaAptRxView::pump() {
    if (!chain_valid_) rebuild_chain();
    if (!chain_valid_) return;

    /* IDEAL TAP: a continuous 12 kHz stream of the demodulated APT audio, which
     * is what upstream's M4 produces. radio::ReceiverModel offers only
     * take_spectrum_samples() — a snapshot of the newest wideband block, taken
     * once per UI frame — so this view runs the whole channel chain itself and
     * sees only the fraction of the signal those snapshots cover. APT needs an
     * unbroken stream for minutes on end: every gap shifts the line phase, so
     * the picture will tear until a channel tap exists. text_tap_ says so on
     * screen; the decoder below is upstream's and is correct on contiguous
     * input (tests/test_noaaapt_rx.cpp). */
    if (!receiver_.take_spectrum_samples(raw_, kTapSamples)) return;
    if (raw_.empty()) return;

    mixed_.resize(raw_.size());
    nco_.mix(raw_.data(), mixed_.data(), raw_.size());

    channel_.clear();
    channel_filter_.process(mixed_.data(), mixed_.size(), channel_);
    if (channel_.empty()) return;

    demod_.clear();
    fm_.process(channel_.data(), channel_.size(), demod_);
    if (demod_.empty()) return;

    audio24_.clear();
    audio_decim_.process(demod_.data(), demod_.size(), audio24_);
    if (audio24_.empty()) return;

    audio12_.clear();
    subcarrier_bpf_.process(audio24_.data(), audio24_.size(), audio12_);

    for (float s : audio12_) {
        const float env = envelope_.process(s);
        if (pixel_clock_.tick()) feed_pixel(noaaapt::amplitude_to_pixel(env));
    }
}

void NoaaAptRxView::on_show() {
    View::on_show();

    receiver_.set_sampling_rate(kCaptureRate);
    /* The receiver's own audio path is only for monitoring; the decode chain
     * above is independent of it. APT is ~34 kHz wide, so the narrower of the
     * two WFM configurations is the closest fit Mayhem's mode list offers. */
    receiver_.set_mode(radio::ReceiverModel::Mode::WidebandFMAudio);
  /* Data decoder: no speaker monitor. It reads its own tap; the
     * demodulated audio would be modem tones nobody needs. */
    receiver_.set_audio_monitor(false);
    receiver_.set_wfm_configuration(radio::ReceiverModel::WfmConfig::Narrow180k);

    chain_valid_ = false;
    if (!receiver_.running()) receiver_.start();

    field_frequency_.focus();
}

void NoaaAptRxView::on_hide() {
    View::on_hide();
}

void NoaaAptRxView::on_frame_sync() {
    View::on_frame_sync();
    frame_counter_++;

    pump();

    if ((frame_counter_ % 12) == 0) {
        const int level = static_cast<int>(std::lround(receiver_.channel_level_db()));
        text_level_.set(to_string_dec_int(level) + " dB");
        text_lines_.set("lines " + to_string_dec_uint(lines_done_));
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_noaaapt_rx{{"noaaapt_rx", "NOAA APT", app::Category::Receive,
                                     ui::Color::green(), &ui::bitmap_icon_sonde,
                                     [] { return std::make_unique<app::NoaaAptRxView>(); }}};
}  // namespace
