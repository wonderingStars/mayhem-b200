/*
 * mayhem-b200 — Detector RX (implementation).
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2018 Furrtek
 * Copyright (C) 2023 gullradriel, Nilorea Studio Inc. (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_detector_rx.hpp"

#include "app_context.hpp"
#include "app_registry.hpp"
#include "bitmaps.hpp"
#include "settings.hpp"
#include "string_format.hpp"
#include "theme.hpp"
#include "ui_menu.hpp"
#include "ui_navigation.hpp"

#include "../audio/audio_out.hpp"
#include "../radio/receiver_model.hpp"
#include "../radio/usrp_radio.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace app {

namespace {

constexpr std::string_view kSection = "rx_detector";

constexpr float kLevelFloorDb = -110.0f;
constexpr float kLevelCeilDb = -10.0f;

/* Freqman list picker. Own copy per app — see doc/PORTING.md. */
class DetectorFilePicker : public ui::View {
   public:
    explicit DetectorFilePicker(std::function<void(std::string)> on_pick)
        : on_pick_{std::move(on_pick)} {
        add_children({&menu_, &text_empty_});

        auto files = core::get_freqman_files();
        if (files.empty()) {
            text_empty_.set("No lists in FREQMAN");
            menu_.hidden(true);
            return;
        }

        text_empty_.hidden(true);
        for (auto& stem : files) {
            std::string name = stem;
            menu_.add_item({name, ui::Color::white(), [this, name]() {
                                if (on_pick_) on_pick_(name);
                                if (auto* nav = app::globals().nav) nav->pop();
                            }});
        }
    }

    std::string title() const override { return "Detector list"; }

    void focus() override {
        if (!menu_.hidden())
            menu_.focus();
        else
            text_empty_.focus();
    }

   private:
    std::function<void(std::string)> on_pick_{};
    ui::MenuView menu_{{0, 0, 240, 288}};
    ui::Text text_empty_{{0, 8, 240, 16}, ""};
};

}  // namespace

/* ======================================================================== */
/* DetectorLevelGraph                                                        */
/* ======================================================================== */

void DetectorLevelGraph::paint(ui::Painter& painter) {
    const auto rect = screen_rect();
    painter.fill_rectangle(rect, ui::Theme::getInstance()->bg_darkest->background);

    if (values_.empty() || max_db_ <= min_db_) return;

    const int h = rect.height();
    int x = rect.left();
    for (float v : values_) {
        float frac = (v - min_db_) / (max_db_ - min_db_);
        frac = std::clamp(frac, 0.0f, 1.0f);
        const int bar = static_cast<int>(frac * static_cast<float>(h));
        if (bar > 0)
            painter.draw_vline({x, rect.bottom() - bar}, bar,
                               ui::Theme::getInstance()->fg_green->foreground);
        x++;
        if (x >= rect.right()) break;
    }
}

/* ======================================================================== */
/* DetectorRxView                                                            */
/* ======================================================================== */

DetectorRxView::DetectorRxView()
    : receiver_{*app::globals().receiver} {
    add_children({&labels_,
                  &field_gain_,
                  &field_beep_squelch_,
                  &button_file_,
                  &button_auto_advance_,
                  &button_index_,
                  &text_entry_desc_,
                  &button_auto_scan_,
                  &button_freq_,
                  &text_power_,
                  &text_rssi_,
                  &level_meter_,
                  &level_graph_});

    {
        auto& s = core::settings();
        beep_squelch_ = static_cast<int32_t>(s.get_int(kSection, "beep_squelch", 0));
        freq_file_stem_ = s.get_string(kSection, "freq_file", "DETECTOR");
        scanner_.set_auto_scan(s.get_bool(kSection, "auto_scan", true));
        scanner_.set_auto_advance(s.get_bool(kSection, "auto_advance", false));
    }

    level_graph_.set_range(kLevelFloorDb, kLevelCeilDb);

    if (auto* r = app::globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
    }
    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t v) { receiver_.set_gain(v); };

    field_beep_squelch_.set_value(beep_squelch_, false);
    field_beep_squelch_.on_change = [this](int32_t v) {
        beep_squelch_ = v;
        core::settings().set_int(kSection, "beep_squelch", v);
    };

    button_file_.on_select = [this](ui::Button&) {
        auto* nav = app::globals().nav;
        if (!nav) return;
        nav->push(std::make_unique<DetectorFilePicker>(
            [this](std::string stem) { load_freqman(stem); }));
    };

    button_index_.on_change = [this]() {
        const int32_t delta = button_index_.get_encoder_delta();
        button_index_.set_encoder_delta(0);
        if (scanner_.step_index(delta)) {
            update_entry_display();
            update_freq_display();
            retune();
        }
    };

    button_freq_.on_change = [this]() {
        const int32_t delta = button_freq_.get_encoder_delta();
        button_freq_.set_encoder_delta(0);
        if (scanner_.step_frequency(delta)) {
            update_freq_display();
            retune();
        }
    };

    button_auto_scan_.on_select = [this](ui::Button&) {
        scanner_.set_auto_scan(!scanner_.auto_scan());
        button_auto_scan_.set_text(scanner_.auto_scan() ? "AUTOSCAN" : "NO SCAN");
        core::settings().set_bool(kSection, "auto_scan", scanner_.auto_scan());
        last_freq_display_kind_ = -1;
        update_freq_display();
    };
    button_auto_scan_.set_text(scanner_.auto_scan() ? "AUTOSCAN" : "NO SCAN");

    button_auto_advance_.on_select = [this](ui::Button&) {
        scanner_.set_auto_advance(!scanner_.auto_advance());
        button_auto_advance_.set_text(scanner_.auto_advance() ? "AUTOADV" : "NO ADV");
        core::settings().set_bool(kSection, "auto_advance", scanner_.auto_advance());
    };
    button_auto_advance_.set_text(scanner_.auto_advance() ? "AUTOADV" : "NO ADV");

    /* Upstream runs the capture baseband at DETECTOR_BW with no demodulation;
     * SpectrumAnalysis is the host's equivalent "measure, do not demodulate". */
    receiver_.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
    receiver_.set_sampling_rate(static_cast<double>(detector_rx::kDetectorBw));
    receiver_.set_squelch_level(0);

    load_freqman(freq_file_stem_);
}

DetectorRxView::~DetectorRxView() {
    core::settings().set_string(kSection, "freq_file", freq_file_stem_);
    core::settings().save();
}

void DetectorRxView::load_freqman(const std::string& stem) {
    /* Upstream loads freqs and ranges only. */
    core::freqman_load_options options{};
    options.load_freqs = true;
    options.load_ranges = true;
    options.load_hamradios = false;
    options.load_repeaters = false;

    core::freqman_db db;
    if (!core::load_freqman_file(stem, db, options) || db.empty()) {
        scanner_.set_list(core::freqman_db{});
        button_file_.set_text("No file!");
        button_index_.set_text("");
        text_entry_desc_.set("");
        button_freq_.set_text("");
        return;
    }

    freq_file_stem_ = stem;
    scanner_.set_list(std::move(db));
    button_file_.set_text(freq_file_stem_);

    last_freq_display_kind_ = -1;
    update_entry_display();
    update_freq_display();
    retune();
}

void DetectorRxView::update_entry_display() {
    if (scanner_.empty()) return;
    button_index_.set_text(to_string_dec_uint(scanner_.current_index() + 1) + "/" +
                           to_string_dec_uint(scanner_.size()));
    text_entry_desc_.set(scanner_.current_entry().description);
}

void DetectorRxView::update_freq_display() {
    if (scanner_.auto_scan() && (scanner_.min_frequency() != scanner_.max_frequency())) {
        if (last_freq_display_kind_ != 1) {
            button_freq_.set_text("RANGE SCAN...");
            last_freq_display_kind_ = 1;
        }
    } else {
        button_freq_.set_text(detector_rx::format_freq_mhz(scanner_.frequency()));
        last_freq_display_kind_ = 0;
    }
}

void DetectorRxView::retune() {
    receiver_.set_target_frequency(
        static_cast<uint64_t>(std::max<int64_t>(scanner_.frequency(), 0)));
}

/* Host stand-in for baseband::request_audio_beep(pitch, 24000, 150): synthesise
 * the tone and hand it to the audio device. Detector runs with no demodulator,
 * so nothing else is writing to the output. */
void DetectorRxView::beep(int32_t max_db) {
    auto* out = app::globals().audio_out;
    if (!out || !out->running()) return;

    const int32_t pitch = detector_rx::beep_frequency(max_db);
    if (pitch <= 0) return;

    const size_t n = static_cast<size_t>(audio::sample_rate) *
                     detector_rx::kBeepDurationMs / 1000u;
    if (out->space() < n) return;  /* do not stall the UI thread on a full ring */

    beep_buffer_.resize(n);
    const double w = 2.0 * 3.14159265358979323846 * static_cast<double>(pitch) /
                     static_cast<double>(audio::sample_rate);
    for (size_t i = 0; i < n; i++) {
        /* Short linear fade in/out so the tone does not click. */
        const double env = std::min<double>(
            1.0, std::min<double>(static_cast<double>(i), static_cast<double>(n - 1 - i)) / 64.0);
        beep_buffer_[i] = static_cast<float>(0.25 * env * std::sin(w * static_cast<double>(i)));
    }
    out->write(beep_buffer_.data(), n);
}

void DetectorRxView::on_show() {
    View::on_show();
    button_index_.focus();
    if (!receiver_.running()) receiver_.start();
}

void DetectorRxView::on_frame_sync() {
    View::on_frame_sync();
    frame_counter_++;
    if (beep_cooldown_frames_ > 0) beep_cooldown_frames_--;

    /* Upstream's on_timer() ran once per DisplayFrameSync. */
    if (scanner_.on_timer()) {
        retune();
        update_entry_display();
        update_freq_display();
    }

    /* Upstream's on_statistics_update() arrived ~10 times a second. */
    if ((frame_counter_ % 6) != 0) return;

    const float level = receiver_.channel_level_db();
    history_.add(level);
    level_graph_.add(level);

    float frac = (level - kLevelFloorDb) / (kLevelCeilDb - kLevelFloorDb);
    frac = std::clamp(frac, 0.0f, 1.0f);
    level_meter_.set_value(static_cast<uint8_t>(frac * 255.0f));

    const int32_t db = static_cast<int32_t>(std::lround(level));
    if (db != last_shown_db_) {
        last_shown_db_ = db;
        text_power_.set("Power: " + to_string_dec_int(db) + " db");
    }

    text_rssi_.set("dB:" + to_string_dec_int(static_cast<int32_t>(std::lround(history_.min()))) +
                   "/" + to_string_dec_int(static_cast<int32_t>(std::lround(history_.avg()))) +
                   "/" + to_string_dec_int(static_cast<int32_t>(std::lround(history_.max()))));

    if (db > beep_squelch_ && beep_cooldown_frames_ == 0) {
        beep(db);
        /* One beep per ~150 ms, matching the tone length upstream requested. */
        beep_cooldown_frames_ = 9;
    }
}

}  // namespace app

/* --- Registration --------------------------------------------------------- */

namespace {
const app::Registrar reg_detector_rx{{"detector_rx", "Detector", app::Category::Receive,
                                      ui::Color::yellow(), &ui::bitmap_icon_looking,
                                      [] { return std::make_unique<app::DetectorRxView>(); }}};
}  // namespace
