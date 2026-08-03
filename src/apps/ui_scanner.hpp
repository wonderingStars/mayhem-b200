/*
 * mayhem-b200 — Scanner.
 *
 * Ported from firmware/application/external/scanner/ui_scanner.*. Scanner steps
 * through a freqman list — or, in "manual search" mode, through a min..max range
 * at a fixed step — dwelling on each frequency for one 50 ms tick, and stops
 * (pauses) when the channel level stays above squelch long enough to look like a
 * real signal rather than a spur.
 *
 * The port splits into pure logic and UI so the whole state machine can be run
 * without a radio or a screen:
 *
 *   ScannerStepper — upstream's ScannerThread, with the ChibiOS thread and its
 *                    50 ms sleep replaced by a tick() the view calls once every
 *                    SCANNER_SLEEP_MS from on_frame_sync(). The stepping maths
 *                    (list wrap, range wrap, one-shot index_stepper priority,
 *                    freq_lock inhibiting the step, deferred frequency delete)
 *                    is upstream's run() body line for line.
 *
 *   ScannerEngine  — upstream's ScannerView::on_statistics_update() plus
 *                    scan_pause/scan_resume/user_resume/update_squelch_while_paused,
 *                    driving a ScannerStepper. Time enters only as "one
 *                    statistics update", exactly as the firmware's
 *                    STATISTICS_UPDATES_PER_SEC ticks did.
 *
 *   ScannerView    — the screen. Owns a ScannerEngine, ticks the stepper every
 *                    50 ms and the engine every 100 ms from on_frame_sync(),
 *                    retunes radio::ReceiverModel and renders the readouts.
 *
 * Host deviations from upstream, each because the PortaPack hardware or firmware
 * structure it needs is absent on a B200 (doc/PORTING.md honesty rules):
 *
 *   - No M4 baseband and no scanner thread: the stepper is ticked from the UI
 *     frame callback. Upstream's 300 ms sleep inside set_scanning_direction()
 *     (a thread-only trick to let the receiver settle after a reversal) has no
 *     host equivalent and is dropped; the caller simply keeps scanning.
 *   - The three HackRF gain controls (LNA/VGA/AMP) collapse to the B200's single
 *     continuous gain, as everywhere else in this port.
 *   - Squelch is compared against radio::ReceiverModel::channel_level_db()
 *     (dBFS) rather than the HackRF's ChannelStatistics max_db, so the useful
 *     threshold range differs. The comparison itself is upstream's.
 *   - Upstream's "MIC TX" button replaced the view with MicTXView; there is no
 *     mic-transmit app in this build, so that button is not present. The "AUDIO"
 *     button is kept and reaches the audio app through the app registry.
 *   - freqman_entry_get_step_value() supplies the manual-search step, matching
 *     upstream's field_step.selected_index_value().
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2018 Furrtek
 * Copyright (C) 2023 Mark Thompson (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SCANNER_H__
#define __MB200_UI_SCANNER_H__

#include "freqman_db.hpp"

#include "ui.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace radio {
class ReceiverModel;
}

namespace app {

/* ======================================================================== *
 *  Pure Scanner logic — no UI, no radio, no clock.                          *
 * ======================================================================== */
namespace scanner {

/* Upstream's #defines, verbatim (ui_scanner.hpp). */
inline constexpr uint32_t kScannerSleepMs = 50;            /* SCANNER_SLEEP_MS */
inline constexpr uint32_t kStatisticsUpdatesPerSec = 10;   /* STATISTICS_UPDATES_PER_SEC */
inline constexpr uint32_t kMaxFreqLock = 10;               /* MAX_FREQ_LOCK */

/* Upstream's scanner_entry_t. */
struct ScannerEntry {
    int64_t freq{0};
    std::string description{};
};

/* Upstream's scanner_range_t. */
struct ScannerRange {
    int64_t min{0};
    int64_t max{0};
};

/* Upstream's bigdisplay_color_type, plus Keep for its "-1 = refresh but do not
 * change the colour" argument. */
enum class BigDisplayColor : int8_t {
    Keep = -1,
    Grey = 0,
    Yellow = 1,
    Green = 2,
    Red = 3,
};

/* --- ScannerStepper --------------------------------------------------------
 * One tick() is one pass of upstream's ScannerThread::run() loop body. The
 * thread slept SCANNER_SLEEP_MS between passes; the view calls tick() at that
 * cadence instead.
 *
 * Both upstream modes are here:
 *   list   — walk a vector of frequencies, wrapping at both ends.
 *   range  — walk (max - min) / step positions from min, wrapping likewise.
 *
 * Upstream reads _stepper inside run() on the freshly-created thread while the
 * view is concurrently calling set_scanning_direction(), so which end the first
 * pass starts from is a race. Here the direction is a start_*() argument, which
 * removes the race without changing the intended behaviour: forward starts the
 * index at `size` so the first step wraps to 0, reverse starts at 0 so the first
 * step wraps to size-1. */
class ScannerStepper {
   public:
    /* What one pass produced. `emitted` mirrors upstream sending a
     * RetuneMessage; `retuned` mirrors it having called
     * receiver_model.set_target_frequency() first. */
    struct Tick {
        bool emitted{false};
        bool retuned{false};
        int64_t freq{0};
        uint32_t index{0};  /* RetuneMessage::range */
    };

    void start_list(std::vector<int64_t> freqs, bool fwd) {
        frequency_list_ = std::move(freqs);
        range_ = {};
        step_hz_ = 0;
        manual_ = false;
        size_ = static_cast<int32_t>(frequency_list_.size());
        stepper_ = fwd ? 1 : -1;
        index_ = (stepper_ > 0) ? size_ : 0;
        index_stepper_ = 0;
        freq_lock_ = 0;
        freq_del_ = 0;
        scanning_ = true;
        running_ = (size_ > 0);
    }

    void start_range(const ScannerRange& range, int64_t step_hz, bool fwd) {
        frequency_list_.clear();
        range_ = range;
        step_hz_ = step_hz;
        manual_ = true;
        size_ = (step_hz_ > 0)
                    ? static_cast<int32_t>((range_.max - range_.min) / step_hz_)
                    : 0;
        stepper_ = fwd ? 1 : -1;
        index_ = (stepper_ > 0) ? size_ : 0;
        index_stepper_ = 0;
        freq_lock_ = 0;
        freq_del_ = 0;
        scanning_ = true;
        /* Upstream: `else if (_manual_search && (def_step_hz_ > 0))`. */
        running_ = (step_hz_ > 0);
    }

    void stop() { running_ = false; }
    bool running() const { return running_; }
    bool manual_search() const { return manual_; }

    int32_t size() const { return size_; }
    int32_t index() const { return index_; }
    int64_t step_hz() const { return step_hz_; }
    const ScannerRange& range() const { return range_; }
    const std::vector<int64_t>& frequencies() const { return frequency_list_; }

    /* Frequency for a position. Clamped: after a delete the cursor can sit one
     * past the end, where upstream would read off the end of its vector. */
    int64_t frequency_at(int32_t index) const {
        if (manual_) {
            const int32_t i = std::clamp(index, 0, std::max(size_, 0));
            return range_.min + static_cast<int64_t>(i) * step_hz_;
        }
        if (frequency_list_.empty()) return 0;
        const int32_t i =
            std::clamp(index, 0, static_cast<int32_t>(frequency_list_.size()) - 1);
        return frequency_list_[static_cast<size_t>(i)];
    }

    int64_t frequency() const { return frequency_at(index_); }

    /* --- upstream's ScannerThread public API --- */
    void set_scanning(bool v) { scanning_ = v; }
    bool is_scanning() const { return scanning_; }

    void set_freq_lock(uint32_t v) { freq_lock_ = v; }
    uint32_t is_freq_lock() const { return freq_lock_; }

    /* Deferred delete. Upstream only services it while the scan is paused, and
     * only in list mode. */
    void set_freq_del(int64_t v) { freq_del_ = v; }
    int64_t freq_del() const { return freq_del_; }

    /* One-shot index nudge; takes priority over the scan direction. */
    void set_index_stepper(int32_t v) { index_stepper_ = v; }
    int32_t index_stepper() const { return index_stepper_; }

    /* Upstream also slept 300 ms here to let the receiver settle after a
     * reversal. There is no thread to sleep on the host; see the file header. */
    void set_scanning_direction(bool fwd) { stepper_ = fwd ? 1 : -1; }
    bool forward() const { return stepper_ > 0; }

    /* One pass of ScannerThread::run(). */
    Tick tick() {
        Tick t{};
        if (!running_) return t;

        const bool force_one_step = (index_stepper_ != 0);
        const int32_t step = force_one_step ? index_stepper_ : stepper_;

        if (scanning_ || force_one_step) {
            if ((freq_lock_ == 0) || force_one_step) {
                index_ += step;
                if (index_ >= size_)
                    index_ = 0;
                else if (index_ < 0)
                    index_ = (size_ > 0) ? size_ - 1 : 0;

                if (force_one_step) index_stepper_ = 0;

                t.retuned = true;  /* receiver_model.set_target_frequency() */
            }
            t.emitted = true;
            t.freq = frequency_at(index_);
            /* Upstream sends the list index in list mode and 0 in range mode. */
            t.index = manual_ ? 0u : static_cast<uint32_t>(std::max(index_, 0));
        } else if (!manual_ && freq_del_ != 0) {
            for (int32_t i = 0; i < size_; i++) {
                if (frequency_list_[static_cast<size_t>(i)] == freq_del_) {
                    frequency_list_.erase(frequency_list_.begin() + i);
                    size_ = static_cast<int32_t>(frequency_list_.size());
                    break;
                }
            }
            freq_del_ = 0;
            running_ = (size_ > 0);
        }

        return t;
    }

   private:
    std::vector<int64_t> frequency_list_{};
    ScannerRange range_{0, 0};
    int64_t step_hz_{0};

    bool manual_{false};
    bool running_{false};
    bool scanning_{true};

    int32_t size_{0};
    int32_t index_{0};
    int32_t stepper_{1};
    int32_t index_stepper_{0};
    uint32_t freq_lock_{0};
    int64_t freq_del_{0};
};

/* --- ScannerEngine ---------------------------------------------------------
 * ScannerView's squelch/dwell state machine, lifted out of the UI. The caller
 * feeds it one channel level per statistics update (10 per second upstream) and
 * reads back the audio-gate state and the big-display colour instead of the
 * firmware's audio::output::start()/stop() and big_display.set_style(). */
class ScannerEngine {
   public:
    ScannerStepper& stepper() { return stepper_; }
    const ScannerStepper& stepper() const { return stepper_; }

    void set_squelch(int32_t db) { squelch_ = db; }
    int32_t squelch() const { return squelch_; }

    void set_browse_wait(uint32_t seconds) { browse_wait_ = seconds; }
    uint32_t browse_wait() const { return browse_wait_; }

    void set_lock_wait(uint32_t seconds) { lock_wait_ = seconds; }
    uint32_t lock_wait() const { return lock_wait_; }

    bool userpause() const { return userpause_; }

    uint32_t browse_timer() const { return browse_timer_; }
    uint32_t lock_timer() const { return lock_timer_; }
    uint32_t color_timer() const { return color_timer_; }

    /* Mirrors audio::output::start()/stop() — true means "audio audible". */
    bool audio_enabled() const { return audio_enabled_; }
    BigDisplayColor color() const { return color_; }

    /* Upstream ScannerView::scan_pause(). */
    void scan_pause() {
        if (stepper_.running() && stepper_.is_scanning()) {
            stepper_.set_freq_lock(0);
            stepper_.set_scanning(false);
        }
        audio_enabled_ = true;
    }

    /* Upstream ScannerView::scan_resume(). */
    void scan_resume() {
        audio_enabled_ = false;
        color_ = BigDisplayColor::Grey;
        if (stepper_.running()) {
            stepper_.set_index_stepper(stepper_.forward() ? 1 : -1);
            stepper_.set_scanning(true);
        }
    }

    /* Upstream ScannerView::user_resume(): arm browse_timer past its limit so
     * the next statistics update resumes and advances a frequency. */
    void user_resume() {
        browse_timer_ = browse_wait_ * kStatisticsUpdatesPerSec + 1;
        userpause_ = false;
    }

    /* Upstream's button_pause handler. */
    void user_pause() {
        scan_pause();
        userpause_ = true;
    }

    /* Upstream ScannerView::handle_retune()'s colour selection. Keep means
     * "freq lock is mid-check, do not touch the display". */
    BigDisplayColor retune_color() const {
        switch (stepper_.is_freq_lock()) {
            case 0:
                return BigDisplayColor::Grey;
            case 1:
                return BigDisplayColor::Yellow;
            case kMaxFreqLock:
                return BigDisplayColor::Green;
            default:
                return BigDisplayColor::Keep;
        }
    }

    void set_color(BigDisplayColor c) {
        if (c != BigDisplayColor::Keep) color_ = c;
    }

    /* Upstream ScannerView::on_statistics_update(). One call == one statistics
     * message == 1/STATISTICS_UPDATES_PER_SEC of a second. */
    void on_statistics_update(int32_t max_db) {
        if (userpause_) {
            update_squelch_while_paused(max_db);
            return;
        }
        if (!stepper_.running()) return;

        /* Resume regardless of signal strength once browse time is reached. */
        if ((browse_wait_ != 0) &&
            (browse_timer_ >= browse_wait_ * kStatisticsUpdatesPerSec)) {
            browse_timer_ = 0;
            scan_resume();
            return;
        }

        if (max_db > squelch_) {
            /* Something on the air. */
            if (stepper_.is_freq_lock() >= kMaxFreqLock) {
                if (!browse_timer_) scan_pause();
                browse_timer_++;
                update_squelch_while_paused(max_db);
            } else {
                stepper_.set_freq_lock(stepper_.is_freq_lock() + 1);
                if (browse_timer_) browse_timer_++;
            }
            lock_timer_ = 0;
        } else {
            /* Nothing on the air. */
            if (!browse_timer_) {
                if (stepper_.is_freq_lock() > 0) {
                    color_ = BigDisplayColor::Grey;
                    stepper_.set_freq_lock(0);
                }
            } else {
                lock_timer_++;
                if (lock_timer_ >= lock_wait_ * kStatisticsUpdatesPerSec) {
                    browse_timer_ = 0;
                    scan_resume();
                } else {
                    browse_timer_++;
                    update_squelch_while_paused(max_db);
                }
            }
        }
    }

    /* Upstream ScannerView::handle_encoder(): "Restart browse timer when
     * frequency changes". Note it restarts to 1, not 0 — a zero browse_timer
     * means "the scan was never paused" to on_statistics_update(). */
    void restart_browse_timer() {
        if (browse_timer_ != 0) browse_timer_ = 1;
    }

    /* Test/UI hook: reset the dwell timers, e.g. after reloading a list. */
    void reset_timers() {
        browse_timer_ = 0;
        lock_timer_ = 0;
        color_timer_ = 0;
    }

   private:
    /* Upstream ScannerView::update_squelch_while_paused(). The color_timer
     * counter is what stops a marginal signal from strobing the display. */
    void update_squelch_while_paused(int32_t max_db) {
        if (++color_timer_ > 2) {
            if (max_db > squelch_) {
                audio_enabled_ = true;
                color_ = BigDisplayColor::Green;
            } else {
                audio_enabled_ = false;
                color_ = BigDisplayColor::Grey;
            }
            color_timer_ = 0;
        }
    }

    ScannerStepper stepper_{};

    int32_t squelch_{-30};      /* upstream default */
    uint32_t browse_wait_{5};   /* seconds */
    uint32_t lock_wait_{2};     /* seconds */

    uint32_t browse_timer_{0};
    uint32_t lock_timer_{0};
    uint32_t color_timer_{0};

    bool userpause_{false};
    bool audio_enabled_{false};
    BigDisplayColor color_{BigDisplayColor::Grey};
};

}  // namespace scanner

/* ======================================================================== *
 *  View                                                                     *
 * ======================================================================== */

class ScannerView : public ui::View {
   public:
    ScannerView();
    ~ScannerView() override;

    ScannerView(const ScannerView&) = delete;
    ScannerView& operator=(const ScannerView&) = delete;

    std::string title() const override { return "Scanner"; }

    void on_show() override;
    void on_frame_sync() override;

   private:
    void frequency_file_load(const std::string& stem);
    void restart_scan();
    void start_scan();
    void show_max_index();
    void handle_retune(int64_t freq, uint32_t index);
    void handle_encoder(int32_t delta);
    void apply_color();
    void apply_mode(int32_t modulation);
    void update_bandwidth_options(int32_t modulation);
    void add_current_frequency();
    void remove_current_frequency();
    std::string loaded_filename() const;

    radio::ReceiverModel& receiver_;
    scanner::ScannerEngine engine_{};

    std::vector<scanner::ScannerEntry> entries_{};
    scanner::ScannerRange frequency_range_{0, 0};
    std::string freqman_file_{"SCANNER"};

    bool manual_search_{false};
    bool fwd_{true};

    uint32_t current_index_{0};
    int64_t current_frequency_{0};

    /* Frame pacing: the stepper runs at SCANNER_SLEEP_MS, the statistics
     * machine at 1/STATISTICS_UPDATES_PER_SEC. */
    std::chrono::steady_clock::time_point last_step_{};
    std::chrono::steady_clock::time_point last_stats_{};
    bool timing_started_{false};

    scanner::BigDisplayColor shown_color_{scanner::BigDisplayColor::Keep};
    int64_t shown_frequency_{-1};
    bool shown_audio_{false};

    /* --- widgets --- */
    ui::Labels labels_{
        {{0, 0}, "Gn", ui::Color::light_grey()},
        {{72, 0}, "Vol", ui::Color::light_grey()},
        {{144, 0}, "Sq", ui::Color::light_grey()},
        {{0, 18}, "Wsa", ui::Color::light_grey()},
        {{80, 18}, "Wsl", ui::Color::light_grey()},
        {{152, 18}, "Md", ui::Color::light_grey()},
        {{0, 140}, "St", ui::Color::light_grey()},
        {{0, 158}, "SRCH START", ui::Color::light_grey()},
        {{86, 158}, "SRCH END", ui::Color::light_grey()},
    };

    ui::NumberField field_gain_{{24, 0}, 3, {0, 76}, 1, ' '};
    ui::NumberField field_volume_{{104, 0}, 2, {0, 99}, 1, ' '};
    ui::NumberField field_squelch_{{168, 0}, 4, {-120, 20}, 1, ' '};

    ui::NumberField field_browse_wait_{{32, 18}, 2, {0, 99}, 1, ' '};
    ui::NumberField field_lock_wait_{{112, 18}, 2, {0, 99}, 1, ' '};
    ui::OptionsField field_mode_{{176, 18}, 4, {}};
    ui::OptionsField field_bw_{{0, 36}, 6, {}};

    ui::VuMeter level_meter_{{56, 38, 108, 12}, 20, false};
    ui::Text text_level_{{168, 36, 72, 16}, ""};

    ui::TextField field_current_index_{{0, 56, 24, 16}, ""};
    ui::Text text_max_index_{{28, 56, 116, 16}, ""};
    ui::Button button_load_{{148, 54, 44, 20}, "LOAD"};
    ui::Button button_clear_{{196, 54, 44, 20}, "MCLR"};

    ui::Text text_current_desc_{{0, 74, 240, 16}, ""};

    ui::BigFrequency big_display_{{0, 92, 240, 44}};

    ui::OptionsField field_step_{{24, 140}, 9, {}};
    ui::Button button_manual_search_{{156, 138, 84, 20}, "SRCH"};

    ui::Button button_manual_start_{{0, 174, 84, 22}, ""};
    ui::Button button_manual_end_{{86, 174, 84, 22}, ""};
    ui::Button button_audio_{{172, 174, 68, 22}, "AUDIO"};

    ui::ButtonWithEncoder button_pause_{{0, 200, 84, 22}, "<PAUSE>"};
    ui::Button button_dir_{{86, 200, 84, 22}, "REVERSE"};
    ui::Button button_add_{{172, 200, 68, 22}, "ADD FQ"};

    ui::Button button_remove_{{0, 226, 84, 22}, "DEL FQ"};
    ui::Text text_status_{{86, 226, 154, 16}, ""};
};

}  // namespace app

#endif /*__MB200_UI_SCANNER_H__*/
