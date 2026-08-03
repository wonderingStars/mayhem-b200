/*
 * mayhem-b200 — Analog TV line-timing / sync-detection / framing tests.
 *
 * Expected values are derived from broadcast-TV specifications, not from this
 * port's own output:
 *   - PAL/SECAM line rate 15625 Hz (625 lines * 25 fps).
 *   - NTSC-M line rate 15734.264 Hz (525 lines * 30/1.001 fps).
 *   - horizontal sync pulse ~4.7 us for both.
 *   - upstream analogtv fixes "128 samples == 2 lines" at 2 MHz for PAL, which
 *     samples_per_line(2e6, 15625) must reproduce exactly.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_analog_tv.hpp"

#include <vector>

using namespace app::analogtv;
using namespace mb200test;

/* ---- samples per line (line-timing maths) --------------------------------- */

TEST(tv_samples_per_line_pal_matches_upstream) {
    /* Upstream's hard-coded figure: 2 MHz / PAL => exactly 128 samples/line. */
    CHECK_NEAR(samples_per_line(2'000'000.0, kLineRatePAL), 128.0, 1e-9);
    CHECK_NEAR(samples_per_line(6'000'000.0, kLineRatePAL), 384.0, 1e-9);
    CHECK_NEAR(samples_per_line(5'000'000.0, kLineRatePAL), 320.0, 1e-9);
}

TEST(tv_samples_per_line_ntsc) {
    /* 2e6 / 15734.264 = 127.111... */
    CHECK_NEAR(samples_per_line(2'000'000.0, kLineRateNTSC), 127.111, 0.01);
    /* 12.272727e6 / 15734.264 ~= 780 not needed; check a clean multiple. */
    CHECK_NEAR(samples_per_line(15734.264 * 200.0, kLineRateNTSC), 200.0, 1e-6);
}

TEST(tv_samples_per_line_guards) {
    CHECK_NEAR(samples_per_line(2'000'000.0, 0.0), 0.0, 1e-12);
    CHECK_NEAR(samples_per_line(0.0, kLineRatePAL), 0.0, 1e-12);
}

/* ---- horizontal sync minimum run length ----------------------------------- */

TEST(tv_hsync_min_samples) {
    /* 4.7us * 0.6 * rate, rounded. */
    CHECK_EQ(hsync_min_samples(2'000'000.0), (size_t)6);   /* 5.64 -> 6 */
    CHECK_EQ(hsync_min_samples(10'000'000.0), (size_t)28); /* 28.2 -> 28 */
}

TEST(tv_hsync_min_samples_floor_is_one) {
    /* Very low rates give a sub-sample pulse; clamp to 1. */
    CHECK_EQ(hsync_min_samples(100'000.0), (size_t)1);  /* 0.282 -> 1 */
    CHECK_EQ(hsync_min_samples(1.0), (size_t)1);
}

/* ---- horizontal sync detection on a synthetic signal ---------------------- */

namespace {

/* Builds `lines` scan lines of `spl` samples in display polarity: a sync tip of
 * `sync_w` low samples (0.05) followed by video (0.8). */
std::vector<float> make_lines(size_t lines, size_t spl, size_t sync_w) {
    std::vector<float> v;
    v.reserve(lines * spl);
    for (size_t l = 0; l < lines; ++l) {
        for (size_t i = 0; i < spl; ++i)
            v.push_back(i < sync_w ? 0.05f : 0.8f);
    }
    return v;
}

}  // namespace

TEST(tv_detect_hsync_finds_line_starts) {
    const auto sig = make_lines(5, 128, 10);
    const auto edges = detect_hsync(sig.data(), sig.size(), 0.18f, 6);

    CHECK_EQ(edges.size(), (size_t)5);
    if (edges.size() == 5) {
        CHECK_EQ(edges[0], (size_t)0);
        CHECK_EQ(edges[1], (size_t)128);
        CHECK_EQ(edges[2], (size_t)256);
        CHECK_EQ(edges[3], (size_t)384);
        CHECK_EQ(edges[4], (size_t)512);
    }
}

TEST(tv_detect_hsync_ignores_short_dips) {
    auto sig = make_lines(3, 128, 10);
    /* Poke a 3-sample dip into the middle of line 0's video — below min_run. */
    sig[60] = 0.05f;
    sig[61] = 0.05f;
    sig[62] = 0.05f;

    const auto edges = detect_hsync(sig.data(), sig.size(), 0.18f, 6);
    CHECK_EQ(edges.size(), (size_t)3);  /* only the three real sync tips */
}

TEST(tv_detect_hsync_run_at_end_counts) {
    /* Signal ending inside a sync run still registers that leading edge. */
    std::vector<float> v(100, 0.9f);
    for (size_t i = 90; i < 100; ++i) v[i] = 0.05f;
    const auto edges = detect_hsync(v.data(), v.size(), 0.18f, 6);
    CHECK_EQ(edges.size(), (size_t)1);
    if (!edges.empty()) CHECK_EQ(edges[0], (size_t)90);
}

TEST(tv_detect_hsync_threshold_respected) {
    /* Raise the "video" level below threshold and it all becomes one pulse;
     * lower the threshold and nothing is a pulse. */
    std::vector<float> v(200, 0.30f);
    CHECK_EQ(detect_hsync(v.data(), v.size(), 0.18f, 6).size(), (size_t)0);
    CHECK_EQ(detect_hsync(v.data(), v.size(), 0.40f, 6).size(), (size_t)1);
}

TEST(tv_detect_hsync_empty) {
    CHECK_EQ(detect_hsync(nullptr, 0, 0.18f, 6).size(), (size_t)0);
}

/* ---- line framer (slicing + x-correction + anti-drift) -------------------- */

namespace {

struct Collected {
    std::vector<size_t> lengths;
    std::vector<float> first_samples;
};

Collected run_framer(LineFramer& f, const std::vector<float>& in) {
    Collected c;
    f.feed(in.data(), in.size(),
           [&c](const float* line, size_t len) {
               c.lengths.push_back(len);
               c.first_samples.push_back(len ? line[0] : -1.0f);
           });
    return c;
}

}  // namespace

TEST(tv_framer_slices_fixed_lines) {
    LineFramer f;
    f.configure(128.0, 0);

    /* 512 samples where sample i has value i, so a line's first sample is its
     * start index. */
    std::vector<float> in(512);
    for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(i);

    const auto c = run_framer(f, in);
    CHECK_EQ(c.lengths.size(), (size_t)4);
    for (size_t len : c.lengths) CHECK_EQ(len, (size_t)128);
    if (c.first_samples.size() == 4) {
        CHECK_NEAR(c.first_samples[0], 0.0f, 1e-6);
        CHECK_NEAR(c.first_samples[1], 128.0f, 1e-6);
        CHECK_NEAR(c.first_samples[2], 256.0f, 1e-6);
        CHECK_NEAR(c.first_samples[3], 384.0f, 1e-6);
    }
}

TEST(tv_framer_applies_x_correction) {
    LineFramer f;
    f.configure(128.0, 5);  /* drop 5 samples before the first line */

    std::vector<float> in(512);
    for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(i);

    const auto c = run_framer(f, in);
    /* 512 - 5 = 507 usable -> 3 full lines of 128. */
    CHECK_EQ(c.lengths.size(), (size_t)3);
    if (!c.first_samples.empty()) CHECK_NEAR(c.first_samples[0], 5.0f, 1e-6);
}

TEST(tv_framer_streams_across_feeds) {
    LineFramer f;
    f.configure(128.0, 0);

    std::vector<float> a(200, 1.0f);
    std::vector<float> b(200, 1.0f);
    Collected c;
    auto emit = [&c](const float*, size_t len) { c.lengths.push_back(len); };
    f.feed(a.data(), a.size(), emit);  /* 200 -> 1 line, 72 buffered */
    CHECK_EQ(c.lengths.size(), (size_t)1);
    f.feed(b.data(), b.size(), emit);  /* 72+200=272 -> 2 more lines */
    CHECK_EQ(c.lengths.size(), (size_t)3);
    for (size_t len : c.lengths) CHECK_EQ(len, (size_t)128);
}

TEST(tv_framer_fractional_spl_no_drift) {
    LineFramer f;
    f.configure(127.111, 0);  /* NTSC-ish */

    std::vector<float> in(12800, 0.5f);
    const auto c = run_framer(f, in);

    /* Every line is either floor or ceil of spl. */
    for (size_t len : c.lengths) CHECK(len == 127 || len == 128);

    size_t total = 0;
    for (size_t len : c.lengths) total += len;
    const double avg = c.lengths.empty()
                           ? 0.0
                           : static_cast<double>(total) / c.lengths.size();
    CHECK_EQ(c.lengths.size(), (size_t)100);
    CHECK_NEAR(avg, 127.111, 0.05);
}

TEST(tv_framer_peek_next_length) {
    LineFramer f;
    f.configure(127.5, 0);
    /* First line takes floor (127) since frac starts at 0 and 0+0.5 < 1. */
    CHECK_EQ(f.peek_next_length(), (size_t)127);
    CHECK_EQ(f.nominal_line_length(), (size_t)128);  /* round(127.5) */
}

/* ---- envelope -> pixel mapping -------------------------------------------- */

TEST(tv_line_to_pixels_nearest_neighbour) {
    /* Two source samples spread over four pixels: [0,0,1,1] mapping. */
    const float env[2] = {0.0f, 1.0f};
    ui::Color out[4];
    line_to_pixels(env, 2, 0.0f, 1.0f, out, 4);

    CHECK_EQ(out[0].v, (uint16_t)0);          /* black */
    CHECK_EQ(out[0].v, out[1].v);             /* same source sample */
    CHECK_EQ(out[2].v, out[3].v);             /* same source sample */
    CHECK_EQ(out[3].v, ui::Color::white().v); /* white */
    CHECK(out[2].v > out[0].v);
}

TEST(tv_line_to_pixels_monotonic_and_clamped) {
    const float env[3] = {0.0f, 0.5f, 2.0f};  /* 2.0 is out of range */
    ui::Color out[3];
    line_to_pixels(env, 3, 0.0f, 1.0f, out, 3);

    CHECK_EQ(out[0].v, (uint16_t)0);
    CHECK(out[1].v > out[0].v);
    CHECK(out[2].v > out[1].v);
    CHECK_EQ(out[2].v, ui::Color::white().v);  /* clamped to full white */
}

TEST(tv_line_to_pixels_uses_lo_hi_window) {
    const float env[2] = {10.0f, 20.0f};
    ui::Color out[2];
    line_to_pixels(env, 2, 10.0f, 20.0f, out, 2);
    CHECK_EQ(out[0].v, (uint16_t)0);
    CHECK_EQ(out[1].v, ui::Color::white().v);
}
