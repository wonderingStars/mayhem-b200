/*
 * mayhem-b200 — modulator tests.
 *
 * Every case here is checked against something outside the implementation: an
 * analytic result (the frequency an FM modulator must produce for a given
 * input, the envelope ratio an AM modulation index must give), a round trip
 * through the demodulator in dsp/demod.hpp, or a value taken verbatim from the
 * PortaPack firmware (the BLE Gaussian pulse table, the DCS parity table, the
 * CTCSS tone list). Nothing is asserted against whatever the code happens to
 * emit.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "demod.hpp"
#include "fir.hpp"
#include "modulate.hpp"

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

std::vector<float> real_tone(double freq_hz, double fs, size_t n, double amplitude = 1.0) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; i++)
        v[i] = static_cast<float>(amplitude *
                                  std::sin(kTwoPi * freq_hz * static_cast<double>(i) / fs));
    return v;
}

/* Instantaneous frequency in Hz from the phase step between two samples — the
 * same discriminator dsp::FmDemod uses, without its scaling. */
double inst_freq(const std::vector<dsp::cfloat>& x, size_t i, double fs) {
    const dsp::cfloat d = x[i] * std::conj(x[i - 1]);
    return std::atan2(static_cast<double>(d.imag()), static_cast<double>(d.real())) *
           fs / kTwoPi;
}

/* Complex amplitude of `x` at `freq_hz`, over `n` samples starting at `start`.
 * Choose n to span a whole number of cycles so there is no leakage. */
std::complex<double> bin(const std::vector<dsp::cfloat>& x,
                         double freq_hz, double fs, size_t start, size_t n) {
    std::complex<double> acc{0.0, 0.0};
    for (size_t i = 0; i < n; i++) {
        const double p = -kTwoPi * freq_hz * static_cast<double>(start + i) / fs;
        acc += std::complex<double>{x[start + i].real(), x[start + i].imag()} *
               std::complex<double>{std::cos(p), std::sin(p)};
    }
    return acc / static_cast<double>(n);
}

/* Peak amplitude of a real sinusoid at `freq_hz` inside `x`. */
double tone_amplitude(const std::vector<float>& x,
                      double freq_hz, double fs, size_t start, size_t n) {
    std::complex<double> acc{0.0, 0.0};
    for (size_t i = 0; i < n; i++) {
        const double p = -kTwoPi * freq_hz * static_cast<double>(start + i) / fs;
        acc += static_cast<double>(x[start + i]) * std::complex<double>{std::cos(p), std::sin(p)};
    }
    return 2.0 * std::abs(acc) / static_cast<double>(n);
}

/* MSB-first bit packing, the order the firmware's keyers read. */
std::vector<uint8_t> bits_from(const char* pattern) {
    std::vector<uint8_t> bytes;
    size_t index = 0;
    for (const char* p = pattern; *p; p++, index++) {
        if ((index % 8) == 0) bytes.push_back(0);
        if (*p == '1') bytes.back() = static_cast<uint8_t>(bytes.back() | (0x80u >> (index % 8)));
    }
    return bytes;
}

}  // namespace

/* --- Gaussian pulse --------------------------------------------------------- */

TEST(gaussian_pulse_matches_firmware_ble_table) {
    /* baseband/proc_ble_tx.hpp carries this pulse as a literal: BT = 0.5, four
     * samples per symbol, four-symbol span. Both are compared after
     * normalisation because upstream scales its copy by 2. */
    const double upstream[16] = {
        7.561773e-09, 1.197935e-06, 8.050684e-05, 2.326833e-03,
        2.959908e-02, 1.727474e-01, 4.999195e-01, 8.249246e-01,
        9.408018e-01, 8.249246e-01, 4.999195e-01, 1.727474e-01,
        2.959908e-02, 2.326833e-03, 8.050684e-05, 1.197935e-06};

    double upstream_sum = 0.0;
    for (double v : upstream) upstream_sum += v;

    const auto taps = dsp::design_gaussian_pulse(0.5, 4.0, 4);
    CHECK_EQ(taps.size(), size_t{16});

    for (size_t i = 0; i < taps.size(); i++)
        CHECK_NEAR(taps[i], upstream[i] / upstream_sum, 1e-7);
}

TEST(gaussian_pulse_has_unit_sum) {
    /* A run of identical symbols must reach exactly the full deviation, which
     * is only true if the pulse integrates to one. */
    const auto taps = dsp::design_gaussian_pulse(0.3, 20.0, 6);
    double sum = 0.0;
    for (float t : taps) sum += t;
    CHECK_NEAR(sum, 1.0, 1e-6);
    CHECK_EQ(taps.size(), size_t{120});
}

TEST(gaussian_pulse_degenerate_inputs_are_pass_through) {
    CHECK_EQ(dsp::design_gaussian_pulse(0.0, 4.0, 4).size(), size_t{1});
    CHECK_EQ(dsp::design_gaussian_pulse(0.5, 0.0, 4).size(), size_t{1});
    CHECK_NEAR(dsp::design_gaussian_pulse(0.0, 4.0, 4)[0], 1.0, 1e-9);
}

/* --- FM modulator ----------------------------------------------------------- */

TEST(fm_modulator_constant_input_gives_exact_frequency_offset) {
    /* An input of 0.5 at a 5 kHz deviation is 2500 Hz of offset, by definition
     * of the deviation. */
    constexpr double fs = 48000.0;
    dsp::FmModulator fm;
    fm.configure(static_cast<float>(fs), 5000.0f);

    const std::vector<float> audio(2048, 0.5f);
    std::vector<dsp::cfloat> out;
    fm.process(audio.data(), audio.size(), out);

    CHECK_EQ(out.size(), audio.size());
    for (size_t i = 1; i < out.size(); i++)
        CHECK_NEAR(inst_freq(out, i, fs), 2500.0, 1e-3);
}

TEST(fm_modulator_negative_input_offsets_the_other_way) {
    constexpr double fs = 48000.0;
    dsp::FmModulator fm;
    fm.configure(static_cast<float>(fs), 2500.0f);

    const std::vector<float> audio(512, -1.0f);
    std::vector<dsp::cfloat> out;
    fm.process(audio.data(), audio.size(), out);

    for (size_t i = 1; i < out.size(); i++)
        CHECK_NEAR(inst_freq(out, i, fs), -2500.0, 1e-3);
}

TEST(fm_modulator_output_is_constant_envelope) {
    constexpr double fs = 48000.0;
    dsp::FmModulator fm;
    fm.configure(static_cast<float>(fs), 5000.0f);

    const auto audio = real_tone(700.0, fs, 4096, 0.9);
    std::vector<dsp::cfloat> out;
    fm.process(audio.data(), audio.size(), out);

    for (const auto& s : out) CHECK_NEAR(std::abs(s), 1.0, 1e-5);
}

TEST(fm_modulator_round_trips_through_fm_demod) {
    /* dsp::FmDemod divides out exactly the scaling the modulator applied, so at
     * the same deviation the pair is an identity on the audio. */
    constexpr double fs = 48000.0;
    constexpr double deviation = 5000.0;
    constexpr double tone_hz = 300.0;
    constexpr double amplitude = 0.7;

    dsp::FmModulator fm;
    fm.configure(static_cast<float>(fs), static_cast<float>(deviation));

    const auto audio = real_tone(tone_hz, fs, 4096, amplitude);
    std::vector<dsp::cfloat> modulated;
    fm.process(audio.data(), audio.size(), modulated);

    dsp::FmDemod demod;
    demod.configure(static_cast<float>(fs), static_cast<float>(deviation));
    std::vector<float> recovered;
    demod.process(modulated.data(), modulated.size(), recovered);

    CHECK_EQ(recovered.size(), audio.size());

    /* Skip the first sample: the discriminator has no previous sample yet. */
    const size_t start = 160;                       /* whole cycles of 300 Hz */
    const size_t n = 3840;                          /* 24 cycles at 300 Hz */
    CHECK_NEAR(tone_amplitude(recovered, tone_hz, fs, start, n), amplitude, 2e-3);
}

TEST(fm_demod_at_a_different_deviation_scales_the_audio) {
    /* Demodulating at twice the deviation the transmitter used halves the
     * recovered audio — the failure mode of a mismatched NFM setting. */
    constexpr double fs = 48000.0;

    dsp::FmModulator fm;
    fm.configure(static_cast<float>(fs), 2500.0f);

    const std::vector<float> audio(4096, 0.8f);
    std::vector<dsp::cfloat> modulated;
    fm.process(audio.data(), audio.size(), modulated);

    dsp::FmDemod demod;
    demod.configure(static_cast<float>(fs), 5000.0f);
    std::vector<float> recovered;
    demod.process(modulated.data(), modulated.size(), recovered);

    CHECK_NEAR(recovered[2048], 0.4, 1e-5);
}

/* --- AM modulator ----------------------------------------------------------- */

TEST(am_modulation_depth_appears_in_the_envelope) {
    /* (Emax - Emin) / (Emax + Emin) is the modulation index, whatever the
     * carrier is scaled to. 1000 Hz at 48 kHz lands samples exactly on the
     * sine's peaks, so the envelope extremes are exact. */
    constexpr double fs = 48000.0;
    const auto audio = real_tone(1000.0, fs, 480, 1.0);

    for (float depth : {1.0f, 0.7f, 0.5f, 0.3f}) {
        dsp::AmModulator am;
        am.configure(static_cast<float>(fs), depth, dsp::AmModulator::Variant::AM);

        std::vector<dsp::cfloat> out;
        am.process(audio.data(), audio.size(), out);

        float e_max = -1e9f;
        float e_min = 1e9f;
        for (const auto& s : out) {
            const float e = std::abs(s);
            e_max = std::max(e_max, e);
            e_min = std::min(e_min, e);
        }

        CHECK_NEAR((e_max - e_min) / (e_max + e_min), depth, 1e-5);
        /* Peak envelope is normalised to 1.0 so nothing clips the DAC. */
        CHECK_NEAR(e_max, 1.0, 1e-5);
    }
}

TEST(am_silent_audio_transmits_the_carrier) {
    dsp::AmModulator am;
    am.configure(48000.0f, 0.5f, dsp::AmModulator::Variant::AM);
    CHECK_NEAR(am.carrier_level(), 1.0 / 1.5, 1e-6);

    const std::vector<float> silence(64, 0.0f);
    std::vector<dsp::cfloat> out;
    am.process(silence.data(), silence.size(), out);

    for (const auto& s : out) CHECK_NEAR(std::abs(s), 1.0 / 1.5, 1e-6);
}

TEST(am_overmodulation_clamps_instead_of_inverting_the_carrier) {
    /* An input past +/-1 would drive the envelope negative, which is a phase
     * reversal, not louder audio. */
    dsp::AmModulator am;
    am.configure(48000.0f, 1.0f, dsp::AmModulator::Variant::AM);

    const std::vector<float> loud = {2.0f, -2.0f, 5.0f, -5.0f, 0.0f};
    std::vector<dsp::cfloat> out;
    am.process(loud.data(), loud.size(), out);

    for (const auto& s : out) CHECK(s.real() >= 0.0f);
    CHECK_NEAR(out[0].real(), 1.0, 1e-6);
    CHECK_NEAR(out[1].real(), 0.0, 1e-6);
    CHECK_NEAR(out[4].real(), 0.5, 1e-6);
}

TEST(dsb_suppresses_the_carrier) {
    /* Mayhem's Mode::DSB: the baseband is the audio itself, so there is no DC
     * term and the envelope is |audio|. */
    constexpr double fs = 48000.0;
    const auto audio = real_tone(1000.0, fs, 480, 0.8);

    dsp::AmModulator dsb;
    dsb.configure(static_cast<float>(fs), 1.0f, dsp::AmModulator::Variant::DSB);

    std::vector<dsp::cfloat> out;
    dsb.process(audio.data(), audio.size(), out);

    double mean = 0.0;
    for (size_t i = 0; i < out.size(); i++) {
        CHECK_NEAR(out[i].real(), audio[i], 1e-6);
        CHECK_NEAR(out[i].imag(), 0.0, 1e-9);
        mean += out[i].real();
    }
    CHECK_NEAR(mean / static_cast<double>(out.size()), 0.0, 1e-6);
}

/* --- SSB modulator ---------------------------------------------------------- */

TEST(ssb_upper_puts_the_tone_above_the_carrier) {
    /* A USB modulator fed a tone must produce exp(+j w t): all the energy in
     * the positive-frequency bin, none in the mirror. */
    constexpr double fs = 48000.0;
    constexpr double tone_hz = 4000.0;

    dsp::SsbModulator ssb;
    ssb.configure(static_cast<float>(fs), dsp::SsbModulator::Sideband::Upper, 255);

    const auto audio = real_tone(tone_hz, fs, 8192, 1.0);
    std::vector<dsp::cfloat> out;
    ssb.process(audio.data(), audio.size(), out);

    const size_t start = 1200;   /* past the 127-sample group delay */
    const size_t n = 6000;       /* 500 whole cycles at 4 kHz */

    const double wanted = std::abs(bin(out, tone_hz, fs, start, n));
    const double image = std::abs(bin(out, -tone_hz, fs, start, n));

    CHECK_NEAR(wanted, 1.0, 0.02);
    /* Opposite-sideband suppression of a phasing exciter is set by the Hilbert
     * transformer's amplitude ripple; 40 dB is the classic usable minimum. */
    CHECK(20.0 * std::log10(wanted / std::max(image, 1e-12)) > 40.0);
}

TEST(ssb_lower_puts_the_tone_below_the_carrier) {
    constexpr double fs = 48000.0;
    constexpr double tone_hz = 4000.0;

    dsp::SsbModulator ssb;
    ssb.configure(static_cast<float>(fs), dsp::SsbModulator::Sideband::Lower, 255);

    const auto audio = real_tone(tone_hz, fs, 8192, 1.0);
    std::vector<dsp::cfloat> out;
    ssb.process(audio.data(), audio.size(), out);

    const size_t start = 1200;
    const size_t n = 6000;

    const double wanted = std::abs(bin(out, -tone_hz, fs, start, n));
    const double image = std::abs(bin(out, tone_hz, fs, start, n));

    CHECK_NEAR(wanted, 1.0, 0.02);
    CHECK(20.0 * std::log10(wanted / std::max(image, 1e-12)) > 40.0);
}

TEST(ssb_round_trips_through_ssb_demod) {
    /* Both stages delay by the Hilbert group delay and the demodulator sums two
     * coherent copies, so the recovered tone is the original at twice the
     * amplitude. */
    constexpr double fs = 48000.0;
    constexpr double tone_hz = 4000.0;
    constexpr double amplitude = 0.6;

    dsp::SsbModulator mod;
    mod.configure(static_cast<float>(fs), dsp::SsbModulator::Sideband::Upper, 255);

    dsp::SsbDemod demod;
    demod.configure(static_cast<float>(fs), dsp::SsbDemod::Sideband::Upper, 255);

    const auto audio = real_tone(tone_hz, fs, 8192, amplitude);
    std::vector<dsp::cfloat> modulated;
    mod.process(audio.data(), audio.size(), modulated);

    std::vector<float> recovered;
    demod.process(modulated.data(), modulated.size(), recovered);

    CHECK_EQ(recovered.size(), audio.size());
    CHECK_NEAR(tone_amplitude(recovered, tone_hz, fs, 1200, 6000), 2.0 * amplitude, 0.03);
}

TEST(ssb_demod_on_the_wrong_sideband_cancels) {
    /* The whole point of the phasing method: a USB signal fed to an LSB
     * demodulator cancels rather than coming out mirrored. */
    constexpr double fs = 48000.0;
    constexpr double tone_hz = 4000.0;

    dsp::SsbModulator mod;
    mod.configure(static_cast<float>(fs), dsp::SsbModulator::Sideband::Upper, 255);

    dsp::SsbDemod demod;
    demod.configure(static_cast<float>(fs), dsp::SsbDemod::Sideband::Lower, 255);

    const auto audio = real_tone(tone_hz, fs, 8192, 1.0);
    std::vector<dsp::cfloat> modulated;
    mod.process(audio.data(), audio.size(), modulated);

    std::vector<float> recovered;
    demod.process(modulated.data(), modulated.size(), recovered);

    CHECK(tone_amplitude(recovered, tone_hz, fs, 1200, 6000) < 0.02);
}

TEST(ssb_group_delay_is_half_the_hilbert_length) {
    dsp::SsbModulator ssb;
    ssb.configure(48000.0f, dsp::SsbModulator::Sideband::Upper, 63);
    CHECK_EQ(ssb.group_delay(), size_t{31});

    /* An even tap count is rounded up to keep the delay an integer. */
    ssb.configure(48000.0f, dsp::SsbModulator::Sideband::Upper, 64);
    CHECK_EQ(ssb.group_delay(), size_t{32});
}

/* --- OOK / ASK keyer -------------------------------------------------------- */

TEST(ook_keyer_holds_each_symbol_for_the_right_number_of_samples) {
    /* 1 kBd at 48 ksps is 48 samples a symbol; eight bits is 384 samples. */
    constexpr float fs = 48000.0f;
    const auto data = bits_from("10110010");

    dsp::OokKeyer ook;
    ook.configure(fs, 1000.0f);
    ook.set_data(data.data(), 8);

    CHECK_NEAR(ook.samples_per_symbol(), 48.0, 1e-12);
    CHECK_EQ(ook.total_samples(), size_t{384});

    std::vector<dsp::cfloat> out(1024);
    const size_t written = ook.process(out.data(), out.size());
    CHECK_EQ(written, size_t{384});
    CHECK(ook.done());

    const char* expected = "10110010";
    for (size_t bit = 0; bit < 8; bit++) {
        const float want = (expected[bit] == '1') ? 1.0f : 0.0f;
        for (size_t k = 0; k < 48; k++) {
            CHECK_NEAR(out[bit * 48 + k].real(), want, 1e-9);
            CHECK_NEAR(out[bit * 48 + k].imag(), 0.0, 1e-9);
        }
    }
}

TEST(ook_keyer_repeat_and_pause_lengths) {
    /* Three repeats of eight bits with four pause symbols between them is
     * 3*8 + 2*4 = 32 symbols. Upstream inserts the pause only between repeats,
     * never after the last one. */
    constexpr float fs = 48000.0f;
    const auto data = bits_from("11111111");

    dsp::OokKeyer ook;
    ook.configure(fs, 1000.0f);
    ook.set_data(data.data(), 8);
    ook.set_repeat(3, 4);

    CHECK_EQ(ook.total_samples(), size_t{32 * 48});

    std::vector<dsp::cfloat> out(4096);
    const size_t written = ook.process(out.data(), out.size());
    CHECK_EQ(written, size_t{32 * 48});
    CHECK_EQ(ook.repeats_sent(), uint32_t{3});

    /* Data symbols high, pause symbols low. */
    CHECK_NEAR(out[8 * 48 - 1].real(), 1.0, 1e-9);
    CHECK_NEAR(out[8 * 48].real(), 0.0, 1e-9);
    CHECK_NEAR(out[12 * 48 - 1].real(), 0.0, 1e-9);
    CHECK_NEAR(out[12 * 48].real(), 1.0, 1e-9);
}

TEST(ook_keyer_survives_being_read_in_small_pieces) {
    constexpr float fs = 48000.0f;
    const auto data = bits_from("10101010");

    dsp::OokKeyer ook;
    ook.configure(fs, 1000.0f);
    ook.set_data(data.data(), 8);

    std::vector<dsp::cfloat> all;
    std::vector<dsp::cfloat> chunk(7);
    for (;;) {
        const size_t got = ook.process(chunk.data(), chunk.size());
        if (got == 0) break;
        all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(got));
    }

    CHECK_EQ(all.size(), size_t{384});
    CHECK_NEAR(all[0].real(), 1.0, 1e-9);
    CHECK_NEAR(all[47].real(), 1.0, 1e-9);
    CHECK_NEAR(all[48].real(), 0.0, 1e-9);
}

TEST(ook_keyer_fractional_symbol_rate_does_not_drift) {
    /* 700 Bd at 48 ksps is 68.571... samples a symbol. Individual symbols round
     * to 68 or 69 samples but the total must stay locked to the ideal. */
    constexpr float fs = 48000.0f;
    constexpr size_t bits = 64;
    std::vector<uint8_t> data(8, 0xFF);

    dsp::OokKeyer ook;
    ook.configure(fs, 700.0f);
    ook.set_data(data.data(), bits);

    const double ideal = static_cast<double>(bits) * (48000.0 / 700.0);
    CHECK_EQ(ook.total_samples(), static_cast<size_t>(std::ceil(ideal)));

    std::vector<dsp::cfloat> out(8192);
    const size_t written = ook.process(out.data(), out.size());
    CHECK_EQ(written, ook.total_samples());
    /* Within one sample of the exact ideal length after 64 symbols. */
    CHECK(std::fabs(static_cast<double>(written) - ideal) < 1.0);
}

TEST(ask_levels_are_configurable) {
    constexpr float fs = 48000.0f;
    const auto data = bits_from("10");

    dsp::OokKeyer ask;
    ask.configure(fs, 1000.0f);
    ask.set_levels(0.25f, 0.75f);
    ask.set_data(data.data(), 2);

    std::vector<dsp::cfloat> out(96);
    CHECK_EQ(ask.process(out.data(), out.size()), size_t{96});
    CHECK_NEAR(out[0].real(), 0.75, 1e-6);
    CHECK_NEAR(out[48].real(), 0.25, 1e-6);
}

TEST(ook_keyer_with_no_data_produces_nothing) {
    dsp::OokKeyer ook;
    ook.configure(48000.0f, 1000.0f);
    CHECK(ook.done());

    std::vector<dsp::cfloat> out(64);
    CHECK_EQ(ook.process(out.data(), out.size()), size_t{0});

    ook.set_data(nullptr, 32);
    CHECK(ook.done());
    CHECK_EQ(ook.total_samples(), size_t{0});
}

/* --- FSK / GFSK keyer ------------------------------------------------------- */

TEST(fsk_keyer_places_the_two_tones_at_plus_and_minus_the_deviation) {
    /* proc_fsk sets shift_zero = -shift_one, so the mark and space tones sit
     * symmetrically about the carrier. */
    constexpr double fs = 48000.0;
    constexpr double deviation = 2400.0;

    const auto ones = bits_from("11111111");
    const auto zeros = bits_from("00000000");

    dsp::FskKeyer fsk;
    fsk.configure(static_cast<float>(fs), 1000.0f, static_cast<float>(deviation));

    fsk.set_data(ones.data(), 8);
    std::vector<dsp::cfloat> high(384);
    CHECK_EQ(fsk.process(high.data(), high.size()), size_t{384});
    for (size_t i = 1; i < high.size(); i++)
        CHECK_NEAR(inst_freq(high, i, fs), deviation, 1e-3);

    fsk.set_data(zeros.data(), 8);
    std::vector<dsp::cfloat> low(384);
    CHECK_EQ(fsk.process(low.data(), low.size()), size_t{384});
    for (size_t i = 1; i < low.size(); i++)
        CHECK_NEAR(inst_freq(low, i, fs), -deviation, 1e-3);
}

TEST(fsk_keyer_symbol_boundaries_follow_the_bit_pattern) {
    constexpr double fs = 48000.0;
    constexpr double deviation = 2400.0;
    const char* pattern = "10110010";
    const auto data = bits_from(pattern);

    dsp::FskKeyer fsk;
    fsk.configure(static_cast<float>(fs), 1000.0f, static_cast<float>(deviation));
    fsk.set_data(data.data(), 8);

    std::vector<dsp::cfloat> out(384);
    CHECK_EQ(fsk.process(out.data(), out.size()), size_t{384});

    for (size_t bit = 0; bit < 8; bit++) {
        const double want = (pattern[bit] == '1') ? deviation : -deviation;
        /* Sample in the middle of the symbol, away from the boundary. */
        CHECK_NEAR(inst_freq(out, bit * 48 + 24, fs), want, 1e-3);
    }
}

TEST(fsk_keyer_output_is_constant_envelope) {
    const auto data = bits_from("10110010");
    dsp::FskKeyer fsk;
    fsk.configure(48000.0f, 1000.0f, 2400.0f);
    fsk.set_data(data.data(), 8);

    std::vector<dsp::cfloat> out(384);
    fsk.process(out.data(), out.size());
    for (const auto& s : out) CHECK_NEAR(std::abs(s), 1.0, 1e-5);
}

TEST(gfsk_reaches_full_deviation_and_limits_the_frequency_slew) {
    /* The Gaussian pulse has unit sum, so a run of identical symbols still
     * reaches the full deviation — it just gets there gradually. */
    constexpr double fs = 48000.0;
    constexpr double deviation = 2400.0;
    /* Eight symbols of each, so a run is comfortably longer than the
     * four-symbol pulse and the shaped symbol has time to settle. */
    const char* pattern = "0000000011111111";
    const auto data = bits_from(pattern);

    dsp::FskKeyer hard;
    hard.configure(static_cast<float>(fs), 1000.0f, static_cast<float>(deviation));
    hard.set_data(data.data(), 16);
    std::vector<dsp::cfloat> hard_out(768);
    hard.process(hard_out.data(), hard_out.size());

    dsp::FskKeyer soft;
    soft.configure(static_cast<float>(fs), 1000.0f, static_cast<float>(deviation));
    soft.set_gaussian(0.5f, 4);
    soft.set_data(data.data(), 16);
    CHECK_EQ(soft.group_delay(), size_t{95});   /* (4 * 48 - 1) / 2 */

    std::vector<dsp::cfloat> soft_out(1024);
    const size_t got = soft.process(soft_out.data(), soft_out.size());
    /* 16 symbols plus the filter flush. */
    CHECK_EQ(got, size_t{16 * 48 + 95});
    soft_out.resize(got);

    /* The 192-tap pulse spans samples [n-191, n]. Symbols 8..15 are ones, i.e.
     * input samples 384..767, so any output from 575 on sees nothing but ones
     * and the unit-sum pulse must give exactly the full deviation. Likewise
     * output 300 sees only the leading zeros. */
    CHECK_NEAR(inst_freq(soft_out, 650, fs), deviation, 1e-3);
    CHECK_NEAR(inst_freq(soft_out, 300, fs), -deviation, 1e-3);

    double hard_step = 0.0;
    for (size_t i = 2; i < hard_out.size(); i++)
        hard_step = std::max(hard_step,
                             std::fabs(inst_freq(hard_out, i, fs) - inst_freq(hard_out, i - 1, fs)));

    double soft_step = 0.0;
    for (size_t i = 2; i < soft_out.size(); i++)
        soft_step = std::max(soft_step,
                             std::fabs(inst_freq(soft_out, i, fs) - inst_freq(soft_out, i - 1, fs)));

    /* Hard keying steps the full 2 * deviation in one sample. */
    CHECK_NEAR(hard_step, 2.0 * deviation, 1.0);
    CHECK(soft_step < hard_step / 10.0);
}

TEST(fsk_keyer_with_no_data_produces_nothing) {
    dsp::FskKeyer fsk;
    fsk.configure(48000.0f, 1000.0f, 2400.0f);
    CHECK(fsk.done());

    std::vector<dsp::cfloat> out(64);
    CHECK_EQ(fsk.process(out.data(), out.size()), size_t{0});
}

/* --- Bit order -------------------------------------------------------------- */

TEST(bits_are_read_most_significant_first) {
    /* Matches the firmware's `(data[n >> 3] << (n & 7)) & 0x80`. */
    const uint8_t data[2] = {0b10010110, 0b00000001};
    const bool expected[16] = {true, false, false, true, false, true, true, false,
                               false, false, false, false, false, false, false, true};
    for (size_t i = 0; i < 16; i++) CHECK_EQ(dsp::bit_at(data, i), expected[i]);

    CHECK_EQ(dsp::bit_at(nullptr, 0), false);
}

/* --- Tone generator --------------------------------------------------------- */

TEST(tone_gen_sine_has_the_right_frequency_and_amplitude) {
    constexpr double fs = 48000.0;
    dsp::ToneGen tone;
    tone.configure(1000.0f, static_cast<float>(fs), dsp::ToneGen::Shape::Sine, 0.8f);

    std::vector<float> out(4800);
    tone.process(out.data(), out.size());

    /* 48 samples a cycle, so index 12 is the peak. */
    CHECK_NEAR(out[0], 0.0, 1e-6);
    CHECK_NEAR(out[12], 0.8, 1e-6);
    CHECK_NEAR(out[36], -0.8, 1e-6);
    CHECK_NEAR(tone_amplitude(out, 1000.0, fs, 0, 4800), 0.8, 1e-4);
}

TEST(tone_gen_shapes_have_their_defining_values) {
    dsp::ToneGen tone;

    /* 48 samples a cycle. Sample 24 sits exactly on the half-turn where the
     * square flips, so it is not asserted: which side of the edge a float phase
     * accumulator lands on there is not a property of the waveform. */
    tone.configure(1000.0f, 48000.0f, dsp::ToneGen::Shape::Square, 1.0f);
    std::vector<float> square(48);
    tone.process(square.data(), square.size());
    CHECK_NEAR(square[0], 1.0, 1e-6);
    CHECK_NEAR(square[23], 1.0, 1e-6);
    CHECK_NEAR(square[25], -1.0, 1e-6);
    CHECK_NEAR(square[47], -1.0, 1e-6);

    tone.configure(1000.0f, 48000.0f, dsp::ToneGen::Shape::SawUp, 1.0f);
    std::vector<float> saw(48);
    tone.process(saw.data(), saw.size());
    CHECK_NEAR(saw[0], -1.0, 1e-6);
    CHECK_NEAR(saw[24], 0.0, 1e-6);

    tone.configure(1000.0f, 48000.0f, dsp::ToneGen::Shape::Triangle, 1.0f);
    std::vector<float> tri(48);
    tone.process(tri.data(), tri.size());
    CHECK_NEAR(tri[0], -1.0, 1e-6);
    CHECK_NEAR(tri[12], 0.0, 1e-6);
    CHECK_NEAR(tri[24], 1.0, 1e-6);
}

TEST(tone_gen_noise_stays_bounded_and_is_not_constant) {
    dsp::ToneGen tone;
    tone.configure(0.0f, 48000.0f, dsp::ToneGen::Shape::Noise, 1.0f);

    std::vector<float> out(4096);
    tone.process(out.data(), out.size());

    float min_v = 2.0f;
    float max_v = -2.0f;
    for (float v : out) {
        CHECK(v >= -1.0f && v <= 1.0f);
        min_v = std::min(min_v, v);
        max_v = std::max(max_v, v);
    }
    CHECK(max_v - min_v > 1.0f);
}

TEST(tone_gen_mix_follows_the_firmware_weighting) {
    /* out = in * (1 - weight) + tone * weight, and a zero frequency leaves the
     * block alone, as upstream's `if (!delta_) return sample_in;` does. */
    dsp::ToneGen tone;
    tone.configure(1000.0f, 48000.0f, dsp::ToneGen::Shape::Square, 1.0f);

    std::vector<float> block(24, 0.5f);
    tone.mix(block.data(), block.size(), 0.25f);
    for (float v : block) CHECK_NEAR(v, 0.5 * 0.75 + 1.0 * 0.25, 1e-6);

    dsp::ToneGen silent;
    silent.configure(0.0f, 48000.0f, dsp::ToneGen::Shape::Sine, 1.0f);
    std::vector<float> untouched(16, 0.3f);
    silent.mix(untouched.data(), untouched.size(), 0.5f);
    for (float v : untouched) CHECK_NEAR(v, 0.3f, 1e-9);
}

TEST(tone_gen_retune_keeps_phase_continuous) {
    dsp::ToneGen tone;
    tone.configure(1000.0f, 48000.0f, dsp::ToneGen::Shape::Sine, 1.0f);

    std::vector<float> out(24);
    tone.process(out.data(), 12);          /* stops at the peak */
    tone.set_frequency(2000.0f);
    tone.process(out.data() + 12, 12);

    /* The sample right after the retune must continue from sin(pi/2), not
     * restart at zero. */
    CHECK_NEAR(out[11], std::sin(kTwoPi * 11.0 / 48.0), 1e-6);
    CHECK_NEAR(out[12], 1.0, 1e-6);
    CHECK_NEAR(out[13], std::sin(kTwoPi * (12.0 / 48.0 + 2000.0 / 48000.0)), 1e-6);
}

/* --- CTCSS and the other tone sets ------------------------------------------ */

TEST(ctcss_table_matches_the_standard_frequencies) {
    /* The 50 EIA/TIA CTCSS tones, as application/tone_key.cpp lists them.
     * std::array knows its length at compile time, so assert it there. */
    static_assert(dsp::tones::ctcss.size() == 50,
                  "CTCSS table must hold the 50 standard tones");

    CHECK_NEAR(dsp::tones::ctcss[0].frequency_hz, 67.0, 1e-4);
    CHECK_STR_EQ(dsp::tones::ctcss[0].name, "1 XZ");
    CHECK_NEAR(dsp::tones::ctcss[1].frequency_hz, 69.3, 1e-4);
    CHECK_NEAR(dsp::tones::ctcss[8].frequency_hz, 88.5, 1e-4);
    CHECK_NEAR(dsp::tones::ctcss[12].frequency_hz, 100.0, 1e-4);
    CHECK_STR_EQ(dsp::tones::ctcss[12].name, "12 1Z");
    CHECK_NEAR(dsp::tones::ctcss[19].frequency_hz, 127.3, 1e-4);
    CHECK_NEAR(dsp::tones::ctcss[21].frequency_hz, 136.5, 1e-4);
    CHECK_NEAR(dsp::tones::ctcss[25].frequency_hz, 156.7, 1e-4);
    CHECK_NEAR(dsp::tones::ctcss[26].frequency_hz, 159.8, 1e-4);
    CHECK_STR_EQ(dsp::tones::ctcss[26].name, "40 --");
    CHECK_NEAR(dsp::tones::ctcss[40].frequency_hz, 203.5, 1e-4);
    CHECK_STR_EQ(dsp::tones::ctcss[40].name, "32 M1");
    CHECK_NEAR(dsp::tones::ctcss[48].frequency_hz, 250.3, 1e-4);
    CHECK_NEAR(dsp::tones::ctcss[49].frequency_hz, 254.1, 1e-4);
    CHECK_STR_EQ(dsp::tones::ctcss[49].name, "50 0Z");
}

TEST(ctcss_table_is_sorted_and_has_no_duplicates) {
    for (size_t i = 1; i < dsp::tones::ctcss.size(); i++)
        CHECK(dsp::tones::ctcss[i].frequency_hz > dsp::tones::ctcss[i - 1].frequency_hz);
}

TEST(ctcss_lookup_finds_the_nearest_tone_within_tolerance) {
    /* Upstream's tolerance is 4 Hz (TONE_FREQ_TOLERANCE_CENTIHZ). */
    CHECK_EQ(dsp::tones::ctcss_index(100.0f), 12);
    CHECK_EQ(dsp::tones::ctcss_index(100.4f), 12);
    CHECK_EQ(dsp::tones::ctcss_index(103.0f), 13);   /* nearer 103.5 than 100.0 */
    CHECK_EQ(dsp::tones::ctcss_index(67.0f), 0);
    CHECK_EQ(dsp::tones::ctcss_index(400.0f), -1);   /* nothing within 4 Hz */
    CHECK_EQ(dsp::tones::ctcss_index(0.0f), -1);
    CHECK_EQ(dsp::tones::ctcss_index(103.0f, 0.1f), -1);  /* tighter tolerance */
}

TEST(selective_calling_tone_sets_match_upstream) {
    /* common/tonesets.hpp. */
    CHECK_NEAR(dsp::tones::ccir[0], 1981.0, 1e-3);
    CHECK_NEAR(dsp::tones::ccir[10], 2400.0, 1e-3);
    CHECK_NEAR(dsp::tones::ccir[11], 930.0, 1e-3);
    CHECK_NEAR(dsp::tones::ccir[15], 1055.0, 1e-3);

    CHECK_NEAR(dsp::tones::eia[0], 600.0, 1e-3);
    CHECK_NEAR(dsp::tones::eia[14], 459.0, 1e-3);

    CHECK_NEAR(dsp::tones::zvei[0], 2400.0, 1e-3);
    CHECK_NEAR(dsp::tones::zvei[10], 2800.0, 1e-3);
    CHECK_NEAR(dsp::tones::zvei[15], 680.0, 1e-3);

    CHECK_NEAR(dsp::tones::roger_beep[0], 1475.0, 1e-3);
    CHECK_NEAR(dsp::tones::roger_beep[5], 740.0, 1e-3);
}

TEST(dtmf_grid_matches_upstream) {
    /* Order is "0123456789ABCD#*", each {column, row}. */
    CHECK_NEAR(dsp::tones::dtmf[0][0], 1336.0, 1e-3);  /* '0' */
    CHECK_NEAR(dsp::tones::dtmf[0][1], 941.0, 1e-3);
    CHECK_NEAR(dsp::tones::dtmf[1][0], 1209.0, 1e-3);  /* '1' */
    CHECK_NEAR(dsp::tones::dtmf[1][1], 697.0, 1e-3);
    CHECK_NEAR(dsp::tones::dtmf[5][0], 1336.0, 1e-3);  /* '5' */
    CHECK_NEAR(dsp::tones::dtmf[5][1], 770.0, 1e-3);
    CHECK_NEAR(dsp::tones::dtmf[10][0], 1633.0, 1e-3); /* 'A' */
    CHECK_NEAR(dsp::tones::dtmf[10][1], 697.0, 1e-3);
    CHECK_NEAR(dsp::tones::dtmf[14][0], 1477.0, 1e-3); /* '#' */
    CHECK_NEAR(dsp::tones::dtmf[14][1], 941.0, 1e-3);
    CHECK_NEAR(dsp::tones::dtmf[15][0], 1209.0, 1e-3); /* '*' */
    CHECK_NEAR(dsp::tones::dtmf[15][1], 941.0, 1e-3);
}

/* --- DCS -------------------------------------------------------------------- */

TEST(dcs_parity_matches_the_firmware_table) {
    /* Entries copied verbatim out of application/protocols/dcs.cpp, spread
     * across the table so a wrong generator polynomial cannot pass. */
    struct Entry {
        uint16_t code;
        uint16_t parity;
    };
    const Entry entries[] = {
        {0, 0b11000111010},   {1, 0b01001001111},   {2, 0b01010100101},
        {7, 0b01110011011},   {23, 0b01000101000},  {31, 0b10011001011},
        {100, 0b11011011011}, {127, 0b00101100001}, {200, 0b11111111000},
        {255, 0b00010001100}, {256, 0b10111100000}, {300, 0b10100101110},
        {383, 0b01010111011}, {447, 0b10110011010}, {500, 0b10101011111},
        {511, 0b01101010110},
    };

    for (const auto& e : entries) {
        const uint32_t word = dsp::tones::dcs_word(e.code);
        CHECK_EQ(word >> 12, static_cast<uint32_t>(e.parity));
    }
}

TEST(dcs_word_layout_matches_the_firmware) {
    /* (parity << 12) | (0b100 << 9) | code. */
    for (uint16_t code : {uint16_t{0}, uint16_t{23}, uint16_t{114}, uint16_t{511}}) {
        const uint32_t word = dsp::tones::dcs_word(code);
        CHECK_EQ(word & 0x1FFu, static_cast<uint32_t>(code));
        CHECK_EQ((word >> 9) & 0x7u, uint32_t{4});
        CHECK(word < (1u << 23));
    }

    /* Codes are masked to nine bits, as upstream's `code &= 511` does. */
    CHECK_EQ(dsp::tones::dcs_word(512), dsp::tones::dcs_word(0));
}

TEST(dcs_encoder_emits_the_word_lsb_first_at_the_configured_baud) {
    /* 48 Bd at 4800 Hz is exactly 100 samples a bit, and both numbers are
     * exactly representable, so the bit boundaries land on whole samples. The
     * real rate is 134.4 Bd; nothing about the encoder depends on which. */
    constexpr float fs = 4800.0f;
    dsp::DcsEncoder dcs;
    dcs.configure(23, fs, 48.0f, 1.0f);

    const uint32_t word = dsp::tones::dcs_word(23);
    CHECK_EQ(dcs.word(), word);

    std::vector<float> out(23 * 100);
    dcs.process(out.data(), out.size());

    for (size_t bit = 0; bit < 23; bit++) {
        const float want = ((word >> bit) & 1u) ? 1.0f : -1.0f;
        CHECK_NEAR(out[bit * 100], want, 1e-9);
        CHECK_NEAR(out[bit * 100 + 99], want, 1e-9);
    }

    /* The word repeats continuously — that is what makes DCS a *continuous*
     * squelch system. */
    std::vector<float> again(100);
    dcs.process(again.data(), again.size());
    CHECK_NEAR(again[0], ((word >> 0) & 1u) ? 1.0 : -1.0, 1e-9);
}

/* --- Interpolator ----------------------------------------------------------- */

TEST(interpolator_multiplies_the_sample_count_and_preserves_level) {
    /* Zero-stuffing drops the level by the interpolation factor; the taps are
     * scaled back up so a DC input comes out at the same DC. */
    const auto taps = dsp::design_lowpass(9000.0, 6000.0, 192000.0, 60.0, 255);

    dsp::FirInterpolateC interp;
    interp.configure(taps, 4);

    const std::vector<dsp::cfloat> in(512, dsp::cfloat{1.0f, -0.5f});
    std::vector<dsp::cfloat> out;
    CHECK_EQ(interp.process(in.data(), in.size(), out), size_t{2048});
    CHECK_EQ(out.size(), size_t{2048});

    /* The polyphase branches only carry identical DC gain to the extent that
     * the filter is zero at multiples of the input rate, so the residual is
     * bounded by (interpolation - 1) times the stopband amplitude: three times
     * 10^(-60/20) here, i.e. 3e-3. Skip the filter's start-up ramp. */
    for (size_t i = 1024; i < out.size(); i++) {
        CHECK_NEAR(out[i].real(), 1.0, 3e-3);
        CHECK_NEAR(out[i].imag(), -0.5, 3e-3);
    }
}

TEST(interpolator_of_one_is_a_pass_through) {
    dsp::FirInterpolateC interp;
    interp.configure({1.0f}, 1);

    const std::vector<dsp::cfloat> in = {{1.0f, 2.0f}, {3.0f, 4.0f}, {-1.0f, 0.5f}};
    std::vector<dsp::cfloat> out;
    CHECK_EQ(interp.process(in.data(), in.size(), out), size_t{3});
    for (size_t i = 0; i < in.size(); i++) {
        CHECK_NEAR(out[i].real(), in[i].real(), 1e-9);
        CHECK_NEAR(out[i].imag(), in[i].imag(), 1e-9);
    }
}

TEST(interpolator_suppresses_the_zero_stuffing_images) {
    /* A 4 kHz tone at 48 ksps interpolated by 4 must appear at 4 kHz in the
     * 192 ksps output and nowhere near 48 +/- 4 kHz. */
    constexpr double in_fs = 48000.0;
    constexpr double out_fs = 192000.0;
    constexpr double tone_hz = 4000.0;

    std::vector<dsp::cfloat> in(4096);
    for (size_t i = 0; i < in.size(); i++) {
        const double p = kTwoPi * tone_hz * static_cast<double>(i) / in_fs;
        in[i] = dsp::cfloat{static_cast<float>(std::cos(p)), static_cast<float>(std::sin(p))};
    }

    dsp::FirInterpolateC interp;
    interp.configure(dsp::design_lowpass(20000.0, 6000.0, out_fs, 60.0, 255), 4);

    std::vector<dsp::cfloat> out;
    interp.process(in.data(), in.size(), out);

    const size_t start = 2048;
    const size_t n = 12288;   /* whole cycles of 4 kHz at 192 ksps */

    const double wanted = std::abs(bin(out, tone_hz, out_fs, start, n));
    const double image = std::abs(bin(out, in_fs + tone_hz, out_fs, start, n));

    CHECK_NEAR(wanted, 1.0, 0.02);
    CHECK(20.0 * std::log10(wanted / std::max(image, 1e-12)) > 50.0);
}
