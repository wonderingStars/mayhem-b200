/*
 * mayhem-b200 — Stopwatch utility.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_stopwatch.hpp"

#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

#include <chrono>
#include <cstdio>

namespace app {

namespace {

/* Monotonic clock in milliseconds, standing in for the firmware's chTimeNow(). */
uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace

/* ---- stopwatch_fmt ------------------------------------------------------- */

namespace stopwatch_fmt {

HMSC decompose_ms(uint64_t ms) {
    HMSC t{};
    t.hours = static_cast<uint32_t>(ms / 3600000ull);
    t.minutes = static_cast<uint32_t>((ms / 60000ull) % 60ull);
    t.seconds = static_cast<uint32_t>((ms / 1000ull) % 60ull);
    t.centis = static_cast<uint32_t>((ms / 10ull) % 100ull);
    return t;
}

std::string format_elapsed(uint64_t ms) {
    const HMSC t = decompose_ms(ms);
    char buf[32];
    if (t.hours > 0) {
        std::snprintf(buf, sizeof buf, "%u:%02u:%02u.%02u",
                      static_cast<unsigned>(t.hours), static_cast<unsigned>(t.minutes),
                      static_cast<unsigned>(t.seconds), static_cast<unsigned>(t.centis));
    } else {
        std::snprintf(buf, sizeof buf, "%02u:%02u.%02u",
                      static_cast<unsigned>(t.minutes), static_cast<unsigned>(t.seconds),
                      static_cast<unsigned>(t.centis));
    }
    return std::string{buf};
}

std::string format_lap_delta(uint64_t delta_ms) {
    return "+" + format_elapsed(delta_ms);
}

}  // namespace stopwatch_fmt

/* ---- StopwatchView ------------------------------------------------------- */

StopwatchView::StopwatchView() {
    add_children({&labels_, &laps_, &button_run_stop_, &button_reset_lap_, &button_exit_});

    laps_.enable_scrolling(true);

    button_run_stop_.on_select = [this](ui::Button&) { toggle_run(); };

    button_reset_lap_.on_select = [this](ui::Button&) {
        if (running_)
            lap();
        else
            reset();
    };

    button_exit_.on_select = [this](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };

    displayed_ = stopwatch_fmt::format_elapsed(0);
}

void StopwatchView::focus() {
    button_run_stop_.focus();
}

void StopwatchView::on_show() {
    View::on_show();
    displayed_ = stopwatch_fmt::format_elapsed(elapsed_now());
    set_dirty();
}

uint64_t StopwatchView::elapsed_now() const {
    if (running_) return accumulated_ms_ + (now_ms() - start_ms_);
    return accumulated_ms_;
}

void StopwatchView::toggle_run() {
    if (running_)
        stop();
    else
        run();
}

void StopwatchView::run() {
    if (running_) return;
    start_ms_ = now_ms();
    running_ = true;
    button_run_stop_.set_text("STOP");
    button_reset_lap_.set_text("LAP");
}

void StopwatchView::stop() {
    if (!running_) return;
    accumulated_ms_ += now_ms() - start_ms_;
    running_ = false;
    button_run_stop_.set_text("START");
    button_reset_lap_.set_text("RESET");
}

void StopwatchView::lap() {
    const uint64_t total = elapsed_now();
    const uint64_t delta = total - last_lap_total_ms_;
    last_lap_total_ms_ = total;
    ++lap_count_;

    char prefix[8];
    std::snprintf(prefix, sizeof prefix, "L%02u ", static_cast<unsigned>(lap_count_));
    laps_.writeln(std::string{prefix} + stopwatch_fmt::format_elapsed(total) + "  " +
                  stopwatch_fmt::format_lap_delta(delta));
}

void StopwatchView::reset() {
    accumulated_ms_ = 0;
    start_ms_ = 0;
    last_lap_total_ms_ = 0;
    lap_count_ = 0;
    laps_.clear();
    displayed_ = stopwatch_fmt::format_elapsed(0);
    set_dirty();
}

void StopwatchView::on_frame_sync() {
    View::on_frame_sync();

    const std::string s = stopwatch_fmt::format_elapsed(elapsed_now());
    if (s != displayed_) {
        displayed_ = s;
        set_dirty();
    }
}

void StopwatchView::paint(ui::Painter& painter) {
    View::paint(painter);  /* clears the background; children paint after */

    /* Big elapsed readout, drawn char-by-char at 2x (16x32 px per glyph). */
    const ui::Style* base = running_ ? ui::Theme::getInstance()->fg_green
                                     : ui::Theme::getInstance()->fg_light;
    const uint8_t zoom = 2;
    const int glyph_w = ui::char_width * zoom;
    const int total_w = static_cast<int>(displayed_.size()) * glyph_w;
    const auto origin = screen_pos();
    int x = origin.x() + (screen_rect().width() - total_w) / 2;
    if (x < origin.x()) x = origin.x();
    const int y = origin.y() + 30;

    for (char c : displayed_)
        x += painter.draw_char({x, y}, *base, c, zoom);
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_stopwatch{{"stopwatch", "Stopwatch",
                                    app::Category::Utilities, ui::Color::cyan(),
                                    &ui::bitmap_icon_utilities,
                                    [] { return std::make_unique<app::StopwatchView>(); }}};
}  // namespace
