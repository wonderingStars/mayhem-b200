/*
 * mayhem-b200 — Audio Test tone-generator and level-meter tests.
 *
 * Expected values are derived from the tone/beep behaviour of the upstream app
 * (external/audio_test + baseband/audio_dma.cpp beep_start) and from first
 * principles (a sine of frequency f played at sample rate r crosses zero 2*f
 * times per second; dBFS = 20*log10(peak)), not from this port's own output.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_audio_test.hpp"

#include <cmath>
#include <vector>

using namespace app;
using namespace app::audio_test_dsp;
using namespace mb200test;

namespace {

int sign_of(float x) { return x > 0.0f ? 1 : (x < 0.0f ? -1 : 0); }

/* Count sign changes across nonzero samples ~= 2 * freq * duration_seconds. */
int zero_crossings(const std::vector<float>& v) {
    int count = 0;
    int prev = 0;
    for (float s : v) {
        const int sg = sign_of(s);
        if (sg != 0) {
            if (prev != 0 && sg != prev) ++count;
            prev = sg;
        }
    }
    return count;
}

float peak_abs(const std::vector<float>& v) {
    float p = 0.0f;
    for (float s : v) p = std::max(p, std::fabs(s));
    return p;
}

}  // namespace

/* ---- tone generator: frequency via zero-crossing count ------------------- */

TEST(audiotest_tone_frequency_1khz_at_48k) {
    AudioTestTone t;
    t.configure(1000, 48000, 0.6f);
    t.reset();

    std::vector<float> buf(48000);  /* exactly 1 second */
    t.render(buf.data(), 48000);

    /* 1000 Hz over 1 s -> ~1000 cycles -> ~2000 sign changes. */
    const int c = zero_crossings(buf);
    CHECK(c >= 1996 && c <= 2004);
}

TEST(audiotest_tone_frequency_2khz_at_48k) {
    AudioTestTone t;
    t.configure(2000, 48000, 0.6f);
    t.reset();

    std::vector<float> buf(48000);
    t.render(buf.data(), 48000);

    /* 2000 Hz over 1 s -> ~4000 sign changes. */
    const int c = zero_crossings(buf);
    CHECK(c >= 3996 && c <= 4004);
}

/* Frequency is relative to the sample rate: 1 kHz at 24 kHz over 24000 samples
 * (also 1 s) must still give ~2000 crossings. */
TEST(audiotest_tone_frequency_scales_with_rate) {
    AudioTestTone t;
    t.configure(1000, 24000, 0.6f);
    t.reset();

    std::vector<float> buf(24000);  /* 1 second at 24 kHz */
    t.render(buf.data(), 24000);

    const int c = zero_crossings(buf);
    CHECK(c >= 1996 && c <= 2004);
}

/* ---- tone generator: amplitude bound ------------------------------------- */

TEST(audiotest_tone_amplitude_bounded) {
    AudioTestTone t;
    t.configure(1000, 48000, 0.6f);
    t.reset();

    std::vector<float> buf(48000);
    t.render(buf.data(), 48000);

    const float peak = peak_abs(buf);
    CHECK(peak <= 0.6f + 1e-4f);  /* never exceeds the configured amplitude */
    CHECK(peak > 0.55f);          /* and actually reaches close to it */
}

TEST(audiotest_tone_amplitude_follows_config) {
    AudioTestTone t;
    t.configure(1000, 48000, 0.25f);
    t.reset();

    std::vector<float> buf(48000);
    t.render(buf.data(), 48000);

    const float peak = peak_abs(buf);
    CHECK(peak <= 0.25f + 1e-4f);
    CHECK(peak > 0.23f);
}

/* freq == 0 is the firmware's divide-by-zero guard: silence, not a crash. */
TEST(audiotest_tone_zero_frequency_is_silent) {
    AudioTestTone t;
    t.configure(0, 48000, 0.6f);
    t.reset();

    std::vector<float> buf(4800);
    t.render(buf.data(), 4800);

    CHECK_EQ(peak_abs(buf), 0.0f);
}

/* Phase is continuous across successive render() calls (no click/reset). */
TEST(audiotest_tone_phase_continuous) {
    AudioTestTone t;
    t.configure(1000, 48000, 0.6f);
    t.reset();

    std::vector<float> a(48000), b(48000);
    t.render(a.data(), 48000);
    t.render(b.data(), 48000);

    /* Two back-to-back seconds should each carry ~2000 crossings. */
    CHECK(zero_crossings(a) >= 1996 && zero_crossings(a) <= 2004);
    CHECK(zero_crossings(b) >= 1996 && zero_crossings(b) <= 2004);
}

/* ---- sample-rate -> frequency band (upstream set_range(v/128, v/2)) ------- */

TEST(audiotest_freq_band_for_rate) {
    CHECK_EQ(freq_min_for_rate(24000), 187u);
    CHECK_EQ(freq_max_for_rate(24000), 12000u);
    CHECK_EQ(freq_min_for_rate(48000), 375u);
    CHECK_EQ(freq_max_for_rate(48000), 24000u);
    CHECK_EQ(freq_min_for_rate(12000), 93u);
    CHECK_EQ(freq_max_for_rate(12000), 6000u);
}

/* ---- duration ms -> samples ---------------------------------------------- */

TEST(audiotest_duration_samples) {
    CHECK_EQ(duration_samples(100, 48000), 4800u);
    CHECK_EQ(duration_samples(0, 48000), 0u);
    CHECK_EQ(duration_samples(60000, 48000), 2880000u);
    CHECK_EQ(duration_samples(50, 44100), 2205u);
    CHECK_EQ(duration_samples(1, 48000), 48u);
}

/* ---- level meter: linear peak -> dBFS ------------------------------------ */

TEST(audiotest_peak_to_dbfs) {
    CHECK_NEAR(peak_to_dbfs(1.0f), 0.0f, 0.01);
    CHECK_NEAR(peak_to_dbfs(0.5f), -6.0206f, 0.01);
    CHECK_NEAR(peak_to_dbfs(0.1f), -20.0f, 0.01);
    CHECK_NEAR(peak_to_dbfs(0.25f), -12.0412f, 0.01);
}

TEST(audiotest_peak_to_dbfs_clamped) {
    /* Silence pins to the floor, not -inf. */
    CHECK_NEAR(peak_to_dbfs(0.0f), -60.0f, 0.001);
    /* Below-floor peaks also pin to the floor. */
    CHECK_NEAR(peak_to_dbfs(0.0005f), -60.0f, 0.001);
    /* Over-full-scale peaks pin to 0 dBFS. */
    CHECK_NEAR(peak_to_dbfs(2.0f), 0.0f, 0.001);
    /* Custom floor is honoured. */
    CHECK_NEAR(peak_to_dbfs(0.0f, -80.0f), -80.0f, 0.001);
}

/* ---- level meter: dBFS -> 0..255 bar ------------------------------------- */

TEST(audiotest_dbfs_to_bar255) {
    CHECK_EQ(dbfs_to_bar255(0.0f, -60.0f), (uint8_t)255);
    CHECK_EQ(dbfs_to_bar255(-60.0f, -60.0f), (uint8_t)0);
    CHECK_EQ(dbfs_to_bar255(-30.0f, -60.0f), (uint8_t)128);  /* frac 0.5 -> 127.5 -> 128 */
    /* frac is computed in float: 0.9f as a double is 0.89999998, so
     * 0.9f * 255 = 229.4999... which lround()s to 229 (not 230). */
    CHECK_EQ(dbfs_to_bar255(-6.0f, -60.0f), (uint8_t)229);
}

TEST(audiotest_dbfs_to_bar255_clamped) {
    CHECK_EQ(dbfs_to_bar255(-70.0f, -60.0f), (uint8_t)0);   /* below floor pins low */
    CHECK_EQ(dbfs_to_bar255(10.0f, -60.0f), (uint8_t)255);  /* above 0 dBFS pins high */
    CHECK_EQ(dbfs_to_bar255(0.0f, 0.0f), (uint8_t)0);       /* degenerate floor */
}

/* ---- end-to-end: generated peak reads back at the expected dBFS ----------- */

TEST(audiotest_generated_peak_matches_dbfs) {
    AudioTestTone t;
    t.configure(1000, 48000, 0.5f);
    t.reset();

    std::vector<float> buf(48000);
    t.render(buf.data(), 48000);

    /* A 0.5 amplitude sine peaks at 0.5 -> -6.02 dBFS. */
    const float db = peak_to_dbfs(peak_abs(buf));
    CHECK_NEAR(db, -6.0206f, 0.1);
}
