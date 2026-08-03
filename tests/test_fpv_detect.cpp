/*
 * mayhem-b200 — tests for the 5.8 GHz FPV presence detector.
 *
 * The band plans and every threshold come from upstream
 * (application/external/fpv_detect/ui_fpv_detect.*). The expected confidence
 * values below are computed here from upstream's published scoring formula
 * rather than read back out of the implementation.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_fpv_detect.hpp"

#include <vector>

using namespace app::fpv;

namespace {

/* Drives a Scanner against a fixed "RF environment": one power reading per
 * channel. Each step is one UI frame: on_timer() then one measurement of
 * whatever channel the scanner has tuned. */
class Bench {
   public:
    explicit Bench(int16_t floor_db = -60) { field_.fill(floor_db); }

    void set(uint8_t band, uint8_t ch, int16_t db) { field_[channel_index(band, ch)] = db; }
    void set_band(uint8_t band, int16_t db) {
        for (uint8_t c = 0; c < kChannelsPerBand; c++) set(band, c, db);
    }

    Scanner& scanner() { return scanner_; }
    const std::vector<uint8_t>& retunes() const { return retunes_; }

    void attach() {
        scanner_.on_retune = [this](uint8_t band, uint8_t ch, uint64_t) {
            retunes_.push_back(channel_index(band, ch));
        };
        scanner_.on_beep = [this](uint32_t hz, uint32_t) { beeps_.push_back(hz); };
    }

    void step(size_t frames = 1) {
        for (size_t i = 0; i < frames; i++) {
            scanner_.on_timer();
            scanner_.on_statistics(field_[channel_index(scanner_.scan_band(),
                                                        scanner_.scan_ch())]);
        }
    }

    const std::vector<uint32_t>& beeps() const { return beeps_; }

   private:
    Scanner scanner_{};
    std::array<int16_t, kTotalChannels> field_{};
    std::vector<uint8_t> retunes_{};
    std::vector<uint32_t> beeps_{};
};

}  // namespace

TEST(fpv_band_plan_matches_upstream) {
    /* Spot checks against the published analogue FPV band plans. */
    CHECK_EQ(kFrequencies[0][0], 5865000000LL);  /* A1 */
    CHECK_EQ(kFrequencies[0][7], 5725000000LL);  /* A8 */
    CHECK_EQ(kFrequencies[1][0], 5733000000LL);  /* B1 */
    CHECK_EQ(kFrequencies[2][3], 5645000000LL);  /* E4, the lowest channel */
    CHECK_EQ(kFrequencies[2][7], 5945000000LL);  /* E8, the highest channel */
    CHECK_EQ(kFrequencies[3][0], 5740000000LL);  /* F1 */
    CHECK_EQ(kFrequencies[4][0], 5658000000LL);  /* R1 */
    CHECK_EQ(kFrequencies[4][7], 5917000000LL);  /* R8 */

    CHECK_EQ(kBandLabels[0], 'A');
    CHECK_EQ(kBandLabels[4], 'R');

    /* The whole plan lies inside a B200's 70 MHz - 6 GHz range, but the top
     * channel leaves only ~55 MHz of headroom. */
    int64_t lo = kFrequencies[0][0];
    int64_t hi = kFrequencies[0][0];
    for (uint8_t b = 0; b < kNumBands; b++) {
        for (uint8_t c = 0; c < kChannelsPerBand; c++) {
            if (kFrequencies[b][c] < lo) lo = kFrequencies[b][c];
            if (kFrequencies[b][c] > hi) hi = kFrequencies[b][c];
        }
    }
    CHECK_EQ(lo, 5645000000LL);
    CHECK_EQ(hi, 5945000000LL);
    CHECK(hi < 6000000000LL);
}

TEST(fpv_channel_index_and_thresholds) {
    CHECK_EQ(channel_index(0, 0), 0u);
    CHECK_EQ(channel_index(2, 5), 21u);
    CHECK_EQ(channel_index(4, 7), 39u);

    Scanner s;
    /* Upstream's default. */
    CHECK_EQ(s.detect_threshold_db(), -38);
    CHECK_EQ(s.unlock_threshold_db(), -44);
    s.set_detect_threshold_db(-20);
    CHECK_EQ(s.detect_threshold_db(), -20);
    CHECK_EQ(s.unlock_threshold_db(), -26);

    CHECK_EQ(clamp_value(5, 0, 3), 3);
    CHECK_EQ(clamp_value(-5, 0, 3), 0);
    CHECK_EQ(clamp_value(2, 0, 3), 2);
}

TEST(fpv_autoscan_walks_every_channel_and_wraps) {
    Bench bench;
    bench.attach();
    bench.scanner().set_band_mode(kAutoScanMode);

    /* set_band_mode() resets and retunes to A1. */
    CHECK_EQ(bench.retunes().size(), 1u);
    CHECK_EQ(bench.retunes()[0], 0u);

    /* Each channel dwells one frame, so a step happens every other frame. */
    bench.step(2 * kTotalChannels);

    const auto& r = bench.retunes();
    CHECK(r.size() >= static_cast<size_t>(kTotalChannels) + 1);
    for (size_t i = 0; i < kTotalChannels && i + 1 < r.size(); i++) {
        CHECK_EQ(r[i], static_cast<uint8_t>(i));
    }
    /* And back to the start. */
    if (r.size() > kTotalChannels) CHECK_EQ(r[kTotalChannels], 0u);
}

TEST(fpv_single_band_mode_stays_in_its_band) {
    Bench bench;
    bench.attach();
    bench.scanner().set_band_mode(2);  /* Band E */

    bench.step(40);
    for (uint8_t idx : bench.retunes()) {
        CHECK(idx >= 16u);
        CHECK(idx < 24u);
    }
    CHECK_EQ(bench.scanner().band_mode(), 2u);
}

TEST(fpv_confidence_matches_the_upstream_formula) {
    /* Score = 18 + centre_margin*7 + peak_margin*4 + hits*5 - misses*10,
     * + 12 when the neighbour margin is >= 6 dB, clamped to 0..99. */
    Bench bench;
    bench.attach();
    bench.scanner().set_band_mode(0);
    bench.set(0, 2, -25);
    bench.set(0, 1, -50);
    bench.set(0, 3, -50);

    /* Four frames reaches A3 and promotes it: A1 dwells on frames 1-2, A2 on
     * 3-4... the scan steps every other frame, so frame 4 is the first
     * measurement of A3. Stopping there pins the score to the single sample
     * enter_candidate() takes. */
    bench.step(4);
    CHECK(bench.scanner().state() == Scanner::State::Candidate);
    CHECK_EQ(bench.scanner().candidate_ch(), 2u);
    CHECK_EQ(bench.scanner().verify_samples(), 1u);
    CHECK_EQ(bench.scanner().verify_hits(), 1u);

    /* centre_margin = peak_margin = -25 - (-38) = 13, one hit, no misses,
     * neighbour margin 25 dB (>= 6, so +12), channel hits = 1 (no +4):
     *   18 + 13*7 + 13*4 + 5 + 12 = 178, clamped to 99. */
    CHECK_EQ(bench.scanner().confidence(), 99u);

    /* A candidate barely over the threshold scores far lower:
     *   18 + 1*7 + 1*4 + 5 + 12 = 46. */
    Bench weak;
    weak.attach();
    weak.scanner().set_band_mode(0);
    weak.set(0, 2, -37);
    weak.set(0, 1, -50);
    weak.set(0, 3, -50);
    weak.step(4);
    CHECK(weak.scanner().state() == Scanner::State::Candidate);
    CHECK_EQ(weak.scanner().verify_samples(), 1u);
    CHECK_EQ(weak.scanner().confidence(), 46u);
}

TEST(fpv_locks_on_a_narrow_strong_carrier) {
    Bench bench;
    bench.attach();
    bench.scanner().set_band_mode(0);
    bench.set(0, 2, -25);  /* the transmitter */
    bench.set(0, 1, -50);
    bench.set(0, 3, -50);

    for (int i = 0; i < 40 && bench.scanner().state() != Scanner::State::Locked; i++) {
        bench.step(1);
    }

    CHECK(bench.scanner().state() == Scanner::State::Locked);
    CHECK_EQ(bench.scanner().candidate_band(), 0u);
    CHECK_EQ(bench.scanner().candidate_ch(), 2u);
    CHECK_EQ(bench.scanner().scan_ch(), 2u);
    CHECK_EQ(bench.scanner().frequency(), static_cast<uint64_t>(kFrequencies[0][2]));
    CHECK_EQ(bench.scanner().lock_hold(), kLockHoldMax);
    CHECK(bench.scanner().confidence() >= kLockConfidenceMin);
}

TEST(fpv_refuses_to_lock_on_a_wide_signal) {
    /* Everything in the band equally strong: the neighbour margin is zero, so
     * however high the raw power is, the centre-versus-neighbour test fails
     * and the candidate is dropped after the five verification samples. */
    Bench bench;
    bench.attach();
    bench.scanner().set_band_mode(0);
    bench.set_band(0, -20);

    for (int i = 0; i < 200; i++) {
        bench.step(1);
        CHECK(bench.scanner().state() != Scanner::State::Locked);
    }
    CHECK(bench.scanner().state() != Scanner::State::Locked);
}

TEST(fpv_ignores_a_signal_below_the_threshold) {
    Bench bench(-60);
    bench.attach();
    bench.scanner().set_band_mode(kAutoScanMode);
    bench.set(3, 4, -39);  /* one dB under the -38 threshold */

    for (int i = 0; i < 300; i++) bench.step(1);
    CHECK(bench.scanner().state() == Scanner::State::Scanning);
    CHECK_EQ(bench.scanner().confidence(), 0u);
    /* But the channel memory still records what was seen. */
    CHECK_EQ(bench.scanner().memory(3, 4).peak_db, -39);
    CHECK_EQ(bench.scanner().memory(3, 4).hits, 0u);
}

TEST(fpv_lock_holds_through_a_fade_then_drops) {
    Bench bench;
    bench.attach();
    bench.scanner().set_band_mode(0);
    bench.set(0, 2, -25);
    bench.set(0, 1, -50);
    bench.set(0, 3, -50);

    for (int i = 0; i < 40 && bench.scanner().state() != Scanner::State::Locked; i++) {
        bench.step(1);
    }
    CHECK(bench.scanner().state() == Scanner::State::Locked);

    /* A brief fade below the -44 dB unlock threshold drains the hold counter
     * but does not release the lock. */
    bench.set(0, 2, -70);
    bench.step(kLockHoldMax - 1);
    CHECK(bench.scanner().state() == Scanner::State::Locked);
    CHECK_EQ(bench.scanner().lock_hold(), 1u);

    /* Recovering refills it. */
    bench.set(0, 2, -25);
    bench.step(kLockHoldMax);
    CHECK(bench.scanner().state() == Scanner::State::Locked);
    CHECK_EQ(bench.scanner().lock_hold(), kLockHoldMax);

    /* A sustained loss releases it. */
    bench.set(0, 2, -70);
    bench.step(kLockHoldMax);
    CHECK(bench.scanner().state() == Scanner::State::Scanning);
    CHECK_EQ(bench.scanner().confidence(), 0u);
    /* And the unlock cue sounded. */
    CHECK(!bench.beeps().empty());
    CHECK_EQ(bench.beeps().back(), 650u);
}

TEST(fpv_first_channel_candidate_samples_only_its_right_neighbour) {
    /* candidate_ch 0 has no left neighbour; upstream jumps straight to
     * neighbour phase 2 rather than indexing channel -1. */
    Bench bench;
    bench.attach();
    bench.scanner().set_band_mode(0);
    bench.set(0, 0, -25);
    bench.set(0, 1, -50);

    for (int i = 0; i < 40 && bench.scanner().state() != Scanner::State::Locked; i++) {
        bench.step(1);
    }
    CHECK(bench.scanner().state() == Scanner::State::Locked);
    CHECK_EQ(bench.scanner().candidate_ch(), 0u);
    /* Only channel 1 was ever measured as a neighbour. */
    CHECK_EQ(bench.scanner().memory(0, 1).last_db, -50);
}

TEST(fpv_beep_guard_lets_at_most_one_cue_through_per_eight_frames) {
    /* Upstream's guard is `(frame_counter_ - last_beep_frame_) < 8`, with both
     * counters starting at zero — so cues in the first eight frames are
     * suppressed, and candidate-then-lock (four frames apart) can never both
     * sound. That is upstream behaviour, not a port artefact; these two cases
     * pin it down. */
    static_assert(kBeepGuardFrames == 8, "upstream BEEP_GUARD_FRAMES");

    /* Transmitter on A3: candidate at frame 4 (inside the opening guard, so
     * silent), lock at frame 8 (exactly eight frames, so it sounds). */
    Bench early;
    early.attach();
    early.scanner().set_band_mode(0);
    early.set(0, 2, -25);
    early.set(0, 1, -50);
    early.set(0, 3, -50);
    for (int i = 0; i < 20 && early.scanner().state() != Scanner::State::Locked; i++) {
        early.step(1);
    }
    CHECK(early.scanner().state() == Scanner::State::Locked);
    CHECK_EQ(early.beeps().size(), 1u);
    if (!early.beeps().empty()) CHECK_EQ(early.beeps()[0], 1850u);

    /* Transmitter on A5: candidate at frame 8 sounds, lock at frame 12 is
     * inside the guard and is dropped. */
    Bench late;
    late.attach();
    late.scanner().set_band_mode(0);
    late.set(0, 4, -25);
    late.set(0, 3, -50);
    late.set(0, 5, -50);
    for (int i = 0; i < 20 && late.scanner().state() != Scanner::State::Locked; i++) {
        late.step(1);
    }
    CHECK(late.scanner().state() == Scanner::State::Locked);
    CHECK_EQ(late.beeps().size(), 1u);
    if (!late.beeps().empty()) CHECK_EQ(late.beeps()[0], 1150u);
}
