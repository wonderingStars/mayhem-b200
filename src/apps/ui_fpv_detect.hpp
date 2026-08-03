/*
 * mayhem-b200 — 5.8 GHz analogue FPV video-transmitter presence detector.
 *
 * Ported from firmware/application/external/fpv_detect/. There is no baseband
 * processor of its own: upstream runs the generic capture image and reads the
 * ChannelStatistics message the M4 emits (max_db over the tuned channel),
 * stepping through the A/B/E/F/R band plans one channel at a time.
 *
 * The detector is a three-state machine, ported here as app::fpv::Scanner so
 * it can be driven from a test without a radio:
 *
 *   Scanning   step a channel per frame; a reading at or above the detect
 *              threshold promotes the channel to Candidate.
 *   Candidate  gather up to five readings, and after the second one re-measure
 *              the two adjacent channels so the neighbour margin is current
 *              rather than stale scan data. Lock needs enough hits, a
 *              confidence score of at least 72 (or 80 on the fast path), and
 *              the centre channel at least 3 dB above both neighbours — which
 *              is what stops a wide noise blob from locking.
 *   Locked     hold until the power stays below (threshold - 6) dB long enough
 *              to drain the twelve-frame hold counter, so brief fades do not
 *              drop the lock.
 *
 * FREQUENCY: the five FPV band plans span 5.645-5.945 GHz. That is inside a
 * B200's 70 MHz - 6 GHz tuning range but close to its top end, where the
 * front-end gain rolls off; the range is read from radio->caps() and shown on
 * screen rather than asserted here. 5.945 GHz leaves only ~55 MHz of headroom,
 * so a device whose caps report a lower ceiling will not reach band E7/E8.
 *
 * POWER READING: upstream's ChannelStatistics.max_db is the M4's per-block peak
 * of the decimated channel. The host equivalent is ReceiverModel's smoothed
 * channel_level_db(), which is a mean rather than a peak — see the note in the
 * .cpp. The detector logic is identical either way; only the numbers a given
 * threshold corresponds to differ.
 *
 * Copyright (C) 2025 berkeozkir (Berke Ozkir) (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_FPV_DETECT_H__
#define __MB200_UI_FPV_DETECT_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace radio {
class ReceiverModel;
}

namespace app {
namespace fpv {

inline constexpr uint8_t kNumBands = 5;
inline constexpr uint8_t kChannelsPerBand = 8;
inline constexpr uint8_t kTotalChannels = kNumBands * kChannelsPerBand;
inline constexpr uint8_t kAutoScanMode = kNumBands;

/* Bands A, B, E, F (Airwave/Boscam/DJI/IRC) and R (Raceband), in Hz. Verbatim
 * from upstream's fpv_frequencies[][]. */
extern const int64_t kFrequencies[kNumBands][kChannelsPerBand];
extern const char kBandLabels[kNumBands];

/* Detector tuning, all verbatim from ui_fpv_detect.cpp. */
inline constexpr uint8_t kScanDwellFrames = 1;
inline constexpr uint8_t kVerifySampleTarget = 5;
inline constexpr uint8_t kVerifyMinHits = 4;
inline constexpr uint8_t kStrongLockPeakMarginDb = 8;
inline constexpr uint8_t kLockHoldMax = 12;
inline constexpr uint8_t kLockConfidenceMin = 72;
inline constexpr int32_t kMinNeighbourMarginForLockDb = 3;
inline constexpr uint8_t kBeepGuardFrames = 8;

struct ChannelMemory {
    int16_t last_db{-120};
    int16_t peak_db{-120};
    uint8_t hits{0};
    uint8_t confidence{0};
};

int32_t clamp_value(int32_t value, int32_t low, int32_t high);
uint8_t channel_index(uint8_t band, uint8_t ch);

class Scanner {
   public:
    enum class State : uint8_t { Scanning, Candidate, Locked };

    /* Called whenever the detector wants the radio on a different channel. */
    std::function<void(uint8_t band, uint8_t ch, uint64_t hz)> on_retune{};
    /* Called for the three audible cues upstream requests from the baseband. */
    std::function<void(uint32_t hz, uint32_t duration_ms)> on_beep{};

    void set_detect_threshold_db(int32_t db) { detect_threshold_db_ = db; }
    int16_t detect_threshold_db() const { return static_cast<int16_t>(detect_threshold_db_); }
    int16_t unlock_threshold_db() const { return static_cast<int16_t>(detect_threshold_db() - 6); }

    /* 0..4 pins one band; kAutoScanMode walks all of them. */
    void set_band_mode(uint8_t mode);
    uint8_t band_mode() const { return band_mode_; }

    void reset(bool retune_current);

    /* One UI frame: advances the scan when nothing is being verified. */
    void on_timer();
    /* One power measurement of whatever channel is currently tuned. */
    void on_statistics(int16_t max_db);

    State state() const { return state_; }
    uint8_t scan_band() const { return scan_band_; }
    uint8_t scan_ch() const { return scan_ch_; }
    uint8_t candidate_band() const { return candidate_band_; }
    uint8_t candidate_ch() const { return candidate_ch_; }
    uint8_t confidence() const { return confidence_; }
    uint8_t lock_hold() const { return lock_hold_; }
    uint8_t verify_samples() const { return verify_samples_; }
    uint8_t verify_hits() const { return verify_hits_; }
    uint8_t verify_misses() const { return verify_misses_; }
    int16_t verify_peak_db() const { return verify_peak_db_; }
    uint64_t frequency() const { return freq_; }
    uint32_t frame_counter() const { return frame_counter_; }

    int16_t candidate_average_db() const;
    int16_t neighbour_best_db() const;
    uint8_t compute_candidate_confidence() const;

    const ChannelMemory& memory(uint8_t band, uint8_t ch) const {
        return memory_[channel_index(band, ch)];
    }

   private:
    void retune_to(uint8_t band, uint8_t ch);
    void step_scan();
    void enter_candidate(int16_t max_db);
    void evaluate_candidate_sample(int16_t max_db);
    void enter_lock();
    void update_lock(int16_t max_db);
    void update_memory(int16_t max_db);
    void request_beep(uint32_t hz, uint32_t duration_ms);

    State state_{State::Scanning};

    uint8_t scan_band_{0};
    uint8_t scan_ch_{0};
    uint8_t band_mode_{kAutoScanMode};

    uint8_t candidate_band_{0};
    uint8_t candidate_ch_{0};
    uint8_t verify_samples_{0};
    uint8_t verify_hits_{0};
    uint8_t verify_misses_{0};
    int32_t verify_sum_db_{0};
    int16_t verify_peak_db_{-120};
    uint8_t confidence_{0};
    uint8_t lock_hold_{0};
    /* 0 = centre, 1 = left neighbour, 2 = right neighbour. */
    uint8_t neighbour_phase_{0};
    bool neighbours_fresh_{false};

    uint8_t scan_dwell_frames_{0};
    uint32_t frame_counter_{0};
    uint32_t last_beep_frame_{0};

    uint64_t freq_{static_cast<uint64_t>(kFrequencies[0][0])};
    int32_t detect_threshold_db_{-38};

    std::array<ChannelMemory, kTotalChannels> memory_{};
};

}  // namespace fpv

/* --- View ----------------------------------------------------------------- */

class FpvDetectView : public ui::View {
   public:
    FpvDetectView();
    ~FpvDetectView() override;

    FpvDetectView(const FpvDetectView&) = delete;
    FpvDetectView& operator=(const FpvDetectView&) = delete;

    std::string title() const override { return "FPV DETECT"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void update_texts();
    void beep(uint32_t hz, uint32_t duration_ms);

    radio::ReceiverModel& receiver_;
    fpv::Scanner scanner_{};

    uint32_t frame_counter_{0};
    /* Frames to wait after a retune before the level reading is trusted. */
    uint32_t settle_frames_{0};

    std::vector<float> beep_buffer_{};

    ui::Labels labels_{
        {{0, 2}, "Band", ui::Color::light_grey()},
        {{0, 22}, "Thr>", ui::Color::light_grey()},
    };

    ui::OptionsField field_band_{
        {40, 2},
        9,
        {{"Band A  ", 0},
         {"Band B  ", 1},
         {"Band E  ", 2},
         {"Band F  ", 3},
         {"Band R  ", 4},
         {"AutoScan", fpv::kAutoScanMode}}};

    ui::Text text_freq_{{120, 2, 120, 16}, ""};

    ui::NumberField field_threshold_{{40, 22}, 4, {-100, 20}, 1, ' '};
    ui::Text text_state_{{96, 22, 96, 16}, "SCANNING"};
    ui::Text text_confidence_{{176, 22, 64, 16}, "Posi 0%"};

    ui::Text text_power_{{0, 44, 240, 16}, "PWR -120 dB"};
    ui::Text text_status_{{0, 62, 240, 16}, "SCANNING FOR FREQS"};
    ui::Text text_detail_{{0, 80, 240, 16}, "Waiting for FPV FREQ"};

    ui::VuMeter meter_{{8, 102, 224, 14}, 28, true};

    ui::Labels grid_labels_{
        {{0, 126}, "Channel memory (peak dB)", ui::Color::light_grey()},
    };
    ui::Text rows_[fpv::kNumBands]{
        ui::Text{{0, 144, 240, 16}, ""},
        ui::Text{{0, 160, 240, 16}, ""},
        ui::Text{{0, 176, 240, 16}, ""},
        ui::Text{{0, 192, 240, 16}, ""},
        ui::Text{{0, 208, 240, 16}, ""},
    };

    ui::Labels notes_{
        {{0, 236}, "Level: ReceiverModel channel", ui::Color::grey()},
        {{0, 252}, "dBFS (mean), not M4 peak.", ui::Color::grey()},
        {{0, 272}, "5.8 GHz: near B200 ceiling.", ui::Color::grey()},
        {{0, 288}, "Untested on air - no radio.", ui::Color::yellow()},
    };
};

}  // namespace app

#endif /*__MB200_UI_FPV_DETECT_H__*/
