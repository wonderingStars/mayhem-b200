/*
 * mayhem-b200 — Mic TX tests.
 *
 * The three pieces of upstream signal maths the port has to reproduce exactly
 * are the deliverable here, and they are covered without a UI or a radio:
 *
 *   1. audio -> deviation scaling   (mictx_deviation_hz + dsp::FmModulator)
 *   2. CTCSS tone-mix level         (mictx_tone_mix_weight + dsp::ToneGen::mix)
 *   3. mic level-meter maths        (mictx_meter_value)
 *
 * Plus an integration check that the helpers drive radio::TransmitterModel to
 * the values upstream would, and that the CTCSS table lookup the sub-tone
 * selector uses is correct. RF is unverified without hardware.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "modulate.hpp"
#include "transmitter_model.hpp"
#include "ui_mictx.hpp"
#include "usrp_radio.hpp"

#include <cmath>
#include <complex>
#include <vector>

using app::mictx_deviation_hz;
using app::mictx_meter_value;
using app::mictx_tone_mix_weight;

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
}

/* --- 1. deviation scaling -------------------------------------------------- */

TEST(mictx_deviation_hz_is_khz_to_hz) {
    /* Upstream carries the UI "TX deviation" (kHz) as channel_bandwidth() and
     * hands it to the FM modulator as deviation_hz. */
    CHECK_NEAR(mictx_deviation_hz(5), 5000.0, 1e-9);
    CHECK_NEAR(mictx_deviation_hz(75), 75000.0, 1e-9);
    CHECK_NEAR(mictx_deviation_hz(150), 150000.0, 1e-9);
    CHECK_NEAR(mictx_deviation_hz(0), 0.0, 1e-9);
}

TEST(mictx_deviation_drives_the_fm_modulator_phase_step) {
    /* An audio value of a must advance the modulator phase by
     * a * 2*pi*deviation/fs per sample. With a constant full-scale input the
     * step between consecutive output samples is exactly that quantity. */
    const double fs = 48000.0;
    const double dev = mictx_deviation_hz(5);  /* 5 kHz */
    const double expected_step = kTwoPi * dev / fs;  /* ~0.654498 rad */

    dsp::FmModulator fm;
    fm.configure(static_cast<float>(fs), static_cast<float>(dev));

    const size_t n = 8;
    std::vector<float> audio(n, 1.0f);
    std::vector<dsp::cfloat> out(n);
    fm.process(audio.data(), n, out.data());

    /* Phase of the first sample is one step from zero. */
    CHECK_NEAR(std::arg(out[0]), expected_step, 1e-4);

    /* Step between successive samples is constant and equals the expected step. */
    for (size_t i = 1; i < n; i++) {
        const double step = std::arg(out[i] * std::conj(out[i - 1]));
        CHECK_NEAR(step, expected_step, 1e-4);
    }

    /* Half-scale audio gives half the deviation, i.e. half the phase step. */
    dsp::FmModulator fm2;
    fm2.configure(static_cast<float>(fs), static_cast<float>(dev));
    std::vector<float> half(n, 0.5f);
    std::vector<dsp::cfloat> out2(n);
    fm2.process(half.data(), n, out2.data());
    CHECK_NEAR(std::arg(out2[0]), expected_step * 0.5, 1e-4);
}

/* --- 2. CTCSS tone-mix level ----------------------------------------------- */

TEST(mictx_tone_mix_weight_matches_persistent_memory) {
    /* tone_mix range 10..99, reset 20; weight = percent / 100. */
    CHECK_NEAR(mictx_tone_mix_weight(20), 0.20f, 1e-6);
    CHECK_NEAR(mictx_tone_mix_weight(50), 0.50f, 1e-6);
    CHECK_NEAR(mictx_tone_mix_weight(10), 0.10f, 1e-6);
    CHECK_NEAR(mictx_tone_mix_weight(99), 0.99f, 1e-6);
    /* Out-of-range values clamp to the stored range. */
    CHECK_NEAR(mictx_tone_mix_weight(0), 0.10f, 1e-6);
    CHECK_NEAR(mictx_tone_mix_weight(5), 0.10f, 1e-6);
    CHECK_NEAR(mictx_tone_mix_weight(200), 0.99f, 1e-6);
}

TEST(mictx_ctcss_mix_is_input_and_tone_weighted) {
    /* ToneGen::mix must produce out = in*(1-w) + tone*w, the upstream tone_gen
     * rule. Generate the pure tone once, then mix into a known audio block with
     * a fresh (identical-phase) generator and compare sample by sample. */
    const float fs = 48000.0f;
    const float freq = 100.0f;  /* a CTCSS frequency */
    const size_t n = 256;
    const float w = mictx_tone_mix_weight(20);  /* 0.2 */
    const float audio_level = 0.5f;

    dsp::ToneGen ref;
    ref.configure(freq, fs, dsp::ToneGen::Shape::Sine, 1.0f);
    std::vector<float> tone(n);
    ref.process(tone.data(), n);

    dsp::ToneGen mixer;
    mixer.configure(freq, fs, dsp::ToneGen::Shape::Sine, 1.0f);
    std::vector<float> mixed(n, audio_level);
    mixer.mix(mixed.data(), n, w);

    for (size_t i = 0; i < n; i++) {
        const float expected = audio_level * (1.0f - w) + tone[i] * w;
        CHECK_NEAR(mixed[i], expected, 1e-5);
    }
}

TEST(mictx_zero_frequency_sub_tone_is_a_no_op) {
    /* "None" leaves a zero-frequency tone, which upstream (and ToneGen::mix)
     * passes through untouched. */
    const size_t n = 64;
    dsp::ToneGen tone;
    tone.configure(0.0f, 48000.0f, dsp::ToneGen::Shape::Sine, 1.0f);
    std::vector<float> block(n, 0.3f);
    tone.mix(block.data(), n, 0.5f);
    for (size_t i = 0; i < n; i++)
        CHECK_NEAR(block[i], 0.3f, 1e-6);
}

/* --- 3. mic level-meter maths ---------------------------------------------- */

TEST(mictx_meter_is_zero_for_silence_and_empty) {
    std::vector<float> silence(512, 0.0f);
    CHECK_EQ(static_cast<int>(mictx_meter_value(silence.data(), silence.size(), 1.0f)), 0);
    CHECK_EQ(static_cast<int>(mictx_meter_value(nullptr, 0, 1.0f)), 0);
    CHECK_EQ(static_cast<int>(mictx_meter_value(silence.data(), 0, 1.0f)), 0);
}

TEST(mictx_meter_scales_512x_mean_abs) {
    /* value = 512 * mean|sample| (clamped 0..255). Constant blocks make the
     * mean-abs exact. */
    std::vector<float> q(400, 0.25f);   /* 0.25 * 512 = 128 */
    CHECK_EQ(static_cast<int>(mictx_meter_value(q.data(), q.size(), 1.0f)), 128);

    std::vector<float> e(400, 0.125f);  /* 0.125 * 512 = 64 */
    CHECK_EQ(static_cast<int>(mictx_meter_value(e.data(), e.size(), 1.0f)), 64);

    std::vector<float> big(400, 0.5f);  /* 0.5 * 512 = 256 -> clamp 255 */
    CHECK_EQ(static_cast<int>(mictx_meter_value(big.data(), big.size(), 1.0f)), 255);

    /* Negative samples count by magnitude. */
    std::vector<float> neg(400, -0.25f);
    CHECK_EQ(static_cast<int>(mictx_meter_value(neg.data(), neg.size(), 1.0f)), 128);
}

TEST(mictx_meter_applies_gain_before_measuring) {
    /* 0.1 at gain 2.0 must read the same as 0.2 at gain 1.0. */
    std::vector<float> a(256, 0.1f);
    std::vector<float> b(256, 0.2f);
    const int at2 = mictx_meter_value(a.data(), a.size(), 2.0f);
    const int b1 = mictx_meter_value(b.data(), b.size(), 1.0f);
    CHECK_EQ(at2, b1);
    CHECK_EQ(at2, 102);  /* 0.2 * 512 = 102.4 -> 102 */
}

TEST(mictx_meter_saturates_on_a_full_scale_tone) {
    /* A full-amplitude sine has mean|x| = 2/pi ~= 0.637, so 512 * that ~= 326,
     * which saturates the 0..255 meter. */
    const size_t n = 4800;  /* 100 periods of 1 kHz at 48 kHz */
    std::vector<float> sine(n);
    for (size_t i = 0; i < n; i++)
        sine[i] = static_cast<float>(std::sin(kTwoPi * 1000.0 * static_cast<double>(i) / 48000.0));
    CHECK_EQ(static_cast<int>(mictx_meter_value(sine.data(), n, 1.0f)), 255);

    /* A quarter-amplitude sine: mean|x| = 0.25 * 2/pi, 512x ~= 81.5. */
    std::vector<float> quiet(n);
    for (size_t i = 0; i < n; i++)
        quiet[i] = 0.25f * static_cast<float>(std::sin(kTwoPi * 1000.0 * static_cast<double>(i) / 48000.0));
    const int m = mictx_meter_value(quiet.data(), n, 1.0f);
    CHECK_NEAR(m, 81, 2);
}

/* --- integration with the transmit chain ----------------------------------- */

TEST(mictx_helpers_drive_the_transmitter_model) {
    using Mode = radio::TransmitterModel::Mode;
    using SubTone = radio::TransmitterModel::SubTone;

    radio::UsrpRadio r;
    radio::TransmitterModel tx{r};

    tx.set_mode(Mode::NarrowbandFM);
    tx.set_deviation(mictx_deviation_hz(12));  /* 12 kHz */
    CHECK_NEAR(tx.deviation(), 12000.0, 1e-6);

    tx.set_deviation(mictx_deviation_hz(75));
    CHECK_NEAR(tx.deviation(), 75000.0, 1e-6);

    /* CTCSS 100.0 Hz ("12 1Z") at the default 20% tone mix. */
    const size_t idx = 12;  /* dsp::tones::ctcss[12] == "12 1Z", 100.0 Hz */
    CHECK_NEAR(dsp::tones::ctcss[idx].frequency_hz, 100.0f, 1e-3);

    tx.set_ctcss(dsp::tones::ctcss[idx].frequency_hz, mictx_tone_mix_weight(20));
    CHECK(tx.sub_tone() == SubTone::Ctcss);
    CHECK_NEAR(tx.ctcss_frequency(), 100.0, 1e-3);
    CHECK_NEAR(tx.sub_tone_mix(), 0.20, 1e-6);

    /* A DCS code masks to nine bits, as the transmitter's word format requires. */
    tx.set_dcs(23, mictx_tone_mix_weight(20));
    CHECK(tx.sub_tone() == SubTone::Dcs);
    CHECK_EQ(static_cast<int>(tx.dcs_code()), 23);
}

TEST(mictx_ctcss_table_lookup_round_trips) {
    /* The sub-tone selector maps its option value straight to a table index, so
     * a known CTCSS frequency must sit where the app expects it. */
    const int i = dsp::tones::ctcss_index(100.0f);
    CHECK(i >= 0);
    CHECK_NEAR(dsp::tones::ctcss[static_cast<size_t>(i)].frequency_hz, 100.0f, 1e-3);

    const int j = dsp::tones::ctcss_index(67.0f);
    CHECK_EQ(j, 0);  /* 67.0 Hz is the lowest CTCSS tone, index 0 */

    /* A frequency far from any tone returns -1 (selector shows "None"). */
    CHECK_EQ(dsp::tones::ctcss_index(1234.0f), -1);
}
