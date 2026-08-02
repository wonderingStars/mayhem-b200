/*
 * mayhem-b200 — spectrum and waterfall widgets.
 *
 * The waterfall uses the display's vertical-scroll region exactly as the
 * firmware does: each new FFT row is written to a single line and the scroll
 * origin steps back one, so drawing a frame costs one row of pixels rather than
 * a full-height blit.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __HOST_UI_SPECTRUM_H__
#define __HOST_UI_SPECTRUM_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ui {
namespace spectrum {

/* Maps a row of dBFS values onto `width` screen columns.
 *
 * When there are more bins than columns the maximum within each column's span
 * is taken, not the mean: peak-holding is what makes a narrow carrier visible
 * at a wide span, and averaging would hide it. */
void bins_to_columns(const std::vector<float>& db,
                     size_t width,
                     std::vector<float>& out);

/* Converts dBFS to a palette index using the configured display range. */
uint8_t db_to_index(float db, float min_db, float max_db);

/* --- Waterfall --------------------------------------------------------------
 * Draws immediately rather than during paint(), because it owns a hardware-
 * style scroll region whose contents must not be repainted by the widget tree. */
class WaterfallWidget : public Widget {
   public:
    explicit WaterfallWidget(Rect parent_rect);

    void on_show() override;
    void on_hide() override;
    void paint(Painter& painter) override;

    /* Renders one spectrum row at the top of the waterfall. */
    void feed(const std::vector<float>& db);

    void clear();

    void set_range(float min_db, float max_db);
    float min_db() const { return min_db_; }
    float max_db() const { return max_db_; }

    /* Called with the x coordinate of a tap, so the app can tune to it. */
    std::function<void(int x)> on_touch_select{};

    bool on_touch(const TouchEvent event) override;

   private:
    std::vector<float> columns_{};
    std::vector<Color> row_{};
    float min_db_{-100.0f};
    float max_db_{-20.0f};
    bool scrolling_{false};
};

/* --- Spectrum trace ---------------------------------------------------------
 * A conventional amplitude-versus-frequency plot, drawn during paint(). */
class SpectrumWidget : public Widget {
   public:
    explicit SpectrumWidget(Rect parent_rect);

    void feed(const std::vector<float>& db);
    void paint(Painter& painter) override;

    void set_range(float min_db, float max_db);
    void set_peak_hold(bool enabled);
    bool peak_hold() const { return peak_hold_; }
    void clear_peaks();

    std::function<void(int x)> on_touch_select{};
    bool on_touch(const TouchEvent event) override;

   private:
    std::vector<float> columns_{};
    std::vector<float> peaks_{};
    float min_db_{-100.0f};
    float max_db_{-20.0f};
    bool peak_hold_{false};
};

/* --- Frequency scale --------------------------------------------------------
 * Ticks, centre marker and a cursor showing the demodulator's channel width. */
class FrequencyScale : public Widget {
   public:
    static constexpr Dim scale_height = 20;

    explicit FrequencyScale(Rect parent_rect);

    /* `center_hz` is the middle of the displayed span. */
    void set_spectrum(uint64_t center_hz, double span_hz);
    /* Width of the tuned channel, drawn as a cursor. Zero hides it. */
    void set_channel_bandwidth(double hz);
    /* Offset of the tuned channel from the centre, in Hz. */
    void set_channel_offset(double hz);

    void paint(Painter& painter) override;

   private:
    uint64_t center_hz_{0};
    double span_hz_{0.0};
    double channel_bandwidth_{0.0};
    double channel_offset_{0.0};
};

}  // namespace spectrum
}  // namespace ui

#endif /*__HOST_UI_SPECTRUM_H__*/
