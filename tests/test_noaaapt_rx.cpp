/*
 * mayhem-b200 — tests for the NOAA APT receiver.
 *
 * Expected values come from upstream (proc_noaaapt_rx.cpp's pixel clock and
 * pixel mapping, AudioOutput::apt_write's envelope detector) and from the APT
 * line format, not from this port's output.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_noaaapt_rx.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace app::noaaapt;

namespace {

constexpr double kPi = 3.14159265358979323846;

/* One APT subcarrier cycle set: amplitude-modulated 2400 Hz at 12 kHz. */
std::vector<float> modulate_subcarrier(const std::vector<float>& amplitudes,
                                       size_t samples_per_level,
                                       double sample_rate = audio_rate_hz,
                                       double tone_hz = subcarrier_hz) {
    std::vector<float> out;
    out.reserve(amplitudes.size() * samples_per_level);
    const double dphi = 2.0 * kPi * tone_hz / sample_rate;
    double phase = 0.0;
    for (float a : amplitudes) {
        for (size_t i = 0; i < samples_per_level; i++) {
            out.push_back(static_cast<float>(a * std::sin(phase)));
            phase += dphi;
            if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
        }
    }
    return out;
}

}  // namespace

/* ------------------------------------------------------------ constants --- */

TEST(apt_line_geometry) {
    /* 2080 words per line at two lines per second is 4160 words per second. */
    CHECK_NEAR(static_cast<double>(px_per_line), 2080.0, 1e-9);
    CHECK_NEAR(pixel_rate_hz, 4160.0, 1e-9);

    /* Upstream's cos/sin constants are those of 2*pi*2400/12000 = 72 degrees;
     * that is the only reason a bare pair of magic numbers works there. */
    CHECK_NEAR(audio_rate_hz / subcarrier_hz, 5.0, 1e-12);
}

/* ------------------------------------------------------------- envelope --- */

TEST(apt_envelope_constants_match_upstream) {
    SubcarrierEnvelope e;
    /* AudioOutput::apt_write: cos_theta 0.30901699437494742410,
     *                         sin_theta 0.95105651629515357212. */
    CHECK_NEAR(e.cos_theta(), 0.30901699437494742410, 1e-6);
    CHECK_NEAR(e.sin_theta(), 0.95105651629515357212, 1e-6);
}

TEST(apt_envelope_recovers_tone_amplitude) {
    /* For x[n] = A sin(wn + p) the two-sample identity is exact, so the
     * detector should return A to within float rounding. */
    for (float amp : {0.15f, 0.4f, 0.75f, 1.0f}) {
        const auto audio = modulate_subcarrier({amp}, 256);
        SubcarrierEnvelope e;
        float last = 0.0f;
        for (size_t i = 0; i < audio.size(); i++) last = e.process(audio[i]);
        CHECK_NEAR(last, amp, 1e-3);
    }
}

TEST(apt_envelope_tracks_a_step) {
    /* Amplitude steps mid-stream: after one sample of settling the detector is
     * back on the new level. Two 128-sample plateaus. */
    const auto audio = modulate_subcarrier({0.2f, 0.8f}, 128);
    SubcarrierEnvelope e;

    std::vector<float> env;
    env.reserve(audio.size());
    for (float s : audio) env.push_back(e.process(s));

    CHECK_NEAR(env[120], 0.2f, 1e-3);   /* well inside the first plateau */
    CHECK_NEAR(env[250], 0.8f, 1e-3);   /* well inside the second */
}

TEST(apt_envelope_of_silence_is_zero_not_nan) {
    SubcarrierEnvelope e;
    for (int i = 0; i < 64; i++) {
        const float v = e.process(0.0f);
        CHECK(!std::isnan(v));
        CHECK_NEAR(v, 0.0f, 1e-9);
    }
}

/* --------------------------------------------------------- pixel mapping -- */

TEST(apt_amplitude_to_pixel_matches_upstream_branches) {
    /* proc_noaaapt_rx: >= 1 -> 255, <= 0 -> 0, otherwise a truncating
     * (uint8_t)(v * 255). */
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(1.0f)), 255);
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(1.7f)), 255);
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(0.0f)), 0);
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(-0.3f)), 0);

    CHECK_EQ(static_cast<int>(amplitude_to_pixel(0.5f)), 127);   /* 127.5 truncated */
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(0.25f)), 63);   /* 63.75 truncated */
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(0.75f)), 191);  /* 191.25 */
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(0.999f)), 254); /* 254.7 */
}

TEST(apt_envelope_to_pixel_end_to_end) {
    /* A grey staircase modulated onto the subcarrier must come back out as the
     * same grey levels. */
    const std::vector<float> levels = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    const auto audio = modulate_subcarrier(levels, 200);

    SubcarrierEnvelope e;
    std::vector<float> env;
    env.reserve(audio.size());
    for (float s : audio) env.push_back(e.process(s));

    for (size_t l = 0; l < levels.size(); l++) {
        const size_t probe = l * 200 + 180;  /* deep inside each plateau */
        const uint8_t px = amplitude_to_pixel(env[probe]);
        const uint8_t expected = amplitude_to_pixel(levels[l]);
        /* One count of slack for the float rounding at the plateau edges. */
        CHECK_NEAR(px, expected, 1);
    }
}

TEST(apt_preview_column_mapping) {
    /* 2080 / 240 is 8 in integer arithmetic, so the last eighth of every line
     * clamps onto column 239. Upstream's behaviour, ported deliberately. */
    CHECK_EQ(preview_column(0), static_cast<uint16_t>(0));
    CHECK_EQ(preview_column(7), static_cast<uint16_t>(0));
    CHECK_EQ(preview_column(8), static_cast<uint16_t>(1));
    CHECK_EQ(preview_column(1919), static_cast<uint16_t>(239));
    CHECK_EQ(preview_column(1920), static_cast<uint16_t>(239));
    CHECK_EQ(preview_column(2079), static_cast<uint16_t>(239));
}

/* ----------------------------------------------------------- pixel clock -- */

TEST(apt_pixel_clock_splits_the_ratio) {
    /* 12000 / 4160 = 2.884615: two whole samples per pixel plus a remainder
     * the accumulator carries. */
    PixelClock c;
    CHECK_EQ(c.samples_per_pixel(), 2u);
    CHECK_NEAR(c.remainder(), 12000.0 / 4160.0 - 2.0, 1e-9);
}

TEST(apt_pixel_clock_emits_one_line_per_half_second) {
    /* One second of 12 kHz audio is exactly two lines, i.e. 4160 pixels. The
     * fractional accumulator is what keeps that exact rather than 6000. */
    PixelClock c;
    int pixels = 0;
    for (int i = 0; i < 12000; i++)
        if (c.tick()) pixels++;
    CHECK_NEAR(pixels, 4160, 2);

    /* And it does not drift over a longer run: ten seconds is 41600. */
    PixelClock c2;
    pixels = 0;
    for (int i = 0; i < 120000; i++)
        if (c2.tick()) pixels++;
    CHECK_NEAR(pixels, 41600, 2);
}

TEST(apt_pixel_clock_alternates_two_and_three) {
    /* With a ratio of 2.88 the clock must produce a mixture of 2- and 3-sample
     * pixels, never a run of one length. */
    PixelClock c;
    int twos = 0, threes = 0, run = 0;
    for (int i = 0; i < 4160 * 3; i++) {
        run++;
        if (c.tick()) {
            if (run == 2)
                twos++;
            else if (run == 3)
                threes++;
            else
                CHECK(false);  /* no other spacing is possible at this ratio */
            run = 0;
        }
    }
    CHECK(twos > 0);
    CHECK(threes > 0);
    /* 0.8846 of pixels take three samples. */
    const double frac = static_cast<double>(threes) / (twos + threes);
    CHECK_NEAR(frac, 12000.0 / 4160.0 - 2.0, 0.01);
}

/* ----------------------------------------------------------- line sync ---- */

TEST(apt_sync_a_pattern_shape) {
    /* 39 words: 4 black, 7 cycles of 2 white + 2 black, 7 black. */
    const auto p = sync_a_pattern();
    CHECK_EQ(p.size(), static_cast<size_t>(39));

    int whites = 0;
    for (int8_t v : p) {
        CHECK(v == 1 || v == -1);
        if (v == 1) whites++;
    }
    CHECK_EQ(whites, 14);  /* 7 cycles x 2 white */

    for (int i = 0; i < 4; i++) CHECK_EQ(static_cast<int>(p[i]), -1);
    CHECK_EQ(static_cast<int>(p[4]), 1);
    CHECK_EQ(static_cast<int>(p[5]), 1);
    CHECK_EQ(static_cast<int>(p[6]), -1);
    for (int i = 32; i < 39; i++) CHECK_EQ(static_cast<int>(p[i]), -1);
}

namespace {

/* A synthetic APT line: mid-grey video with a sync-A burst starting at
 * `sync_at`. Video is deliberately not black so a correlator that keys on
 * absolute level rather than shape would fail. */
std::vector<uint8_t> make_line(size_t sync_at, uint8_t video = 128) {
    std::vector<uint8_t> line(px_per_line, video);
    const auto pattern = sync_a_pattern();
    for (size_t i = 0; i < pattern.size(); i++)
        line[(sync_at + i) % line.size()] = (pattern[i] > 0) ? 255 : 0;
    return line;
}

}  // namespace

TEST(apt_sync_detector_locates_the_burst) {
    for (size_t offset : {size_t{0}, size_t{37}, size_t{500}, size_t{1500}}) {
        const auto line = make_line(offset);

        SyncDetector d;
        bool fired = false;
        uint64_t start = 0;
        for (size_t i = 0; i < line.size(); i++) {
            if (d.process(line[i])) {
                fired = true;
                start = d.last_sync_start();
                break;
            }
        }
        CHECK(fired);
        CHECK_EQ(start, static_cast<uint64_t>(offset));
    }
}

TEST(apt_sync_detector_is_indifferent_to_gain_and_offset) {
    /* Normalised correlation: a low-contrast, DC-shifted burst still matches. */
    std::vector<uint8_t> line(px_per_line, 100);
    const auto pattern = sync_a_pattern();
    for (size_t i = 0; i < pattern.size(); i++)
        line[300 + i] = (pattern[i] > 0) ? static_cast<uint8_t>(140)
                                         : static_cast<uint8_t>(110);

    SyncDetector d;
    bool fired = false;
    for (size_t i = 0; i < line.size(); i++) {
        if (d.process(line[i])) {
            CHECK_EQ(d.last_sync_start(), static_cast<uint64_t>(300));
            fired = true;
            break;
        }
    }
    CHECK(fired);
}

TEST(apt_sync_detector_ignores_a_line_without_a_burst) {
    /* A smooth ramp has no 1040 Hz structure at all. */
    std::vector<uint8_t> line(px_per_line);
    for (size_t i = 0; i < line.size(); i++)
        line[i] = static_cast<uint8_t>((i * 255) / px_per_line);

    SyncDetector d;
    for (size_t i = 0; i < line.size(); i++) CHECK(!d.process(line[i]));
}

TEST(apt_sync_detector_ignores_a_flat_line) {
    /* Zero variance: the correlation is undefined and must not fire. */
    std::vector<uint8_t> line(px_per_line, 200);
    SyncDetector d;
    for (size_t i = 0; i < line.size(); i++) CHECK(!d.process(line[i]));
}

TEST(apt_sync_detector_holds_off_for_most_of_a_line) {
    /* Two consecutive lines, each with a burst at the same phase: exactly two
     * detections, one line apart. The refractory period stops the burst from
     * re-triggering on its own tail. */
    std::vector<uint8_t> stream;
    for (int l = 0; l < 3; l++) {
        const auto line = make_line(40);
        stream.insert(stream.end(), line.begin(), line.end());
    }

    SyncDetector d;
    std::vector<uint64_t> starts;
    for (size_t i = 0; i < stream.size(); i++)
        if (d.process(stream[i])) starts.push_back(d.last_sync_start());

    CHECK_EQ(starts.size(), static_cast<size_t>(3));
    if (starts.size() == 3) {
        CHECK_EQ(starts[0], static_cast<uint64_t>(40));
        CHECK_EQ(starts[1], static_cast<uint64_t>(40 + px_per_line));
        CHECK_EQ(starts[2], static_cast<uint64_t>(40 + 2 * px_per_line));
    }
}

TEST(apt_sync_detector_returns_the_pixels_after_the_burst) {
    /* Peak picking reports the match a few pixels late, so the view needs those
     * pixels back to open the new line without a hole in it. */
    const auto line = make_line(600);

    SyncDetector d;
    for (size_t i = 0; i < line.size(); i++) {
        if (d.process(line[i])) {
            const size_t n = d.pattern_size();
            const size_t expected =
                static_cast<size_t>(d.pixels_seen() - (d.last_sync_start() + n));
            std::vector<uint8_t> tail(64, 0);
            const size_t got = d.copy_since_sync(tail.data(), tail.size());
            CHECK_EQ(got, expected);
            for (size_t k = 0; k < got; k++)
                CHECK_EQ(static_cast<int>(tail[k]), static_cast<int>(line[600 + n + k]));
            return;
        }
    }
    CHECK(false);  /* never matched */
}

TEST(apt_sync_detector_window_is_recoverable) {
    /* The view copies the matched window back out to open the new line with the
     * sync burst it just consumed. */
    const auto line = make_line(600);

    SyncDetector d;
    for (size_t i = 0; i < line.size(); i++) {
        if (d.process(line[i])) {
            std::vector<uint8_t> w(d.pattern_size());
            d.copy_window(w.data());
            const auto pattern = sync_a_pattern();
            for (size_t k = 0; k < w.size(); k++)
                CHECK_EQ(static_cast<int>(w[k]), (pattern[k] > 0) ? 255 : 0);
            return;
        }
    }
    CHECK(false);  /* never matched */
}
