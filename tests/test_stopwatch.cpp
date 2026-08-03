/*
 * mayhem-b200 — Stopwatch elapsed-time formatting tests.
 *
 * Durations are injected directly (milliseconds), so nothing waits on a real
 * clock. Expected strings are computed by hand from the definition:
 *   hours   = ms / 3600000
 *   minutes = (ms / 60000) % 60
 *   seconds = (ms / 1000)  % 60
 *   centis  = (ms / 10)    % 100   (truncated hundredths)
 * with the hour field shown only once the hour rolls over.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_stopwatch.hpp"

using namespace app::stopwatch_fmt;
using namespace mb200test;

/* ---- decompose ----------------------------------------------------------- */

TEST(sw_decompose_zero) {
    const HMSC t = decompose_ms(0);
    CHECK_EQ(t.hours, 0u);
    CHECK_EQ(t.minutes, 0u);
    CHECK_EQ(t.seconds, 0u);
    CHECK_EQ(t.centis, 0u);
}

TEST(sw_decompose_mixed) {
    const HMSC t = decompose_ms(65432);  /* 1 min 5.432 s */
    CHECK_EQ(t.hours, 0u);
    CHECK_EQ(t.minutes, 1u);
    CHECK_EQ(t.seconds, 5u);
    CHECK_EQ(t.centis, 43u);  /* truncated, not rounded */
}

TEST(sw_decompose_with_hours) {
    const HMSC t = decompose_ms(3661500);  /* 1 h 1 min 1.5 s */
    CHECK_EQ(t.hours, 1u);
    CHECK_EQ(t.minutes, 1u);
    CHECK_EQ(t.seconds, 1u);
    CHECK_EQ(t.centis, 50u);
}

/* ---- format under an hour: MM:SS.CC -------------------------------------- */

TEST(sw_format_zero) {
    CHECK_STR_EQ(format_elapsed(0), "00:00.00");
}

TEST(sw_format_sub_second) {
    CHECK_STR_EQ(format_elapsed(9), "00:00.00");   /* 9 ms -> 0 cs (truncated) */
    CHECK_STR_EQ(format_elapsed(19), "00:00.01");  /* 19 ms -> 1 cs */
    CHECK_STR_EQ(format_elapsed(990), "00:00.99");
    CHECK_STR_EQ(format_elapsed(999), "00:00.99");
}

TEST(sw_format_seconds_and_minutes) {
    CHECK_STR_EQ(format_elapsed(1000), "00:01.00");
    CHECK_STR_EQ(format_elapsed(1050), "00:01.05");
    CHECK_STR_EQ(format_elapsed(65432), "01:05.43");
    CHECK_STR_EQ(format_elapsed(599990), "09:59.99");
    CHECK_STR_EQ(format_elapsed(600000), "10:00.00");
}

TEST(sw_format_just_under_an_hour) {
    /* 59:59.99, still no hour field. */
    CHECK_STR_EQ(format_elapsed(3599990), "59:59.99");
}

/* ---- hours rollover: H:MM:SS.CC ------------------------------------------ */

TEST(sw_format_hour_rollover) {
    CHECK_STR_EQ(format_elapsed(3600000), "1:00:00.00");
    CHECK_STR_EQ(format_elapsed(3661500), "1:01:01.50");
}

TEST(sw_format_multi_digit_hours) {
    CHECK_STR_EQ(format_elapsed(36000000), "10:00:00.00");
    CHECK_STR_EQ(format_elapsed(45296780), "12:34:56.78");
}

/* ---- lap deltas ---------------------------------------------------------- */

TEST(sw_lap_delta_basic) {
    CHECK_STR_EQ(format_lap_delta(0), "+00:00.00");
    CHECK_STR_EQ(format_lap_delta(12340), "+00:12.34");
    CHECK_STR_EQ(format_lap_delta(13340), "+00:13.34");
}

TEST(sw_lap_delta_with_hours) {
    CHECK_STR_EQ(format_lap_delta(3661500), "+1:01:01.50");
}

/* A realistic lap sequence: deltas are consecutive-total differences. */
TEST(sw_lap_delta_sequence) {
    const uint64_t totals[] = {12340, 25680, 40000};
    const char* expected_delta[] = {"+00:12.34", "+00:13.34", "+00:14.32"};
    uint64_t prev = 0;
    for (int i = 0; i < 3; ++i) {
        const uint64_t d = totals[i] - prev;
        CHECK_STR_EQ(format_lap_delta(d), expected_delta[i]);
        prev = totals[i];
    }
}
