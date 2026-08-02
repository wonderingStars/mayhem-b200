/*
 * mayhem-b200 — spectrum and waterfall widgets.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_spectrum.hpp"

#include "../core/string_format.hpp"
#include "display.hpp"
#include "spectrum_color_lut.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>

namespace ui {
namespace spectrum {

void bins_to_columns(const std::vector<float>& db, size_t width, std::vector<float>& out) {
    out.assign(width, -140.0f);
    if (db.empty() || width == 0) return;

    if (db.size() >= width) {
        /* Decimate by peak: each column reports the strongest bin it covers. */
        for (size_t x = 0; x < width; x++) {
            const size_t first = (x * db.size()) / width;
            size_t last = ((x + 1) * db.size()) / width;
            if (last <= first) last = first + 1;
            if (last > db.size()) last = db.size();

            float peak = db[first];
            for (size_t i = first + 1; i < last; i++) peak = std::max(peak, db[i]);
            out[x] = peak;
        }
        return;
    }

    /* Fewer bins than columns: linear interpolation keeps the trace smooth. */
    for (size_t x = 0; x < width; x++) {
        const double pos = (static_cast<double>(x) * (db.size() - 1)) /
                           static_cast<double>(width - 1 ? width - 1 : 1);
        const size_t i = static_cast<size_t>(pos);
        const double frac = pos - static_cast<double>(i);
        const float a = db[std::min(i, db.size() - 1)];
        const float b = db[std::min(i + 1, db.size() - 1)];
        out[x] = static_cast<float>(a + (b - a) * frac);
    }
}

uint8_t db_to_index(float db, float min_db, float max_db) {
    if (max_db <= min_db) return 0;
    const float t = (db - min_db) / (max_db - min_db);
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lrint(clamped * 255.0f));
}

/* --- WaterfallWidget ------------------------------------------------------- */

WaterfallWidget::WaterfallWidget(Rect parent_rect)
    : Widget{parent_rect} {
}

void WaterfallWidget::on_show() {
    clear();
    const auto r = screen_rect();
    host::display.scroll_set_area(r.top(), r.bottom());
    scrolling_ = true;
}

void WaterfallWidget::on_hide() {
    if (scrolling_) {
        host::display.scroll_disable();
        scrolling_ = false;
    }
    clear();
}

void WaterfallWidget::clear() {
    host::display.fill_rectangle(screen_rect(), Color::black());
}

void WaterfallWidget::paint(Painter&) {
    /* Deliberately empty. The waterfall's pixels are written by feed() straight
     * into the scroll region; repainting here would erase the history. */
}

void WaterfallWidget::set_range(float min_db, float max_db) {
    min_db_ = min_db;
    max_db_ = max_db;
}

void WaterfallWidget::feed(const std::vector<float>& db) {
    if (!scrolling_ || hidden()) return;

    const auto r = screen_rect();
    const size_t width = static_cast<size_t>(r.width());
    if (width == 0) return;

    bins_to_columns(db, width, columns_);

    row_.resize(width);
    for (size_t x = 0; x < width; x++)
        row_[x] = spectrum_rgb4_lut[db_to_index(columns_[x], min_db_, max_db_)];

    /* Step the scroll origin back one line; the returned row is where the newest
     * line belongs so that it appears at the top. */
    const auto draw_y = host::display.scroll(1);
    host::display.render_line({r.left(), draw_y}, static_cast<uint16_t>(width), row_.data());
}

bool WaterfallWidget::on_touch(const TouchEvent event) {
    if (event.type == TouchEvent::Type::Start && on_touch_select)
        on_touch_select(event.point.x() - screen_rect().left());
    return true;
}

/* --- SpectrumWidget -------------------------------------------------------- */

SpectrumWidget::SpectrumWidget(Rect parent_rect)
    : Widget{parent_rect} {
}

void SpectrumWidget::set_range(float min_db, float max_db) {
    min_db_ = min_db;
    max_db_ = max_db;
    set_dirty();
}

void SpectrumWidget::set_peak_hold(bool enabled) {
    peak_hold_ = enabled;
    if (!enabled) peaks_.clear();
    set_dirty();
}

void SpectrumWidget::clear_peaks() {
    peaks_.clear();
    set_dirty();
}

void SpectrumWidget::feed(const std::vector<float>& db) {
    const auto r = screen_rect();
    const size_t width = static_cast<size_t>(r.width());
    if (width == 0) return;

    bins_to_columns(db, width, columns_);

    if (peak_hold_) {
        if (peaks_.size() != width) peaks_.assign(width, -140.0f);
        for (size_t x = 0; x < width; x++) peaks_[x] = std::max(peaks_[x], columns_[x]);
    }

    set_dirty();
}

void SpectrumWidget::paint(Painter& painter) {
    const auto r = screen_rect();
    painter.fill_rectangle(r, Color::black());

    if (columns_.empty()) return;

    const int h = r.height();
    const float span = std::max(1.0f, max_db_ - min_db_);

    const Color trace = Theme::getInstance()->fg_green->foreground;
    const Color peak_color = Theme::getInstance()->fg_yellow->foreground;

    /* Horizontal graticule every 20 dB. */
    for (float level = max_db_; level >= min_db_; level -= 20.0f) {
        const int y = r.bottom() - static_cast<int>(((level - min_db_) / span) * h);
        if (y > r.top() && y < r.bottom())
            painter.draw_hline({r.left(), y}, r.width(), Color::darker_grey());
    }

    for (size_t x = 0; x < columns_.size() && static_cast<int>(x) < r.width(); x++) {
        const float t = std::clamp((columns_[x] - min_db_) / span, 0.0f, 1.0f);
        const int bar = static_cast<int>(t * h);
        if (bar > 0)
            painter.fill_rectangle({r.left() + static_cast<int>(x), r.bottom() - bar, 1, bar},
                                   trace);

        if (peak_hold_ && x < peaks_.size()) {
            const float pt = std::clamp((peaks_[x] - min_db_) / span, 0.0f, 1.0f);
            const int py = r.bottom() - static_cast<int>(pt * h);
            if (py >= r.top() && py < r.bottom())
                painter.fill_rectangle({r.left() + static_cast<int>(x), py, 1, 1}, peak_color);
        }
    }
}

bool SpectrumWidget::on_touch(const TouchEvent event) {
    if (event.type == TouchEvent::Type::Start && on_touch_select)
        on_touch_select(event.point.x() - screen_rect().left());
    return true;
}

/* --- FrequencyScale -------------------------------------------------------- */

FrequencyScale::FrequencyScale(Rect parent_rect)
    : Widget{parent_rect} {
}

void FrequencyScale::set_spectrum(uint64_t center_hz, double span_hz) {
    if (center_hz == center_hz_ && span_hz == span_hz_) return;
    center_hz_ = center_hz;
    span_hz_ = span_hz;
    set_dirty();
}

void FrequencyScale::set_channel_bandwidth(double hz) {
    if (hz == channel_bandwidth_) return;
    channel_bandwidth_ = hz;
    set_dirty();
}

void FrequencyScale::set_channel_offset(double hz) {
    if (hz == channel_offset_) return;
    channel_offset_ = hz;
    set_dirty();
}

void FrequencyScale::paint(Painter& painter) {
    const auto r = screen_rect();
    const auto& s = style();
    const auto& font = font::fixed_5x8;

    painter.fill_rectangle(r, s.background);

    if (span_hz_ <= 0.0) return;

    const double hz_per_px = span_hz_ / static_cast<double>(r.width());
    const int mid_x = r.left() + r.width() / 2;

    /* Channel cursor, drawn under the ticks so the ticks stay readable. */
    if (channel_bandwidth_ > 0.0) {
        const int half_w = std::max(1, static_cast<int>((channel_bandwidth_ / 2.0) / hz_per_px));
        const int cursor_x = mid_x + static_cast<int>(channel_offset_ / hz_per_px);
        const Rect cursor{cursor_x - half_w, r.top(), half_w * 2, r.height()};
        const auto clipped = cursor.intersect(r);
        if (!clipped.is_empty())
            painter.fill_rectangle(clipped, Theme::getInstance()->bg_dark->background);
    }

    /* Choose a tick spacing that gives roughly one label every 60 px. */
    static constexpr double steps[] = {
        1e3, 2e3, 5e3, 1e4, 2.5e4, 5e4, 1e5, 2e5, 5e5, 1e6, 2e6, 5e6, 1e7, 2e7, 5e7};
    double step = steps[0];
    for (double candidate : steps) {
        step = candidate;
        if (candidate / hz_per_px >= 60.0) break;
    }

    const double left_hz = static_cast<double>(center_hz_) - span_hz_ / 2.0;
    const double right_hz = left_hz + span_hz_;

    double tick = std::ceil(left_hz / step) * step;
    for (; tick < right_hz; tick += step) {
        const int x = r.left() + static_cast<int>((tick - left_hz) / hz_per_px);
        if (x < r.left() || x >= r.right()) continue;

        painter.draw_vline({x, r.bottom() - 5}, 5, s.foreground);

        const auto label = to_string_rounded_freq(static_cast<uint64_t>(tick), 3);
        const int label_w = static_cast<int>(label.size()) * font.char_width();
        const int label_x = std::clamp(x - label_w / 2, r.left(), r.right() - label_w);
        painter.draw_string({label_x, r.top()}, font, s.foreground, s.background, label);
    }

    /* Centre marker last so it is always on top. */
    painter.draw_vline({mid_x, r.top()}, r.height(),
                       Theme::getInstance()->fg_red->foreground);
}

}  // namespace spectrum
}  // namespace ui
