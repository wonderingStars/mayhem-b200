/*
 * mayhem-b200 — tests for the WeFax receiver.
 *
 * Expected values come from proc_wefaxrx.cpp (pixel clock, pixel thresholds,
 * start-tone constants), common/dsp_sos_config.hpp (filter coefficients and the
 * scipy calls that generated them), baseband/audio_compressor.cpp (ratio,
 * threshold, make-up gain) and the definition of the Index Of Cooperation —
 * never from what this port happens to produce.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_wefax_rx.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace app::wefax;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> tone(double freq_hz, double sample_rate, size_t count, double amplitude) {
    std::vector<float> out;
    out.reserve(count);
    const double dphi = 2.0 * kPi * freq_hz / sample_rate;
    double phase = 0.0;
    for (size_t i = 0; i < count; i++) {
        out.push_back(static_cast<float>(amplitude * std::sin(phase)));
        phase += dphi;
        if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
    }
    return out;
}

double rms(const std::vector<float>& v, size_t from) {
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = from; i < v.size(); i++) {
        acc += static_cast<double>(v[i]) * v[i];
        n++;
    }
    return n ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
}

}  // namespace

/* ------------------------------------------------------- line geometry ---- */

TEST(wefax_line_duration) {
    /* Lines per minute to seconds per line. */
    CHECK_NEAR(line_duration_s(60), 1.0, 1e-12);
    CHECK_NEAR(line_duration_s(90), 2.0 / 3.0, 1e-12);
    CHECK_NEAR(line_duration_s(100), 0.6, 1e-12);
    CHECK_NEAR(line_duration_s(120), 0.5, 1e-12);
    CHECK_NEAR(line_duration_s(180), 1.0 / 3.0, 1e-12);
    CHECK_NEAR(line_duration_s(240), 0.25, 1e-12);
    /* Zero must not divide. */
    CHECK_NEAR(line_duration_s(0), 0.0, 1e-12);
}

TEST(wefax_samples_per_line) {
    /* At upstream's 12 kHz channel rate. */
    CHECK_NEAR(samples_per_line(12000.0, 120), 6000.0, 1e-9);
    CHECK_NEAR(samples_per_line(12000.0, 60), 12000.0, 1e-9);
    CHECK_NEAR(samples_per_line(12000.0, 240), 3000.0, 1e-9);
}

TEST(wefax_samples_per_pixel) {
    /* update_params(): fs / ((lpm/60) * 840). */
    CHECK_NEAR(samples_per_pixel(12000.0, 120, px_per_line), 12000.0 / 1680.0, 1e-9);
    CHECK_NEAR(samples_per_pixel(12000.0, 120, px_per_line), 7.142857142857, 1e-9);
    CHECK_NEAR(samples_per_pixel(12000.0, 60, px_per_line), 14.285714285714, 1e-9);
    CHECK_NEAR(samples_per_pixel(12000.0, 240, px_per_line), 3.571428571428, 1e-9);
    CHECK_NEAR(samples_per_pixel(12000.0, 90, px_per_line), 9.523809523809, 1e-9);

    /* Whatever the LPM, one line is always 840 pixels' worth of samples. */
    for (uint32_t lpm : {60u, 90u, 100u, 120u, 180u, 240u}) {
        const double spp = samples_per_pixel(12000.0, lpm, px_per_line);
        CHECK_NEAR(spp * px_per_line, samples_per_line(12000.0, lpm), 1e-6);
    }

    CHECK_NEAR(samples_per_pixel(12000.0, 0, px_per_line), 0.0, 1e-12);
}

TEST(wefax_ioc_values_and_start_tones) {
    /* proc_wefaxrx::update_params: 300 Hz for IOC 576, 675 Hz for IOC 288. */
    CHECK_EQ(ioc_value(Ioc::Ioc576), 576u);
    CHECK_EQ(ioc_value(Ioc::Ioc288), 288u);
    CHECK_EQ(start_tone_hz(Ioc::Ioc576), 300u);
    CHECK_EQ(start_tone_hz(Ioc::Ioc288), 675u);
    CHECK_NEAR(static_cast<double>(stop_tone_hz), 450.0, 1e-9);
}

TEST(wefax_ioc_line_pixels) {
    /* One scan line carries pi * IOC picture elements. */
    CHECK_NEAR(ioc_line_pixels(576), 1809.5573684678, 1e-6);
    CHECK_NEAR(ioc_line_pixels(288), 904.7786842339, 1e-6);

    /* Upstream's fixed 840-pixel grid is therefore about half the horizontal
     * resolution an IOC-576 chart carries, and a little under IOC 288's:
     * 840 < 904.8 < 1809.6. */
    CHECK(ioc_line_pixels(576) > 2.0 * px_per_line);
    CHECK(ioc_line_pixels(288) > px_per_line);
    CHECK(ioc_line_pixels(288) < 1.1 * px_per_line);

    /* IOC 576 carries exactly twice the elements of IOC 288. */
    CHECK_NEAR(ioc_line_pixels(576) / ioc_line_pixels(288), 2.0, 1e-12);
}

/* ----------------------------------------------------------- pixel clock -- */

TEST(wefax_pixel_clock_splits_the_ratio) {
    PixelClock c;
    c.configure(12000.0, 120, px_per_line);
    CHECK_EQ(c.samples_per_pixel(), 7u);
    CHECK_NEAR(c.remainder(), 12000.0 / 1680.0 - 7.0, 1e-9);

    c.configure(12000.0, 240, px_per_line);
    CHECK_EQ(c.samples_per_pixel(), 3u);
    CHECK_NEAR(c.remainder(), 12000.0 / 3360.0 - 3.0, 1e-9);
}

TEST(wefax_pixel_clock_emits_840_pixels_per_line) {
    for (uint32_t lpm : {60u, 90u, 100u, 120u, 180u, 240u}) {
        PixelClock c;
        c.configure(12000.0, lpm, px_per_line);

        const int samples = static_cast<int>(std::lround(samples_per_line(12000.0, lpm) * 10.0));
        int pixels = 0;
        for (int i = 0; i < samples; i++)
            if (c.tick()) pixels++;

        /* Ten lines' worth of samples must give ten lines' worth of pixels. */
        CHECK_NEAR(pixels, 10 * px_per_line, 2);
    }
}

/* --------------------------------------------------------- pixel mapping -- */

TEST(wefax_amplitude_to_pixel_matches_upstream_branches) {
    /* proc_wefaxrx: >= 0.68 -> 255; >= 0.45 -> (v - 0.45) * 1108 truncated;
     * otherwise 0. */
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(0.68f)), 255);
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(0.9f)), 255);
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(1.0f)), 255);

    CHECK_EQ(static_cast<int>(amplitude_to_pixel(0.45f)), 0);
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(0.44f)), 0);
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(0.0f)), 0);
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(-1.0f)), 0);

    /* (0.5 - 0.45) * 1108 = 55.4 -> 55 */
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(0.5f)), 55);
    /* (0.6 - 0.45) * 1108 = 166.2 -> 166 */
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(0.6f)), 166);
    /* Just under the white knee: (0.6799 - 0.45) * 1108 = 254.7 -> 254 */
    CHECK_EQ(static_cast<int>(amplitude_to_pixel(0.6799f)), 254);

    /* The ramp is monotonic across the whole grey band. */
    int prev = -1;
    for (int i = 0; i <= 100; i++) {
        const float v = 0.45f + (0.23f * i) / 100.0f;
        const int px = amplitude_to_pixel(v);
        CHECK(px >= prev);
        prev = px;
    }
}

TEST(wefax_preview_column_mapping) {
    /* 840 / 240 is 3 in integer arithmetic, so from position 720 on everything
     * clamps onto the last column. Upstream's behaviour, ported deliberately. */
    CHECK_EQ(preview_column(0), static_cast<uint16_t>(0));
    CHECK_EQ(preview_column(2), static_cast<uint16_t>(0));
    CHECK_EQ(preview_column(3), static_cast<uint16_t>(1));
    CHECK_EQ(preview_column(717), static_cast<uint16_t>(239));
    CHECK_EQ(preview_column(720), static_cast<uint16_t>(239));
    CHECK_EQ(preview_column(839), static_cast<uint16_t>(239));
}

/* ------------------------------------------------------- start-tone sync -- */

TEST(wefax_start_tone_detector_needs_110_in_band_samples) {
    StartToneDetector d;

    /* 109 in-band samples are not enough. */
    for (int i = 0; i < 109; i++) CHECK(!d.process(0.2f));
    CHECK_EQ(d.sync_count(), static_cast<uint16_t>(109));
    /* The 110th declares sync and clears both counters. */
    CHECK(d.process(0.2f));
    CHECK_EQ(d.sync_count(), static_cast<uint16_t>(0));
    CHECK_EQ(d.bad_count(), static_cast<uint16_t>(0));
}

TEST(wefax_start_tone_detector_band_edges) {
    /* Upstream's window is (0.0001 <= v <= 0.33): silence is excluded at the
     * bottom, picture level at the top. */
    StartToneDetector d;
    for (int i = 0; i < 109; i++) d.process(0.33f);
    CHECK(d.process(0.33f));

    StartToneDetector e;
    for (int i = 0; i < 200; i++) CHECK(!e.process(0.34f));  /* above the band */

    StartToneDetector f;
    for (int i = 0; i < 200; i++) CHECK(!f.process(0.0f));   /* below the band */
}

TEST(wefax_start_tone_detector_resets_after_20_bad_samples) {
    StartToneDetector d;
    for (int i = 0; i < 100; i++) d.process(0.2f);
    CHECK_EQ(d.sync_count(), static_cast<uint16_t>(100));

    /* Nineteen out-of-band samples leave the count alone... */
    for (int i = 0; i < 19; i++) d.process(0.9f);
    CHECK_EQ(d.sync_count(), static_cast<uint16_t>(100));
    /* ...the twentieth wipes it. */
    d.process(0.9f);
    CHECK_EQ(d.sync_count(), static_cast<uint16_t>(0));
    CHECK_EQ(d.bad_count(), static_cast<uint16_t>(0));

    /* So 100 good samples followed by a reset need a fresh 110. */
    for (int i = 0; i < 109; i++) CHECK(!d.process(0.2f));
    CHECK(d.process(0.2f));
}

/* --------------------------------------------------------------- biquad --- */

TEST(wefax_biquad_is_scipy_sosfilt) {
    /* H(z) = (0.5 + 0.25 z^-1 + 0.125 z^-2) / (1 - 0.5 z^-1 + 0.25 z^-2).
     * Impulse response by the recurrence h[n] = b[n] - a1 h[n-1] - a2 h[n-2]:
     *   h0 = 0.5
     *   h1 = 0.25 + 0.5*0.5              = 0.5
     *   h2 = 0.125 + 0.5*0.5 - 0.25*0.5  = 0.25
     *   h3 = 0.5*0.25 - 0.25*0.5         = 0
     *   h4 = 0.5*0 - 0.25*0.25           = -0.0625 */
    Biquad b;
    b.configure({0.5f, 0.25f, 0.125f, 1.0f, -0.5f, 0.25f});

    CHECK_NEAR(b.execute(1.0f), 0.5, 1e-6);
    CHECK_NEAR(b.execute(0.0f), 0.5, 1e-6);
    CHECK_NEAR(b.execute(0.0f), 0.25, 1e-6);
    CHECK_NEAR(b.execute(0.0f), 0.0, 1e-6);
    CHECK_NEAR(b.execute(0.0f), -0.0625, 1e-6);
}

TEST(wefax_biquad_normalises_by_a0) {
    /* Every coefficient doubled with a0 = 2 is the same filter. */
    Biquad plain, scaled;
    plain.configure({0.5f, 0.25f, 0.125f, 1.0f, -0.5f, 0.25f});
    scaled.configure({1.0f, 0.5f, 0.25f, 2.0f, -1.0f, 0.5f});

    for (int i = 0; i < 16; i++) {
        const float x = (i == 0) ? 1.0f : 0.0f;
        CHECK_NEAR(plain.execute(x), scaled.execute(x), 1e-6);
    }
}

TEST(wefax_biquad_reset_clears_state) {
    Biquad b;
    b.configure({0.5f, 0.25f, 0.125f, 1.0f, -0.5f, 0.25f});
    b.execute(1.0f);
    b.execute(0.0f);
    b.reset();
    CHECK_NEAR(b.execute(1.0f), 0.5, 1e-6);
}

/* --------------------------------------------------- SOS filter responses - */

TEST(wefax_quarter_band_lpf_cuts_at_1500hz) {
    /* scipy.signal.iirfilter(ftype="ellip", N=10, rp=0.5, rs=60, Wn=0.25,
     * "lowpass") at 12 kHz — a 1.5 kHz cutoff with a 60 dB stopband. That
     * placement is what makes the WeFax slope detector work: white (2300 Hz)
     * produces a 1400 Hz product component that passes, black (1500 Hz)
     * produces a 3000 Hz one that does not. */
    auto run = [](double f) {
        SosFilter<5> s;
        s.configure(quarter_band_lpf_config);
        const auto in = tone(f, 12000.0, 6000, 1.0);
        std::vector<float> out;
        out.reserve(in.size());
        for (float v : in) out.push_back(s.execute(v));
        return rms(out, 3000);  /* skip the transient */
    };

    const double pass_1400 = run(1400.0);
    const double stop_3000 = run(3000.0);
    const double reference = 1.0 / std::sqrt(2.0);  /* RMS of a unit sine */

    /* Passband: within the 0.5 dB design ripple. */
    CHECK_NEAR(pass_1400 / reference, 1.0, 0.1);
    /* Stopband: the design asks for 60 dB; allow for the transient tail. */
    CHECK(stop_3000 / reference < 0.01);
    CHECK(pass_1400 > 40.0 * stop_3000);
}

TEST(wefax_full_band_lpf_passes_the_audio_band) {
    /* Wn = 0.99 at 12 kHz is a 5.94 kHz cutoff, so everything WeFax cares about
     * (1500-2300 Hz) goes straight through. */
    SosFilter<5> s;
    s.configure(full_band_lpf_config);
    const auto in = tone(1900.0, 12000.0, 6000, 1.0);
    std::vector<float> out;
    out.reserve(in.size());
    for (float v : in) out.push_back(s.execute(v));

    const double reference = 1.0 / std::sqrt(2.0);
    CHECK_NEAR(rms(out, 3000) / reference, 1.0, 0.15);
}

/* -------------------------------------------------------- discriminator --- */

TEST(wefax_smuad_model_truncates_and_drops_negatives) {
    /* The model of upstream's __SMUAD(float, float): convert to integer towards
     * zero, negatives contribute nothing. */
    CHECK_NEAR(Discriminator::smuad_model(3.7f, 2.2f), 6.0, 1e-6);   /* 3 * 2 */
    CHECK_NEAR(Discriminator::smuad_model(10.0f, 10.0f), 100.0, 1e-6);
    CHECK_NEAR(Discriminator::smuad_model(-3.7f, 2.2f), 0.0, 1e-9);
    CHECK_NEAR(Discriminator::smuad_model(3.7f, -2.2f), 0.0, 1e-9);
    CHECK_NEAR(Discriminator::smuad_model(-3.7f, -2.2f), 0.0, 1e-9);
    /* Sub-unit operands truncate to nothing, which is why the detector needs
     * the int16-domain input scaling. */
    CHECK_NEAR(Discriminator::smuad_model(0.9f, 0.9f), 0.0, 1e-9);
    /* Never negative — the "S" curve downstream diverges below -sqrt(3). */
    for (float a = -5.0f; a <= 5.0f; a += 0.5f)
        for (float b = -5.0f; b <= 5.0f; b += 0.5f)
            CHECK(Discriminator::smuad_model(a, b) >= 0.0f);
}

TEST(wefax_soft_clip_is_upstreams_one_sided_curve) {
    /* out > 1 -> 1, else x*(1.5 - x^2/2). Unity at both ends of [0, 1] and
     * monotonic in between, so it lifts mid greys without touching the rails. */
    CHECK_NEAR(Discriminator::soft_clip(0.0f), 0.0, 1e-6);
    CHECK_NEAR(Discriminator::soft_clip(0.5f), 0.5 * (1.5 - 0.125), 1e-6);
    CHECK_NEAR(Discriminator::soft_clip(0.5f), 0.6875, 1e-6);
    CHECK_NEAR(Discriminator::soft_clip(1.0f), 1.0, 1e-6);
    CHECK_NEAR(Discriminator::soft_clip(1.5f), 1.0, 1e-6);
    CHECK_NEAR(Discriminator::soft_clip(1000.0f), 1.0, 1e-6);

    float prev = -1.0f;
    for (int i = 0; i <= 100; i++) {
        const float y = Discriminator::soft_clip(i / 100.0f);
        CHECK(y >= prev);
        CHECK(y <= 1.0f);
        prev = y;
    }

    /* And the documented sharp edge: the limit is one-sided, so a big negative
     * excursion is amplified rather than clamped. Pinned because it is
     * upstream's behaviour, not because it is desirable. */
    CHECK_NEAR(Discriminator::soft_clip(-0.5f), -0.6875, 1e-6);
    CHECK_NEAR(Discriminator::soft_clip(-3.0f), 9.0, 1e-5);
    CHECK(Discriminator::soft_clip(-10.0f) > 100.0f);
}

TEST(wefax_discriminator_output_is_finite) {
    /* Whatever the drive level, the video path must stay numerically sound —
     * the pixel mapping then turns anything wild into a black or white pixel
     * rather than a broken image. */
    for (double f : {1500.0, 1900.0, 2300.0}) {
        for (double amp : {50.0, 300.0, 4000.0}) {
            Discriminator d;
            const auto in = tone(f, 12000.0, 4000, amp);
            for (float v : in) {
                const float out = d.process(v);
                CHECK(!std::isnan(out));
                CHECK(!std::isinf(out));
                /* Any value at all still lands inside the 8-bit grey range. */
                const uint8_t px = amplitude_to_pixel(out);
                CHECK(px <= 255);
            }
        }
    }
}

TEST(wefax_discriminator_responds_to_a_tone) {
    /* A silent input must give a silent output, and a tone must not. */
    Discriminator quiet;
    std::vector<float> zero_out;
    for (int i = 0; i < 2000; i++) zero_out.push_back(quiet.process(0.0f));
    CHECK_NEAR(rms(zero_out, 1000), 0.0, 1e-6);

    Discriminator active;
    const auto in = tone(2300.0, 12000.0, 4000, 300.0);
    std::vector<float> out;
    out.reserve(in.size());
    for (float v : in) out.push_back(active.process(v));
    CHECK(rms(out, 2000) > 1e-3);
}

TEST(wefax_discriminator_reset_makes_it_repeatable) {
    Discriminator d;
    const auto in = tone(2000.0, 12000.0, 1500, 400.0);

    std::vector<float> first;
    for (float v : in) first.push_back(d.process(v));

    d.reset();
    std::vector<float> second;
    for (float v : in) second.push_back(d.process(v));

    for (size_t i = 0; i < first.size(); i++) CHECK_NEAR(first[i], second[i], 1e-9);
}

/* ------------------------------------------------------------ compressor -- */

TEST(wefax_compressor_makeup_gain_below_threshold) {
    /* -30 dBFS threshold, 10:1, make-up 10^((-30 - -3)/-20) = 10^1.35 =
     * 22.3872. Anything below the threshold sees only the make-up gain. */
    std::vector<float> buf(64, 0.001f);
    Compressor c;
    c.execute_in_place(buf.data(), buf.size());
    CHECK_NEAR(buf[63], 0.001 * 22.3872, 1e-4);
}

TEST(wefax_compressor_compresses_ten_to_one_above_threshold) {
    /* At -6.02 dBFS the overshoot is 23.98 dB, so the output should settle at
     * -30 + 2.398 = -27.60 dBFS before make-up: 0.04169 * 22.3872 = 0.9334.
     * The attack is 10 ms at 12 kHz, so one second is plenty of settling. */
    Compressor c;
    std::vector<float> buf(12000, 0.5f);
    c.execute_in_place(buf.data(), buf.size());
    CHECK_NEAR(buf.back(), 0.9334, 0.01);

    /* And a 20 dB smaller input gives a 2 dB smaller output. */
    Compressor c2;
    std::vector<float> quiet(12000, 0.05f);
    c2.execute_in_place(quiet.data(), quiet.size());
    CHECK_NEAR(quiet.back(), 0.7413, 0.01);

    const double ratio_db = 20.0 * std::log10(buf.back() / quiet.back());
    CHECK_NEAR(ratio_db, 2.0, 0.15);
}

TEST(wefax_compressor_handles_silence) {
    Compressor c;
    std::vector<float> buf(256, 0.0f);
    c.execute_in_place(buf.data(), buf.size());
    for (float v : buf) {
        CHECK(!std::isnan(v));
        CHECK_NEAR(v, 0.0, 1e-9);
    }
}
