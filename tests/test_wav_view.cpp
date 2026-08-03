/*
 * mayhem-b200 — WAV viewer reduction/scale tests.
 *
 * The interesting, hardware-free logic in the WAV viewer is the whole-file
 * overview reduction (peak magnitude per output column), the peak→colour-index
 * mapping, and the ns-per-pixel scale. Expected values are computed directly
 * from the definitions in wav_view_detail (half-open column ranges
 * [count*i/cols, count*(i+1)/cols), 32-bit abs, >>7 index, (1e9/rate)*scale),
 * not from whatever the code emits.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_view_wav.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace app::wav_view_detail;

/* --- peak_columns: even division ------------------------------------------- */

TEST(wav_peak_even_division) {
    const int16_t s[8] = {1, -3, 5, -2, 0, 7, -9, 4};
    uint16_t out[4] = {0, 0, 0, 0};
    peak_columns(s, 8, out, 4);

    /* Each column spans two samples; peak is the larger absolute value. */
    CHECK_EQ(out[0], uint16_t{3});  /* |1|,|-3| */
    CHECK_EQ(out[1], uint16_t{5});  /* |5|,|-2| */
    CHECK_EQ(out[2], uint16_t{7});  /* |0|,|7|  */
    CHECK_EQ(out[3], uint16_t{9});  /* |-9|,|4| */
}

/* --- peak_columns: fewer samples than columns (empty bins) ------------------ */

TEST(wav_peak_sparse_bins) {
    const int16_t s[3] = {10, -20, 30};
    uint16_t out[6] = {9, 9, 9, 9, 9, 9};
    peak_columns(s, 3, out, 6);

    /* lo=(3*i)/6, hi=(3*(i+1))/6 -> bins {}, {0}, {}, {1}, {}, {2}. */
    CHECK_EQ(out[0], uint16_t{0});
    CHECK_EQ(out[1], uint16_t{10});
    CHECK_EQ(out[2], uint16_t{0});
    CHECK_EQ(out[3], uint16_t{20});
    CHECK_EQ(out[4], uint16_t{0});
    CHECK_EQ(out[5], uint16_t{30});
}

/* --- peak_columns: uneven division (10 samples into 3 columns) ------------- */

TEST(wav_peak_uneven_division) {
    /* Values chosen so each range has a distinct, obvious peak. */
    const int16_t s[10] = {1, 2, 3, /*|*/ 40, -5, 6, /*|*/ 7, 8, -99, 10};
    uint16_t out[3] = {0, 0, 0};
    peak_columns(s, 10, out, 3);

    /* i0:[0,3) {1,2,3}=3; i1:[3,6) {40,-5,6}=40; i2:[6,10) {7,8,-99,10}=99. */
    CHECK_EQ(out[0], uint16_t{3});
    CHECK_EQ(out[1], uint16_t{40});
    CHECK_EQ(out[2], uint16_t{99});
}

/* --- peak_columns: INT16_MIN abs is 32768, not a wrapped negative ---------- */

TEST(wav_peak_int16_min) {
    const int16_t s[1] = {-32768};
    uint16_t out[1] = {0};
    peak_columns(s, 1, out, 1);
    CHECK_EQ(out[0], uint16_t{32768});
}

/* --- peak_columns: single column = global peak; degenerate guards ----------- */

TEST(wav_peak_single_column_and_guards) {
    const int16_t s[5] = {4, -8, 15, -16, 23};
    uint16_t out[1] = {0};
    peak_columns(s, 5, out, 1);
    CHECK_EQ(out[0], uint16_t{23});

    /* columns == 0 and out == nullptr must be safe no-ops. */
    uint16_t sentinel = 42;
    peak_columns(s, 5, &sentinel, 0);
    CHECK_EQ(sentinel, uint16_t{42});
    peak_columns(s, 5, nullptr, 4);  /* no crash */
}

/* --- peak_columns: synthetic amplitude ramp -------------------------------- */

TEST(wav_peak_amplitude_ramp) {
    /* 2400 samples, 240 columns -> exactly 10 samples per column. sample[i]=i,
     * so the peak in column c is the last index it covers: 10*c + 9. */
    std::vector<int16_t> s(2400);
    for (size_t i = 0; i < s.size(); i++) s[i] = static_cast<int16_t>(i % 32768);

    std::vector<uint16_t> out(240, 0);
    peak_columns(s.data(), s.size(), out.data(), 240);

    CHECK_EQ(out[0], uint16_t{9});
    CHECK_EQ(out[1], uint16_t{19});
    CHECK_EQ(out[10], uint16_t{109});
    CHECK_EQ(out[239], uint16_t{2399});
}

/* --- peak_columns: synthetic sine ------------------------------------------ */

TEST(wav_peak_sine_envelope) {
    /* A full-scale sine: no column peak may exceed the amplitude, and the
     * column that straddles the crest must reach it (within one sample step). */
    constexpr int amp = 30000;
    std::vector<int16_t> s(4800);
    for (size_t i = 0; i < s.size(); i++)
        s[i] = static_cast<int16_t>(std::lround(amp * std::sin(2.0 * M_PI * i / 480.0)));

    std::vector<uint16_t> out(240, 0);
    peak_columns(s.data(), s.size(), out.data(), 240);

    uint16_t max_peak = 0;
    for (uint16_t v : out) {
        CHECK(v <= amp);
        if (v > max_peak) max_peak = v;
    }
    /* The discrete crest is within a few counts of the ideal amplitude. */
    CHECK(max_peak >= amp - 50);
}

/* --- peak_to_lut_index ----------------------------------------------------- */

TEST(wav_peak_to_lut_index) {
    CHECK_EQ(int{peak_to_lut_index(0)}, 0);
    CHECK_EQ(int{peak_to_lut_index(127)}, 0);     /* 127 >> 7 = 0 */
    CHECK_EQ(int{peak_to_lut_index(128)}, 1);     /* 128 >> 7 = 1 */
    CHECK_EQ(int{peak_to_lut_index(256)}, 2);
    CHECK_EQ(int{peak_to_lut_index(16384)}, 128);
    CHECK_EQ(int{peak_to_lut_index(32768)}, 255); /* 256 clamped to 255 */
    CHECK_EQ(int{peak_to_lut_index(65535)}, 255); /* 511 clamped to 255 */
}

/* --- ns_per_pixel ---------------------------------------------------------- */

TEST(wav_ns_per_pixel) {
    CHECK_EQ(ns_per_pixel(1000000, 1), uint64_t{1000});
    CHECK_EQ(ns_per_pixel(48000, 1), uint64_t{20833});      /* 1e9/48000 truncated */
    CHECK_EQ(ns_per_pixel(48000, 8), uint64_t{20833 * 8});
    CHECK_EQ(ns_per_pixel(0, 4), uint64_t{0});              /* guard */
}
