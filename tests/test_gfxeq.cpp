/*
 * mayhem-b200 — gfxEQ decode-path tests.
 *
 * The app's deliverable is not the bar widget (Phase A owns and tests GraphEq)
 * but the chain that produces the AudioSpectrum it eats. Everything here is
 * checked against upstream's implementation rather than against whatever this
 * port happens to emit:
 *
 *   - the dB -> byte curve is proc_wfm_audio.cpp's `(db * 5) + 255`, clamped;
 *   - the FFT normalisation is upstream's: audio/32 into an unnormalised
 *     256-point transform, referenced to 32768, so a 0.25-amplitude sine reads
 *     0 dB / 255 and every 0.2 dB below that costs one count;
 *   - one bin is 375 Hz (96 kHz / 256), which is the number GraphEq's
 *     FREQUENCY_BANDS table is quoted against;
 *   - the WFM tap runs upstream's rates (384 kHz demod, 96 kHz spectrum feed).
 *
 * No radio is attached, so nothing here proves RF reception. What it proves is
 * that the pipeline is correct when fed samples: the end-to-end test synthesises
 * a real FM-modulated carrier, pushes it through mix -> channel filter ->
 * discriminator -> decimation -> FFT -> band reduction, and asserts the audio
 * tone lands in the bar it belongs in.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_gfxeq.hpp"

#include <cmath>
#include <complex>
#include <vector>

using namespace app;

namespace {

constexpr double kPi = 3.14159265358979323846;

/* A real sine at `hz`, sampled at the analyser's 96 kHz. */
std::vector<float> tone(double hz, double amplitude, size_t count, double phase = 0.0) {
    std::vector<float> out(count);
    for (size_t i = 0; i < count; i++) {
        const double t = static_cast<double>(i) / AudioSpectrumAnalyzer::kAudioRate;
        out[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * t + phase));
    }
    return out;
}

/* Loudest published bin, skipping DC (which GraphEq also skips). */
size_t peak_bin(const ui::AudioSpectrum& s) {
    size_t best = 1;
    for (size_t i = 2; i < s.db.size(); i++)
        if (s.db[i] > s.db[best]) best = i;
    return best;
}

size_t peak_bar(const ui::GraphEq& eq) {
    size_t best = 0;
    for (size_t b = 1; b < static_cast<size_t>(ui::GraphEq::NUM_BARS); b++)
        if (eq.bar_height(b) > eq.bar_height(best)) best = b;
    return best;
}

/* Runs one spectrum through a fresh GraphEq laid out like the app's. */
size_t bar_for_spectrum(const ui::AudioSpectrum& s) {
    ui::GraphEq eq{{0, 0, 240, 288}};
    eq.set_parent_rect({0, 0, 240, 288});
    eq.update_audio_spectrum(s);
    return peak_bar(eq);
}

/* Complex baseband carrier at `carrier_hz`, frequency-modulated by a sine of
 * `audio_hz` with `deviation_hz` peak deviation, sampled at `rate`. This is the
 * signal a B200 would hand the app for a broadcast FM station sitting
 * `carrier_hz` away from the LO. */
std::vector<dsp::cfloat> fm_signal(double rate,
                                   double carrier_hz,
                                   double audio_hz,
                                   double deviation_hz,
                                   size_t count,
                                   double& phase_state,
                                   size_t sample_offset) {
    std::vector<dsp::cfloat> out(count);
    for (size_t i = 0; i < count; i++) {
        const double t = static_cast<double>(sample_offset + i) / rate;
        const double instantaneous =
            carrier_hz + deviation_hz * std::sin(2.0 * kPi * audio_hz * t);
        phase_state += 2.0 * kPi * instantaneous / rate;
        out[i] = dsp::cfloat{static_cast<float>(std::cos(phase_state)),
                             static_cast<float>(std::sin(phase_state))};
    }
    return out;
}

}  // namespace

/* --- the dB -> byte curve -------------------------------------------------- */

TEST(gfxeq_db_to_byte_follows_upstream_curve) {
    /* proc_wfm_audio.cpp: v = db * 5 + 255, clamped to 0..255. */
    CHECK_EQ(AudioSpectrumAnalyzer::db_to_byte(0.0f), (uint8_t)255);
    CHECK_EQ(AudioSpectrumAnalyzer::db_to_byte(-20.0f), (uint8_t)155);
    CHECK_EQ(AudioSpectrumAnalyzer::db_to_byte(-40.0f), (uint8_t)55);
    /* One count is 0.2 dB, so -50.8 dB is the last non-zero level. */
    CHECK_EQ(AudioSpectrumAnalyzer::db_to_byte(-50.8f), (uint8_t)1);
}

TEST(gfxeq_db_to_byte_clamps_both_ends) {
    /* Anything at or under -51 dB pins to 0 rather than wrapping — upstream
     * relies on the M4's saturating float->unsigned conversion for this. */
    CHECK_EQ(AudioSpectrumAnalyzer::db_to_byte(-51.0f), (uint8_t)0);
    CHECK_EQ(AudioSpectrumAnalyzer::db_to_byte(-140.0f), (uint8_t)0);
    CHECK_EQ(AudioSpectrumAnalyzer::db_to_byte(-1000.0f), (uint8_t)0);
    /* Above the reference it saturates instead of overflowing the byte. */
    CHECK_EQ(AudioSpectrumAnalyzer::db_to_byte(0.5f), (uint8_t)255);
    CHECK_EQ(AudioSpectrumAnalyzer::db_to_byte(60.0f), (uint8_t)255);
}

TEST(gfxeq_bin_arithmetic_is_375hz) {
    CHECK_NEAR(AudioSpectrumAnalyzer::kBinHz, 375.0f, 0.0001);
    CHECK_EQ(AudioSpectrumAnalyzer::bin_for_hz(375.0f), (size_t)1);
    CHECK_EQ(AudioSpectrumAnalyzer::bin_for_hz(3000.0f), (size_t)8);
    CHECK_EQ(AudioSpectrumAnalyzer::bin_for_hz(24375.0f), (size_t)65);
    CHECK_EQ(AudioSpectrumAnalyzer::bin_for_hz(-10.0f), (size_t)0);
}

/* --- the analyser ---------------------------------------------------------- */

TEST(gfxeq_analyzer_buffers_until_a_full_block) {
    AudioSpectrumAnalyzer a;
    const auto block = tone(3000.0, 0.5, 400);

    CHECK(!a.feed(block.data(), 100));
    CHECK_EQ(a.pending(), (size_t)100);

    /* 100 + 156 = 256 = one transform, nothing left over. */
    CHECK(a.feed(block.data(), 156));
    CHECK_EQ(a.pending(), (size_t)0);

    /* 300 samples is one transform plus a 44-sample remainder. */
    CHECK(a.feed(block.data(), 300));
    CHECK_EQ(a.pending(), (size_t)44);
}

TEST(gfxeq_analyzer_ignores_degenerate_input) {
    AudioSpectrumAnalyzer a;
    const auto block = tone(3000.0, 0.5, 256);

    CHECK(!a.feed(nullptr, 256));
    CHECK(!a.feed(block.data(), 0));
    CHECK_EQ(a.pending(), (size_t)0);

    /* And nothing was published. */
    for (size_t i = 0; i < a.spectrum().db.size(); i++)
        CHECK_EQ(a.spectrum().db[i], (uint8_t)0);
}

TEST(gfxeq_analyzer_reports_silence_as_zero) {
    AudioSpectrumAnalyzer a;
    const std::vector<float> silence(AudioSpectrumAnalyzer::kFftSize, 0.0f);

    CHECK(a.feed(silence.data(), silence.size()));
    for (size_t i = 0; i < a.spectrum().db.size(); i++)
        CHECK_EQ(a.spectrum().db[i], (uint8_t)0);
}

TEST(gfxeq_analyzer_puts_a_tone_in_its_own_bin) {
    AudioSpectrumAnalyzer a;
    /* 3000 Hz is bin 8 exactly, and 256 samples at 96 kHz hold exactly eight
     * cycles of it, so a rectangular window leaks nothing into its neighbours. */
    const auto block = tone(3000.0, 0.5, AudioSpectrumAnalyzer::kFftSize);
    CHECK(a.feed(block.data(), block.size()));

    const auto& s = a.spectrum();
    CHECK_EQ(peak_bin(s), (size_t)8);
    /* Amplitude 0.5 is +6 dB against upstream's 0.25 reference: saturated. */
    CHECK_EQ(s.db[8], (uint8_t)255);
    CHECK_EQ(s.db[7], (uint8_t)0);
    CHECK_EQ(s.db[9], (uint8_t)0);
}

TEST(gfxeq_analyzer_level_matches_the_upstream_reference) {
    /* Upstream's 0 dB point is a quarter-scale sine: |FFT| = A*N/2 = 32, and
     * 32/32 = 1.0 -> 0 dB -> 255. */
    {
        AudioSpectrumAnalyzer a;
        const auto block = tone(3000.0, 0.25, AudioSpectrumAnalyzer::kFftSize);
        a.feed(block.data(), block.size());
        CHECK_NEAR(a.spectrum().db[8], 255, 1);
    }
    /* A tenth of that amplitude is 20 dB down: 255 - 20*5 = 155. */
    {
        AudioSpectrumAnalyzer a;
        const auto block = tone(3000.0, 0.025, AudioSpectrumAnalyzer::kFftSize);
        a.feed(block.data(), block.size());
        CHECK_NEAR(a.spectrum().db[8], 155, 1);
    }
    /* 40 dB down: 255 - 200 = 55. */
    {
        AudioSpectrumAnalyzer a;
        const auto block = tone(3000.0, 0.0025, AudioSpectrumAnalyzer::kFftSize);
        a.feed(block.data(), block.size());
        CHECK_NEAR(a.spectrum().db[8], 55, 1);
    }
}

TEST(gfxeq_analyzer_reset_clears_everything) {
    AudioSpectrumAnalyzer a;
    const auto block = tone(3000.0, 0.5, 300);
    a.feed(block.data(), block.size());
    CHECK_EQ(a.pending(), (size_t)44);
    CHECK_EQ(a.spectrum().db[8], (uint8_t)255);

    a.reset();
    CHECK_EQ(a.pending(), (size_t)0);
    for (size_t i = 0; i < a.spectrum().db.size(); i++)
        CHECK_EQ(a.spectrum().db[i], (uint8_t)0);
}

/* --- band reduction: bins -> the eleven bars ------------------------------- */

TEST(gfxeq_band_reduction_lands_in_the_right_bar) {
    /* GraphEq's FREQUENCY_BANDS edges, in Hz:
     *   375 750 1500 2250 3375 4875 6750 9375 13125 16875 20625 24375
     * A tone placed inside a band, on a bin that band does not share with its
     * neighbours, must make that bar and only that bar rise. */
    struct Case {
        double hz;
        size_t bin;
        size_t bar;
    };
    const Case cases[] = {
        {375.0, 1, 0},     /* band 0 covers bins 1..2 */
        {1875.0, 5, 2},    /* band 2 covers bins 4..6 */
        {3000.0, 8, 3},    /* band 3 covers bins 6..9 */
        {5625.0, 15, 5},   /* band 5 covers bins 13..18 */
        {9750.0, 26, 7},   /* band 7 covers bins 25..35 */
        {20250.0, 54, 9},  /* band 9 covers bins 45..55 */
    };

    for (const auto& c : cases) {
        AudioSpectrumAnalyzer a;
        const auto block = tone(c.hz, 0.5, AudioSpectrumAnalyzer::kFftSize);
        CHECK(a.feed(block.data(), block.size()));

        CHECK_EQ(peak_bin(a.spectrum()), c.bin);
        CHECK_EQ(bar_for_spectrum(a.spectrum()), c.bar);
    }
}

TEST(gfxeq_band_reduction_leaves_silence_flat) {
    AudioSpectrumAnalyzer a;
    const std::vector<float> silence(AudioSpectrumAnalyzer::kFftSize, 0.0f);
    a.feed(silence.data(), silence.size());

    ui::GraphEq eq{{0, 0, 240, 288}};
    eq.set_parent_rect({0, 0, 240, 288});
    eq.update_audio_spectrum(a.spectrum());

    for (size_t b = 0; b < static_cast<size_t>(ui::GraphEq::NUM_BARS); b++)
        CHECK_EQ(eq.bar_height(b), ui::Dim{0});
}

/* --- the WFM tap ----------------------------------------------------------- */

TEST(gfxeq_tap_rate_plan_matches_upstream) {
    WfmAudioTap tap;
    tap.configure(GfxEqView::kSampleRate);

    CHECK(tap.configured());
    /* 1.536 Msps / 4 = 384 kHz, upstream's WFM demodulator rate. */
    CHECK_EQ(tap.channel_decimation(), (size_t)4);
    CHECK_NEAR(tap.channel_rate(), 384000.0, 0.001);
    /* 384 kHz / 4 = 96 kHz, upstream's audio spectrum feed rate. */
    CHECK_EQ(tap.audio_decimation(), (size_t)4);
    CHECK_NEAR(tap.audio_rate(), AudioSpectrumAnalyzer::kAudioRate, 0.001);
}

TEST(gfxeq_tap_rejects_a_bad_configuration) {
    WfmAudioTap tap;
    std::vector<float> out;
    const std::vector<dsp::cfloat> block(1024, dsp::cfloat{1.0f, 0.0f});

    /* Never configured. */
    CHECK_EQ(tap.process(block.data(), block.size(), out), (size_t)0);

    tap.configure(0.0);
    CHECK(!tap.configured());
    CHECK_EQ(tap.process(block.data(), block.size(), out), (size_t)0);

    /* Configured, but nothing to chew on. */
    tap.configure(GfxEqView::kSampleRate);
    CHECK(tap.configured());
    CHECK_EQ(tap.process(nullptr, 16, out), (size_t)0);
    CHECK_EQ(tap.process(block.data(), 0, out), (size_t)0);
}

TEST(gfxeq_tap_block_size_feeds_exactly_one_transform) {
    WfmAudioTap tap;
    tap.configure(GfxEqView::kSampleRate);

    double phase = 0.0;
    const auto block = fm_signal(GfxEqView::kSampleRate, 0.0, 3000.0, 30000.0,
                                 GfxEqView::kTapSamples, phase, 0);

    std::vector<float> audio;
    /* 4096 / 4 / 4 = 256: one tap read is one 256-point FFT, which is why the
     * app asks the receiver for 4096 samples a frame. */
    CHECK_EQ(tap.process(block.data(), block.size(), audio),
             AudioSpectrumAnalyzer::kFftSize);
}

TEST(gfxeq_tap_resamples_an_awkward_device_rate) {
    /* If the device will not give us 1.536 Msps the bins must still be 375 Hz
     * wide, so the tap trims what the integer decimators cannot. */
    WfmAudioTap tap;
    tap.configure(2'000'000.0);

    CHECK(tap.configured());
    CHECK_EQ(tap.channel_decimation(), (size_t)5);
    CHECK_NEAR(tap.channel_rate(), 400000.0, 0.001);
    CHECK_EQ(tap.audio_decimation(), (size_t)4);
    CHECK_NEAR(tap.audio_rate(), 100000.0, 0.001);

    double phase = 0.0;
    const auto block = fm_signal(2'000'000.0, 0.0, 3000.0, 30000.0, 8192, phase, 0);
    std::vector<float> audio;
    const size_t produced = tap.process(block.data(), block.size(), audio);

    /* 8192 samples is 4.096 ms; at 96 kHz that is ~393 audio samples. */
    CHECK(produced > 380);
    CHECK(produced < 400);
}

/* --- end to end: FM carrier in, bar out ------------------------------------ */

TEST(gfxeq_decodes_a_synthetic_fm_station_into_the_right_bar) {
    /* A 3 kHz tone on a broadcast-FM carrier sitting 200 kHz above the LO —
     * the case where the receiver has parked the LO off-channel and left the
     * app's NCO to make up the difference. */
    constexpr double kOffset = 200000.0;
    constexpr double kAudioHz = 3000.0;

    WfmAudioTap tap;
    tap.set_offset(kOffset);
    tap.configure(GfxEqView::kSampleRate);

    AudioSpectrumAnalyzer analyzer;
    std::vector<float> audio;

    double phase = 0.0;
    size_t offset_samples = 0;
    bool published = false;

    /* Six blocks: the first couple flush the filters' zero history, and the
     * spectrum checked below is the last one published. */
    for (int block_index = 0; block_index < 6; block_index++) {
        const auto raw = fm_signal(GfxEqView::kSampleRate, kOffset, kAudioHz,
                                   30000.0, GfxEqView::kTapSamples, phase,
                                   offset_samples);
        offset_samples += GfxEqView::kTapSamples;

        const size_t n = tap.process(raw.data(), raw.size(), audio);
        CHECK_EQ(n, AudioSpectrumAnalyzer::kFftSize);
        published = analyzer.feed(audio.data(), n);
        CHECK(published);
    }

    const auto& s = analyzer.spectrum();
    /* 3 kHz / 375 Hz = bin 8. */
    CHECK_EQ(peak_bin(s), (size_t)8);
    /* 30 kHz deviation against the demodulator's 75 kHz full scale is 0.4, well
     * over upstream's 0.25 reference, so the bin saturates. */
    CHECK_EQ(s.db[8], (uint8_t)255);
    /* A clean tone through a flat channel leaves no harmonics: bin 8 is the only
     * thing in the published spectrum, and band 3 (2250-3375 Hz) the only bar. */
    CHECK_EQ(s.db[16], (uint8_t)0);
    CHECK_EQ(s.db[24], (uint8_t)0);
    CHECK_EQ(bar_for_spectrum(s), (size_t)3);

    ui::GraphEq eq{{0, 0, 240, 288}};
    eq.set_parent_rect({0, 0, 240, 288});
    eq.update_audio_spectrum(s);
    CHECK(eq.bar_height(3) > 0);
    for (size_t b = 0; b < static_cast<size_t>(ui::GraphEq::NUM_BARS); b++)
        if (b != 3) CHECK_EQ(eq.bar_height(b), ui::Dim{0});
}

TEST(gfxeq_decodes_a_treble_tone_into_the_treble_bar) {
    /* Same chain, an audio tone up where the treble bands live: 9750 Hz is bin
     * 26, inside band 7 (9375-13125 Hz). */
    constexpr double kAudioHz = 9750.0;

    WfmAudioTap tap;
    tap.set_offset(0.0);
    tap.configure(GfxEqView::kSampleRate);

    AudioSpectrumAnalyzer analyzer;
    std::vector<float> audio;

    double phase = 0.0;
    size_t offset_samples = 0;

    for (int block_index = 0; block_index < 6; block_index++) {
        const auto raw = fm_signal(GfxEqView::kSampleRate, 0.0, kAudioHz, 30000.0,
                                   GfxEqView::kTapSamples, phase, offset_samples);
        offset_samples += GfxEqView::kTapSamples;
        const size_t n = tap.process(raw.data(), raw.size(), audio);
        analyzer.feed(audio.data(), n);
    }

    CHECK_EQ(peak_bin(analyzer.spectrum()), (size_t)26);
    CHECK_EQ(bar_for_spectrum(analyzer.spectrum()), (size_t)7);
}

TEST(gfxeq_off_channel_station_reads_as_harmonic_distortion) {
    /* The same 200 kHz-offset station, but with the tap left tuned to the LO.
     *
     * Measured, not assumed: the display does NOT go dark. A phase
     * discriminator is amplitude-blind, and gfxEQ has no squelch (upstream has
     * none either), so a station the channel filter is attenuating still
     * demodulates at full level. What changes is the *shape*: the filter's skirt
     * is not flat across the FM sidebands, so the recovered tone is distorted
     * and its harmonics — 6 kHz at bin 16, 9 kHz at bin 24, and on up — appear
     * alongside it. In tune those bins are 0 (asserted above), which is what
     * makes this the negative control for the test above. */
    WfmAudioTap tap;
    tap.set_offset(0.0);
    tap.configure(GfxEqView::kSampleRate);

    AudioSpectrumAnalyzer analyzer;
    std::vector<float> audio;

    double phase = 0.0;
    size_t offset_samples = 0;
    for (int block_index = 0; block_index < 6; block_index++) {
        const auto raw = fm_signal(GfxEqView::kSampleRate, 200000.0, 3000.0, 30000.0,
                                   GfxEqView::kTapSamples, phase, offset_samples);
        offset_samples += GfxEqView::kTapSamples;
        const size_t n = tap.process(raw.data(), raw.size(), audio);
        analyzer.feed(audio.data(), n);
    }

    const auto& s = analyzer.spectrum();
    /* The fundamental survives... */
    CHECK_EQ(s.db[8], (uint8_t)255);
    /* ...but so do its harmonics, which the in-tune case does not have. */
    CHECK_EQ(s.db[16], (uint8_t)255);
    CHECK_EQ(s.db[24], (uint8_t)255);

    ui::GraphEq eq{{0, 0, 240, 288}};
    eq.set_parent_rect({0, 0, 240, 288});
    eq.update_audio_spectrum(s);

    size_t lit = 0;
    for (size_t b = 0; b < static_cast<size_t>(ui::GraphEq::NUM_BARS); b++)
        if (eq.bar_height(b) > 0) lit++;
    CHECK(lit > 1);
}

TEST(gfxeq_unmodulated_carrier_produces_no_bars) {
    /* A dead carrier demodulates to DC, which GraphEq's band table starts above
     * — nothing should light up. */
    WfmAudioTap tap;
    tap.set_offset(0.0);
    tap.configure(GfxEqView::kSampleRate);

    AudioSpectrumAnalyzer analyzer;
    std::vector<float> audio;

    double phase = 0.0;
    size_t offset_samples = 0;
    for (int block_index = 0; block_index < 6; block_index++) {
        const auto raw = fm_signal(GfxEqView::kSampleRate, 0.0, 1000.0, 0.0,
                                   GfxEqView::kTapSamples, phase, offset_samples);
        offset_samples += GfxEqView::kTapSamples;
        const size_t n = tap.process(raw.data(), raw.size(), audio);
        analyzer.feed(audio.data(), n);
    }

    ui::GraphEq eq{{0, 0, 240, 288}};
    eq.set_parent_rect({0, 0, 240, 288});
    eq.update_audio_spectrum(analyzer.spectrum());

    for (size_t b = 0; b < static_cast<size_t>(ui::GraphEq::NUM_BARS); b++)
        CHECK_EQ(eq.bar_height(b), ui::Dim{0});
}

/* --- themes ---------------------------------------------------------------- */

TEST(gfxeq_themes_match_upstream_and_wrap) {
    CHECK_EQ(gfxeq_theme_count(), (size_t)20);

    /* First and last entries of upstream's `themes` array. */
    CHECK_EQ(gfxeq_theme(0).base.v, ui::Color(255, 0, 255).v);
    CHECK_EQ(gfxeq_theme(0).peak.v, ui::Color(255, 255, 255).v);
    CHECK_EQ(gfxeq_theme(19).base.v, ui::Color(255, 192, 0).v);
    CHECK_EQ(gfxeq_theme(19).peak.v, ui::Color(0, 64, 128).v);

    /* MOOD just increments, so the accessor has to wrap. */
    CHECK_EQ(gfxeq_theme(20).base.v, gfxeq_theme(0).base.v);
    CHECK_EQ(gfxeq_theme(41).peak.v, gfxeq_theme(1).peak.v);
}
