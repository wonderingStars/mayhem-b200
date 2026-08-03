/*
 * mayhem-b200 — Recon.
 *
 * Ported from firmware/application/apps/ui_recon.* and ui_recon_settings.*.
 * Recon sweeps a frequency range or steps through a freqman list, dwelling on
 * each frequency long enough to tell whether it is active, and stopping
 * (locking) when a signal is present. It is the PortaPack's search/scanner app.
 *
 * The port splits into two pieces so the state machine can be tested without a
 * radio or a screen:
 *
 *   ReconEngine  — the pure logic: frequency stepping across ranges and lists
 *                  (with end wrap and !continuous stop), the lock/dwell/hold
 *                  timing state machine, and a lock-out set of frequencies to
 *                  skip. It depends only on core/freqman_db.hpp and is header-
 *                  only so the tests link against it directly. Time and signal
 *                  level are injected, exactly the way the firmware fed it a
 *                  ChannelStatistics message every STATS_UPDATE_INTERVAL ms.
 *
 *   ReconView    — the UI. It owns a ReconEngine, drives it once every
 *                  ~STATS_UPDATE_INTERVAL from on_frame_sync() using the
 *                  receiver's channel level, retunes the radio when the engine
 *                  moves, and renders the readouts.
 *
 * Host deviations from upstream, each because the PortaPack hardware it needs
 * is not on a B200 (see doc/PORTING.md honesty rules):
 *
 *   - No IQ record / replay "repeater" TX-back feature. That path drove a
 *     RecordView + ReplayThread over the M4 baseband; it is omitted and the
 *     setup screen says so rather than faking a transmit.
 *   - The three HackRF gain controls (LNA/VGA/AMP) collapse to the B200's one
 *     continuous gain, as everywhere else in this port.
 *   - "Remove current" in Recon mode adds the frequency to a lock-out set
 *     (skip it for the rest of the session) instead of erasing it from the
 *     loaded list. It is non-destructive to the file and is the standard
 *     scanner "lock-out" behaviour the task asks for; Scanner/Manual still
 *     delete from the output file, as upstream does.
 *   - Squelch is compared against the receiver's channel level in dBFS rather
 *     than the HackRF max_db scale, so the useful threshold range differs; the
 *     exact value is RF-dependent and unverified without a radio.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2018 Furrtek
 * Copyright (C) 2023 gullradriel, Nilorea Studio Inc. (original app design)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_RECON_H__
#define __MB200_UI_RECON_H__

#include "freqman_db.hpp"

#include "ui.hpp"
#include "ui_navigation.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace app {

/* --- Recon tuning/timing constants (firmware ui_recon_settings.hpp) --------- */

inline constexpr int32_t kReconMatchContinuous = 0;
inline constexpr int32_t kReconMatchSparse = 1;

/* The cadence at which the engine is stepped. The firmware got a statistics
 * message this often; the host polls the channel level this often. */
inline constexpr int32_t kStatsUpdateIntervalMs = 100;

inline constexpr int32_t kReconMaxLockDuration = 9900;
inline constexpr int32_t kReconDefSquelch = -14;
inline constexpr uint32_t kReconDefNbMatch = 3;
inline constexpr uint32_t kReconMinLockDuration = 100;   /* multiple of interval */
inline constexpr int32_t kReconDefWaitDuration = 1000;

/* Modulation indices — line up with radio::ReceiverModel::Mode 0..3 and with
 * freqman's m= values (AM/NFM/WFM/SPEC). */
inline constexpr int32_t kModAM = 0;
inline constexpr int32_t kModNFM = 1;
inline constexpr int32_t kModWFM = 2;
inline constexpr int32_t kModSPEC = 3;

/* Scan status, matching the firmware's `status` int8_t. */
enum class ReconStatus : int8_t {
    Idle = -1,
    Searching = 0,
    Locking = 1,
    Locked = 2,
};

/* --- recon_step_range ------------------------------------------------------
 * The elementary case the tests hammer: one continuous range that loops at its
 * ends. Advancing past `max_freq` wraps to `min_freq` (and reports it); past
 * `min_freq` going down wraps to `max_freq`. This is exactly what ReconEngine
 * does inside a single Range entry with continuous looping, factored out so it
 * can be checked in isolation. */
struct RangeStep {
    int64_t freq;
    bool wrapped;
};

inline RangeStep recon_step_range(int64_t freq, int64_t min_freq, int64_t max_freq,
                                  int64_t step, bool fwd) {
    RangeStep r{freq, false};
    if (max_freq < min_freq) {
        /* Degenerate input: nothing sensible to step through. */
        r.freq = min_freq;
        r.wrapped = true;
        return r;
    }
    if (fwd) {
        r.freq = freq + step;
        if (r.freq > max_freq) {
            r.freq = min_freq;
            r.wrapped = true;
        }
    } else {
        r.freq = freq - step;
        if (r.freq < min_freq) {
            r.freq = max_freq;
            r.wrapped = true;
        }
    }
    return r;
}

/* --- LockoutSet ------------------------------------------------------------
 * A set of frequencies (Hz) the engine skips while scanning. */
class LockoutSet {
   public:
    void add(int64_t hz) { freqs_.insert(hz); }
    bool remove(int64_t hz) { return freqs_.erase(hz) > 0; }
    void clear() { freqs_.clear(); }
    bool contains(int64_t hz) const { return freqs_.count(hz) > 0; }
    size_t size() const { return freqs_.size(); }
    bool empty() const { return freqs_.empty(); }

   private:
    std::set<int64_t> freqs_{};
};

/* --- ReconEngine -----------------------------------------------------------
 * Pure recon state machine. No radio, no UI, no clock — the caller injects the
 * channel level and the elapsed time on every process() call. */
class ReconEngine {
   public:
    /* --- list --- */
    core::freqman_db& list() { return list_; }
    const core::freqman_db& list() const { return list_; }

    /* Takes ownership of a loaded list and resets to its first entry. */
    void set_list(core::freqman_db list) {
        list_ = std::move(list);
        reset_indexes();
    }

    size_t size() const { return list_.size(); }
    bool has_entries() const { return !list_.empty(); }
    bool current_is_valid() const {
        return current_index_ >= 0 &&
               static_cast<size_t>(current_index_) < list_.size();
    }
    const core::freqman_entry& current_entry() const {
        return *list_[static_cast<size_t>(current_index_)];
    }

    /* --- configuration --- */
    void set_squelch(int32_t db) { squelch_ = db; }
    int32_t squelch() const { return squelch_; }

    void set_wait(int32_t ms) {
        wait_ = ms;
        /* The firmware turns -100 into -200: at -100 the hold logic and the
         * interval decrement fight and freeze the scan. Kept faithfully. */
        if (wait_ == -100) wait_ = -200;
    }
    int32_t wait() const { return wait_; }

    void set_lock_nb_match(uint32_t n) {
        lock_nb_match_ = n == 0 ? 1 : n;
        if (freq_lock_ > lock_nb_match_) freq_lock_ = lock_nb_match_;
    }
    uint32_t lock_nb_match() const { return lock_nb_match_; }

    void set_lock_duration(uint32_t ms) { lock_duration_ = ms; }
    uint32_t lock_duration() const { return lock_duration_; }

    void set_match_mode(int32_t mode) { match_mode_ = mode; }
    int32_t match_mode() const { return match_mode_; }

    void set_continuous(bool v) { continuous_ = v; }
    bool continuous() const { return continuous_; }

    void set_forward(bool v) { fwd_ = v; }
    bool forward() const { return fwd_; }

    /* Default step in Hz used for ranges whose entry carries no step. */
    void set_default_step(int32_t hz) { def_step_hz_ = hz; }
    int32_t default_step() const { return def_step_hz_; }

    /* --- readable state --- */
    int64_t freq() const { return freq_; }
    int64_t min_freq() const { return minfreq_; }
    int64_t max_freq() const { return maxfreq_; }
    int32_t step() const { return step_; }
    int32_t current_index() const { return current_index_; }
    ReconStatus status() const { return status_; }
    uint32_t freq_lock() const { return freq_lock_; }
    int32_t timer() const { return timer_; }
    bool recon() const { return recon_; }
    bool has_looped() const { return has_looped_; }

    /* --- control --- */
    void reset_indexes() {
        current_index_ = 0;
        status_ = ReconStatus::Idle;
        freq_lock_ = 0;
        timer_ = 0;
        stepper_ = 0;
        index_stepper_ = 0;
        has_looped_ = false;
    }

    /* Loads freq/min/max/step from the current entry — the firmware's
     * frequency_file_load tail. Call after set_list(). */
    void prepare_start() {
        reset_indexes();
        last_freq_ = -1;  /* force the first retune */
        if (!has_entries()) {
            freq_ = 0;
            return;
        }
        const auto& e = current_entry();
        int32_t sv = core::freqman_entry_get_step_value(e.step);
        step_ = (sv > 0) ? sv : def_step_hz_;
        if (e.type == core::freqman_type::Single) {
            freq_ = e.frequency_a;
        } else if (e.type != core::freqman_type::Unknown) {
            minfreq_ = e.frequency_a;
            maxfreq_ = e.frequency_b;
            freq_ = fwd_ ? minfreq_ : maxfreq_;
        }
    }

    void pause() {
        timer_ = 0;
        freq_lock_ = 0;
        recon_ = false;
    }

    void resume() {
        timer_ = 0;
        freq_lock_ = 0;
        recon_ = true;
    }

    /* One-shot nudge by whole entries/ranges (the encoder on the buttons). */
    void request_step(int32_t v) {
        if (v > 0) fwd_ = true;
        if (v < 0) fwd_ = false;
        if (has_entries()) stepper_ = v;
        freq_lock_ = 0;
        timer_ = 0;
    }

    /* One-shot jump to a different list index. */
    void request_index(int32_t v) {
        if (v > 0) fwd_ = true;
        if (v < 0) fwd_ = false;
        if (has_entries()) index_stepper_ = v;
        freq_lock_ = 0;
        timer_ = 0;
    }

    /* --- lock-out --- */
    LockoutSet& lockout() { return lockout_; }
    const LockoutSet& lockout() const { return lockout_; }
    void lockout_current() {
        if (has_entries()) lockout_.add(freq_);
    }

    /* --- the heart: one statistics update ---
     * db          channel level (dBFS on the host, max_db on the firmware).
     * interval_ms elapsed since the previous call.
     * Returns true if the tuned frequency changed (caller should retune). */
    bool process(int32_t db, int32_t interval_ms) {
        if (recon_) {
            if (timer_ == 0) {
                status_ = ReconStatus::Searching;
                freq_lock_ = 0;
                timer_ = static_cast<int32_t>(lock_duration_);
            }
            if (freq_lock_ < lock_nb_match_) {  /* LOCKING */
                status_ = ReconStatus::Locking;
                if (db > squelch_) {            /* signal present */
                    freq_lock_++;
                    timer_ += interval_ms;      /* extend the dwell */
                } else if (match_mode_ == kReconMatchContinuous) {
                    /* a gap resets the run and moves on immediately */
                    freq_lock_ = 0;
                    timer_ = 0;
                }
                /* SPARSE: a gap leaves freq_lock_ and timer_ alone, so matches
                 * may accumulate over the lock_duration window. */
            }
            if (freq_lock_ >= lock_nb_match_) {  /* LOCKED */
                if (status_ != ReconStatus::Locked) {
                    status_ = ReconStatus::Locked;
                    if (wait_ >= 0) timer_ = wait_;
                }
                if (wait_ < 0 && db > squelch_) {
                    /* stay abs(wait) after the last activity */
                    timer_ = std::abs(wait_);
                }
            }
        }

        if (timer_ != 0) {
            timer_ -= interval_ms;
            if (timer_ < 0) timer_ = 0;
        }

        bool retuned = false;
        if (recon_ || stepper_ != 0 || index_stepper_ != 0) {
            if (timer_ == 0 || stepper_ != 0 || index_stepper_ != 0) {
                if (has_entries()) {
                    step_list_with_lockout(stepper_, index_stepper_);
                    index_stepper_ = 0;
                    if (stepper_ < 0) stepper_++;
                    if (stepper_ > 0) stepper_--;
                }
            }
        }

        if (freq_ != last_freq_) {
            last_freq_ = freq_;
            retuned = true;
        }
        return retuned;
    }

   private:
    /* Loads freq/min/max from the current entry after an index change — the
     * firmware's reload switch. `moving_fwd` decides which end of a range or
     * ham pair to start from. */
    void load_entry_freq(bool moving_fwd) {
        if (!current_is_valid()) return;
        const auto& e = current_entry();
        switch (e.type) {
            case core::freqman_type::Repeater:
            case core::freqman_type::Single:
                freq_ = e.frequency_a;
                break;
            case core::freqman_type::Range:
            case core::freqman_type::HamRadio:
                minfreq_ = e.frequency_a;
                maxfreq_ = e.frequency_b;
                freq_ = moving_fwd ? minfreq_ : maxfreq_;
                break;
            default:
                break;
        }
        int32_t sv = core::freqman_entry_get_step_value(e.step);
        step_ = (sv > 0) ? sv : def_step_hz_;
    }

    /* One advance of the scan cursor — the firmware's stepping block. Mutates
     * current_index_, freq_, minfreq_/maxfreq_ and sets has_looped_. */
    void step_list(int32_t stepper_val, int32_t index_stepper_val) {
        const int sz = static_cast<int>(list_.size());
        if (sz == 0) return;

        has_looped_ = false;
        bool entry_has_changed = false;

        const bool go_fwd = (fwd_ && stepper_val == 0) || stepper_val > 0;
        const bool go_rev = (!fwd_ && stepper_val == 0) || stepper_val < 0;

        if (index_stepper_val == 0) {
            const auto type = current_entry().type;
            if (type == core::freqman_type::Range) {
                if (go_fwd) {
                    freq_ += step_;
                    if (freq_ > maxfreq_) {
                        current_index_++;
                        entry_has_changed = true;
                        if (current_index_ >= sz) {
                            has_looped_ = true;
                            current_index_ = 0;
                        }
                    }
                } else if (go_rev) {
                    freq_ -= step_;
                    if (freq_ < minfreq_) {
                        current_index_--;
                        entry_has_changed = true;
                        if (current_index_ < 0) {
                            has_looped_ = true;
                            current_index_ = sz - 1;
                        }
                    }
                }
            } else if (type == core::freqman_type::Single ||
                       type == core::freqman_type::Repeater) {
                if (go_fwd) {
                    current_index_++;
                    entry_has_changed = true;
                    if (current_index_ >= sz) {
                        has_looped_ = true;
                        current_index_ = 0;
                    }
                } else if (go_rev) {
                    current_index_--;
                    entry_has_changed = true;
                    if (current_index_ < 0) {
                        has_looped_ = true;
                        current_index_ = sz - 1;
                    }
                }
            } else if (type == core::freqman_type::HamRadio) {
                if (go_fwd) {
                    if ((minfreq_ != maxfreq_) && freq_ == minfreq_) {
                        freq_ = maxfreq_;
                    } else {
                        current_index_++;
                        entry_has_changed = true;
                        if (current_index_ >= sz) {
                            has_looped_ = true;
                            current_index_ = 0;
                        }
                    }
                } else if (go_rev) {
                    if ((minfreq_ != maxfreq_) && freq_ == maxfreq_) {
                        freq_ = minfreq_;
                    } else {
                        current_index_--;
                        entry_has_changed = true;
                        if (current_index_ < 0) {
                            has_looped_ = true;
                            current_index_ = sz - 1;
                        }
                    }
                }
            }

            if (has_looped_ && !continuous_) {
                entry_has_changed = true;
                if (go_fwd)
                    current_index_ = 0;
                else if (go_rev)
                    current_index_ = sz - 1;
            }
        } else {
            current_index_ += index_stepper_val;
            if (current_index_ < 0) current_index_ += sz;
            if (current_index_ >= sz) current_index_ -= sz;
            entry_has_changed = true;
        }

        if (entry_has_changed) {
            timer_ = 0;
            const bool moving_fwd =
                (fwd_ && stepper_val == 0 && index_stepper_val == 0) ||
                stepper_val > 0 || index_stepper_val > 0;
            load_entry_freq(moving_fwd);
        }

        if (has_looped_ && !continuous_) {
            pause();
        }
    }

    /* step_list, then keep stepping (in the scan direction) while the tuned
     * frequency is locked out. Bounded so an all-locked list cannot spin. */
    void step_list_with_lockout(int32_t stepper_val, int32_t index_stepper_val) {
        if (!has_entries()) return;
        step_list(stepper_val, index_stepper_val);
        if (lockout_.empty()) return;
        const int max_iters = static_cast<int>(list_.size()) * 4 + 8;
        int guard = 0;
        while (lockout_.contains(freq_) && guard++ < max_iters) {
            step_list(0, 0);
        }
    }

    core::freqman_db list_{};
    LockoutSet lockout_{};

    /* configuration */
    int32_t squelch_{kReconDefSquelch};
    int32_t wait_{kReconDefWaitDuration};
    uint32_t lock_nb_match_{kReconDefNbMatch};
    uint32_t lock_duration_{kReconMinLockDuration};
    int32_t match_mode_{kReconMatchContinuous};
    bool continuous_{true};
    bool fwd_{true};
    int32_t def_step_hz_{25000};

    /* state */
    int64_t freq_{0};
    int64_t minfreq_{0};
    int64_t maxfreq_{0};
    int32_t step_{25000};
    int32_t current_index_{0};
    ReconStatus status_{ReconStatus::Idle};
    uint32_t freq_lock_{0};
    int32_t timer_{0};
    bool recon_{true};
    int32_t stepper_{0};
    int32_t index_stepper_{0};
    bool has_looped_{false};
    int64_t last_freq_{-1};
};

}  // namespace app

/* --- The views ------------------------------------------------------------ */

namespace radio {
class ReceiverModel;
}

namespace app {

/* Recon setup: input/output file names and the load/behaviour flags. Persisted
 * to the [recon] section of core::settings(). The firmware's TX "repeat
 * recorded" tab is intentionally absent on the B200 (see the file header). */
class ReconSetupView : public ui::View {
   public:
    ReconSetupView(std::string input_file, std::string output_file);

    std::string title() const override { return "Recon setup"; }
    void focus() override;

    /* Fired with {input_file, output_file} when SAVE is pressed. */
    std::function<void(std::vector<std::string>)> on_changed{};

   private:
    void save();

    std::string input_file_{};
    std::string output_file_{};

    ui::Labels labels_{
        {{0, 4}, "Input list:", ui::Color::light_grey()},
        {{0, 44}, "Output list:", ui::Color::light_grey()},
    };

    ui::Button button_input_file_{{0, 20, 240, 20}, "RECON"};
    ui::Button button_output_file_{{0, 60, 240, 20}, "RECON_RESULTS"};

    ui::Checkbox check_autosave_{{0, 88}, 20, "autosave found freqs"};
    ui::Checkbox check_autostart_{{0, 108}, 20, "autostart recon"};
    ui::Checkbox check_clear_output_{{0, 128}, 20, "clear output at start"};

    ui::Checkbox check_load_freqs_{{0, 152}, 14, "load freqs"};
    ui::Checkbox check_load_ranges_{{124, 152}, 14, "load ranges"};
    ui::Checkbox check_load_ham_{{0, 172}, 14, "load ham"};
    ui::Checkbox check_load_repeaters_{{124, 172}, 14, "load repeaters"};
    ui::Checkbox check_update_ranges_{{0, 192}, 20, "auto-update m-ranges"};

    ui::Text text_note_{
        {0, 216, 240, 32},
        "TX repeat-record not on B200"};

    ui::Button button_save_{{56, 264, 128, 32}, "SAVE"};
};

class ReconView : public ui::View {
   public:
    ReconView();
    ~ReconView() override;

    std::string title() const override { return "Recon"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void load_persisted_settings();
    void frequency_file_load();
    void reload_restart();
    void apply_mode(int32_t modulation);
    void update_bandwidth_options(int32_t modulation);
    void set_scanner_mode(bool scanner);
    void set_loop_config(bool continuous);
    void start_manual_search();
    void handle_retune();
    void update_readouts();
    void recon_save_freq();
    void handle_remove();
    void open_config();
    core::freqman_load_options make_load_options() const;

    radio::ReceiverModel& receiver_;
    ReconEngine engine_{};

    /* file names + behaviour flags (persisted). */
    std::string input_file_{"RECON"};
    std::string output_file_{"RECON_RESULTS"};
    bool autosave_{true};
    bool autostart_{true};
    bool clear_output_{false};
    bool load_freqs_{true};
    bool load_ranges_{true};
    bool load_ham_{true};
    bool load_repeaters_{true};
    bool update_ranges_{true};

    bool scanner_mode_{false};
    bool manual_mode_{false};

    /* manual range endpoints */
    int64_t range_min_{0};
    int64_t range_max_{0};

    /* frame timing for the ~100 ms engine tick */
    std::chrono::steady_clock::time_point last_tick_{};
    bool tick_started_{false};

    /* readout change detection */
    int64_t last_shown_freq_{-1};
    uint32_t last_shown_lock_{0xffffffff};
    int32_t last_shown_timer_{-1};
    size_t last_shown_size_{static_cast<size_t>(-1)};
    ReconStatus last_shown_status_{ReconStatus::Idle};
    int32_t last_index_{-1};
    int32_t last_applied_mod_{-1};

    /* --- widgets --- */
    ui::BigFrequency big_freq_{{0, 0, 240, 32}};
    ui::Text freq_stats_{{0, 34, 152, 16}, ""};
    ui::Text text_timer_{{154, 34, 86, 16}, ""};
    ui::Text file_name_{{0, 50, 240, 16}, "=>"};
    ui::Text desc_cycle_{{0, 66, 240, 16}, "...no description..."};
    ui::VuMeter level_meter_{{0, 84, 148, 12}, 24, false};
    ui::Text text_max_{{152, 82, 88, 16}, ""};
    ui::Text text_nb_locks_{{152, 98, 88, 16}, ""};

    ui::Labels param_labels_{
        {{0, 116}, "Md", ui::Color::light_grey()},
        {{60, 116}, "BW", ui::Color::light_grey()},
        {{140, 116}, "St", ui::Color::light_grey()},
        {{136, 134}, "Sq", ui::Color::light_grey()},
        {{0, 152}, "NbL", ui::Color::light_grey()},
        {{64, 152}, "W", ui::Color::light_grey()},
        {{132, 152}, "Lk", ui::Color::light_grey()},
        {{0, 170}, "Gn", ui::Color::light_grey()},
        {{80, 170}, "Vol", ui::Color::light_grey()},
    };

    ui::OptionsField field_mode_{
        {24, 116},
        4,
        {{"AM  ", kModAM}, {"NFM ", kModNFM}, {"WFM ", kModWFM}, {"SPEC", kModSPEC}}};

    ui::OptionsField field_bw_{{84, 116}, 6, {}};
    ui::OptionsField step_mode_{{164, 116}, 9, {}};

    ui::OptionsField field_match_mode_{
        {0, 134},
        16,
        {{"MATCH:CONTINOUS", kReconMatchContinuous}, {"MATCH:SPARSE   ", kReconMatchSparse}}};

    ui::NumberField field_squelch_{{160, 134}, 4, {-120, 10}, 1, ' '};

    ui::NumberField field_nblocks_{{28, 152}, 2, {1, 99}, 1, ' '};
    ui::NumberField field_wait_{
        {76, 152}, 5, {-kReconMaxLockDuration, kReconMaxLockDuration}, kStatsUpdateIntervalMs, ' '};
    ui::NumberField field_lock_wait_{
        {148, 152}, 4, {kStatsUpdateIntervalMs, kReconMaxLockDuration}, kStatsUpdateIntervalMs, ' '};

    ui::NumberField field_gain_{{24, 170}, 3, {0, 76}, 1, ' '};
    ui::NumberField field_volume_{{104, 170}, 2, {0, 99}, 1, ' '};

    ui::Button button_pause_{{0, 190, 74, 22}, "PAUSE"};
    ui::Button button_scanner_mode_{{78, 190, 74, 22}, "RECON"};
    ui::Button button_dir_{{156, 190, 40, 22}, "FW>"};
    ui::Button button_restart_{{200, 190, 40, 22}, "RST"};

    ui::Button button_config_{{0, 214, 74, 22}, "CONFIG"};
    ui::Button button_loop_{{78, 214, 74, 22}, "[LOOP]"};
    ui::Button button_add_{{156, 214, 84, 22}, "<STORE>"};

    ui::ButtonWithEncoder button_manual_start_{{0, 238, 74, 22}, ""};
    ui::ButtonWithEncoder button_manual_end_{{78, 238, 74, 22}, ""};
    ui::Button button_manual_recon_{{156, 238, 84, 22}, "SEARCH"};

    ui::Button button_remove_{{0, 262, 152, 22}, "<REMOVE>"};
};

}  // namespace app

#endif /*__MB200_UI_RECON_H__*/
