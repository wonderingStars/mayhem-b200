/*
 * mayhem-b200 — DSP tests.
 *
 * Every demodulator here is checked against a signal whose correct output is
 * known analytically, not against a golden vector, so a failure points at the
 * maths rather than at a changed constant.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "demod.hpp"
#include "fft.hpp"
#include "fir.hpp"
#include "ring_buffer.hpp"

#include <cmath>
#include <complex>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<dsp::cfloat> complex_tone(double freq_hz, double fs, size_t n, double amplitude = 1.0) {
    std::vector<dsp::cfloat> v(n);
    for (size_t i = 0; i < n; i++) {
        const double p = 2.0 * kPi * freq_hz * static_cast<double>(i) / fs;
        v[i] = dsp::cfloat{static_cast<float>(amplitude * std::cos(p)),
                           static_cast<float>(amplitude * std::sin(p))};
    }
    return v;
}

/* RMS of the second half of a buffer, skipping filter start-up transients. */
float steady_state_rms(const std::vector<float>& v) {
    if (v.size() < 4) return 0.0f;
    const size_t start = v.size() / 2;
    return dsp::rms(v.data() + start, v.size() - start);
}

}  // namespace

/* --- RingBuffer ------------------------------------------------------------ */

TEST(ring_buffer_roundtrip) {
    dsp::RingBuffer<int> rb{16};
    CHECK(rb.empty());

    const int in[5] = {1, 2, 3, 4, 5};
    CHECK_EQ(rb.write(in, 5), size_t{5});
    CHECK_EQ(rb.size(), size_t{5});

    int out[5] = {};
    CHECK_EQ(rb.read(out, 5), size_t{5});
    for (int i = 0; i < 5; i++) CHECK_EQ(out[i], in[i]);
    CHECK(rb.empty());
}

TEST(ring_buffer_short_write_on_overrun) {
    /* Capacity rounds up to a power of two and keeps one slot free, so a
     * request for 4 yields 7 usable slots. The point of the test is that an
     * over-long write is truncated rather than corrupting the buffer. */
    dsp::RingBuffer<int> rb{4};
    std::vector<int> big(1000, 7);

    const size_t written = rb.write(big.data(), big.size());
    CHECK(written < big.size());
    CHECK_EQ(written, rb.capacity());
    CHECK_EQ(rb.space(), size_t{0});
}

TEST(ring_buffer_short_read_on_underrun) {
    dsp::RingBuffer<int> rb{16};
    const int in[2] = {9, 8};
    rb.write(in, 2);

    int out[10] = {};
    CHECK_EQ(rb.read(out, 10), size_t{2});
    CHECK_EQ(out[0], 9);
    CHECK_EQ(out[1], 8);
}

TEST(ring_buffer_wraps_around) {
    dsp::RingBuffer<int> rb{8};
    /* Push and pop repeatedly so the indices wrap several times. */
    for (int round = 0; round < 20; round++) {
        const int in[3] = {round, round + 1, round + 2};
        CHECK_EQ(rb.write(in, 3), size_t{3});
        int out[3] = {};
        CHECK_EQ(rb.read(out, 3), size_t{3});
        CHECK_EQ(out[0], round);
        CHECK_EQ(out[2], round + 2);
    }
}

/* --- FIR ------------------------------------------------------------------- */

TEST(lowpass_has_unity_dc_gain) {
    const auto taps = dsp::design_lowpass(1000.0, 500.0, 48000.0, 60.0);
    double sum = 0.0;
    for (float t : taps) sum += t;
    CHECK_NEAR(sum, 1.0, 1e-5);
    /* Odd length gives an integer group delay. */
    CHECK_EQ(taps.size() % 2, size_t{1});
}

TEST(lowpass_passes_dc_and_stops_nyquist) {
    const double fs = 48000.0;
    const auto taps = dsp::design_lowpass(2000.0, 1000.0, fs, 60.0);

    dsp::FirDecimateC filt{taps, 1};
    std::vector<dsp::cfloat> out;

    /* DC (a constant complex value) should pass at unity. */
    const std::vector<dsp::cfloat> dc(2048, dsp::cfloat{1.0f, 0.0f});
    filt.process(dc.data(), dc.size(), out);
    CHECK_NEAR(std::abs(out.back()), 1.0, 1e-3);

    /* A tone well into the stopband should be strongly attenuated. */
    filt.reset();
    out.clear();
    const auto tone = complex_tone(10000.0, fs, 2048);
    filt.process(tone.data(), tone.size(), out);

    const float tail = std::abs(out[out.size() - 1]);
    CHECK(tail < 0.01f);  /* better than -40 dB */
}

TEST(decimator_output_count_and_antialiasing) {
    const double fs = 48000.0;
    const size_t decim = 4;
    const auto taps = dsp::design_lowpass(4000.0, 2000.0, fs, 60.0);

    dsp::FirDecimateC filt{taps, decim};
    std::vector<dsp::cfloat> out;

    const auto tone = complex_tone(1000.0, fs, 4096);
    const size_t produced = filt.process(tone.data(), tone.size(), out);

    CHECK_EQ(produced, tone.size() / decim);
    CHECK_EQ(out.size(), produced);

    /* A 1 kHz tone is inside the passband, so amplitude survives. */
    CHECK_NEAR(std::abs(out.back()), 1.0, 0.05);
}

TEST(real_decimator_matches_expected_count) {
    dsp::FirDecimateR filt{dsp::design_lowpass_fixed(1000.0, 48000.0, 31), 3};
    std::vector<float> in(300, 1.0f);
    std::vector<float> out;

    const size_t n = filt.process(in.data(), in.size(), out);
    CHECK_EQ(n, size_t{100});
    /* Constant input through a unity-DC-gain filter settles at that constant. */
    CHECK_NEAR(out.back(), 1.0, 1e-3);
}

/* --- FFT ------------------------------------------------------------------- */

TEST(fft_rounds_up_to_power_of_two) {
    dsp::Fft f{1000};
    CHECK_EQ(f.size(), size_t{1024});
}

TEST(fft_inverse_reconstructs_input) {
    const size_t n = 256;
    std::vector<dsp::cfloat> original(n);
    for (size_t i = 0; i < n; i++)
        original[i] = dsp::cfloat{static_cast<float>(std::sin(i * 0.1)),
                                  static_cast<float>(std::cos(i * 0.07))};

    auto buf = original;
    dsp::Fft f{n};
    f.transform(buf, false);
    f.transform(buf, true);

    for (size_t i = 0; i < n; i++) {
        CHECK_NEAR(buf[i].real(), original[i].real(), 1e-4);
        CHECK_NEAR(buf[i].imag(), original[i].imag(), 1e-4);
    }
}

TEST(fft_tone_lands_in_expected_bin) {
    const size_t n = 1024;
    const double fs = 48000.0;
    /* Put the tone exactly on bin centre 64 above DC so there is no leakage. */
    const double bin_hz = fs / static_cast<double>(n);
    const double freq = 64.0 * bin_hz;

    const auto tone = complex_tone(freq, fs, n);

    dsp::Fft f{n};
    const auto window = dsp::make_window(dsp::WindowType::Rectangular, n);
    std::vector<float> spectrum;
    f.spectrum_db(tone.data(), window, spectrum);

    CHECK_EQ(spectrum.size(), n);

    /* Output is reordered so DC sits at n/2; the tone is 64 bins right of it. */
    const size_t expected_bin = n / 2 + 64;
    size_t peak = 0;
    for (size_t i = 1; i < spectrum.size(); i++)
        if (spectrum[i] > spectrum[peak]) peak = i;

    CHECK_EQ(peak, expected_bin);
    /* A unit-amplitude complex tone reads 0 dBFS. */
    CHECK_NEAR(spectrum[peak], 0.0, 0.1);
}

TEST(fft_negative_frequency_lands_left_of_centre) {
    const size_t n = 512;
    const double fs = 48000.0;
    const double freq = -32.0 * (fs / static_cast<double>(n));

    const auto tone = complex_tone(freq, fs, n);
    dsp::Fft f{n};
    const auto window = dsp::make_window(dsp::WindowType::Rectangular, n);
    std::vector<float> spectrum;
    f.spectrum_db(tone.data(), window, spectrum);

    size_t peak = 0;
    for (size_t i = 1; i < spectrum.size(); i++)
        if (spectrum[i] > spectrum[peak]) peak = i;

    CHECK_EQ(peak, n / 2 - 32);
}

TEST(window_coherent_gain_is_normalised) {
    for (auto type : {dsp::WindowType::Hamming, dsp::WindowType::Hann,
                      dsp::WindowType::Blackman, dsp::WindowType::BlackmanHarris}) {
        const auto w = dsp::make_window(type, 256);
        double sum = 0.0;
        for (float v : w) sum += v;
        CHECK_NEAR(sum / 256.0, 1.0, 1e-4);
    }
}

/* --- FM -------------------------------------------------------------------- */

TEST(fm_demod_scales_deviation_to_unity) {
    const float fs = 48000.0f;
    const float deviation = 5000.0f;

    dsp::FmDemod fm;
    fm.configure(fs, deviation);

    /* A constant frequency offset equal to half the deviation must read 0.5. */
    const auto tone = complex_tone(deviation / 2.0, fs, 1024);
    std::vector<float> out;
    fm.process(tone.data(), tone.size(), out);

    CHECK_EQ(out.size(), size_t{1024});
    /* Skip the first sample: it is measured against the initial phase state. */
    CHECK_NEAR(out[500], 0.5, 1e-3);
    CHECK_NEAR(out[1023], 0.5, 1e-3);
}

TEST(fm_demod_sign_follows_offset_direction) {
    const float fs = 48000.0f;
    dsp::FmDemod fm;
    fm.configure(fs, 5000.0f);

    const auto negative = complex_tone(-2500.0, fs, 512);
    std::vector<float> out;
    fm.process(negative.data(), negative.size(), out);
    CHECK_NEAR(out[400], -0.5, 1e-3);
}

TEST(fm_demod_reports_block_power) {
    dsp::FmDemod fm;
    fm.configure(48000.0f, 5000.0f);

    const auto tone = complex_tone(1000.0, 48000.0, 512, 0.5);
    std::vector<float> out;
    fm.process(tone.data(), tone.size(), out);

    /* Mean square of a complex tone of amplitude 0.5 is 0.25. */
    CHECK_NEAR(fm.last_block_power(), 0.25, 1e-4);
}

/* --- AM -------------------------------------------------------------------- */

TEST(am_demod_recovers_modulation) {
    const float fs = 48000.0f;
    const float audio_hz = 1000.0f;
    const float depth = 0.5f;

    dsp::AmDemod am;
    am.configure(fs);

    /* Carrier at DC, amplitude modulated. Envelope is (1 + depth*cos). */
    std::vector<dsp::cfloat> in(8192);
    for (size_t i = 0; i < in.size(); i++) {
        const double t = static_cast<double>(i) / fs;
        const float envelope = 1.0f + depth * static_cast<float>(std::cos(2.0 * kPi * audio_hz * t));
        in[i] = dsp::cfloat{envelope, 0.0f};
    }

    std::vector<float> out;
    am.process(in.data(), in.size(), out);

    /* After DC removal the residue is depth*cos, whose RMS is depth/sqrt(2). */
    CHECK_NEAR(steady_state_rms(out), depth / std::sqrt(2.0), 0.02);
}

/* --- SSB ------------------------------------------------------------------- */

TEST(ssb_usb_passes_positive_rejects_negative) {
    const float fs = 48000.0f;
    const size_t taps = 127;

    dsp::SsbDemod ssb;
    ssb.configure(fs, dsp::SsbDemod::Sideband::Upper, taps);

    /* 3 kHz is comfortably inside the band where a 127-tap Hilbert is accurate. */
    const auto positive = complex_tone(3000.0, fs, 8192);
    std::vector<float> out_pos;
    ssb.process(positive.data(), positive.size(), out_pos);

    ssb.reset();
    const auto negative = complex_tone(-3000.0, fs, 8192);
    std::vector<float> out_neg;
    ssb.process(negative.data(), negative.size(), out_neg);

    const float pass = steady_state_rms(out_pos);
    const float reject = steady_state_rms(out_neg);

    /* USB = I - H{Q}: the wanted tone doubles, the image cancels. */
    CHECK_NEAR(pass, 2.0 / std::sqrt(2.0), 0.05);
    CHECK(reject < pass * 0.05f);  /* better than 26 dB opposite-sideband rejection */
}

TEST(ssb_lsb_is_the_mirror_of_usb) {
    const float fs = 48000.0f;

    dsp::SsbDemod lsb;
    lsb.configure(fs, dsp::SsbDemod::Sideband::Lower, 127);

    const auto negative = complex_tone(-3000.0, fs, 8192);
    std::vector<float> out_neg;
    lsb.process(negative.data(), negative.size(), out_neg);

    lsb.reset();
    const auto positive = complex_tone(3000.0, fs, 8192);
    std::vector<float> out_pos;
    lsb.process(positive.data(), positive.size(), out_pos);

    CHECK_NEAR(steady_state_rms(out_neg), 2.0 / std::sqrt(2.0), 0.05);
    CHECK(steady_state_rms(out_pos) < steady_state_rms(out_neg) * 0.05f);
}

/* --- Resampler ------------------------------------------------------------- */

TEST(resampler_downsamples_by_expected_ratio) {
    dsp::Resampler r;
    r.configure(96000.0, 48000.0);

    std::vector<float> in(1000, 1.0f);
    std::vector<float> out;
    r.process(in.data(), in.size(), out);

    /* Allow one sample of slack for the phase accumulator's start-up. */
    CHECK(out.size() >= 499 && out.size() <= 501);
}

TEST(resampler_upsamples_by_expected_ratio) {
    dsp::Resampler r;
    r.configure(48000.0, 96000.0);

    std::vector<float> in(500, 1.0f);
    std::vector<float> out;
    r.process(in.data(), in.size(), out);

    CHECK(out.size() >= 999 && out.size() <= 1001);
}

TEST(resampler_preserves_a_constant) {
    dsp::Resampler r;
    r.configure(44100.0, 48000.0);

    std::vector<float> in(2000, 0.75f);
    std::vector<float> out;
    r.process(in.data(), in.size(), out);

    CHECK(!out.empty());
    for (size_t i = 10; i < out.size(); i++) CHECK_NEAR(out[i], 0.75, 1e-5);
}

/* --- Squelch --------------------------------------------------------------- */

TEST(squelch_level_zero_always_open) {
    dsp::Squelch sq;
    sq.configure(48000.0f);
    sq.set_level(0);

    CHECK(sq.update(0.0f));
    CHECK(sq.update(1e-9f));
    CHECK(sq.open());
}

TEST(squelch_closes_on_weak_signal_and_opens_on_strong) {
    dsp::Squelch sq;
    sq.configure(48000.0f);
    sq.set_level(50);  /* about -50 dBFS */

    /* Drive it well below threshold for long enough to settle. */
    for (int i = 0; i < 200; i++) sq.update(1e-5f);
    CHECK(!sq.open());

    for (int i = 0; i < 200; i++) sq.update(0.5f);
    CHECK(sq.open());
}

TEST(squelch_hysteresis_prevents_chatter) {
    dsp::Squelch sq;
    sq.configure(48000.0f, 6.0f);
    sq.set_level(50);

    for (int i = 0; i < 200; i++) sq.update(0.5f);
    CHECK(sq.open());

    /* A level just under the open threshold must not immediately close it. */
    const float threshold_amp = std::pow(10.0f, -50.0f / 20.0f);
    for (int i = 0; i < 200; i++) sq.update(threshold_amp * 0.8f);
    CHECK(sq.open());
}

/* --- AGC ------------------------------------------------------------------- */

TEST(agc_brings_quiet_audio_up_towards_target) {
    dsp::AudioAgc agc;
    agc.configure(48000.0f, 5.0f, 100.0f, 0.5f, 64.0f);

    std::vector<float> quiet(48000, 0.0f);
    for (size_t i = 0; i < quiet.size(); i++)
        quiet[i] = 0.02f * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * i / 48000.0));

    agc.process(quiet.data(), quiet.size());

    /* The tail should sit near the target amplitude, well above the input. */
    const float tail = dsp::rms(quiet.data() + 40000, 8000);
    CHECK(tail > 0.1f);
    CHECK(tail < 0.8f);
}

TEST(agc_disabled_is_a_passthrough) {
    dsp::AudioAgc agc;
    agc.configure(48000.0f);
    agc.set_enabled(false);

    std::vector<float> x{0.1f, -0.2f, 0.3f};
    const auto before = x;
    agc.process(x.data(), x.size());

    for (size_t i = 0; i < x.size(); i++) CHECK_NEAR(x[i], before[i], 1e-9);
}

/* --- helpers --------------------------------------------------------------- */

TEST(to_db_floors_silence) {
    CHECK_NEAR(dsp::to_db(1.0f), 0.0, 1e-4);
    CHECK_NEAR(dsp::to_db(0.5f), -6.0206, 1e-3);
    /* Must not return -inf. */
    const float silent = dsp::to_db(0.0f);
    CHECK(std::isfinite(silent));
    CHECK_NEAR(silent, -140.0, 1e-3);
}

TEST(nco_shifts_a_tone) {
    const double fs = 48000.0;
    auto tone = complex_tone(1000.0, fs, 1024);

    dsp::Nco nco;
    nco.set_frequency(-1000.0, fs);
    nco.mix(tone.data(), tone.data(), tone.size());

    /* Shifting a +1 kHz tone down by 1 kHz leaves DC: constant phase. */
    for (size_t i = 800; i < 1024; i++) {
        CHECK_NEAR(tone[i].real(), 1.0, 1e-3);
        CHECK_NEAR(tone[i].imag(), 0.0, 1e-3);
    }
}
