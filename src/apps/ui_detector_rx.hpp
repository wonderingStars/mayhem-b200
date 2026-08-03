/*
 * mayhem-b200 — Detector RX (near-field activity detector).
 *
 * Ported from firmware/application/external/detector_rx/ui_detector_rx.*.
 * Detector is a "sniffer": it parks a wide (DETECTOR_BW = 750 kHz) capture
 * channel on a frequency, shows the channel power and an RSSI history graph, and
 * beeps at a pitch proportional to the level whenever it exceeds a user-set
 * "bip" squelch. With AUTOSCAN on it walks a freqman entry — stepping a Range in
 * DETECTOR_BW-sized (or the entry's own step) hops and wrapping at the top — and
 * with AUTOADV on it rolls on to the next entry in the list.
 *
 * The port splits into pure logic and UI:
 *
 *   DetectorScanner — upstream's init_current_entry(), on_timer() and the two
 *                     encoder handlers, over a core::freqman_db. No radio, no
 *                     clock: one on_timer() call is one display frame, exactly as
 *                     upstream drove it from DisplayFrameSync.
 *
 *   map_range()/beep_frequency() — upstream's map() and its
 *                     request_audio_beep(map(max_db, -100, 20, 400, 2600), ...)
 *                     pitch mapping, kept as free functions so the mapping is
 *                     testable.
 *
 *   DetectorRxView  — the screen. Drives the scanner from on_frame_sync(),
 *                     retunes radio::ReceiverModel, renders power/RSSI, and
 *                     synthesises the beep directly into audio::AudioOut.
 *
 * Host deviations from upstream (doc/PORTING.md honesty rules):
 *
 *   - The firmware's `baseband::request_audio_beep(freq, 24000, 150)` reached an
 *     M4 tone generator. There is no M4 and no beep service here, so the view
 *     synthesises the same 150 ms tone at the mapped pitch and writes it to the
 *     host audio device. The pitch mapping is upstream's, unchanged.
 *   - Upstream's RSSI widget reads the HackRF's hardware RSSI thread and reports
 *     min/avg/max bytes. The B200 has no equivalent readout, so the graph is fed
 *     from radio::ReceiverModel::channel_level_db() (dBFS) and min/avg/max are
 *     computed here over the same history window.
 *   - Capture-mode oversampling (`get_oversample_rate`, `filter_bandwidth_for_
 *     sampling_rate`) has no host equivalent; the receiver is put into
 *     SpectrumAnalysis mode at DETECTOR_BW, which is the same "no demodulation,
 *     just measure the channel" arrangement.
 *   - The three HackRF gain controls collapse to the B200's single gain.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2018 Furrtek
 * Copyright (C) 2023 gullradriel, Nilorea Studio Inc. (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_DETECTOR_RX_H__
#define __MB200_UI_DETECTOR_RX_H__

#include "freqman_db.hpp"
#include "string_format.hpp"

#include "ui.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace radio {
class ReceiverModel;
}

namespace app {

/* ======================================================================== *
 *  Pure Detector logic — no UI, no radio, no clock.                         *
 * ======================================================================== */
namespace detector_rx {

/* ui_detector_rx.hpp: #define DETECTOR_BW 750000 */
inline constexpr int32_t kDetectorBw = 750'000;

/* Upstream DetectorRxView::map() — the Arduino map(), integer arithmetic and
 * all. A zero-width input range would divide by zero on the host where the ARM
 * build merely produced nonsense, so that case returns toLow. */
inline int32_t map_range(int32_t value, int32_t from_low, int32_t from_high,
                         int32_t to_low, int32_t to_high) {
    if (from_high == from_low) return to_low;
    return to_low + (value - from_low) * (to_high - to_low) / (from_high - from_low);
}

/* Upstream's beep pitch: map(statistics.max_db, -100, 20, 400, 2600). */
inline int32_t beep_frequency(int32_t max_db) {
    return map_range(max_db, -100, 20, 400, 2600);
}

/* Upstream's beep call was request_audio_beep(pitch, 24000, 150). */
inline constexpr uint32_t kBeepDurationMs = 150;

/* Upstream DetectorRxView::format_freq_mhz(). Header-only so the whole pure
 * layer can be tested without linking the view. */
inline std::string format_freq_mhz(int64_t freq_hz) {
    const int64_t mhz = freq_hz / 1'000'000;
    const int64_t khz_frac = (freq_hz % 1'000'000) / 1000;
    return "< " + to_string_dec_uint(static_cast<uint64_t>(mhz)) + "." +
           to_string_dec_uint(static_cast<uint64_t>(khz_frac), 3, '0') + " MHz >";
}

/* --- DetectorScanner -------------------------------------------------------
 * The frequency cursor: which list entry, where inside it, and how it advances.
 * One on_timer() is one display frame, as upstream. */
class DetectorScanner {
   public:
    /* Takes ownership of a loaded list and initialises the cursor on entry 0.
     * Returns false for an empty list (upstream's "No file!" path). */
    bool set_list(core::freqman_db list) {
        list_ = std::move(list);
        current_index_ = 0;
        if (list_.empty()) {
            current_freq_ = 0;
            minfreq_ = 0;
            maxfreq_ = 0;
            current_step_ = kDetectorBw;
            return false;
        }
        init_current_entry();
        return true;
    }

    const core::freqman_db& list() const { return list_; }
    size_t size() const { return list_.size(); }
    bool empty() const { return list_.empty(); }

    size_t current_index() const { return current_index_; }
    int64_t frequency() const { return current_freq_; }
    int64_t min_frequency() const { return minfreq_; }
    int64_t max_frequency() const { return maxfreq_; }
    int32_t step() const { return current_step_; }

    bool auto_scan() const { return auto_scan_; }
    void set_auto_scan(bool v) { auto_scan_ = v; }

    bool auto_advance() const { return auto_advance_; }
    void set_auto_advance(bool v) { auto_advance_ = v; }

    bool current_is_range() const {
        return !list_.empty() &&
               list_[current_index_]->type == core::freqman_type::Range;
    }

    const core::freqman_entry& current_entry() const { return *list_[current_index_]; }

    /* Upstream DetectorRxView::init_current_entry(). */
    void init_current_entry() {
        if (list_.empty()) return;

        const auto& entry = *list_[current_index_];
        if (entry.type == core::freqman_type::Range) {
            minfreq_ = entry.frequency_a;
            maxfreq_ = entry.frequency_b;
            current_freq_ = minfreq_;
            const int32_t sv = core::freqman_entry_get_step_value(entry.step);
            current_step_ = core::is_valid(entry.step) && sv > 0 ? sv : kDetectorBw;
        } else {
            current_freq_ = entry.frequency_a;
            minfreq_ = current_freq_;
            maxfreq_ = current_freq_;
            current_step_ = kDetectorBw;
        }
    }

    /* Upstream DetectorRxView::on_timer(). Returns true when the caller should
     * retune (upstream called receiver_model.set_target_frequency()). */
    bool on_timer() {
        if (list_.empty() || !auto_scan_) return false;

        if (list_[current_index_]->type == core::freqman_type::Range) {
            current_freq_ += current_step_;
            if (current_freq_ > maxfreq_) {
                if (auto_advance_) {
                    current_index_ = (current_index_ + 1) % list_.size();
                    init_current_entry();
                    return true;
                }
                current_freq_ = minfreq_;
            }
            return true;
        }

        if (auto_advance_) {
            current_index_ = (current_index_ + 1) % list_.size();
            init_current_entry();
            return true;
        }
        return false;
    }

    /* Upstream's button_index encoder: signed wraparound over the list. */
    bool step_index(int32_t delta) {
        if (list_.empty() || delta == 0) return false;
        const int32_t n = static_cast<int32_t>(list_.size());
        int32_t idx = static_cast<int32_t>(current_index_);
        idx = (delta > 0) ? (idx + 1) % n : (idx - 1 + n) % n;
        current_index_ = static_cast<size_t>(idx);
        init_current_entry();
        return true;
    }

    /* Upstream's button_freq encoder: only meaningful inside a Range. */
    bool step_frequency(int32_t delta) {
        if (list_.empty() || delta == 0) return false;
        if (list_[current_index_]->type != core::freqman_type::Range) return false;

        if (delta > 0) {
            current_freq_ += current_step_;
            if (current_freq_ > maxfreq_) current_freq_ = minfreq_;
        } else {
            current_freq_ -= current_step_;
            if (current_freq_ < minfreq_) current_freq_ = maxfreq_;
        }
        return true;
    }

   private:
    core::freqman_db list_{};
    size_t current_index_{0};
    int64_t current_freq_{0};
    int64_t minfreq_{0};
    int64_t maxfreq_{0};
    int32_t current_step_{kDetectorBw};
    bool auto_scan_{true};
    bool auto_advance_{false};
};

/* --- LevelHistory ----------------------------------------------------------
 * The min/avg/max the firmware's RSSI widget reported from its hardware RSSI
 * thread. The B200 has no such readout, so the same three numbers are derived
 * here from a bounded history of channel levels (dBFS). */
class LevelHistory {
   public:
    explicit LevelHistory(size_t capacity = 256)
        : capacity_{capacity == 0 ? size_t{1} : capacity} {}

    void clear() { values_.clear(); }
    void add(float db) {
        values_.push_back(db);
        while (values_.size() > capacity_) values_.pop_front();
    }

    bool empty() const { return values_.empty(); }
    size_t size() const { return values_.size(); }
    size_t capacity() const { return capacity_; }
    const std::deque<float>& values() const { return values_; }

    float min() const {
        if (values_.empty()) return 0.0f;
        return *std::min_element(values_.begin(), values_.end());
    }
    float max() const {
        if (values_.empty()) return 0.0f;
        return *std::max_element(values_.begin(), values_.end());
    }
    float avg() const {
        if (values_.empty()) return 0.0f;
        double sum = 0.0;
        for (float v : values_) sum += v;
        return static_cast<float>(sum / static_cast<double>(values_.size()));
    }

   private:
    std::deque<float> values_{};
    size_t capacity_;
};

}  // namespace detector_rx

/* ======================================================================== *
 *  View                                                                     *
 * ======================================================================== */

/* Rolling level plot. Kept private to this app rather than added to the shared
 * widget set (doc/PORTING.md: no new shared files while others are writing). */
class DetectorLevelGraph : public ui::Widget {
   public:
    explicit DetectorLevelGraph(ui::Rect parent_rect)
        : ui::Widget{parent_rect} {}

    void set_range(float min_db, float max_db) {
        min_db_ = min_db;
        max_db_ = max_db;
    }

    void add(float db) {
        const size_t columns = static_cast<size_t>(std::max(parent_rect().width(), 1));
        values_.push_back(db);
        while (values_.size() > columns) values_.pop_front();
        set_dirty();
    }

    void clear() {
        values_.clear();
        set_dirty();
    }

    void paint(ui::Painter& painter) override;

   private:
    std::deque<float> values_{};
    float min_db_{-100.0f};
    float max_db_{-10.0f};
};

class DetectorRxView : public ui::View {
   public:
    DetectorRxView();
    ~DetectorRxView() override;

    DetectorRxView(const DetectorRxView&) = delete;
    DetectorRxView& operator=(const DetectorRxView&) = delete;

    std::string title() const override { return "Detector RX"; }

    void on_show() override;
    void on_frame_sync() override;

   private:
    void load_freqman(const std::string& stem);
    void update_entry_display();
    void update_freq_display();
    void retune();
    void beep(int32_t max_db);

    radio::ReceiverModel& receiver_;
    detector_rx::DetectorScanner scanner_{};
    detector_rx::LevelHistory history_{256};

    std::string freq_file_stem_{"DETECTOR"};
    int32_t beep_squelch_{0};

    int8_t last_freq_display_kind_{-1};
    int32_t last_shown_db_{1000};
    uint32_t frame_counter_{0};
    uint32_t beep_cooldown_frames_{0};
    std::vector<float> beep_buffer_{};

    /* --- widgets --- */
    ui::Labels labels_{
        {{0, 0}, "Gn", ui::Color::light_grey()},
        {{80, 0}, "Bip>", ui::Color::light_grey()},
    };

    ui::NumberField field_gain_{{24, 0}, 3, {0, 76}, 1, ' '};
    ui::NumberField field_beep_squelch_{{120, 0}, 4, {-120, 20}, 1, ' '};

    ui::Button button_file_{{0, 20, 160, 22}, "DETECTOR"};
    ui::Button button_auto_advance_{{164, 20, 76, 22}, "NO ADV"};

    ui::ButtonWithEncoder button_index_{{0, 46, 44, 22}, ""};
    ui::Text text_entry_desc_{{48, 48, 112, 16}, ""};
    ui::Button button_auto_scan_{{164, 46, 76, 22}, "AUTOSCAN"};

    ui::ButtonWithEncoder button_freq_{{0, 72, 240, 22}, ""};

    ui::Text text_power_{{0, 98, 120, 16}, ""};
    ui::Text text_rssi_{{120, 98, 120, 16}, ""};

    ui::VuMeter level_meter_{{0, 116, 240, 12}, 24, false};
    DetectorLevelGraph level_graph_{{0, 134, 240, 150}};
};

}  // namespace app

#endif /*__MB200_UI_DETECTOR_RX_H__*/
