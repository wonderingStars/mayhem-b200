/*
 * mayhem-b200 — Signal generator encoder tests.
 *
 * Two deliverables are checked here:
 *   1. The frequency sweep math (siggen::sweep_step_count / sweep_frequency_at),
 *      against hand-computed step counts and endpoints.
 *   2. dsp::ToneGen's tone frequency, measured independently by zero-crossing
 *      counting — the same generator the app drives the transmitter with.
 *
 * MB200_ENCODER_ONLY keeps the app header's UI/UHD half out of the build.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define MB200_ENCODER_ONLY
#include "ui_siggen.hpp"

#include "modulate.hpp"
#include "test_main.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace app::siggen;

namespace {

size_t zero_crossings(const std::vector<float>& x) {
    size_t c = 0;
    for (size_t i = 1; i < x.size(); i++)
        if ((x[i - 1] < 0.0f && x[i] >= 0.0f) || (x[i - 1] >= 0.0f && x[i] < 0.0f))
            c++;
    return c;
}

double measured_freq(const std::vector<float>& x, double fs) {
    return static_cast<double>(zero_crossings(x)) / 2.0 * fs /
           static_cast<double>(x.size());
}

std::vector<float> render(dsp::ToneGen::Shape shape, float freq, double fs, size_t n) {
    dsp::ToneGen g;
    g.configure(freq, static_cast<float>(fs), shape, 1.0f);
    std::vector<float> v(n);
    for (auto& s : v) s = g.process_one();
    return v;
}

}  // namespace

/* --- Sweep step count ------------------------------------------------------ */

TEST(siggen_sweep_step_count) {
    CHECK_EQ(sweep_step_count(1000, 2000, 100), uint32_t{11});
    CHECK_EQ(sweep_step_count(2000, 1000, 100), uint32_t{11});  /* symmetric */
    CHECK_EQ(sweep_step_count(1000, 1000, 100), uint32_t{1});   /* single point */
    CHECK_EQ(sweep_step_count(1000, 2000, 0), uint32_t{1});     /* zero step */
    CHECK_EQ(sweep_step_count(1000, 1950, 100), uint32_t{10});  /* unaligned end */
    CHECK_EQ(sweep_step_count(0, 20000, 500), uint32_t{41});
}

/* --- Sweep frequency at step ----------------------------------------------- */

TEST(siggen_sweep_frequency_at) {
    /* Ascending 1000 → 2000 by 100. */
    CHECK_EQ(sweep_frequency_at(1000, 2000, 100, 0), uint32_t{1000});
    CHECK_EQ(sweep_frequency_at(1000, 2000, 100, 5), uint32_t{1500});
    CHECK_EQ(sweep_frequency_at(1000, 2000, 100, 10), uint32_t{2000});

    /* Descending 2000 → 1000 by 100. */
    CHECK_EQ(sweep_frequency_at(2000, 1000, 100, 0), uint32_t{2000});
    CHECK_EQ(sweep_frequency_at(2000, 1000, 100, 10), uint32_t{1000});

    /* Out-of-range index clamps to the last step, never overshoots. */
    CHECK_EQ(sweep_frequency_at(1000, 2000, 100, 99), uint32_t{2000});

    /* Unaligned range: last step is the largest step within [lo,hi], not end. */
    const uint32_t n = sweep_step_count(1000, 1950, 100);
    CHECK_EQ(sweep_frequency_at(1000, 1950, 100, n - 1), uint32_t{1900});

    /* Zero step is a fixed point at start. */
    CHECK_EQ(sweep_frequency_at(1234, 5678, 0, 7), uint32_t{1234});
}

/* --- ToneGen frequency, via zero crossings --------------------------------- */

TEST(siggen_tonegen_frequency) {
    const double fs = 48000.0;

    for (double f : {100.0, 1000.0, 3000.0, 7500.0}) {
        auto sine = render(dsp::ToneGen::Shape::Sine, static_cast<float>(f), fs, 48000);
        CHECK_NEAR(measured_freq(sine, fs), f, 1.0);
    }

    /* Non-sine periodic shapes cross zero the same number of times per cycle. */
    auto tri = render(dsp::ToneGen::Shape::Triangle, 1000.0f, fs, 48000);
    CHECK_NEAR(measured_freq(tri, fs), 1000.0, 1.0);
    auto sq = render(dsp::ToneGen::Shape::Square, 1000.0f, fs, 48000);
    CHECK_NEAR(measured_freq(sq, fs), 1000.0, 1.0);
}

/* --- Waveform shape sanity ------------------------------------------------- */

TEST(siggen_shapes) {
    const double fs = 48000.0;

    /* Square is bipolar full-scale. */
    auto sq = render(dsp::ToneGen::Shape::Square, 1000.0f, fs, 4800);
    float mn = 1.0f, mx = -1.0f;
    for (float v : sq) { mn = std::min(mn, v); mx = std::max(mx, v); }
    CHECK_NEAR(mx, 1.0f, 1e-4f);
    CHECK_NEAR(mn, -1.0f, 1e-4f);

    /* Pseudo-noise (LFSR) is deterministic: same seed → same first samples. */
    auto n1 = render(dsp::ToneGen::Shape::Noise, 0.0f, fs, 64);
    auto n2 = render(dsp::ToneGen::Shape::Noise, 0.0f, fs, 64);
    for (size_t i = 0; i < n1.size(); i++)
        CHECK_EQ(n1[i], n2[i]);
    /* ...and not a constant (the LFSR actually runs). */
    bool varies = false;
    for (size_t i = 1; i < n1.size(); i++)
        if (n1[i] != n1[0]) { varies = true; break; }
    CHECK(varies);
}
