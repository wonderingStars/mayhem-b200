/*
 * mayhem-b200 — Level app logic tests.
 *
 * Covers the pure scaling maths: upstream LevelView::map() (verified against
 * the exact call the original makes for its level->beep pitch), and the host
 * dBFS-to-bar / dBFS-to-percent mappings that drive the meter, including their
 * clamp/degenerate paths. The live RF/channel readings themselves require a
 * USRP and are not exercised here (no hardware).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_level.hpp"

using namespace app;

/* --- level_map_range (upstream LevelView::map) --------------------------- */

TEST(map_range_endpoints_are_exact) {
    /* Upstream's own use: map(max_db, -100, 20, 400, 2600) for the beep pitch. */
    CHECK_EQ(level_map_range(-100, -100, 20, 400, 2600), 400);
    CHECK_EQ(level_map_range(20, -100, 20, 400, 2600), 2600);
}

TEST(map_range_interpolates_linearly) {
    /* v=-40: 400 + (60)*(2200)/120 = 400 + 1100 = 1500. */
    CHECK_EQ(level_map_range(-40, -100, 20, 400, 2600), 1500);
    /* Simple identity remap. */
    CHECK_EQ(level_map_range(5, 0, 10, 0, 100), 50);
    CHECK_EQ(level_map_range(0, 0, 10, 0, 100), 0);
    CHECK_EQ(level_map_range(10, 0, 10, 0, 100), 100);
}

TEST(map_range_guards_zero_width_input) {
    /* Upstream would divide by zero; the host returns out_low. */
    CHECK_EQ(level_map_range(50, 10, 10, 400, 2600), 400);
}

/* --- level_db_to_bar255 -------------------------------------------------- */

TEST(bar255_hits_endpoints) {
    CHECK_EQ(level_db_to_bar255(-100.0f, -100.0f, 0.0f), (uint8_t)0);
    CHECK_EQ(level_db_to_bar255(0.0f, -100.0f, 0.0f), (uint8_t)255);
}

TEST(bar255_midpoint_rounds) {
    /* -50 dBFS is the midpoint of [-100, 0]: 0.5 * 255 = 127.5 -> 128. */
    CHECK_EQ(level_db_to_bar255(-50.0f, -100.0f, 0.0f), (uint8_t)128);
    /* -75 dBFS: 0.25 * 255 = 63.75 -> 64. */
    CHECK_EQ(level_db_to_bar255(-75.0f, -100.0f, 0.0f), (uint8_t)64);
}

TEST(bar255_clamps_out_of_range) {
    CHECK_EQ(level_db_to_bar255(-140.0f, -100.0f, 0.0f), (uint8_t)0);
    CHECK_EQ(level_db_to_bar255(20.0f, -100.0f, 0.0f), (uint8_t)255);
}

TEST(bar255_guards_degenerate_range) {
    /* ceil <= floor must not divide by zero or wrap. */
    CHECK_EQ(level_db_to_bar255(-50.0f, 0.0f, 0.0f), (uint8_t)0);
    CHECK_EQ(level_db_to_bar255(-50.0f, 0.0f, -100.0f), (uint8_t)0);
}

/* --- level_db_to_percent ------------------------------------------------- */

TEST(percent_endpoints_and_midpoint) {
    CHECK_EQ(level_db_to_percent(-100.0f, -100.0f, 0.0f), 0);
    CHECK_EQ(level_db_to_percent(0.0f, -100.0f, 0.0f), 100);
    CHECK_EQ(level_db_to_percent(-50.0f, -100.0f, 0.0f), 50);
    CHECK_EQ(level_db_to_percent(-25.0f, -100.0f, 0.0f), 75);
}

TEST(percent_clamps_and_guards) {
    CHECK_EQ(level_db_to_percent(-150.0f, -100.0f, 0.0f), 0);
    CHECK_EQ(level_db_to_percent(10.0f, -100.0f, 0.0f), 100);
    CHECK_EQ(level_db_to_percent(-50.0f, 0.0f, 0.0f), 0);  /* degenerate */
}

/* --- display range sanity ------------------------------------------------ */

TEST(level_display_range_is_ordered) {
    /* volatile defeats constant-folding so /W4 does not flag C4127. */
    volatile float floor_db = kLevelFloorDb;
    volatile float ceil_db = kLevelCeilDb;
    CHECK(floor_db < ceil_db);
}
