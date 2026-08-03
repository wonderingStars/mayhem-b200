/*
 * mayhem-b200 — 5.8 GHz analogue FPV video-transmitter presence detector.
 *
 * Copyright (C) 2025 berkeozkir (Berke Ozkir) (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_fpv_detect.hpp"

#include "../audio/audio_out.hpp"
#include "../core/string_format.hpp"
#include "../radio/receiver_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"

#include <cmath>
#include <cstdio>

namespace app {
namespace fpv {

const int64_t kFrequencies[kNumBands][kChannelsPerBand] = {
    /* Band A */ {5865000000LL, 5845000000LL, 5825000000LL, 5805000000LL,
                  5785000000LL, 5765000000LL, 5745000000LL, 5725000000LL},
    /* Band B */ {5733000000LL, 5752000000LL, 5771000000LL, 5790000000LL,
                  5809000000LL, 5828000000LL, 5847000000LL, 5866000000LL},
    /* Band E */ {5705000000LL, 5685000000LL, 5665000000LL, 5645000000LL,
                  5885000000LL, 5905000000LL, 5925000000LL, 5945000000LL},
    /* Band F */ {5740000000LL, 5760000000LL, 5780000000LL, 5800000000LL,
                  5820000000LL, 5840000000LL, 5860000000LL, 5880000000LL},
    /* Band R */ {5658000000LL, 5695000000LL, 5732000000LL, 5769000000LL,
                  5806000000LL, 5843000000LL, 5880000000LL, 5917000000LL},
};

const char kBandLabels[kNumBands] = {'A', 'B', 'E', 'F', 'R'};

int32_t clamp_value(const int32_t value, const int32_t low, const int32_t high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

uint8_t channel_index(const uint8_t band, const uint8_t ch) {
    return static_cast<uint8_t>(band * kChannelsPerBand + ch);
}

void Scanner::set_band_mode(const uint8_t mode) {
    band_mode_ = mode;
    scan_band_ = (band_mode_ < kNumBands) ? band_mode_ : 0;
    scan_ch_ = 0;
    reset(true);
}

void Scanner::reset(const bool retune_current) {
    state_ = State::Scanning;
    candidate_band_ = scan_band_;
    candidate_ch_ = scan_ch_;
    verify_samples_ = 0;
    verify_hits_ = 0;
    verify_misses_ = 0;
    verify_sum_db_ = 0;
    verify_peak_db_ = -120;
    confidence_ = 0;
    lock_hold_ = 0;
    neighbour_phase_ = 0;
    neighbours_fresh_ = false;
    scan_dwell_frames_ = 0;

    if (retune_current) {
        retune_to(scan_band_, scan_ch_);
        scan_dwell_frames_ = kScanDwellFrames;
    }
}

void Scanner::retune_to(const uint8_t band, const uint8_t ch) {
    scan_band_ = band;
    scan_ch_ = ch;
    freq_ = static_cast<uint64_t>(kFrequencies[band][ch]);
    if (on_retune) on_retune(band, ch, freq_);
}

void Scanner::step_scan() {
    if (band_mode_ < kNumBands) {
        scan_band_ = band_mode_;
        scan_ch_ = static_cast<uint8_t>((scan_ch_ + 1) % kChannelsPerBand);
    } else {
        scan_ch_ = static_cast<uint8_t>(scan_ch_ + 1);
        if (scan_ch_ >= kChannelsPerBand) {
            scan_ch_ = 0;
            scan_band_ = static_cast<uint8_t>(scan_band_ + 1);
            if (scan_band_ >= kNumBands) scan_band_ = 0;
        }
    }

    retune_to(scan_band_, scan_ch_);
    scan_dwell_frames_ = kScanDwellFrames;
}

void Scanner::request_beep(const uint32_t hz, const uint32_t duration_ms) {
    if ((frame_counter_ - last_beep_frame_) < kBeepGuardFrames) return;
    last_beep_frame_ = frame_counter_;
    if (on_beep) on_beep(hz, duration_ms);
}

int16_t Scanner::candidate_average_db() const {
    if (!verify_samples_) return -120;
    return static_cast<int16_t>(verify_sum_db_ / verify_samples_);
}

int16_t Scanner::neighbour_best_db() const {
    int16_t best = -120;
    if (candidate_ch_ > 0) {
        const auto& left = memory_[channel_index(candidate_band_,
                                                 static_cast<uint8_t>(candidate_ch_ - 1))];
        if (left.last_db > best) best = left.last_db;
    }
    if ((candidate_ch_ + 1) < kChannelsPerBand) {
        const auto& right = memory_[channel_index(candidate_band_,
                                                  static_cast<uint8_t>(candidate_ch_ + 1))];
        if (right.last_db > best) best = right.last_db;
    }
    return best;
}

uint8_t Scanner::compute_candidate_confidence() const {
    if (!verify_samples_) return 0;

    const int32_t avg_db = candidate_average_db();
    const int32_t centre_margin = avg_db - detect_threshold_db();
    const int32_t peak_margin = verify_peak_db_ - detect_threshold_db();
    const int32_t neighbour_margin = avg_db - neighbour_best_db();

    int32_t score = 18;
    score += centre_margin * 7;
    score += peak_margin * 4;
    score += static_cast<int32_t>(verify_hits_) * 5;
    score -= static_cast<int32_t>(verify_misses_) * 10;

    if (neighbour_margin >= 6)
        score += 12;
    else if (neighbour_margin >= 3)
        score += 6;
    else if (neighbour_margin <= 0)
        score -= 8;

    const auto& mem = memory_[channel_index(candidate_band_, candidate_ch_)];
    if (mem.hits >= 2) score += 4;

    return static_cast<uint8_t>(clamp_value(score, 0, 99));
}

void Scanner::update_memory(const int16_t max_db) {
    auto& mem = memory_[channel_index(scan_band_, scan_ch_)];
    mem.last_db = max_db;
    if (max_db > mem.peak_db) mem.peak_db = max_db;
    if (max_db >= detect_threshold_db() && mem.hits < 255) mem.hits++;
    mem.confidence = confidence_;
}

void Scanner::enter_candidate(const int16_t max_db) {
    state_ = State::Candidate;
    candidate_band_ = scan_band_;
    candidate_ch_ = scan_ch_;
    verify_samples_ = 1;
    verify_hits_ = 1;
    verify_misses_ = 0;
    verify_sum_db_ = max_db;
    verify_peak_db_ = max_db;
    confidence_ = compute_candidate_confidence();
    neighbour_phase_ = 0;
    neighbours_fresh_ = false;

    retune_to(candidate_band_, candidate_ch_);
    request_beep(1150, 60);
}

void Scanner::evaluate_candidate_sample(const int16_t max_db) {
    if (state_ != State::Candidate) return;

    verify_samples_++;
    verify_sum_db_ += max_db;
    if (max_db > verify_peak_db_) verify_peak_db_ = max_db;

    if (max_db >= detect_threshold_db())
        verify_hits_++;
    else
        verify_misses_++;

    confidence_ = compute_candidate_confidence();

    /* Refresh the adjacent channels so the neighbour margin uses current
     * measurements, before any lock decision. */
    if (verify_samples_ >= 2 && !neighbours_fresh_) {
        if (candidate_ch_ > 0) {
            neighbour_phase_ = 1;
            retune_to(candidate_band_, static_cast<uint8_t>(candidate_ch_ - 1));
        } else {
            neighbour_phase_ = 2;
            retune_to(candidate_band_, static_cast<uint8_t>(candidate_ch_ + 1));
        }
        return;
    }

    if (!neighbours_fresh_) return;

    const bool strong_lock =
        verify_samples_ >= 3 &&
        verify_hits_ >= 3 &&
        verify_peak_db_ >= (detect_threshold_db() + kStrongLockPeakMarginDb) &&
        confidence_ >= 80 &&
        (candidate_average_db() - neighbour_best_db()) >= kMinNeighbourMarginForLockDb;

    if (strong_lock) {
        enter_lock();
        return;
    }

    if (verify_samples_ < kVerifySampleTarget) return;

    const bool centre_stronger =
        (candidate_average_db() - neighbour_best_db()) >= kMinNeighbourMarginForLockDb;
    if (verify_hits_ >= kVerifyMinHits && confidence_ >= kLockConfidenceMin && centre_stronger) {
        enter_lock();
        return;
    }

    state_ = State::Scanning;
    confidence_ = static_cast<uint8_t>(confidence_ / 2);
    verify_samples_ = 0;
    verify_hits_ = 0;
    verify_misses_ = 0;
    verify_sum_db_ = 0;
    verify_peak_db_ = -120;
    scan_dwell_frames_ = 0;
}

void Scanner::enter_lock() {
    state_ = State::Locked;
    lock_hold_ = kLockHoldMax;
    confidence_ = static_cast<uint8_t>(clamp_value(confidence_, kLockConfidenceMin, 99));
    retune_to(candidate_band_, candidate_ch_);
    request_beep(1850, 180);
}

void Scanner::update_lock(const int16_t max_db) {
    if (state_ != State::Locked) return;

    if (max_db >= unlock_threshold_db()) {
        if (lock_hold_ < kLockHoldMax) lock_hold_++;
        if (confidence_ < 99) confidence_++;
    } else {
        if (lock_hold_ > 0) lock_hold_--;
        if (confidence_ > 0) confidence_--;
    }

    if (lock_hold_ > 0) return;

    state_ = State::Scanning;
    confidence_ = 0;
    verify_samples_ = 0;
    verify_hits_ = 0;
    verify_misses_ = 0;
    verify_sum_db_ = 0;
    verify_peak_db_ = -120;
    scan_dwell_frames_ = 0;

    request_beep(650, 90);
}

void Scanner::on_timer() {
    frame_counter_++;

    if (state_ != State::Scanning) return;
    if (scan_dwell_frames_ > 0) {
        scan_dwell_frames_--;
        return;
    }
    step_scan();
}

void Scanner::on_statistics(const int16_t max_db) {
    const bool sampling_neighbour =
        (state_ == State::Candidate && (neighbour_phase_ == 1 || neighbour_phase_ == 2));
    if (!sampling_neighbour) update_memory(max_db);

    switch (state_) {
        case State::Scanning:
            if (max_db >= detect_threshold_db()) enter_candidate(max_db);
            break;

        case State::Candidate:
            if (neighbour_phase_ == 1) {
                memory_[channel_index(candidate_band_,
                                      static_cast<uint8_t>(candidate_ch_ - 1))].last_db = max_db;
                if (candidate_ch_ + 1 < kChannelsPerBand) {
                    neighbour_phase_ = 2;
                    retune_to(candidate_band_, static_cast<uint8_t>(candidate_ch_ + 1));
                } else {
                    neighbour_phase_ = 0;
                    neighbours_fresh_ = true;
                    retune_to(candidate_band_, candidate_ch_);
                }
                break;
            }
            if (neighbour_phase_ == 2) {
                memory_[channel_index(candidate_band_,
                                      static_cast<uint8_t>(candidate_ch_ + 1))].last_db = max_db;
                neighbour_phase_ = 0;
                neighbours_fresh_ = true;
                retune_to(candidate_band_, candidate_ch_);
                break;
            }
            evaluate_candidate_sample(max_db);
            break;

        case State::Locked:
            update_lock(max_db);
            break;
    }
}

}  // namespace fpv

/* --- View ----------------------------------------------------------------- */

FpvDetectView::FpvDetectView()
    : receiver_{*globals().receiver} {
    add_children({&labels_,
                  &field_band_,
                  &text_freq_,
                  &field_threshold_,
                  &text_state_,
                  &text_confidence_,
                  &text_power_,
                  &text_status_,
                  &text_detail_,
                  &meter_,
                  &grid_labels_,
                  &notes_});
    for (auto& row : rows_) add_child(&row);

    field_threshold_.set_value(scanner_.detect_threshold_db(), false);
    field_threshold_.on_change = [this](int32_t v) {
        scanner_.set_detect_threshold_db(v);
        update_texts();
    };

    field_band_.on_change = [this](size_t, int32_t value) {
        scanner_.set_band_mode(static_cast<uint8_t>(value));
        update_texts();
    };

    scanner_.on_retune = [this](uint8_t, uint8_t, uint64_t hz) {
        receiver_.set_target_frequency(hz);
        /* Retuning an AD936x is not instantaneous and the level readout is
         * smoothed, so give it a couple of frames before trusting a reading.
         * Upstream has no equivalent: the M4 restarts its statistics on
         * every retune. */
        settle_frames_ = 3;
    };

    scanner_.on_beep = [this](uint32_t hz, uint32_t ms) { this->beep(hz, ms); };

    field_band_.set_by_value(fpv::kAutoScanMode, false);
    scanner_.set_band_mode(fpv::kAutoScanMode);
    update_texts();
}

FpvDetectView::~FpvDetectView() = default;

void FpvDetectView::on_show() {
    ui::View::on_show();
    field_band_.focus();

    receiver_.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
    /* One FPV channel is ~20 MHz wide; upstream captures 750 kHz and reads the
     * peak. Match its bandwidth rather than the video bandwidth so a strong
     * carrier is what triggers, not the whole channel's power. */
    receiver_.set_sampling_rate(750'000.0);
    if (!receiver_.running()) receiver_.start();
    receiver_.set_target_frequency(scanner_.frequency());
}

void FpvDetectView::on_hide() {
    ui::View::on_hide();
}

void FpvDetectView::beep(const uint32_t hz, const uint32_t duration_ms) {
    auto* ao = globals().audio_out;
    if (ao == nullptr || !ao->running()) return;

    const size_t n = static_cast<size_t>(audio::sample_rate) * duration_ms / 1000;
    const size_t space = ao->space();
    const size_t count = (n < space) ? n : space;
    if (count == 0) return;

    beep_buffer_.resize(count);
    const double inc = 2.0 * M_PI * static_cast<double>(hz) /
                       static_cast<double>(audio::sample_rate);
    double phase = 0.0;
    for (size_t i = 0; i < count; i++) {
        beep_buffer_[i] = 0.35f * static_cast<float>(std::sin(phase));
        phase += inc;
    }
    ao->write(beep_buffer_.data(), count);
}

void FpvDetectView::update_texts() {
    char buf[48];

    std::snprintf(buf, sizeof(buf), "%c%d %llu MHz",
                  fpv::kBandLabels[scanner_.scan_band()],
                  static_cast<int>(scanner_.scan_ch() + 1),
                  static_cast<unsigned long long>(scanner_.frequency() / 1000000ULL));
    text_freq_.set(buf);

    switch (scanner_.state()) {
        case fpv::Scanner::State::Scanning: text_state_.set("SCANNING"); break;
        case fpv::Scanner::State::Candidate: text_state_.set("CHECKING"); break;
        case fpv::Scanner::State::Locked: text_state_.set(STR_COLOR_RED "FREQ FOUND"); break;
    }

    std::snprintf(buf, sizeof(buf), "Posi %u%%", static_cast<unsigned>(scanner_.confidence()));
    text_confidence_.set(buf);

    switch (scanner_.state()) {
        case fpv::Scanner::State::Scanning:
            if (scanner_.band_mode() < fpv::kNumBands)
                std::snprintf(buf, sizeof(buf), "Scanning band %c",
                              fpv::kBandLabels[scanner_.band_mode()]);
            else
                std::snprintf(buf, sizeof(buf), "Scanning all from list");
            break;
        case fpv::Scanner::State::Candidate:
            std::snprintf(buf, sizeof(buf), "CHKING %c%d  %llu MHz",
                          fpv::kBandLabels[scanner_.candidate_band()],
                          static_cast<int>(scanner_.candidate_ch() + 1),
                          static_cast<unsigned long long>(
                              fpv::kFrequencies[scanner_.candidate_band()]
                                               [scanner_.candidate_ch()] / 1000000LL));
            break;
        case fpv::Scanner::State::Locked:
            std::snprintf(buf, sizeof(buf), "FREQ FOUND %c%d  %llu",
                          fpv::kBandLabels[scanner_.candidate_band()],
                          static_cast<int>(scanner_.candidate_ch() + 1),
                          static_cast<unsigned long long>(
                              fpv::kFrequencies[scanner_.candidate_band()]
                                               [scanner_.candidate_ch()] / 1000000LL));
            break;
    }
    text_status_.set(buf);

    switch (scanner_.state()) {
        case fpv::Scanner::State::Scanning:
            std::snprintf(buf, sizeof(buf), "Need >= %d dB, stable dwell",
                          static_cast<int>(scanner_.detect_threshold_db()));
            break;
        case fpv::Scanner::State::Candidate:
            std::snprintf(buf, sizeof(buf), "avg %d  peak %d  hits %u/%u",
                          static_cast<int>(scanner_.candidate_average_db()),
                          static_cast<int>(scanner_.verify_peak_db()),
                          static_cast<unsigned>(scanner_.verify_hits()),
                          static_cast<unsigned>(scanner_.verify_samples()));
            break;
        case fpv::Scanner::State::Locked:
            std::snprintf(buf, sizeof(buf), "LOCKED %c%d  posi %u%%  hold %u",
                          fpv::kBandLabels[scanner_.candidate_band()],
                          static_cast<int>(scanner_.candidate_ch() + 1),
                          static_cast<unsigned>(scanner_.confidence()),
                          static_cast<unsigned>(scanner_.lock_hold()));
            break;
    }
    text_detail_.set(buf);

    for (uint8_t band = 0; band < fpv::kNumBands; band++) {
        std::string row{fpv::kBandLabels[band]};
        row += ' ';
        for (uint8_t ch = 0; ch < fpv::kChannelsPerBand; ch++) {
            const int16_t db = scanner_.memory(band, ch).peak_db;
            row += to_string_dec_int(db, 4);
        }
        rows_[band].set(row);
    }
}

void FpvDetectView::on_frame_sync() {
    ui::View::on_frame_sync();

    frame_counter_++;
    scanner_.on_timer();

    if (settle_frames_ > 0) {
        settle_frames_--;
        return;
    }

    /* POWER READING — the honest note.
     *
     * Upstream reads ChannelStatistics.max_db, the M4's peak of the decimated
     * channel over one baseband block. ReceiverModel exposes channel_level_db()
     * instead, a smoothed mean of the channel after the channel filter. It is
     * the closest equivalent the host has, and the detector's logic is
     * unchanged by the substitution, but the absolute numbers differ from
     * upstream's, so the default -38 dB threshold is a starting point rather
     * than a calibrated value. */
    const float db = receiver_.channel_level_db();
    const int16_t max_db = static_cast<int16_t>(
        fpv::clamp_value(static_cast<int32_t>(std::lround(db)), -120, 20));

    scanner_.on_statistics(max_db);

    char buf[24];
    std::snprintf(buf, sizeof(buf), "PWR %d dB", static_cast<int>(max_db));
    text_power_.set(buf);

    /* -120..0 dBFS onto the 0..255 bar. */
    const int32_t bar = fpv::clamp_value((max_db + 120) * 255 / 120, 0, 255);
    meter_.set_value(static_cast<uint8_t>(bar));

    /* The readouts change on every frame but only need to be legible. */
    if ((frame_counter_ % 6) == 0) update_texts();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream: menu_location RX, icon_color orange. Its own bitmap is a
 * quadcopter; bitmaps.hpp has none, and the scanner icon is the closest honest
 * match for a channel sweeper. */
const app::Registrar reg_fpv_detect{{"fpv_detect", "FPV Detect", app::Category::Receive,
                                     ui::Color::orange(), &ui::bitmap_icon_scanner,
                                     [] { return std::make_unique<app::FpvDetectView>(); }}};
}  // namespace
