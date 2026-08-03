/*
 * mayhem-b200 — Two-Tone pager TX encoder tests.
 *
 * The deliverable is the tone encoder: the Motorola/CTCSS tables, the phase-
 * delta and sample-count math, and the A → [gap] → B sequence. Every value here
 * is checked against upstream (the tables and the tone_delta/ms_to_samples
 * formulas from firmware/application/external/two_tone_pager) or against an
 * independent analytic result (the frequency of a rendered segment, measured by
 * zero-crossing counting). Nothing is asserted against whatever the code
 * happens to produce.
 *
 * MB200_ENCODER_ONLY keeps the app header's UI/UHD half out of the build so
 * these tests link against only the DSP objects.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define MB200_ENCODER_ONLY
#include "ui_two_tone_pager.hpp"

#include "modulate.hpp"
#include "test_main.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace app::two_tone_pager;

namespace {

/* Count sign changes in a real signal; a full sine cycle crosses zero twice, so
 * measured_freq = crossings / 2 / duration. */
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

}  // namespace

/* --- Tables verbatim from upstream ----------------------------------------- */

TEST(two_tone_tables_match_upstream) {
    /* Copy the constexpr tables into runtime buffers so the comparisons below
     * are not constant expressions (which /W4 would flag as C4127). */
    std::vector<uint32_t> moto(MOTO_FREQS.begin(), MOTO_FREQS.end());
    std::vector<uint32_t> ctcss(CTCSS_FREQS.begin(), CTCSS_FREQS.end());

    CHECK_EQ(moto.size(), size_t{45});
    CHECK_EQ(ctcss.size(), size_t{51});

    /* Endpoints and the two upstream default indices. */
    CHECK_EQ(moto[0], uint32_t{2885});    /* 288.5 Hz */
    CHECK_EQ(moto[8], uint32_t{4457});    /* group A default index → 445.7 Hz */
    CHECK_EQ(moto[24], uint32_t{10642});  /* group B default index → 1064.2 Hz */
    CHECK_EQ(moto[44], uint32_t{20275});  /* last */

    CHECK_EQ(ctcss[0], uint32_t{0});      /* None */
    CHECK_EQ(ctcss[12], uint32_t{1000});  /* 100.0 Hz */
    CHECK_EQ(ctcss[50], uint32_t{2541});  /* 254.1 Hz, last */
}

/* --- Phase-delta math (upstream TwoTonePagerView::tone_delta) --------------- */

TEST(two_tone_tone_delta_matches_upstream) {
    std::vector<uint32_t> sr{SAMPLE_RATE};  /* runtime load dodges C4127 */
    CHECK_EQ(sr[0], uint32_t{1'536'000});

    /* Precomputed from delta = (freq_x10 << 32) / (SAMPLE_RATE * 10). */
    CHECK_EQ(tone_delta(10000), uint32_t{2796202});  /* 1000.0 Hz */
    CHECK_EQ(tone_delta(2885), uint32_t{806704});    /* MOTO[0]  288.5 Hz */
    CHECK_EQ(tone_delta(4457), uint32_t{1246267});   /* MOTO[8]  445.7 Hz */
    CHECK_EQ(tone_delta(10642), uint32_t{2975718});  /* MOTO[24] 1064.2 Hz */
    CHECK_EQ(tone_delta(1000), uint32_t{279620});    /* CTCSS[12] 100.0 Hz */
    CHECK_EQ(tone_delta(0), uint32_t{0});            /* None → silence */

    /* The defining identity, checked at full 64-bit precision. */
    for (uint32_t fx10 : {2885u, 5539u, 10073u, 20275u}) {
        const uint32_t expect = static_cast<uint32_t>(
            (static_cast<uint64_t>(fx10) << 32) /
            (static_cast<uint64_t>(SAMPLE_RATE) * 10ULL));
        CHECK_EQ(tone_delta(fx10), expect);
    }
}

/* --- Sample-count math (upstream ms_to_samples) ---------------------------- */

TEST(two_tone_ms_to_samples_matches_upstream) {
    CHECK_EQ(ms_to_samples(1000), uint32_t{1'536'000});
    CHECK_EQ(ms_to_samples(3000), uint32_t{4'608'000});
    CHECK_EQ(ms_to_samples(100), uint32_t{153'600});
    CHECK_EQ(ms_to_samples(0), uint32_t{0});
}

/* --- Sequence structure ---------------------------------------------------- */

TEST(two_tone_sequence_no_gap) {
    /* Upstream: with gap_ms == 0 the sequence is just A then B. */
    auto seq = build_sequence(405.3f, 813.9f, 0.0f, 1000, 3000, 0, 48000);
    CHECK_EQ(seq.size(), size_t{2});

    CHECK_NEAR(seq[0].freq_hz, 405.3f, 0.01f);
    CHECK_EQ(seq[0].ctcss_hz, 0.0f);
    CHECK_EQ(seq[0].samples, uint32_t{48000});   /* 1000 ms @ 48 kHz */

    CHECK_NEAR(seq[1].freq_hz, 813.9f, 0.01f);
    CHECK_EQ(seq[1].samples, uint32_t{144000});  /* 3000 ms @ 48 kHz */
}

TEST(two_tone_sequence_with_gap_and_ctcss) {
    /* gap_ms > 0 inserts a silent middle segment; CTCSS rides under A and B but
     * never under the gap. */
    auto seq = build_sequence(405.3f, 813.9f, 100.0f, 700, 1000, 500, 48000);
    CHECK_EQ(seq.size(), size_t{3});

    CHECK_NEAR(seq[0].freq_hz, 405.3f, 0.01f);
    CHECK_NEAR(seq[0].ctcss_hz, 100.0f, 0.01f);
    CHECK_EQ(seq[0].samples, uint32_t{33600});   /* 700 ms */

    CHECK_EQ(seq[1].freq_hz, 0.0f);              /* gap is silent */
    CHECK_EQ(seq[1].ctcss_hz, 0.0f);
    CHECK_EQ(seq[1].samples, uint32_t{24000});   /* 500 ms */

    CHECK_NEAR(seq[2].freq_hz, 813.9f, 0.01f);
    CHECK_NEAR(seq[2].ctcss_hz, 100.0f, 0.01f);
    CHECK_EQ(seq[2].samples, uint32_t{48000});   /* 1000 ms */
}

/* --- Rendered-tone frequency (analytic, via zero crossings) ---------------- */

TEST(two_tone_rendered_frequencies) {
    const double fs = 48000.0;

    /* Tone A at 405.3 Hz. */
    dsp::ToneGen g;
    g.configure(405.3f, static_cast<float>(fs), dsp::ToneGen::Shape::Sine, 1.0f);
    std::vector<float> a(48000);
    for (auto& s : a) s = g.process_one();
    CHECK_NEAR(measured_freq(a, fs), 405.3, 1.0);

    /* Tone B at 813.9 Hz. */
    g.configure(813.9f, static_cast<float>(fs), dsp::ToneGen::Shape::Sine, 1.0f);
    std::vector<float> b(48000);
    for (auto& s : b) s = g.process_one();
    CHECK_NEAR(measured_freq(b, fs), 813.9, 1.0);
}
