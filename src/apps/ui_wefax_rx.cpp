/*
 * mayhem-b200 — WeFax receiver.
 *
 * Copyright (C) 2025 Brumi, HTotoo
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_wefax_rx.hpp"

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

/* Upstream's baseband rate; /256 gives exactly the 12 kHz channel rate the
 * discriminator's SOS coefficients were designed for. */
constexpr double kCaptureRate = 3'072'000.0;
constexpr size_t kTapSamples = 4096;

/* taps_2k6_usb_wefax_channel: a 2.6 kHz single-sideband channel at 12 kHz. */
constexpr double kChannelRate = 12'000.0;
constexpr double kChannelBandwidth = 2'600.0;

}  // namespace

namespace wefax {

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
    const auto r = screen_rect();
    host::display.draw_pixels(r, fb_.data(), fb_.size());
}

}  // namespace wefax

WeFaxRxView::WeFaxRxView()
    : receiver_{*globals().receiver} {
    add_children({&labels_,
                  &field_frequency_,
                  &field_gain_,
                  &text_level_,
                  &options_lpm_,
                  &options_ioc_,
                  &check_tone_,
                  &text_status_,
                  &button_ss_,
                  &text_geometry_,
                  &text_note_,
                  &canvas_});

    line_.assign(wefax::px_per_line, 0);
    row_.assign(240, ui::Color::black());

    text_note_.set_style(ui::Theme::getInstance()->fg_yellow);

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    field_frequency_.set_step_index(2);  /* 100 Hz, as upstream */
    field_frequency_.set_value(receiver_.target_frequency(), false);
    field_frequency_.on_change = [this](uint64_t hz) {
        receiver_.set_target_frequency(hz);
        chain_valid_ = false;
        update_range_warning();
    };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    options_lpm_.on_change = [this](size_t, int32_t v) {
        lpm_ = static_cast<uint32_t>(v);
        on_settings_changed();
    };
    options_ioc_.on_change = [this](size_t, int32_t v) {
        ioc_ = static_cast<wefax::Ioc>(static_cast<uint8_t>(v));
        on_settings_changed();
    };

    check_tone_.set_value(false);
    check_tone_.on_select = [this](ui::Checkbox&, bool v) {
        use_start_tone_ = v;
        start_tone_.reset();
    };

    button_ss_.on_select = [this](ui::Button&) { toggle_capture(); };

    options_lpm_.set_by_value(120, false);
    options_ioc_.set_selected_index(0, false);
    on_settings_changed();
}

WeFaxRxView::~WeFaxRxView() {
    if (bmp_.is_loaded()) bmp_.close();
}

void WeFaxRxView::focus() {
    field_frequency_.focus();
}

void WeFaxRxView::set_status(const std::string& s) {
    if (status_ == s) return;
    status_ = s;
    text_status_.set(s);
}

void WeFaxRxView::on_settings_changed() {
    /* update_params(): the pixel clock is the only thing the LPM changes, and
     * the IOC selects the start tone. */
    pixel_clock_.configure(chain_audio_rate_, lpm_, wefax::px_per_line);
    discriminator_.reset();
    compressor_.reset();
    start_tone_.reset();
    line_pos_ = 0;
    set_status("Waiting for signal.");

    const double spp = wefax::samples_per_pixel(chain_audio_rate_, lpm_, wefax::px_per_line);
    const double ioc_px = wefax::ioc_line_pixels(wefax::ioc_value(ioc_));
    text_geometry_.set(to_string_decimal(static_cast<float>(spp), 2) + " sa/px  IOC " +
                       to_string_dec_uint(static_cast<uint64_t>(std::lround(ioc_px))) + "px  st " +
                       to_string_dec_uint(wefax::start_tone_hz(ioc_)) + "Hz");
}

void WeFaxRxView::update_range_warning() {
    double min_hz = 70'000'000.0;
    if (auto* r = globals().radio) min_hz = r->caps().rx_freq.min;

    if (static_cast<double>(receiver_.target_frequency()) < min_hz) {
        /* Honest, not hidden: WeFax lives on HF and a bare B200 does not. */
        text_note_.set_style(ui::Theme::getInstance()->fg_red);
        text_note_.set("HF: below this B200's " +
                       to_string_dec_uint(static_cast<uint64_t>(min_hz / 1'000'000.0)) +
                       " MHz limit");
        return;
    }

    text_note_.set_style(ui::Theme::getInstance()->fg_yellow);
    const double input_rate = receiver_.sampling_rate();
    const double duty =
        input_rate > 0.0 ? (static_cast<double>(kTapSamples) * 60.0) / input_rate : 0.0;
    const int pct = static_cast<int>(std::lround(std::min(1.0, duty) * 100.0));
    text_note_.set("tap " + to_string_dec_uint(static_cast<uint64_t>(pct)) +
                   "% - no chan tap");
}

void WeFaxRxView::toggle_capture() {
    if (capturing_) {
        bmp_.close();
        capturing_ = false;
        button_ss_.set_text("START");
        set_status("Stopped.");
        return;
    }

    const std::string dir = core::data_directory() + "/BMP";
    core::ensure_directory(dir);
    capture_path_ = dir + "/wefax_" + to_string_timestamp_now() + ".bmp";

    if (!bmp_.create(capture_path_, wefax::px_per_line, 1)) {
        set_status("Cannot create BMP");
        return;
    }
    capturing_ = true;
    lines_done_ = 0;
    button_ss_.set_text("STOP");
    set_status("Recording.");
}

void WeFaxRxView::rebuild_chain() {
    const double input_rate = receiver_.sampling_rate();
    if (input_rate <= 0.0) return;

    size_t decimation = static_cast<size_t>(std::lround(input_rate / kChannelRate));
    if (decimation < 1) decimation = 1;
    chain_audio_rate_ = input_rate / static_cast<double>(decimation);

    /* DEVIATION: upstream's channel filter has asymmetric (single-sideband)
     * taps and SSB_FM then takes Re{} of the result. dsp:: has no complex-tap
     * filter, so the port uses a symmetric channel filter of the same width
     * followed by dsp::SsbDemod's phasing method, which produces the same real
     * upper-sideband audio. */
    const double cutoff = kChannelBandwidth;
    const double transition = std::max(500.0, chain_audio_rate_ * 0.5 - cutoff);
    channel_filter_.configure(dsp::design_lowpass(cutoff, transition, input_rate, 50.0),
                              decimation);
    ssb_.configure(static_cast<float>(chain_audio_rate_), dsp::SsbDemod::Sideband::Upper, 127);

    discriminator_.reset();
    compressor_.reset();
    pixel_clock_.configure(chain_audio_rate_, lpm_, wefax::px_per_line);

    /* Upstream applies WEFAX_FREQ_OFFSET through receiver_model's hidden
     * offset; the host receiver has no such control, so it is folded into this
     * view's own mixer. */
    double offset = static_cast<double>(wefax::frequency_offset_hz);
    if (auto* r = globals().radio) {
        const double lo = r->rx_frequency();
        if (lo > 0.0) offset += static_cast<double>(receiver_.target_frequency()) - lo;
    }
    nco_.set_frequency(-offset, input_rate);

    update_range_warning();
    chain_valid_ = true;
}

void WeFaxRxView::flush_line() {
    if (capturing_ && bmp_.is_loaded()) {
        for (size_t i = 0; i < line_.size(); i++) {
            const ui::Color c{line_[i], line_[i], line_[i]};
            bmp_.write_next_px(c);
        }
        bmp_.expand_y_delta(1);
    }

    std::fill(row_.begin(), row_.end(), ui::Color::black());
    for (uint16_t i = 0; i < wefax::px_per_line; i++) {
        const uint16_t x = wefax::preview_column(i);
        row_[x] = ui::Color{line_[i], line_[i], line_[i]};
    }
    canvas_.set_row(line_num_, row_.data());

    line_num_++;
    if (line_num_ >= canvas_.rows()) line_num_ = 0;
    lines_done_++;
}

void WeFaxRxView::feed_pixel(uint8_t px) {
    if (line_pos_ < line_.size()) line_[line_pos_++] = px;
    if (line_pos_ >= line_.size()) {
        flush_line();
        line_pos_ = 0;
        set_status("Image arriving.");
    }
}

void WeFaxRxView::pump() {
    if (!chain_valid_) rebuild_chain();
    if (!chain_valid_) return;

    /* IDEAL TAP: a continuous 12 kHz stream of the SSB channel audio, which is
     * what upstream's M4 hands SSB_FM. radio::ReceiverModel exposes only
     * take_spectrum_samples() — the newest wideband block, once per UI frame —
     * so this view runs the channel chain itself over the fraction of the
     * signal those snapshots cover. A fax page takes minutes and every gap
     * shifts the line phase, so the picture will tear until a channel tap
     * exists. text_note_ says so on screen; the decode maths below is
     * upstream's and is tested on contiguous synthetic input
     * (tests/test_wefax_rx.cpp). */
    if (!receiver_.take_spectrum_samples(raw_, kTapSamples)) return;
    if (raw_.empty()) return;

    mixed_.resize(raw_.size());
    nco_.mix(raw_.data(), mixed_.data(), raw_.size());

    channel_.clear();
    channel_filter_.process(mixed_.data(), mixed_.size(), channel_);
    if (channel_.empty()) return;

    audio_.clear();
    ssb_.process(channel_.data(), channel_.size(), audio_);
    if (audio_.empty()) return;

    /* Upstream's samples are int16-scaled; the discriminator's /32768 assumes
     * it. Host audio is float in [-1, 1], so scale into the same domain. */
    for (auto& s : audio_) s = discriminator_.process(s * 32768.0f);

    /* execute_in_place: upstream applies the compressor to the same buffer the
     * pixel loop then reads, so it is part of the picture path. */
    compressor_.execute_in_place(audio_.data(), audio_.size());

    for (float v : audio_) {
        if (use_start_tone_ && start_tone_.process(v)) set_status("Synced.");
        if (pixel_clock_.tick()) feed_pixel(wefax::amplitude_to_pixel(v));
    }
}

void WeFaxRxView::on_show() {
    View::on_show();

    receiver_.set_sampling_rate(kCaptureRate);
    /* Monitoring only; the decode chain above is independent. */
    receiver_.set_mode(radio::ReceiverModel::Mode::AMAudio);
  /* Data decoder: no speaker monitor. It reads its own tap; the
     * demodulated audio would be modem tones nobody needs. */
    receiver_.set_audio_monitor(false);
    receiver_.set_am_configuration(radio::ReceiverModel::AmConfig::USB);

    chain_valid_ = false;
    if (!receiver_.running()) receiver_.start();

    field_frequency_.focus();
}

void WeFaxRxView::on_hide() {
    View::on_hide();
}

void WeFaxRxView::on_frame_sync() {
    View::on_frame_sync();
    frame_counter_++;

    pump();

    if ((frame_counter_ % 12) == 0) {
        const int level = static_cast<int>(std::lround(receiver_.channel_level_db()));
        text_level_.set(to_string_dec_int(level) + "dB " + to_string_dec_uint(lines_done_) + "L");
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_wefax_rx{{"wefax_rx", "WeFax", app::Category::Receive,
                                   ui::Color::green(), &ui::bitmap_icon_sonde,
                                   [] { return std::make_unique<app::WeFaxRxView>(); }}};
}  // namespace
