/*
 * mayhem-b200 — VOR receiver.
 *
 * Copyright (C) 2026 PortaPack Mayhem (original app and baseband)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_vor_rx.hpp"

#include "../core/file_path.hpp"
#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "theme.hpp"
#include "ui_font_fixed_5x8.hpp"
#include "ui_painter.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace app {

/* --- VorCdiIndicator -------------------------------------------------------- */

VorCdiIndicator::VorCdiIndicator(ui::Rect parent_rect)
    : ui::Widget{parent_rect} {}

void VorCdiIndicator::set_course(uint16_t course_deg) {
    if (course_deg_ != course_deg) {
        course_deg_ = course_deg;
        set_dirty();
    }
}

void VorCdiIndicator::set_radial(uint16_t radial_deg) {
    if (radial_deg_ != radial_deg) {
        radial_deg_ = radial_deg;
        set_dirty();
    }
}

void VorCdiIndicator::set_valid(bool valid) {
    if (valid_ != valid) {
        valid_ = valid;
        set_dirty();
    }
}

void VorCdiIndicator::paint(ui::Painter& painter) {
    const auto r = screen_rect();
    const auto line_color = valid_ ? ui::Color::light_grey() : ui::Color::dark_grey();
    const auto needle_color = valid_ ? ui::Color::red() : ui::Color::dark_grey();

    painter.fill_rectangle(r, style().background);

    const auto center_x = static_cast<ui::Coord>(r.left() + r.width() / 2);
    const auto center_y = static_cast<ui::Coord>(r.top() + r.height() / 2);

    painter.draw_hline({static_cast<ui::Coord>(r.left() + 8), center_y},
                       r.width() - 16, line_color);

    for (int32_t tick = -2; tick <= 2; ++tick) {
        const auto x = static_cast<ui::Coord>(center_x + tick * tick_spacing);
        const int len = (tick == 0) ? 14 : 8;
        painter.draw_vline({x, static_cast<ui::Coord>(center_y - len / 2)}, len, line_color);
    }

    const auto offset = static_cast<ui::Coord>(
        vor_cdi_needle_offset(radial_deg_, course_deg_, tick_spacing));
    painter.draw_vline({static_cast<ui::Coord>(center_x + offset),
                        static_cast<ui::Coord>(r.top() + 2)},
                       r.height() - 4, needle_color);
}

/* --- VorCompass ------------------------------------------------------------- */

VorCompass::VorCompass(ui::Rect parent_rect)
    : ui::Widget{parent_rect} {}

void VorCompass::set_radial(uint16_t radial_deg) {
    if (radial_deg_ != radial_deg) {
        radial_deg_ = radial_deg;
        set_dirty();
    }
}

void VorCompass::set_course(uint16_t course_deg) {
    if (course_deg_ != course_deg) {
        course_deg_ = course_deg;
        set_dirty();
    }
}

void VorCompass::set_valid(bool valid) {
    if (valid_ != valid) {
        valid_ = valid;
        set_dirty();
    }
}

ui::Point VorCompass::polar(ui::Point center, int radius, int32_t bearing_deg) {
    const double a = static_cast<double>(bearing_deg) * M_PI / 180.0;
    return {static_cast<ui::Coord>(center.x() + std::lround(std::sin(a) * radius)),
            static_cast<ui::Coord>(center.y() - std::lround(std::cos(a) * radius))};
}

void VorCompass::paint(ui::Painter& painter) {
    const auto r = screen_rect();
    painter.fill_rectangle(r, style().background);

    const ui::Point center{static_cast<ui::Coord>(r.left() + r.width() / 2),
                           static_cast<ui::Coord>(r.top() + r.height() / 2)};
    const int outer = std::min(r.width(), r.height()) / 2 - 2;
    if (outer < 12) return;

    const auto ring = ui::Color::grey();

    /* Card: a 72-segment ring, a long tick every 30 degrees and a short one
     * every 10. Bearings run clockwise from up, the way a compass card reads. */
    ui::Point prev = polar(center, outer, 0);
    for (int32_t a = 5; a <= 360; a += 5) {
        const auto p = polar(center, outer, a);
        painter.draw_line(prev, p, ring);
        prev = p;
    }
    for (int32_t a = 0; a < 360; a += 10) {
        const int len = (a % 30 == 0) ? 8 : 4;
        painter.draw_line(polar(center, outer - len, a), polar(center, outer, a),
                          (a % 90 == 0) ? ui::Color::light_grey() : ring);
    }

    const auto& font = ui::font::fixed_5x8;
    const auto bg = style().background;
    static const char* const cardinal[4] = {"N", "E", "S", "W"};
    for (int i = 0; i < 4; i++) {
        const auto p = polar(center, outer - 16, i * 90);
        painter.draw_string({static_cast<ui::Coord>(p.x() - font.char_width() / 2),
                             static_cast<ui::Coord>(p.y() - font.line_height() / 2)},
                            font, ui::Color::light_grey(), bg, cardinal[i]);
    }

    /* Selected OBS course: a hollow marker on the rim. */
    const auto course_tip = polar(center, outer - 2, course_deg_);
    const auto course_base = polar(center, outer - 12, course_deg_);
    painter.draw_line(course_base, course_tip, ui::Color::cyan());
    painter.draw_line(polar(center, outer - 10, course_deg_ - 6), course_tip,
                      ui::Color::cyan());
    painter.draw_line(polar(center, outer - 10, course_deg_ + 6), course_tip,
                      ui::Color::cyan());

    /* Received radial: a needle from the centre, drawn three lines wide so it
     * stays readable, plus its reciprocal as a thin tail. */
    const auto needle = valid_ ? ui::Color::red() : ui::Color::dark_grey();
    const auto tip = polar(center, outer - 14, radial_deg_);
    painter.draw_line(center, tip, needle);
    painter.draw_line(polar(center, 6, radial_deg_ + 90), tip, needle);
    painter.draw_line(polar(center, 6, radial_deg_ - 90), tip, needle);
    painter.draw_line(center, polar(center, outer - 20, radial_deg_ + 180),
                      ui::Color::dark_grey());

    /* Centre dot. */
    painter.fill_rectangle({static_cast<ui::Coord>(center.x() - 2),
                            static_cast<ui::Coord>(center.y() - 2), 5, 5},
                           ui::Color::light_grey());
}

/* --- VorRxView -------------------------------------------------------------- */

namespace {

/* The receive path this app needs is not the one ReceiverModel exposes.
 *
 * IDEAL TAP: a contiguous stream of channel-filtered samples (or of the AM
 * envelope) at 40-48 kHz, i.e. the point in ReceiverModel::dsp_thread_main()
 * right after channel_filter_, before audio_filter_/deemph_/agc_. The firmware
 * gets exactly that inside proc_vor_rx.
 *
 * WHAT EXISTS: take_spectrum_samples(), a 4096-sample window of raw pre-mix,
 * pre-filter IQ that is refreshed once per 8192-sample DSP block. So this app
 * mixes and filters the band itself (VorChannelizer), and gets its samples in
 * bursts with unknown gaps between them.
 *
 * Two consequences, both handled rather than hidden:
 *  - Every burst is marked as a discontinuity, and VorDecoder combines bursts
 *    as REFERENCE * conj(VARIABLE) so an unknown gap cancels out.
 *  - A burst is 4096/sample_rate seconds long. One cycle of 30 Hz is 33.3 ms,
 *    so a burst only covers a whole cycle below ~123 ksps — under the B200's
 *    minimum. The app therefore asks for the lowest rate the device offers, to
 *    make the bursts as long as possible, and says on screen that the decode is
 *    running on a burst-sampled tap. */
double preferred_sampling_rate(radio::ReceiverModel& receiver) {
    double lowest = 2.0 * VorChannelizer::kMinChannelRateHz;
    if (auto* r = app::globals().radio) {
        const auto& caps = r->caps();
        if (caps.rx_rate.min > 0.0) lowest = std::max(lowest, caps.rx_rate.min);
    }
    /* Never raise the rate above what the receiver is already using if that is
     * already lower — a wider band only shortens the bursts. */
    const double current = receiver.sampling_rate();
    return (current > 0.0 && current < lowest) ? current : lowest;
}

std::string timestamp_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

}  // namespace

VorRxView::VorRxView()
    : receiver_{*globals().receiver} {
    add_children({&labels_,
                  &field_frequency_,
                  &step_view_,
                  &field_gain_,
                  &field_volume_,
                  &text_state_,
                  &button_start_stop_,
                  &text_radial_,
                  &text_flag_,
                  &text_metrics_,
                  &field_course_,
                  &field_calibration_,
                  &cdi_,
                  &compass_,
                  &check_log_,
                  &text_note_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
    }

    /* VOR is 108.00-117.95 MHz on a 50 kHz grid. Upstream steps 8333 Hz (the
     * VHF comm spacing); the host's FrequencyField carries a fixed step table
     * without that value, so it starts on 25 kHz, which divides the VOR grid. */
    field_frequency_.set_range(kVorBandLowHz, kVorBandHighHz);
    field_frequency_.set_step_index(8);  /* 25 kHz */
    uint64_t start_hz = receiver_.target_frequency();
    if (start_hz < kVorBandLowHz || start_hz > kVorBandHighHz) start_hz = 110'000'000ull;
    field_frequency_.set_value(start_hz, false);
    field_frequency_.on_change = [this](uint64_t hz) {
        receiver_.set_target_frequency(hz);
        decoder_.reset();
        smoother_.reset();
        flag_.reset();
        have_status_ = false;
        windows_decoded_ = 0;
    };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    field_volume_.set_value(receiver_.volume(), false);
    field_volume_.on_change = [this](int32_t v) {
        receiver_.set_volume(static_cast<uint8_t>(v));
    };

    field_course_.set_value(0, false);
    field_course_.on_change = [this](int32_t course) {
        cdi_.set_course(static_cast<uint16_t>(course));
        compass_.set_course(static_cast<uint16_t>(course));
        refresh_radial();
    };

    field_calibration_.set_value(0, false);
    field_calibration_.on_change = [this](int32_t) { refresh_radial(); };

    check_log_.set_value(false);
    check_log_.on_select = [this](ui::Checkbox&, bool v) {
        logging_ = v;
        update_logging();
    };

    button_start_stop_.on_select = [this](ui::Button&) {
        if (running_)
            stop_receiver();
        else
            start_receiver();
    };

    update_tap_note();
    update_status_text();
}

VorRxView::~VorRxView() {
    log_file_.reset();
}

void VorRxView::on_show() {
    View::on_show();
    field_frequency_.focus();
    start_receiver();
}

void VorRxView::on_hide() {
    View::on_hide();
    /* The stream is left running, as the other RX apps do, but the decoder
     * state is dropped so a return does not average across the gap. */
    decoder_.reset();
    smoother_.reset();
    flag_.reset();
    have_status_ = false;
}

void VorRxView::start_receiver() {
    receiver_.set_target_frequency(field_frequency_.value());
    receiver_.set_frequency_step(kVorChannelStepHz);

    /* AM, so the loudspeaker gets the station's Morse identification. The
     * receiver's own AM audio path low-passes at 4.5 kHz, which removes the
     * 9960 Hz subcarrier from what is heard — exactly what upstream does with
     * its two cascaded audio low-passes. The decode does not use that path. */
    receiver_.set_mode(radio::ReceiverModel::Mode::AMAudio);
    receiver_.set_am_configuration(radio::ReceiverModel::AmConfig::DSB9k);
    receiver_.set_sampling_rate(preferred_sampling_rate(receiver_));

    if (!receiver_.running()) receiver_.start();

    configured_input_rate_ = 0.0;  /* forces the channelizer to be built */
    decoder_.reset();
    smoother_.reset();
    flag_.reset();
    have_status_ = false;
    windows_decoded_ = 0;

    running_ = true;
    update_status_text();
}

void VorRxView::stop_receiver() {
    if (!running_) return;
    running_ = false;
    update_status_text();
}

void VorRxView::update_status_text() {
    button_start_stop_.set_text(running_ ? "Stop" : "Start");
    if (!running_) {
        text_state_.set("Idle");
        return;
    }
    if (!receiver_.running()) {
        text_state_.set(STR_COLOR_YELLOW "No radio");
        return;
    }
    /* Three distinct states, because they have three distinct causes: no
     * samples are reaching the decoder at all, samples are arriving but no VOR
     * is locked, or it is locked. */
    if (windows_decoded_ == 0) {
        text_state_.set("Waiting");
        return;
    }
    text_state_.set(last_valid_ ? STR_COLOR_GREEN "Locked" : "Searching");
}

/* How much of a 30 Hz cycle each burst from take_spectrum_samples() covers.
 * Below 1.00 the decoder is combining fragments of a cycle and the bearing gets
 * noisier; the number is on screen because it is the single figure that says
 * how good this app can be on a given sample rate, and it is a property of the
 * tap, not of the signal. */
void VorRxView::update_tap_note() {
    const double rate = receiver_.sampling_rate();
    if (rate <= 0.0) {
        text_note_.set("no stream");
        return;
    }
    const double cycles = (4096.0 / rate) * static_cast<double>(kVorToneHz);
    const std::string text = "IQ brst " + to_string_decimal(static_cast<float>(cycles), 2) + "cy";
    text_note_.set(cycles < 1.0 ? (STR_COLOR_YELLOW + text) : text);
}

void VorRxView::pump_samples() {
    if (!running_ || !receiver_.running()) return;

    const double input_rate = receiver_.sampling_rate();
    if (input_rate <= 0.0) return;

    double lo = 0.0;
    if (auto* r = globals().radio) lo = r->rx_frequency();
    const double offset = (lo > 0.0)
                              ? static_cast<double>(receiver_.target_frequency()) - lo
                              : 0.0;

    if (input_rate != configured_input_rate_) {
        channelizer_.configure(input_rate, offset);
        configured_input_rate_ = input_rate;
        decoder_.configure(channelizer_.channel_rate());
        if (!logging_) update_tap_note();
    } else if (std::fabs(offset - channelizer_.carrier_offset()) > 1.0) {
        channelizer_.set_carrier_offset(offset);
    }

    if (!receiver_.take_spectrum_samples(raw_, 4096)) return;

    envelope_.clear();
    channelizer_.process(raw_.data(), raw_.size(), envelope_);
    if (envelope_.empty()) return;

    /* Each grab is a fresh burst: the samples before it were consumed by the
     * DSP thread and never reached us. */
    decoder_.mark_discontinuity();
    decoder_.process(envelope_.data(), envelope_.size());

    VorStatus s{};
    while (decoder_.take_status(s)) on_vor_status(s);
}

void VorRxView::on_vor_status(const VorStatus& status) {
    windows_decoded_++;

    last_valid_ = status.valid;
    have_status_ = true;

    if (status.valid) {
        last_radial_deg_ = smoother_.update(status.radial_deg);
    } else {
        last_radial_deg_ = status.radial_deg;
        smoother_.reset();
    }

    text_metrics_.set("Dev " + to_string_dec_uint(status.ref_level, 3) +
                      "Hz Dep " + to_string_dec_uint(status.var_level, 3) +
                      " Q " + to_string_dec_uint(status.quality, 3));

    refresh_radial();
    update_status_text();

    if (logging_ && log_file_) log_status(status, vor_calibrated_radial(
                                                      last_radial_deg_,
                                                      field_calibration_.value()));
}

void VorRxView::refresh_radial() {
    if (!have_status_) return;

    const uint16_t radial = vor_calibrated_radial(last_radial_deg_,
                                                  field_calibration_.value());
    text_radial_.set(last_valid_ ? (to_string_dec_uint(radial, 3) + " deg") : "---");

    const auto flag = flag_.update(radial, field_course_.value(), last_valid_);
    text_flag_.set(vor_flag_label(flag));

    cdi_.set_radial(radial);
    cdi_.set_valid(last_valid_);
    compass_.set_radial(radial);
    compass_.set_valid(last_valid_);
}

void VorRxView::update_logging() {
    if (!logging_) {
        log_file_.reset();
        update_tap_note();
        return;
    }

    const std::string dir = core::data_directory() + "/LOGS";
    if (!core::ensure_directory(dir)) {
        logging_ = false;
        check_log_.set_value(false);
        text_note_.set(STR_COLOR_YELLOW "log dir?");
        return;
    }

    log_path_ = dir + "/VOR_" + timestamp_now() + ".CSV";
    log_file_ = std::make_unique<std::ofstream>(log_path_, std::ios::out | std::ios::app);
    if (!log_file_ || !log_file_->good()) {
        log_file_.reset();
        logging_ = false;
        check_log_.set_value(false);
        text_note_.set(STR_COLOR_YELLOW "log open?");
        return;
    }

    /* Upstream's VorLogger::write_header(), plus the two host-side columns. */
    *log_file_ << "Time;Course;Radial;Deviation;Quality;Flag;Dev_Hz;Depth\n";
    log_file_->flush();
    text_note_.set(STR_COLOR_GREEN "logging");
}

void VorRxView::log_status(const VorStatus& status, uint16_t radial_deg) {
    const int32_t course = field_course_.value();
    const int32_t deviation = vor_course_deviation(radial_deg, course);

    *log_file_ << timestamp_now() << ';' << course << ';' << radial_deg << ';'
               << deviation << ';' << static_cast<int>(status.quality) << ';'
               << (status.valid ? vor_flag_label(flag_.flag()) : "--") << ';'
               << status.ref_level << ';' << status.var_level << '\n';
}

void VorRxView::on_frame_sync() {
    View::on_frame_sync();

    pump_samples();

    /* The receiver may have been started or stopped from elsewhere; keep the
     * state line honest without repainting it every frame. */
    if ((++frame_counter_ % 30) == 0) update_status_text();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream declares menu_location RX and icon_color orange (external/vor_rx/
 * main.cpp). Its bitmap is a plain diamond that nothing in bitmaps.hpp matches,
 * so this takes the generic tile rather than borrowing a misleading icon. */
const app::Registrar reg_vor_rx{{"vor_rx", "VOR RX", app::Category::Receive,
                                 ui::Color::orange(), nullptr,
                                 [] { return std::make_unique<app::VorRxView>(); }}};
}  // namespace
