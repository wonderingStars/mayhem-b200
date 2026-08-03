/*
 * mayhem-b200 — tests for the antenna length calculator (ui_whipcalc).
 *
 * Values are hand-computed from c/f and the wavelength fraction, and the
 * element-matching results are traced by hand from the upstream algorithm
 * against Mayhem's built-in ANT500 element table.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"
#include "ui_whipcalc.hpp"

#include <vector>

using namespace mb200test;
using namespace app::antenna;

/* Mayhem's default ANT500 telescopic whip, cumulative segment length in mm. */
static const std::vector<uint16_t> ANT500 = {185, 315, 450, 586, 724, 862};

TEST(whipcalc_full_wave_at_c) {
    /* At f = c (in Hz) one full wavelength is exactly 1 metre / 1 foot. */
    CHECK_NEAR(antenna_length_m(299792458ull, 8), 1.0, 1e-9);
    CHECK_NEAR(antenna_length_ft(983571087ull /*~c in ft*/, 8), 1.0, 1e-6);
}

TEST(whipcalc_fractions) {
    /* eighths select the wavelength fraction: 8=full, 4=half, 2=quarter, 1=1/8. */
    const uint64_t f = 299792458ull;  // 1 m full wave
    CHECK_NEAR(antenna_length_m(f, 8), 1.0, 1e-9);
    CHECK_NEAR(antenna_length_m(f, 4), 0.5, 1e-9);
    CHECK_NEAR(antenna_length_m(f, 2), 0.25, 1e-9);
    CHECK_NEAR(antenna_length_m(f, 1), 0.125, 1e-9);
    CHECK_NEAR(antenna_length_m(f, 6), 0.75, 1e-9);
}

TEST(whipcalc_metric_100mhz) {
    /* 100 MHz: full wave = c/f = 2.99792458 m; quarter = 0.749481145 m. */
    CHECK_NEAR(antenna_length_m(100000000ull, 8), 2.99792458, 1e-8);
    CHECK_NEAR(antenna_length_m(100000000ull, 2), 0.749481145, 1e-9);
}

TEST(whipcalc_imperial_100mhz) {
    /* 100 MHz: full wave in feet = 983571087.90472 / 1e8 = 9.8357108790472 ft. */
    CHECK_NEAR(antenna_length_ft(100000000ull, 8), 9.8357108790472, 1e-6);
    CHECK_NEAR(antenna_length_ft(100000000ull, 4), 4.9178554395236, 1e-6);
}

TEST(whipcalc_7mhz_quarter) {
    /* 40 m band: quarter wave at 7 MHz = c/f/4 = 10.70687... m. */
    CHECK_NEAR(antenna_length_m(7000000ull, 2), 10.7068735, 1e-4);
}

TEST(whipcalc_zero_frequency) {
    /* Upstream shows "infinity+"; the helper returns 0 for a 0 Hz input. */
    CHECK_NEAR(antenna_length_m(0, 8), 0.0, 1e-12);
    CHECK_NEAR(antenna_length_ft(0, 4), 0.0, 1e-12);
}

TEST(whipcalc_match_exact_first_element) {
    auto r = match_antenna(185.0, ANT500);
    CHECK_EQ(r.elements, 1);
    CHECK_EQ(r.quarter, 0);
}

TEST(whipcalc_match_exact_second_element) {
    auto r = match_antenna(315.0, ANT500);
    CHECK_EQ(r.elements, 2);
    CHECK_EQ(r.quarter, 0);
}

TEST(whipcalc_match_exact_fourth_element) {
    auto r = match_antenna(586.0, ANT500);
    CHECK_EQ(r.elements, 4);
    CHECK_EQ(r.quarter, 0);
}

TEST(whipcalc_match_half_into_third) {
    /* 380 mm: between 315 and 450. quarter = (380-315)*4/135 = 1.925 -> round to
     * 2/4 of the segment. */
    auto r = match_antenna(380.0, ANT500);
    CHECK_EQ(r.elements, 2);
    CHECK_EQ(r.quarter, 2);
}

TEST(whipcalc_match_threequarter_into_third) {
    /* 400 mm: quarter = (400-315)*4/135 = 2.518 -> rounds up to 3/4. */
    auto r = match_antenna(400.0, ANT500);
    CHECK_EQ(r.elements, 2);
    CHECK_EQ(r.quarter, 3);
}

TEST(whipcalc_match_roll_to_next_element) {
    /* 448 mm: quarter = (448-315)*4/135 = 3.94 -> rounds to 4, which rolls into
     * the next element (quarter resets, element++). */
    auto r = match_antenna(448.0, ANT500);
    CHECK_EQ(r.elements, 3);
    CHECK_EQ(r.quarter, 0);
}

TEST(whipcalc_match_beyond_last_element) {
    /* 900 mm exceeds the fully extended whip (862 mm): all six elements, no
     * fraction. */
    auto r = match_antenna(900.0, ANT500);
    CHECK_EQ(r.elements, 6);
    CHECK_EQ(r.quarter, 0);
}

TEST(whipcalc_match_below_first_element_guarded) {
    /* Shorter than the first segment. Upstream would read elements[-1]; the port
     * guards this with prev=0. quarter = 100*4/185 = 2.16 -> 2/4, element 0. */
    auto r = match_antenna(100.0, ANT500);
    CHECK_EQ(r.elements, 0);
    CHECK_EQ(r.quarter, 2);
}
