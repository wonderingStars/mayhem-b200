/*
 * mayhem-b200 — Metronome timing / beep tests.
 *
 * Expected values are derived from the upstream metronome algorithm
 * (firmware/application/external/metronome/ui_metronome.cpp), not from this
 * port's own output:
 *   base_interval = (60 * 1000) / bpm
 *   actual        = (base_interval * 4) / beat_unit
 *   accent when   (beat % beats_per_measure) == 0
 * The sample-domain figures come from beat_seconds = 240/(bpm*beat_unit).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_metronome.hpp"

#include <cmath>
#include <vector>

using namespace app;
using namespace app::metronome_dsp;
using namespace mb200test;

/* ---- beat interval in milliseconds (upstream two-step integer formula) ---- */

TEST(metro_beat_interval_ms_quarter) {
    CHECK_EQ(beat_interval_ms(120, 4), 500u);   /* 60000/120=500, *4/4 */
    CHECK_EQ(beat_interval_ms(60, 4), 1000u);
    CHECK_EQ(beat_interval_ms(240, 4), 250u);
}

TEST(metro_beat_interval_ms_denominator) {
    CHECK_EQ(beat_interval_ms(120, 8), 250u);   /* eighth notes: half a beat */
    CHECK_EQ(beat_interval_ms(120, 2), 1000u);  /* half notes: double a beat */
    CHECK_EQ(beat_interval_ms(120, 16), 125u);
}

TEST(metro_beat_interval_ms_truncates_like_upstream) {
    /* 60000/90 = 666 (integer), then *4/4 = 666. */
    CHECK_EQ(beat_interval_ms(90, 4), 666u);
    /* 60000/7 = 8571, *4/4. */
    CHECK_EQ(beat_interval_ms(7, 4), 8571u);
}

TEST(metro_beat_interval_ms_edge) {
    CHECK_EQ(beat_interval_ms(0, 4), 0u);       /* guard: no divide by zero */
    CHECK_EQ(beat_interval_ms(120, 0), 2000u);  /* beat_unit clamped to 1 */
}

/* ---- samples per beat: 240 * rate / (bpm * beat_unit) --------------------- */

TEST(metro_samples_per_beat_48k) {
    CHECK_EQ(samples_per_beat(120, 4, 48000), 24000u);  /* 0.5 s */
    CHECK_EQ(samples_per_beat(60, 4, 48000), 48000u);   /* 1.0 s */
    CHECK_EQ(samples_per_beat(120, 8, 48000), 12000u);  /* eighths */
    CHECK_EQ(samples_per_beat(90, 4, 48000), 32000u);   /* exact, unlike ms */
}

TEST(metro_samples_per_beat_other_rate) {
    CHECK_EQ(samples_per_beat(120, 4, 44100), 22050u);
    CHECK_EQ(samples_per_beat(60, 2, 8000), 16000u);
}

TEST(metro_samples_per_beat_edge) {
    CHECK_EQ(samples_per_beat(0, 4, 48000), 0u);
    CHECK_EQ(samples_per_beat(120, 0, 48000), 96000u);  /* beat_unit clamped 1 */
}

/* ---- beep length --------------------------------------------------------- */

TEST(metro_beep_samples) {
    CHECK_EQ(beep_samples(100, 48000), 4800u);
    CHECK_EQ(beep_samples(10, 48000), 480u);
    CHECK_EQ(beep_samples(0, 48000), 0u);
    CHECK_EQ(beep_samples(250, 44100), 11025u);
}

/* ---- accent pattern ------------------------------------------------------ */

TEST(metro_is_accent_common_time) {
    CHECK(is_accent(0, 4));
    CHECK(!is_accent(1, 4));
    CHECK(!is_accent(2, 4));
    CHECK(!is_accent(3, 4));
    CHECK(is_accent(4, 4));
    CHECK(is_accent(8, 4));
    CHECK(!is_accent(9, 4));
}

TEST(metro_is_accent_triple_time) {
    CHECK(is_accent(0, 3));
    CHECK(!is_accent(1, 3));
    CHECK(!is_accent(2, 3));
    CHECK(is_accent(3, 3));
}

TEST(metro_is_accent_edge) {
    CHECK(is_accent(0, 1));  /* one beat per measure: every beat accented */
    CHECK(is_accent(7, 1));
    CHECK(is_accent(0, 0));  /* clamp to 1 */
    CHECK(is_accent(5, 0));
}

/* ---- tap tempo ----------------------------------------------------------- */

TEST(metro_bpm_from_interval) {
    CHECK_EQ(bpm_from_interval_ms(500), (uint16_t)120);
    CHECK_EQ(bpm_from_interval_ms(1000), (uint16_t)60);
    CHECK_EQ(bpm_from_interval_ms(250), (uint16_t)240);
    CHECK_EQ(bpm_from_interval_ms(0), (uint16_t)0);  /* guard */
}

/* ---- engine: beat length, beep placement, amplitude ---------------------- */

namespace {

int sign_of(float x) { return x > 0.0f ? 1 : (x < 0.0f ? -1 : 0); }

/* Count sign changes across nonzero samples ~= 2 * freq * duration. */
int zero_crossings(const std::vector<float>& v, size_t begin, size_t end) {
    int count = 0;
    int prev = 0;
    for (size_t i = begin; i < end && i < v.size(); ++i) {
        const int s = sign_of(v[i]);
        if (s != 0) {
            if (prev != 0 && s != prev) ++count;
            prev = s;
        }
    }
    return count;
}

}  // namespace

TEST(metro_engine_one_beat_length) {
    MetronomeEngine e;
    e.configure(120, 4, 4, 880, 440, 100, 48000);
    e.reset();

    std::vector<float> buf(24000, -99.0f);
    const uint32_t beats = e.render(buf.data(), 24000);

    CHECK_EQ(beats, 1u);
    CHECK_EQ(e.beat_index(), 1u);

    /* Tone in the first 4800 samples, silence after. */
    bool tone_present = false;
    for (size_t i = 0; i < 4800; ++i)
        if (buf[i] != 0.0f) tone_present = true;
    CHECK(tone_present);

    bool silent_after = true;
    for (size_t i = 4800; i < 24000; ++i)
        if (buf[i] != 0.0f) silent_after = false;
    CHECK(silent_after);
}

TEST(metro_engine_amplitude_bounded) {
    MetronomeEngine e;
    e.configure(120, 4, 4, 880, 440, 100, 48000);
    e.reset();

    std::vector<float> buf(24000);
    e.render(buf.data(), 24000);

    float peak = 0.0f;
    for (float s : buf) peak = std::max(peak, std::fabs(s));
    CHECK(peak <= 0.6f + 1e-4f);
    CHECK(peak > 0.3f);  /* the beep is actually audible, not near-silent */
}

TEST(metro_engine_render_counts_multiple_beats) {
    MetronomeEngine e;
    e.configure(120, 4, 4, 880, 440, 100, 48000);
    e.reset();

    std::vector<float> buf(72000);
    const uint32_t beats = e.render(buf.data(), 72000);
    CHECK_EQ(beats, 3u);
    CHECK_EQ(e.beat_index(), 3u);
}

/* Accent beat uses accent_hz, other beats use unaccent_hz — checked by the
 * frequency of the beep (via zero crossings) and the accent flag. */
TEST(metro_engine_accent_frequency) {
    MetronomeEngine e;
    e.configure(120, 4, 4, 880, 440, 100, 48000);
    e.reset();

    std::vector<float> beat0(24000);
    e.render(beat0.data(), 24000);
    CHECK(e.current_is_accent());  /* beat 0 is the downbeat */
    const int cross0 = zero_crossings(beat0, 0, 4800);
    /* 880 Hz over 0.1 s -> ~88 cycles -> ~175 sign changes. */
    CHECK(cross0 >= 170 && cross0 <= 182);

    std::vector<float> beat1(24000);
    e.render(beat1.data(), 24000);
    CHECK(!e.current_is_accent());  /* beat 1 */
    const int cross1 = zero_crossings(beat1, 0, 4800);
    /* 440 Hz over 0.1 s -> ~44 cycles -> ~87 sign changes. */
    CHECK(cross1 >= 82 && cross1 <= 94);
}

TEST(metro_engine_accent_pattern_over_measure) {
    MetronomeEngine e;
    e.configure(100, 4, 4, 880, 440, 50, 48000);
    e.reset();

    std::vector<float> buf(30000);
    const uint32_t spb = samples_per_beat(100, 4, 48000);  /* 28800 */
    bool expected_accent[5] = {true, false, false, false, true};
    for (int b = 0; b < 5; ++b) {
        e.render(buf.data(), spb);
        CHECK_EQ(e.current_is_accent(), expected_accent[b]);
    }
    CHECK_EQ(e.beat_index(), 5u);
    CHECK_EQ(e.beat_in_measure(), 1u);  /* next beat is index 5 -> 5%4 = 1 */
}
