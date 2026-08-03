/*
 * mayhem-b200 — tests for the Search energy-scanner logic.
 *
 * Expected values are derived from the upstream implementation
 * (firmware/application/apps/ui_search.cpp): the bin↔frequency arithmetic, the
 * DC/edge blanking, the mean and cross-slice peak selection, the snap quirk,
 * the slice planner and the lock/release state machine. Not from this port's
 * own output.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_search.hpp"

#include <array>
#include <cstdint>
#include <string>

using namespace app;

/* --- Bin → frequency mapping (exact) ---------------------------------------
 *
 * bin 128 is DC (the slice centre); bins above are higher, below are lower.
 * SEARCH_BIN_WIDTH = 2'500'000 / 256 = 9765 (integer, as upstream). */

TEST(search_bin_width_is_integer_division) {
    CHECK_EQ(search::bin_width(2'500'000, 256), int64_t{9765});
    CHECK_EQ(search::bin_width(2'560'000, 256), int64_t{10000});
    CHECK_EQ(search::bin_width(0, 0), int64_t{0});  // guarded
}

TEST(search_fixed_bin_to_frequency_matches_upstream) {
    const uint64_t c = 100'000'000;
    /* center + SEARCH_BIN_WIDTH * (bin - 128) */
    CHECK_EQ(search::bin_to_frequency(c, 128), uint64_t{100'000'000});
    CHECK_EQ(search::bin_to_frequency(c, 129), uint64_t{100'009'765});
    CHECK_EQ(search::bin_to_frequency(c, 127), uint64_t{99'990'235});
    CHECK_EQ(search::bin_to_frequency(c, 0), uint64_t{98'750'080});    // -9765*128
    CHECK_EQ(search::bin_to_frequency(c, 255), uint64_t{101'240'155});  // +9765*127
}

TEST(search_general_bin_to_frequency_is_exact_for_centre_and_span) {
    /* 2.56 MHz over 256 bins → exactly 10 kHz per bin. */
    const uint64_t c = 433'000'000;
    CHECK_EQ(search::bin_to_frequency(c, 128, 2'560'000, 256), uint64_t{433'000'000});
    CHECK_EQ(search::bin_to_frequency(c, 138, 2'560'000, 256), uint64_t{433'100'000});
    CHECK_EQ(search::bin_to_frequency(c, 118, 2'560'000, 256), uint64_t{432'900'000});

    /* The general form with Search's parameters equals the fixed form. */
    CHECK_EQ(search::bin_to_frequency(c, 129, 2'500'000, 256),
             search::bin_to_frequency(c, 129));
}

/* --- DC / edge blanking ----------------------------------------------------- */

TEST(search_blanking_matches_upstream_mask) {
    /* Outer two bins each side. */
    CHECK(search::bin_blanked(0));
    CHECK(search::bin_blanked(1));
    CHECK(!search::bin_blanked(2));
    CHECK(!search::bin_blanked(253));
    CHECK(search::bin_blanked(254));
    CHECK(search::bin_blanked(255));

    /* Centre twelve bins, 122..133 inclusive; DC bin 128 is inside it. */
    CHECK(!search::bin_blanked(121));
    CHECK(search::bin_blanked(122));
    CHECK(search::bin_blanked(128));
    CHECK(search::bin_blanked(133));
    CHECK(!search::bin_blanked(134));
}

/* --- Snap (upstream truncates to the step below) ---------------------------- */

TEST(search_snap_truncates_not_rounds) {
    /* 100'009'765 is nearer 100'012'500, but upstream's round() acts on an
     * already-integer division, so the effective behaviour is truncation. */
    CHECK_EQ(search::snap_frequency(100'009'765, 12'500), uint64_t{100'000'000});
    CHECK_EQ(search::snap_frequency(100'000'000, 25'000), uint64_t{100'000'000});
    CHECK_EQ(search::snap_frequency(100'024'999, 25'000), uint64_t{100'000'000});
    CHECK_EQ(search::snap_frequency(100'025'000, 25'000), uint64_t{100'025'000});
    /* A zero step is a no-op, not a divide-by-zero. */
    CHECK_EQ(search::snap_frequency(123'456'789, 0), uint64_t{123'456'789});
}

/* --- dBFS → power byte ------------------------------------------------------ */

TEST(search_db_to_power_is_linear_and_clamped) {
    CHECK_EQ(static_cast<int>(search::db_to_power(-110.0f, -110.0f, -20.0f)), 0);
    CHECK_EQ(static_cast<int>(search::db_to_power(-20.0f, -110.0f, -20.0f)), 255);
    CHECK_EQ(static_cast<int>(search::db_to_power(-65.0f, -110.0f, -20.0f)), 128);  // midpoint
    CHECK_EQ(static_cast<int>(search::db_to_power(0.0f, -110.0f, -20.0f)), 255);    // above ceil
    CHECK_EQ(static_cast<int>(search::db_to_power(-140.0f, -110.0f, -20.0f)), 0);   // below floor
    CHECK_EQ(static_cast<int>(search::db_to_power(-50.0f, -20.0f, -20.0f)), 0);     // degenerate
}

/* --- Per-slice scan (peak + mean, with blanking) --------------------------- */

TEST(search_scan_slice_finds_peak_and_ignores_blanked_bins) {
    std::array<uint8_t, search::bin_nb> p{};
    p.fill(0);
    p[200] = 150;  // the real peak
    p[60] = 100;   // a secondary
    p[128] = 255;  // DC spike — must be blanked away
    p[0] = 255;    // edge — blanked
    p[122] = 200;  // inside the centre blank — ignored

    const auto s = search::scan_slice(p.data());
    CHECK_EQ(static_cast<int>(s.max_power), 150);
    CHECK_EQ(static_cast<int>(s.max_index), 200);
    /* Only the two surviving bins contribute to the mean accumulator. */
    CHECK_EQ(s.power_sum, uint32_t{250});
}

TEST(search_scan_slice_all_blanked_reports_no_energy) {
    std::array<uint8_t, search::bin_nb> p{};
    p.fill(0);
    p[128] = 255;  // only DC
    const auto s = search::scan_slice(p.data());
    CHECK_EQ(static_cast<int>(s.max_power), 0);
    CHECK_EQ(static_cast<int>(s.max_index), 0);
    CHECK_EQ(s.power_sum, uint32_t{0});
}

TEST(search_mean_power_divides_by_non_dc_bins_times_slices) {
    /* mean = Σpower / (240 * slices_nb). */
    CHECK_EQ(search::mean_power(240u * 80u, 1), uint32_t{80});
    CHECK_EQ(search::mean_power(240u * 80u * 2u, 2), uint32_t{80});
    CHECK_EQ(search::mean_power(1234, 0), uint32_t{0});  // guarded
}

/* --- Cross-slice peak selection -------------------------------------------- */

TEST(search_find_peak_picks_strongest_over_threshold) {
    search::Slice slices[3]{};
    slices[0] = {0, 100, 140};
    slices[1] = {0, 200, 150};
    slices[2] = {0, 50, 160};

    /* mean + threshold = 80: slices 0 and 1 qualify, slice 1 is strongest. */
    const auto peak = search::find_peak(slices, 3, /*mean*/ 30, /*threshold*/ 50);
    CHECK_EQ(static_cast<int>(peak.power_max), 200);
    CHECK_EQ(static_cast<int>(peak.bin_max), 150);
    CHECK_EQ(peak.slice_max, uint32_t{1});
    CHECK_EQ(static_cast<int>(peak.overall_power_max), 200);
}

TEST(search_find_peak_reports_no_peak_below_threshold_but_keeps_overall_max) {
    search::Slice slices[3]{};
    slices[0] = {0, 100, 140};
    slices[1] = {0, 200, 150};
    slices[2] = {0, 50, 160};

    /* mean + threshold = 230: nothing clears it. */
    const auto peak = search::find_peak(slices, 3, /*mean*/ 180, /*threshold*/ 50);
    CHECK_EQ(static_cast<int>(peak.bin_max), -1);
    CHECK_EQ(static_cast<int>(peak.overall_power_max), 200);
}

/* --- Slice planning -------------------------------------------------------- */

TEST(search_plan_single_slice_for_a_narrow_range) {
    const auto plan = search::plan_slices(100'000'000, 101'000'000);
    CHECK_EQ(plan.slices_nb, 1);
    CHECK(!plan.overflow);
    CHECK_EQ(plan.center[0], uint64_t{100'500'000});
}

TEST(search_plan_wide_range_matches_upstream_worked_example) {
    /* 100M~115M (15M span): 6 slices, first centre 101.25M, 2.5M apart. */
    const auto plan = search::plan_slices(100'000'000, 115'000'000);
    CHECK_EQ(plan.slices_nb, 6);
    CHECK(!plan.overflow);
    CHECK_EQ(plan.center[0], uint64_t{101'250'000});
    CHECK_EQ(plan.center[1], uint64_t{103'750'000});
    CHECK_EQ(plan.center[5], uint64_t{113'750'000});
}

TEST(search_plan_clamps_to_thirty_two_slices) {
    /* 200 MHz span → 80 slices requested, clamped to 32. */
    const auto plan = search::plan_slices(100'000'000, 300'000'000);
    CHECK_EQ(plan.slices_nb, 32);
    CHECK(plan.overflow);
}

/* --- Lock / release state machine ------------------------------------------ */

namespace {

/* Runs a stable peak through the detector until it locks (or gives up), driving
 * the ~10 Hz timer between detections the way the view does. Returns the Lock
 * outcome, or a None outcome if it never locked. */
search::Outcome drive_to_lock(search::Detector& det, int16_t bin, uint32_t slice,
                              uint64_t center, uint64_t fmin, uint64_t fmax,
                              bool snap = false, uint32_t step = 0) {
    /* First detection establishes last_bin (a fresh peak always starts in the
     * "moved" branch). */
    det.do_detection(bin, slice, center, snap, step, fmin, fmax);
    for (int i = 0; i < 20; i++) {
        det.do_timers();
        const auto out = det.do_detection(bin, slice, center, snap, step, fmin, fmax);
        if (out.action == search::Action::Lock || out.out_of_range) return out;
    }
    return {};
}

}  // namespace

TEST(search_detector_locks_after_the_detect_delay) {
    search::Detector det;
    const uint64_t center = 100'000'000;
    const uint64_t rf = search::bin_to_frequency(center, 130);  // 100'019'530

    const auto out = drive_to_lock(det, 130, 0, center, 100'000'000, 200'000'000);

    CHECK_EQ(static_cast<int>(out.action), static_cast<int>(search::Action::Lock));
    CHECK_EQ(out.frequency, rf);
    CHECK(det.locked());
    CHECK_EQ(det.resolved_frequency(), rf);
    CHECK_EQ(det.duration(), uint32_t{0});
}

TEST(search_detector_snaps_the_locked_frequency) {
    search::Detector det;
    const uint64_t center = 100'000'000;
    /* bin 130 → 100'019'530, snapped to 12.5 kHz → 100'012'500 (truncated). */
    const auto out = drive_to_lock(det, 130, 0, center, 100'000'000, 200'000'000,
                                   /*snap*/ true, /*step*/ 12'500);
    CHECK_EQ(static_cast<int>(out.action), static_cast<int>(search::Action::Lock));
    CHECK_EQ(out.frequency, uint64_t{100'012'500});
}

TEST(search_detector_rejects_out_of_range_and_stays_unlocked) {
    search::Detector det;
    const uint64_t center = 100'000'000;  // resolves ~100 MHz
    const auto out = drive_to_lock(det, 130, 0, center, 200'000'000, 300'000'000);

    CHECK(out.out_of_range);
    CHECK_EQ(static_cast<int>(out.action), static_cast<int>(search::Action::None));
    CHECK(!det.locked());
}

TEST(search_detector_moving_peak_resets_the_detect_timer) {
    search::Detector det;
    const uint64_t center = 100'000'000;

    /* Establish a peak and let the detect timer climb a little. */
    det.do_detection(130, 0, center, false, 0, 100'000'000, 200'000'000);
    det.do_timers();
    det.do_detection(130, 0, center, false, 0, 100'000'000, 200'000'000);
    det.do_timers();
    det.do_detection(130, 0, center, false, 0, 100'000'000, 200'000'000);
    CHECK(det.detect_timer() > 0);

    /* Peak jumps far away: the detect timer must reset. */
    det.do_detection(200, 0, center, false, 0, 100'000'000, 200'000'000);
    CHECK_EQ(static_cast<int>(det.detect_timer()), 0);
    CHECK(!det.locked());
}

TEST(search_detector_accumulates_duration_and_releases) {
    search::Detector det;
    const uint64_t center = 100'000'000;
    const uint64_t rf = search::bin_to_frequency(center, 130);

    const auto lock = drive_to_lock(det, 130, 0, center, 100'000'000, 200'000'000);
    CHECK_EQ(static_cast<int>(lock.action), static_cast<int>(search::Action::Lock));

    /* Signal persists: duration climbs while locked. */
    for (int i = 0; i < 10; i++) {
        det.do_timers();
        det.do_detection(130, 0, center, false, 0, 100'000'000, 200'000'000);
    }
    CHECK(det.duration() >= 10u);
    const uint32_t dur_before_release = det.duration();

    /* Signal disappears (bin_max = -1): after the release delay a Release fires
     * carrying the accumulated duration, and the detector unlocks. */
    search::Outcome rel{};
    for (int i = 0; i < 20; i++) {
        det.do_timers();
        const auto out = det.do_detection(-1, 0, 0, false, 0, 100'000'000, 200'000'000);
        if (out.action == search::Action::Release) {
            rel = out;
            break;
        }
    }
    CHECK_EQ(static_cast<int>(rel.action), static_cast<int>(search::Action::Release));
    CHECK_EQ(rel.frequency, rf);
    CHECK(rel.duration >= dur_before_release);
    CHECK(!det.locked());
}

/* --- Results table: strongest / most-recent hits --------------------------- */

TEST(search_results_are_most_recent_first_and_keyed_by_frequency) {
    SearchRecentEntries recent;

    ui::on_packet(recent, uint64_t{100'000'000});
    ui::on_packet(recent, uint64_t{200'000'000});
    ui::on_packet(recent, uint64_t{300'000'000});

    CHECK_EQ(recent.size(), size_t{3});
    CHECK_EQ(recent.front().key(), uint64_t{300'000'000});
    CHECK_EQ(recent.back().key(), uint64_t{100'000'000});
}

TEST(search_results_update_existing_entry_not_duplicate) {
    SearchRecentEntries recent;

    auto& first = ui::on_packet(recent, uint64_t{145'500'000});
    first.set_time("12:00:00");
    first.set_duration(42);

    ui::on_packet(recent, uint64_t{433'000'000});

    /* Re-hearing 145.5 MHz bumps the same row to the front, keeping its data. */
    auto& again = ui::on_packet(recent, uint64_t{145'500'000});
    CHECK_EQ(recent.size(), size_t{2});
    CHECK_EQ(again.key(), uint64_t{145'500'000});
    CHECK_EQ(again.duration, uint32_t{42});
    CHECK_STR_EQ(again.time, "12:00:00");
    CHECK_EQ(recent.front().key(), uint64_t{145'500'000});
}

TEST(search_results_evict_the_oldest_past_the_bound) {
    SearchRecentEntries recent;

    /* Default bound is 64, dropping from the back. */
    for (uint64_t i = 1; i <= 70; i++)
        ui::on_packet(recent, i * 1'000'000);

    CHECK_EQ(recent.size(), size_t{64});
    CHECK_EQ(recent.front().key(), uint64_t{70'000'000});
    CHECK_EQ(recent.back().key(), uint64_t{7'000'000});
    CHECK(ui::find_entry(recent, uint64_t{6'000'000}) == recent.end());
}

/* --- Duration formatting (results row) ------------------------------------- */

TEST(search_format_duration_matches_upstream) {
    CHECK_STR_EQ(search::format_duration(5), "0.5s");
    CHECK_STR_EQ(search::format_duration(15), "1.5s");
    CHECK_STR_EQ(search::format_duration(599), "59.9s");
    CHECK_STR_EQ(search::format_duration(600), "1m0s");
    CHECK_STR_EQ(search::format_duration(665), "1m6s");
    CHECK_STR_EQ(search::format_duration(1200), "2m0s");
}
