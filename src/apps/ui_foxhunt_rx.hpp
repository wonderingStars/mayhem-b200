/*
 * mayhem-b200 — Fox hunt (RSSI-guided direction finding).
 *
 * Ported from firmware/application/external/foxhunt/ui_foxhunt_rx.*. Upstream
 * tunes an AM channel, plots the RSSI history and the channel power, and drops
 * markers on a GeoMap from the PortaPack's external GPS and compass modules so
 * you can see where you were when the signal was strong.
 *
 * A USRP B200 has no GPS and no magnetometer, and this build has no driver for
 * either. Faking a position or a heading would be exactly the "silence that
 * looks like success" doc/PORTING.md forbids, so the map is not ported. What is
 * ported is the part that actually finds the fox with a directional antenna: the
 * RSSI trace and the power readout, plus the smoothing, peak-hold, trend and
 * threshold logic a hunter reads while swinging the beam. Bearings are entered
 * by hand (from a real compass) and recorded against the smoothed level, which
 * is how a hand-held hunt is run anyway.
 *
 * The pure logic lives in namespace app::foxhunt so it can be tested without a
 * radio or a screen:
 *
 *   FoxhuntEngine — exponential smoothing of the channel level, decaying peak
 *                   hold, closer/farther trend classification against a
 *                   reference taken `trend_window` updates ago, and a
 *                   hysteretic detect threshold. Time enters only as "one
 *                   update", the same way upstream was driven by one
 *                   ChannelStatistics message per interval.
 *
 *   BearingLog    — the manual replacement for upstream's GeoMap markers: a
 *                   bearing in degrees paired with the smoothed level at the
 *                   moment it was marked, and the strongest bearing so far.
 *
 * Other host deviations:
 *   - The three HackRF gain controls collapse to the B200's single gain.
 *   - Levels are radio::ReceiverModel::channel_level_db() in dBFS rather than
 *     the HackRF's ChannelStatistics max_db, so the numeric range differs; the
 *     smoothing and threshold maths are scale-independent.
 *
 * Copyright (C) 2024 HTotoo (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_FOXHUNT_RX_H__
#define __MB200_UI_FOXHUNT_RX_H__

#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace radio {
class ReceiverModel;
}

namespace app {

/* ======================================================================== *
 *  Pure Fox hunt logic — no UI, no radio, no clock.                         *
 * ======================================================================== */
namespace foxhunt {

/* Which way the signal is going, as shown on the trend indicator. */
enum class Trend : int8_t {
    Unknown = 0,
    Closer = 1,   /* level rising by more than the dead band  */
    Steady = 2,
    Farther = 3,  /* level falling by more than the dead band */
};

/* Header-only so the pure layer needs no link against the view. */
inline const char* trend_text(Trend t) {
    switch (t) {
        case Trend::Closer:
            return "CLOSER  ^";
        case Trend::Farther:
            return "FARTHER v";
        case Trend::Steady:
            return "STEADY  =";
        case Trend::Unknown:
        default:
            return "---";
    }
}

/* --- FoxhuntEngine ---------------------------------------------------------
 * One update() per level measurement. Everything here is deterministic and
 * frame-rate independent in the sense that "one update" is the only unit of
 * time; the view decides how often that is.
 *
 * smoothing_alpha is the usual exponential-moving-average coefficient:
 *   s[n] = s[n-1] + alpha * (x[n] - s[n-1])
 * with s[0] = x[0], so the first reading is not dragged up from a cold start.
 * alpha = 1 disables smoothing; alpha is clamped to (0, 1].
 *
 * The peak hold rises instantly to any new maximum and otherwise decays by
 * peak_decay_db per update, so a peak from a previous sweep of the antenna
 * fades instead of pinning the display forever.
 *
 * The detect threshold is hysteretic: it latches on above `threshold` and only
 * releases below `threshold - hysteresis`, which stops a marginal signal
 * chattering the indicator. */
class FoxhuntEngine {
   public:
    void set_smoothing_alpha(float alpha) {
        alpha_ = std::clamp(alpha, 0.001f, 1.0f);
    }
    float smoothing_alpha() const { return alpha_; }

    void set_peak_decay_db(float db) { peak_decay_ = std::max(0.0f, db); }
    float peak_decay_db() const { return peak_decay_; }

    void set_threshold_db(float db) { threshold_ = db; }
    float threshold_db() const { return threshold_; }

    void set_hysteresis_db(float db) { hysteresis_ = std::max(0.0f, db); }
    float hysteresis_db() const { return hysteresis_; }

    /* Dead band, in dB, inside which the trend reads Steady. */
    void set_trend_deadband_db(float db) { deadband_ = std::max(0.0f, db); }
    float trend_deadband_db() const { return deadband_; }

    /* How many updates back the trend reference is taken from. */
    void set_trend_window(size_t n) { trend_window_ = (n == 0) ? 1 : n; }
    size_t trend_window() const { return trend_window_; }

    void reset() {
        primed_ = false;
        smoothed_ = 0.0f;
        peak_ = 0.0f;
        detected_ = false;
        trend_ = Trend::Unknown;
        history_.clear();
        updates_ = 0;
    }

    /* One measurement. Returns the smoothed level. */
    float update(float db) {
        if (!primed_) {
            primed_ = true;
            smoothed_ = db;
            peak_ = db;
        } else {
            smoothed_ += alpha_ * (db - smoothed_);
            if (smoothed_ > peak_)
                peak_ = smoothed_;
            else
                peak_ -= peak_decay_;
        }

        /* Hysteretic detect. */
        if (!detected_) {
            if (smoothed_ > threshold_) detected_ = true;
        } else {
            if (smoothed_ < threshold_ - hysteresis_) detected_ = false;
        }

        /* Trend against the reading trend_window updates ago. */
        history_.push_back(smoothed_);
        while (history_.size() > trend_window_ + 1) history_.pop_front();

        if (history_.size() > trend_window_) {
            const float ref = history_.front();
            const float delta = smoothed_ - ref;
            if (delta > deadband_)
                trend_ = Trend::Closer;
            else if (delta < -deadband_)
                trend_ = Trend::Farther;
            else
                trend_ = Trend::Steady;
        } else {
            trend_ = Trend::Unknown;
        }

        updates_++;
        return smoothed_;
    }

    bool primed() const { return primed_; }
    float smoothed() const { return smoothed_; }
    float peak() const { return peak_; }
    bool detected() const { return detected_; }
    Trend trend() const { return trend_; }
    uint32_t updates() const { return updates_; }

    /* Level relative to the running peak — the number that tells you whether
     * this bearing is the best one seen so far. */
    float below_peak_db() const { return peak_ - smoothed_; }

   private:
    float alpha_{0.25f};
    float peak_decay_{0.05f};
    float threshold_{-60.0f};
    float hysteresis_{3.0f};
    float deadband_{1.0f};
    size_t trend_window_{8};

    bool primed_{false};
    float smoothed_{0.0f};
    float peak_{0.0f};
    bool detected_{false};
    Trend trend_{Trend::Unknown};
    uint32_t updates_{0};

    std::deque<float> history_{};
};

/* --- BearingLog ------------------------------------------------------------
 * The manual stand-in for upstream's GeoMap markers. Each mark is a compass
 * bearing the operator read off a real compass, with the smoothed level at that
 * moment. The strongest mark points at the fox. */
struct Bearing {
    uint16_t degrees{0};
    float level_db{0.0f};
};

class BearingLog {
   public:
    static constexpr size_t max_marks = 32;

    /* Bearings are taken modulo 360. */
    void add(uint16_t degrees, float level_db) {
        marks_.push_back({static_cast<uint16_t>(degrees % 360), level_db});
        while (marks_.size() > max_marks) marks_.erase(marks_.begin());
    }

    void clear() { marks_.clear(); }
    bool empty() const { return marks_.empty(); }
    size_t size() const { return marks_.size(); }
    const std::vector<Bearing>& marks() const { return marks_; }

    /* Strongest mark. Ties keep the earlier one, so a later equal reading does
     * not shift the answer. Index is max_marks when the log is empty. */
    size_t best_index() const {
        if (marks_.empty()) return max_marks;
        size_t best = 0;
        for (size_t i = 1; i < marks_.size(); i++)
            if (marks_[i].level_db > marks_[best].level_db) best = i;
        return best;
    }

    Bearing best() const {
        const auto i = best_index();
        return (i == max_marks) ? Bearing{} : marks_[i];
    }

   private:
    std::vector<Bearing> marks_{};
};

}  // namespace foxhunt

/* ======================================================================== *
 *  View                                                                     *
 * ======================================================================== */

/* Rolling level plot with the peak drawn as a horizontal marker. Private to
 * this app — doc/PORTING.md forbids adding a shared widget header here. */
class FoxhuntLevelGraph : public ui::Widget {
   public:
    explicit FoxhuntLevelGraph(ui::Rect parent_rect)
        : ui::Widget{parent_rect} {}

    void set_range(float min_db, float max_db) {
        min_db_ = min_db;
        max_db_ = max_db;
    }

    void add(float db, float peak_db) {
        const size_t columns = static_cast<size_t>(std::max(parent_rect().width(), 1));
        values_.push_back(db);
        while (values_.size() > columns) values_.pop_front();
        peak_db_ = peak_db;
        set_dirty();
    }

    void clear() {
        values_.clear();
        set_dirty();
    }

    void paint(ui::Painter& painter) override;

   private:
    std::deque<float> values_{};
    float peak_db_{0.0f};
    float min_db_{-110.0f};
    float max_db_{-10.0f};
};

class FoxhuntRxView : public ui::View {
   public:
    FoxhuntRxView();
    ~FoxhuntRxView() override;

    FoxhuntRxView(const FoxhuntRxView&) = delete;
    FoxhuntRxView& operator=(const FoxhuntRxView&) = delete;

    std::string title() const override { return "Fox hunt"; }

    void on_show() override;
    void on_frame_sync() override;

   private:
    void mark_bearing(uint16_t degrees);
    void refresh_marks();

    radio::ReceiverModel& receiver_;
    foxhunt::FoxhuntEngine engine_{};
    foxhunt::BearingLog log_{};

    uint32_t frame_counter_{0};

    /* --- widgets --- */
    ui::Labels labels_{
        {{0, 0}, "Freq", ui::Color::light_grey()},
        {{0, 20}, "Gn", ui::Color::light_grey()},
        {{80, 20}, "Vol", ui::Color::light_grey()},
        {{152, 20}, "Thr", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 0}};

    ui::NumberField field_gain_{{24, 20}, 3, {0, 76}, 1, ' '};
    ui::NumberField field_volume_{{112, 20}, 2, {0, 99}, 1, ' '};
    ui::NumberField field_threshold_{{184, 20}, 4, {-120, 10}, 1, ' '};

    ui::Text text_power_{{0, 40, 128, 16}, ""};
    ui::Text text_trend_{{132, 40, 108, 16}, ""};

    ui::VuMeter level_meter_{{0, 58, 240, 12}, 24, false};
    FoxhuntLevelGraph level_graph_{{0, 74, 240, 70}};

    ui::Text text_peak_{{0, 148, 240, 16}, ""};

    ui::Button button_mark_{{0, 168, 74, 22}, "MARK"};
    ui::Button button_clear_{{78, 168, 74, 22}, "CLEAR"};
    ui::Button button_reset_{{156, 168, 84, 22}, "RST PEAK"};

    ui::Text text_best_{{0, 194, 240, 16}, "No bearings marked"};

    ui::Console console_{{0, 212, 240, 74}};

    ui::Text text_note_{{0, 288, 240, 16}, "No GPS/compass on B200"};
};

}  // namespace app

#endif /*__MB200_UI_FOXHUNT_RX_H__*/
