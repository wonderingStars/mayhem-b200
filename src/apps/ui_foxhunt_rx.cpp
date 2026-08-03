/*
 * mayhem-b200 — Fox hunt (implementation).
 *
 * Copyright (C) 2024 HTotoo (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_foxhunt_rx.hpp"

#include "app_context.hpp"
#include "app_registry.hpp"
#include "bitmaps.hpp"
#include "settings.hpp"
#include "string_format.hpp"
#include "theme.hpp"
#include "ui_alphanum.hpp"
#include "ui_navigation.hpp"

#include "../radio/receiver_model.hpp"
#include "../radio/usrp_radio.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

namespace app {

namespace {

constexpr std::string_view kSection = "rx_foxhunt";

constexpr float kLevelFloorDb = -110.0f;
constexpr float kLevelCeilDb = -10.0f;

std::string db_string(float db) {
    return to_string_dec_int(static_cast<int32_t>(std::lround(db)));
}

}  // namespace

/* ======================================================================== */
/* FoxhuntLevelGraph                                                         */
/* ======================================================================== */

void FoxhuntLevelGraph::paint(ui::Painter& painter) {
    const auto rect = screen_rect();
    painter.fill_rectangle(rect, ui::Theme::getInstance()->bg_darkest->background);

    if (max_db_ <= min_db_) return;

    const int h = rect.height();
    const auto scale = [&](float v) {
        float frac = (v - min_db_) / (max_db_ - min_db_);
        frac = std::clamp(frac, 0.0f, 1.0f);
        return static_cast<int>(frac * static_cast<float>(h));
    };

    int x = rect.left();
    for (float v : values_) {
        const int bar = scale(v);
        if (bar > 0)
            painter.draw_vline({x, rect.bottom() - bar}, bar,
                               ui::Theme::getInstance()->fg_green->foreground);
        x++;
        if (x >= rect.right()) break;
    }

    if (!values_.empty()) {
        const int py = rect.bottom() - scale(peak_db_);
        if (py >= rect.top() && py < rect.bottom())
            painter.draw_hline({rect.left(), py}, rect.width(),
                               ui::Theme::getInstance()->fg_yellow->foreground);
    }
}

/* ======================================================================== */
/* FoxhuntRxView                                                             */
/* ======================================================================== */

FoxhuntRxView::FoxhuntRxView()
    : receiver_{*app::globals().receiver} {
    add_children({&labels_,
                  &field_frequency_,
                  &field_gain_,
                  &field_volume_,
                  &field_threshold_,
                  &text_power_,
                  &text_trend_,
                  &level_meter_,
                  &level_graph_,
                  &text_peak_,
                  &button_mark_,
                  &button_clear_,
                  &button_reset_,
                  &text_best_,
                  &console_,
                  &text_note_});

    level_graph_.set_range(kLevelFloorDb, kLevelCeilDb);
    text_note_.set_style(ui::Theme::getInstance()->fg_yellow);
    console_.set_max_lines(64);

    {
        auto& s = core::settings();
        engine_.set_threshold_db(
            static_cast<float>(s.get_int(kSection, "threshold_db", -60)));
    }
    engine_.set_smoothing_alpha(0.25f);
    engine_.set_peak_decay_db(0.05f);
    engine_.set_hysteresis_db(3.0f);
    engine_.set_trend_deadband_db(1.0f);
    engine_.set_trend_window(8);

    if (auto* r = app::globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    field_frequency_.set_step_index(2);  /* 100 Hz, upstream's set_step(100) */
    field_frequency_.set_value(receiver_.target_frequency(), false);
    field_frequency_.on_change = [this](uint64_t hz) {
        receiver_.set_target_frequency(hz);
        engine_.reset();
        level_graph_.clear();
    };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t v) { receiver_.set_gain(v); };

    field_volume_.set_value(receiver_.volume(), false);
    field_volume_.on_change = [this](int32_t v) {
        receiver_.set_volume(static_cast<uint8_t>(v));
    };

    field_threshold_.set_value(static_cast<int32_t>(std::lround(engine_.threshold_db())), false);
    field_threshold_.on_change = [this](int32_t v) {
        engine_.set_threshold_db(static_cast<float>(v));
        core::settings().set_int(kSection, "threshold_db", v);
    };

    button_mark_.on_select = [this](ui::Button&) {
        auto* nav = app::globals().nav;
        if (!nav) return;
        static std::string buf;
        buf = "";
        ui::text_prompt(*nav, buf, 3, ENTER_KEYBOARD_MODE_DIGITS,
                        [this](std::string& b) {
                            const long deg = std::strtol(b.c_str(), nullptr, 10);
                            mark_bearing(static_cast<uint16_t>(
                                std::clamp<long>(deg, 0, 359)));
                        });
    };

    button_clear_.on_select = [this](ui::Button&) {
        log_.clear();
        refresh_marks();
    };

    button_reset_.on_select = [this](ui::Button&) {
        engine_.reset();
        level_graph_.clear();
    };

    /* Upstream runs the AM audio baseband so the hunter can hear the fox. */
    receiver_.set_mode(radio::ReceiverModel::Mode::AMAudio);
    receiver_.set_squelch_level(0);

    refresh_marks();
    text_trend_.set(foxhunt::trend_text(foxhunt::Trend::Unknown));
}

FoxhuntRxView::~FoxhuntRxView() {
    core::settings().save();
}

void FoxhuntRxView::mark_bearing(uint16_t degrees) {
    if (!engine_.primed()) return;
    log_.add(degrees, engine_.smoothed());
    refresh_marks();
}

void FoxhuntRxView::refresh_marks() {
    console_.clear();
    for (const auto& m : log_.marks())
        console_.writeln(to_string_dec_uint(m.degrees, 3, '0') + " deg  " +
                         db_string(m.level_db) + " dB");

    if (log_.empty()) {
        text_best_.set("No bearings marked");
        text_best_.set_style(ui::Theme::getInstance()->fg_light);
        return;
    }

    const auto best = log_.best();
    text_best_.set("Best: " + to_string_dec_uint(best.degrees, 3, '0') + " deg  " +
                   db_string(best.level_db) + " dB");
    text_best_.set_style(ui::Theme::getInstance()->fg_green);
}

void FoxhuntRxView::on_show() {
    View::on_show();
    field_frequency_.focus();
    if (!receiver_.running()) receiver_.start();
}

void FoxhuntRxView::on_frame_sync() {
    View::on_frame_sync();
    frame_counter_++;

    /* ~10 Hz, the cadence upstream's ChannelStatistics arrived at. */
    if ((frame_counter_ % 6) != 0) return;

    const float level = receiver_.channel_level_db();
    engine_.update(level);

    float frac = (engine_.smoothed() - kLevelFloorDb) / (kLevelCeilDb - kLevelFloorDb);
    frac = std::clamp(frac, 0.0f, 1.0f);
    level_meter_.set_value(static_cast<uint8_t>(frac * 255.0f));
    level_graph_.add(engine_.smoothed(), engine_.peak());

    text_power_.set("Power: " + db_string(level) + " db");
    text_power_.set_style(engine_.detected() ? ui::Theme::getInstance()->fg_green
                                             : ui::Theme::getInstance()->fg_light);

    text_trend_.set(foxhunt::trend_text(engine_.trend()));

    text_peak_.set("Avg " + db_string(engine_.smoothed()) + "  Peak " +
                   db_string(engine_.peak()) + "  -" +
                   db_string(engine_.below_peak_db()) + " from peak");
}

}  // namespace app

/* --- Registration --------------------------------------------------------- */

namespace {
const app::Registrar reg_foxhunt{{"foxhunt", "Fox hunt", app::Category::Receive,
                                  ui::Color::yellow(), &ui::bitmap_icon_looking,
                                  [] { return std::make_unique<app::FoxhuntRxView>(); }}};
}  // namespace
