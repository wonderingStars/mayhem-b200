/*
 * mayhem-b200 — Search: wideband energy-search scanner.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_search.hpp"

#include "../core/file_path.hpp"
#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"

namespace app {

namespace {

/* The byte-scale window used to quantise the dBFS FFT for the detector. About
 * the noise floor of a B200 at moderate gain (→0) up to a strong signal (→255).
 * Host-only: the firmware's baseband delivered bytes already. */
constexpr float kFloorDb = -110.0f;
constexpr float kCeilDb = -20.0f;

/* "HH:MM:SS" from the host clock — the firmware fills the Time column from the
 * RTC's hour/minute/second. to_string_datetime_now() is "YYYY-MM-DD HH:MM:SS". */
std::string now_hms() {
    const std::string dt = to_string_datetime_now();
    return dt.size() >= 19 ? dt.substr(11, 8) : dt;
}

}  // namespace

SearchView::SearchView()
    : receiver_{*globals().receiver} {
    add_children({&labels_,
                  &field_frequency_min_,
                  &field_frequency_max_,
                  &field_gain_,
                  &field_threshold_,
                  &text_mean_,
                  &text_slices_,
                  &text_rate_,
                  &trace_,
                  &progress_timers_,
                  &vu_max_,
                  &text_infos_,
                  &big_display_,
                  &check_snap_,
                  &options_snap_,
                  &check_log_,
                  &recent_view_});

    window_ = dsp::make_window(dsp::WindowType::BlackmanHarris, fft_.size());
    trace_.set_range(kFloorDb, kCeilDb);

    /* Ranges from the device where we can; the published figures are only a
     * fallback (PORTING radio facts). */
    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_min_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                       static_cast<uint64_t>(caps.rx_freq.max));
        field_frequency_max_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                       static_cast<uint64_t>(caps.rx_freq.max));
    }

    field_frequency_min_.set_step_index(9);  /* 100 kHz */
    field_frequency_min_.set_value(freq_min_, false);
    field_frequency_min_.on_change = [this](uint64_t hz) {
        freq_min_ = hz;
        on_range_changed();
    };

    field_frequency_max_.set_step_index(9);
    field_frequency_max_.set_value(freq_max_, false);
    field_frequency_max_.on_change = [this](uint64_t hz) {
        freq_max_ = hz;
        on_range_changed();
    };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    field_threshold_.set_value(static_cast<int32_t>(power_threshold_), false);
    field_threshold_.on_change = [this](int32_t v) {
        power_threshold_ = static_cast<uint32_t>(v);
    };

    check_snap_.set_value(snap_search_);
    check_snap_.on_select = [this](ui::Checkbox&, bool v) { snap_search_ = v; };

    options_snap_.set_by_value(static_cast<int32_t>(snap_step_), false);
    options_snap_.on_change = [this](size_t, int32_t value) {
        snap_step_ = static_cast<uint32_t>(value);
    };

    check_log_.set_value(false);
    check_log_.on_select = [this](ui::Checkbox&, bool v) { set_logging(v); };

    text_mean_.set("---");
    text_rate_.set("---");
    progress_timers_.set_max(search::detect_delay);

    /* Results table: recency-ordered, keyed by frequency. */
    recent_view_.set_parent_rect({0, 196, 240, 108});
    recent_view_.on_select = [this](const SearchRecentEntry& entry) {
        /* The firmware pushes FrequencySaveView here; this host build has no
         * such view yet, so selecting a hit tunes the receiver to it. */
        receiver_.set_target_frequency(entry.frequency);
        big_display_.set(entry.frequency);
    };

    recent_view_.table().on_draw =
        [](const SearchRecentEntry& entry, const ui::Rect& target_rect, ui::Painter& painter,
           const ui::Style& style, ui::RecentEntriesColumns& columns) {
            std::string freq = to_string_short_freq(entry.frequency);
            freq.resize(columns.at(0).second, ' ');

            std::string time = entry.time;
            time.resize(columns.at(1).second, ' ');

            std::string dur = search::format_duration(entry.duration);
            dur.resize(columns.at(2).second, ' ');

            painter.draw_string(target_rect.location(), style, freq + " " + time + " " + dur);
        };
}

SearchView::~SearchView() {
    if (log_file_.is_open()) log_file_.close();
    /* Put the receiver back the way we found it. */
    receiver_.set_mode(previous_mode_);
    receiver_.set_sampling_rate(previous_sample_rate_);
}

void SearchView::on_show() {
    View::on_show();

    previous_mode_ = receiver_.mode();
    previous_sample_rate_ = receiver_.sampling_rate();

    /* Spectrum-only mode, captured at exactly one slice wide so the 256-bin FFT
     * maps to SEARCH_BIN_WIDTH per bin. */
    receiver_.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
    receiver_.set_sampling_rate(static_cast<double>(search::slice_width_hz));

    on_range_changed();

    if (!receiver_.running()) receiver_.start();

    field_frequency_min_.focus();
}

void SearchView::on_hide() {
    View::on_hide();
}

void SearchView::on_range_changed() {
    const auto plan = search::plan_slices(freq_min_, freq_max_);
    slices_nb_ = plan.slices_nb;
    for (int s = 0; s < slices_nb_; s++)
        slices_[s].center_frequency = plan.center[s];

    if (plan.overflow)
        text_slices_.set("!!");
    else
        text_slices_.set(to_string_dec_uint(static_cast<uint64_t>(slices_nb_), 2, ' '));

    slice_counter_ = 0;
    mean_acc_ = 0;
    receiver_.set_target_frequency(slices_[0].center_frequency);
}

void SearchView::process_spectrum() {
    if (!receiver_.take_spectrum_samples(samples_, search::bin_nb)) return;
    if (samples_.size() < static_cast<size_t>(search::bin_nb)) return;

    fft_.spectrum_db(samples_.data(), window_, spectrum_db_);
    if (spectrum_db_.size() < static_cast<size_t>(search::bin_nb)) return;

    for (int i = 0; i < search::bin_nb; i++)
        power_[i] = search::db_to_power(spectrum_db_[i], kFloorDb, kCeilDb);

    trace_.feed(spectrum_db_);

    const auto scan = search::scan_slice(power_.data());
    slices_[slice_counter_].max_power = scan.max_power;
    slices_[slice_counter_].max_index = scan.max_index;
    mean_acc_ += scan.power_sum;

    slice_counter_++;
    if (slice_counter_ >= slices_nb_) {
        do_detection();
        slice_counter_ = 0;
    }

    /* Retune for the next slice (single-slice search stays put). */
    if (slices_nb_ > 1)
        receiver_.set_target_frequency(slices_[slice_counter_].center_frequency);
}

void SearchView::do_detection() {
    mean_power_ = search::mean_power(mean_acc_, slices_nb_);
    mean_acc_ = 0;

    const auto peak = search::find_peak(slices_, slices_nb_, mean_power_, power_threshold_);
    overall_power_max_ = peak.overall_power_max;

    const uint64_t peak_center =
        (peak.bin_max > -1) ? slices_[peak.slice_max].center_frequency : 0;

    const auto outcome = detector_.do_detection(peak.bin_max, peak.slice_max, peak_center,
                                                snap_search_, snap_step_,
                                                freq_min_, freq_max_);

    if (outcome.display_dirty)
        big_display_.set(outcome.frequency);

    switch (outcome.action) {
        case search::Action::Lock: {
            auto& entry = ui::on_packet(recent_, outcome.frequency);
            entry.set_time(now_hms());
            recent_view_.table().set_dirty();
            text_infos_.set("Locked !");
            big_display_.set(outcome.frequency);
            break;
        }

        case search::Action::Release: {
            auto& entry = ui::on_packet(recent_, outcome.frequency);
            entry.set_duration(outcome.duration);
            if (logging_) log_entry(entry);
            recent_view_.table().set_dirty();
            text_infos_.set("Listening");
            break;
        }

        case search::Action::None:
            if (outcome.out_of_range) text_infos_.set("Out of range");
            break;
    }

    search_counter_++;
}

void SearchView::update_timers_ui() {
    if (detector_.locked()) {
        progress_timers_.set_max(search::release_delay);
        progress_timers_.set_value(search::release_delay - detector_.release_timer());
    } else {
        progress_timers_.set_max(search::detect_delay);
        progress_timers_.set_value(detector_.detect_timer());
    }
}

void SearchView::update_power_ui() {
    text_mean_.set(to_string_dec_uint(mean_power_, 3));
    vu_max_.set_value(overall_power_max_);
    const uint32_t mark = mean_power_ + power_threshold_;
    vu_max_.set_mark(static_cast<uint8_t>(mark > 255 ? 255 : mark));
}

void SearchView::update_rate_ui() {
    text_rate_.set(to_string_dec_uint(search_counter_, 3));
    search_counter_ = 0;
}

void SearchView::set_logging(bool enabled) {
    if (log_file_.is_open()) log_file_.close();
    logging_ = enabled;
    if (!enabled) return;

    const std::string dir = core::data_directory() + "/LOGS";
    core::ensure_directory(dir);
    const std::string path = dir + "/SEARCH_" + to_string_timestamp_now() + ".CSV";
    log_file_.open(path, std::ios::out | std::ios::app);
    if (log_file_.is_open())
        log_file_ << "Time;Freq;Duration\n";
    else
        logging_ = false;  /* could not open — do not pretend to log. */
}

void SearchView::log_entry(const SearchRecentEntry& entry) {
    if (!log_file_.is_open()) return;
    log_file_ << entry.time << ";" << to_string_short_freq(entry.frequency) << ";"
              << to_string_dec_uint(static_cast<uint64_t>(entry.duration)) << "\n";
    log_file_.flush();
}

void SearchView::on_frame_sync() {
    View::on_frame_sync();
    frame_counter_++;

    process_spectrum();

    /* ~10 Hz: advance the detector's timers and the progress indicator. */
    if ((frame_counter_ % 6) == 0) {
        detector_.do_timers();
        update_timers_ui();
    }
    /* ~5 Hz: power readouts. */
    if ((frame_counter_ % 12) == 0)
        update_power_ui();
    /* ~1 Hz: sweep rate. */
    if ((frame_counter_ % 60) == 0)
        update_rate_ui();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_search{{"search", "Search", app::Category::Receive,
                                 ui::Color::green(), &ui::bitmap_icon_search,
                                 [] { return std::make_unique<app::SearchView>(); }}};
}  // namespace
