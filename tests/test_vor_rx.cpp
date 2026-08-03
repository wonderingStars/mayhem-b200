/*
 * mayhem-b200 — tests for the VOR receiver.
 *
 * The reference signal every decode test is measured against is built straight
 * from the definition of a VOR composite, not from anything the decoder does:
 *
 *   e(t) = 1 + m*cos(2*pi*30*t - theta)                       <- VARIABLE (AM)
 *            + m*cos(2*pi*9960*t + (480/30)*sin(2*pi*30*t))   <- REFERENCE
 *                                                                (FM subcarrier)
 *
 * The subcarrier's instantaneous deviation is 480*cos(2*pi*30*t), so the
 * FM-demodulated reference tone is a cosine of phase 0 and the variable tone
 * lags it by theta. The radial the decoder must report is therefore theta.
 * (This matches upstream's transmitter, baseband/proc_vor_tx.cpp, which forms
 * the variable tone as sine_table[phase_30 - radial_offset].)
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "../src/apps/ui_vor_rx.hpp"
#include "../src/dsp/modulate.hpp"

#include <cmath>
#include <complex>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

double wrap_error(double got, double want) {
    double e = got - want;
    while (e > 180.0) e -= 360.0;
    while (e <= -180.0) e += 360.0;
    return e;
}

/* Textbook VOR composite envelope, per the header comment above. */
std::vector<float> make_vor_envelope(double fs, double seconds, double radial_deg,
                                     double var_depth = 0.30,
                                     double sub_depth = 0.30,
                                     double deviation_hz = 480.0,
                                     double carrier = 1.0) {
    const auto n = static_cast<size_t>(fs * seconds);
    std::vector<float> out(n);
    const double w30 = 2.0 * kPi * 30.0;
    const double wsc = 2.0 * kPi * 9960.0;
    const double beta = deviation_hz / 30.0;
    const double theta = radial_deg * kPi / 180.0;
    for (size_t i = 0; i < n; i++) {
        const double t = static_cast<double>(i) / fs;
        const double variable = std::cos(w30 * t - theta);
        const double subcarrier = std::cos(wsc * t + beta * std::sin(w30 * t));
        out[i] = static_cast<float>(carrier * (1.0 + var_depth * variable +
                                               sub_depth * subcarrier));
    }
    return out;
}

/* Decode one window of a synthetic composite and return the reported status. */
app::VorStatus decode_composite(double fs, double radial_deg,
                                double var_depth = 0.30, double sub_depth = 0.30) {
    app::VorDecoder decoder;
    decoder.configure(static_cast<float>(fs));
    const auto signal = make_vor_envelope(fs, 0.35, radial_deg, var_depth, sub_depth);
    decoder.process(signal.data(), signal.size());
    app::VorStatus status{};
    decoder.take_status(status);
    return status;
}

}  // namespace

/* --- Pure helpers ----------------------------------------------------------- */

TEST(vor_normalize_degrees_wraps_and_rounds) {
    CHECK_EQ(app::vor_normalize_degrees(0.0f), 0);
    CHECK_EQ(app::vor_normalize_degrees(123.4f), 123);
    CHECK_EQ(app::vor_normalize_degrees(123.6f), 124);
    CHECK_EQ(app::vor_normalize_degrees(-1.0f), 359);
    CHECK_EQ(app::vor_normalize_degrees(-90.0f), 270);
    CHECK_EQ(app::vor_normalize_degrees(360.0f), 0);
    CHECK_EQ(app::vor_normalize_degrees(719.0f), 359);
    /* Must not report 360: it rounds up out of the circle. */
    CHECK_EQ(app::vor_normalize_degrees(359.8f), 0);
    CHECK_EQ(app::vor_normalize_degrees(-720.4f), 0);
}

TEST(vor_wrap_signed_degrees_matches_upstream_range) {
    CHECK_EQ(app::vor_wrap_signed_degrees(0), 0);
    CHECK_EQ(app::vor_wrap_signed_degrees(180), 180);
    CHECK_EQ(app::vor_wrap_signed_degrees(181), -179);
    CHECK_EQ(app::vor_wrap_signed_degrees(-180), 180);
    CHECK_EQ(app::vor_wrap_signed_degrees(-181), 179);
    CHECK_EQ(app::vor_wrap_signed_degrees(359), -1);
    CHECK_EQ(app::vor_wrap_signed_degrees(-359), 1);
}

TEST(vor_course_deviation_is_signed_and_wrapped) {
    CHECK_EQ(app::vor_course_deviation(90, 90), 0);
    CHECK_EQ(app::vor_course_deviation(95, 90), 5);
    CHECK_EQ(app::vor_course_deviation(85, 90), -5);
    /* Across the 0/360 seam. */
    CHECK_EQ(app::vor_course_deviation(2, 358), 4);
    CHECK_EQ(app::vor_course_deviation(358, 2), -4);
    /* Diametrically opposite lands on the -180 end of the range. */
    CHECK_EQ(app::vor_course_deviation(180, 0), -180);
}

TEST(vor_calibrated_radial_wraps_both_ways) {
    CHECK_EQ(app::vor_calibrated_radial(10, 0), 10);
    CHECK_EQ(app::vor_calibrated_radial(350, 20), 10);
    CHECK_EQ(app::vor_calibrated_radial(10, -20), 350);
    CHECK_EQ(app::vor_calibrated_radial(0, -359), 1);
    CHECK_EQ(app::vor_calibrated_radial(359, 359), 358);
}

TEST(vor_cdi_needle_offset_matches_upstream_scale) {
    /* Upstream: +/-10 degrees full scale mapped onto +/-2 ticks of 40 px. */
    CHECK_EQ(app::vor_cdi_needle_offset(90, 90, 40), 0);
    CHECK_EQ(app::vor_cdi_needle_offset(95, 90, 40), 40);
    CHECK_EQ(app::vor_cdi_needle_offset(100, 90, 40), 80);
    CHECK_EQ(app::vor_cdi_needle_offset(85, 90, 40), -40);
    CHECK_EQ(app::vor_cdi_needle_offset(80, 90, 40), -80);
    /* Beyond full scale the needle pins rather than running off the widget. */
    CHECK_EQ(app::vor_cdi_needle_offset(140, 90, 40), 80);
    CHECK_EQ(app::vor_cdi_needle_offset(300, 90, 40), -80);
    /* And it takes the short way round the seam. */
    CHECK_EQ(app::vor_cdi_needle_offset(355, 5, 40), -80);
    CHECK_EQ(app::vor_cdi_needle_offset(2, 358, 40), 32);
}

TEST(vor_flag_state_switches_at_abeam_with_hysteresis) {
    app::VorFlagState flag;

    /* Flying the selected course leads away from the station. */
    CHECK_EQ(static_cast<int>(flag.update(90, 90, true)), static_cast<int>(app::VorFlag::From));
    /* ... and toward it on the reciprocal. */
    CHECK_EQ(static_cast<int>(flag.update(270, 90, true)), static_cast<int>(app::VorFlag::To));

    /* Inside the +/-5 degree dead zone around abeam the previous answer holds. */
    CHECK_EQ(static_cast<int>(flag.update(180, 90, true)), static_cast<int>(app::VorFlag::To));
    flag.reset();
    CHECK_EQ(static_cast<int>(flag.update(90, 90, true)), static_cast<int>(app::VorFlag::From));
    CHECK_EQ(static_cast<int>(flag.update(180, 90, true)), static_cast<int>(app::VorFlag::From));

    /* Past the dead zone it does switch. Note 4 degrees against a 90 degree
     * course is 86 degrees apart, which is still inside the dead zone; 10 is
     * 80 apart and clears it. */
    CHECK_EQ(static_cast<int>(flag.update(186, 90, true)), static_cast<int>(app::VorFlag::To));
    CHECK_EQ(static_cast<int>(flag.update(4, 90, true)), static_cast<int>(app::VorFlag::To));
    CHECK_EQ(static_cast<int>(flag.update(10, 90, true)), static_cast<int>(app::VorFlag::From));

    /* Losing lock clears it. */
    CHECK_EQ(static_cast<int>(flag.update(90, 90, false)), static_cast<int>(app::VorFlag::Unknown));
    CHECK_STR_EQ(app::vor_flag_label(app::VorFlag::Unknown), "--");
    CHECK_STR_EQ(app::vor_flag_label(app::VorFlag::From), "FROM");
    CHECK_STR_EQ(app::vor_flag_label(app::VorFlag::To), "TO");
}

TEST(vor_radial_smoother_tracks_and_wraps) {
    app::VorRadialSmoother s;
    CHECK(!s.primed());
    /* The first reading is taken as-is. */
    CHECK_EQ(s.update(100), 100);
    CHECK(s.primed());

    /* 20% of the way toward each new reading. */
    CHECK_EQ(s.update(200), 120);

    /* A constant input converges to it. */
    uint16_t v = 0;
    for (int i = 0; i < 200; i++) v = s.update(200);
    CHECK_EQ(v, 200);

    /* Crossing the seam takes the short arc: from 355 toward 5 must go up
     * through 0, not all the way back down through 180. */
    app::VorRadialSmoother w;
    w.update(355);
    const uint16_t stepped = w.update(5);
    CHECK(stepped == 357 || stepped == 358);
    for (int i = 0; i < 200; i++) v = w.update(5);
    CHECK_EQ(v, 5);

    w.reset();
    CHECK(!w.primed());
    CHECK_EQ(w.update(42), 42);
}

/* --- Decoder configuration -------------------------------------------------- */

TEST(vor_decoder_window_matches_upstream_at_48k) {
    app::VorDecoder d;
    d.configure(48000.0f);
    /* Upstream: vor_window_samples = 4800, 100 ms, exactly three 30 Hz cycles. */
    CHECK_EQ(d.window_samples(), 4800u);
    CHECK_NEAR(d.window_seconds(), 0.1, 1e-6);
    /* Upstream's hard-coded one-pole coefficient, restated as a 600 Hz corner. */
    CHECK_NEAR(d.subcarrier_lp_alpha(), 0.9245, 0.0005);

    /* And the window still spans whole 30 Hz cycles at another rate. */
    app::VorDecoder e;
    e.configure(41666.667f);
    CHECK_EQ(e.window_samples(), 4167u);
}

TEST(vor_reference_phase_lag_is_the_filter_plus_the_measured_bias) {
    const float alpha = std::exp(-2.0f * static_cast<float>(kPi) * 600.0f / 48000.0f);
    const float lag = app::VorDecoder::compute_reference_phase_lag_deg(48000.0f, alpha);

    /* One-pole phase at 30 Hz: atan2(a sin w, 1 - a cos w) = 2.751 deg.
     * Discriminator half-sample delay at 30 Hz: 0.1125 deg.
     * Measured subcarrier-demod bias: -0.34 deg. */
    CHECK_NEAR(lag, 2.751 + 0.1125 - 0.34, 0.01);

    /* Doubling the rate halves the discriminator term and moves the one-pole a
     * little closer to its continuous-time value of atan(30/600) = 2.862 deg;
     * the two changes very nearly cancel, so the total is rate-independent —
     * which is what makes one bias constant valid at every rate. */
    const float alpha96 = std::exp(-2.0f * static_cast<float>(kPi) * 600.0f / 96000.0f);
    const float lag96 = app::VorDecoder::compute_reference_phase_lag_deg(96000.0f, alpha96);
    CHECK_NEAR(lag96, 2.8065 + 0.05625 - 0.34, 0.01);
    CHECK_NEAR(lag96, lag, 0.02);

    CHECK_NEAR(app::VorDecoder::compute_reference_phase_lag_deg(0.0f, alpha), 0.0, 1e-9);
}

/* --- The bearing itself ------------------------------------------------------
 *
 * The headline requirement: two 30 Hz tones with a known phase offset must
 * produce the matching bearing. */

TEST(vor_bearing_from_two_30hz_tones_at_the_quadrants) {
    struct Case { double radial; uint16_t expect; };
    const Case cases[] = {{0.0, 0}, {90.0, 90}, {180.0, 180}, {270.0, 270}};

    for (const auto& c : cases) {
        const auto s = decode_composite(48000.0, c.radial);
        CHECK(s.valid);
        CHECK_EQ(s.radial_deg, c.expect);
        /* phase_deg and radial_deg are the same quantity, as upstream reports. */
        CHECK_EQ(s.phase_deg, s.radial_deg);
    }
}

TEST(vor_bearing_across_the_whole_circle) {
    for (int deg = 0; deg < 360; deg += 15) {
        const auto s = decode_composite(48000.0, deg);
        CHECK(s.valid);
        const double err = wrap_error(s.radial_deg, deg);
        if (std::fabs(err) > 1.0)
            CHECK_NEAR(static_cast<double>(s.radial_deg), static_cast<double>(deg), 1.0);
    }
}

TEST(vor_bearing_is_independent_of_sample_rate) {
    for (double fs : {41666.667, 44100.0, 48000.0, 50000.0, 62500.0}) {
        const auto s = decode_composite(fs, 217.0);
        CHECK(s.valid);
        CHECK_NEAR(wrap_error(s.radial_deg, 217.0), 0.0, 1.5);
    }
}

TEST(vor_bearing_is_independent_of_signal_level_and_depth) {
    /* Halving the depths and quartering the carrier must not move the bearing:
     * the reference is phase-only and the variable is normalised by the
     * carrier. */
    app::VorDecoder d;
    d.configure(48000.0f);
    const auto quiet = make_vor_envelope(48000.0, 0.35, 137.0, 0.20, 0.20, 480.0, 0.25);
    d.process(quiet.data(), quiet.size());
    app::VorStatus s{};
    CHECK(d.take_status(s));
    CHECK(s.valid);
    CHECK_NEAR(wrap_error(s.radial_deg, 137.0), 0.0, 1.5);
    /* Depth is a ratio to the carrier, so it still reads ~200/1000. */
    CHECK_NEAR(s.var_level, 200, 12);
}

TEST(vor_reported_metrics_match_the_transmitted_signal) {
    const auto s = decode_composite(48000.0, 45.0);
    CHECK(s.valid);
    /* Recovered subcarrier deviation, in Hz. Transmitted 480. */
    CHECK_NEAR(s.ref_level, 480, 8);
    /* 30 Hz AM depth x1000. Transmitted 0.30. */
    CHECK_NEAR(s.var_level, 300, 8);
    /* Quality is deviation over upstream's 496.6 Hz reference, as a percent. */
    CHECK_NEAR(s.quality, 96, 3);
}

/* --- Subcarrier FM demodulation, closed against Phase A's modulator ---------- */

TEST(vor_subcarrier_fm_demod_recovers_the_30hz_reference) {
    /* Build the 9960 Hz subcarrier with dsp::FmModulator instead of a closed
     * form: modulate a 30 Hz tone at 480 Hz deviation, then heterodyne the
     * resulting complex baseband up to 9960 Hz and take the real part. What
     * comes back out of VorDecoder's FM path must be that same 30 Hz tone. */
    constexpr double fs = 48000.0;
    constexpr double radial = 123.0;
    const auto n = static_cast<size_t>(fs * 0.35);

    std::vector<float> tone(n);
    for (size_t i = 0; i < n; i++)
        tone[i] = static_cast<float>(std::cos(2.0 * kPi * 30.0 * static_cast<double>(i) / fs));

    dsp::FmModulator fm;
    fm.configure(static_cast<float>(fs), 480.0f);
    std::vector<dsp::cfloat> iq;
    fm.process(tone.data(), n, iq);

    std::vector<float> envelope(n);
    const double theta = radial * kPi / 180.0;
    for (size_t i = 0; i < n; i++) {
        const double t = static_cast<double>(i) / fs;
        const auto rot = std::polar(1.0, 2.0 * kPi * 9960.0 * t);
        const double subcarrier = (std::complex<double>{iq[i].real(), iq[i].imag()} * rot).real();
        const double variable = std::cos(2.0 * kPi * 30.0 * t - theta);
        envelope[i] = static_cast<float>(1.0 + 0.30 * variable + 0.30 * subcarrier);
    }

    app::VorDecoder d;
    d.configure(static_cast<float>(fs));
    d.process(envelope.data(), envelope.size());
    app::VorStatus s{};
    CHECK(d.take_status(s));

    /* The FM path recovered a 30 Hz tone of the transmitted deviation... */
    CHECK(s.valid);
    CHECK_NEAR(s.ref_level, 480, 10);
    /* ... at the phase the modulator put it at, so the bearing comes out. */
    CHECK_NEAR(wrap_error(s.radial_deg, radial), 0.0, 1.5);
}

TEST(vor_reference_deviation_tracks_the_modulator_deviation) {
    /* Two different transmitted deviations must be reported as such — that is
     * what makes ref_level a lock metric rather than a magic number. */
    app::VorDecoder d240;
    d240.configure(48000.0f);
    const auto sig240 = make_vor_envelope(48000.0, 0.35, 0.0, 0.30, 0.30, 240.0);
    d240.process(sig240.data(), sig240.size());
    app::VorStatus s240{};
    CHECK(d240.take_status(s240));
    CHECK_NEAR(s240.ref_level, 240, 10);
    /* 240 Hz is only just above the 229.2 Hz lock threshold. */
    CHECK(s240.valid);

    app::VorDecoder d100;
    d100.configure(48000.0f);
    const auto sig100 = make_vor_envelope(48000.0, 0.35, 0.0, 0.30, 0.30, 100.0);
    d100.process(sig100.data(), sig100.size());
    app::VorStatus s100{};
    CHECK(d100.take_status(s100));
    CHECK_NEAR(s100.ref_level, 100, 10);
    /* Below threshold: no reference, so no usable bearing. */
    CHECK(!s100.valid);
    CHECK_EQ(s100.quality, 0);
}

/* --- Malformed and degenerate input ----------------------------------------- */

TEST(vor_decoder_rejects_signals_that_are_not_vor) {
    app::VorDecoder d;
    d.configure(48000.0f);
    app::VorStatus s{};

    /* Silence. */
    std::vector<float> zeros(d.window_samples(), 0.0f);
    d.process(zeros.data(), zeros.size());
    CHECK(d.take_status(s));
    CHECK(!s.valid);
    CHECK_EQ(s.quality, 0);
    CHECK_EQ(s.var_level, 0);

    /* An unmodulated carrier: no tones at all. */
    app::VorDecoder carrier;
    carrier.configure(48000.0f);
    std::vector<float> ones(carrier.window_samples(), 1.0f);
    carrier.process(ones.data(), ones.size());
    CHECK(carrier.take_status(s));
    CHECK(!s.valid);
    CHECK_EQ(s.var_level, 0);
    CHECK_EQ(s.quality, 0);

    /* Variable tone but no 9960 Hz subcarrier — an AM station, not a VOR. */
    const auto no_reference = decode_composite(48000.0, 90.0, 0.30, 0.0);
    CHECK(!no_reference.valid);
    CHECK(no_reference.ref_level < 20);
    CHECK_NEAR(no_reference.var_level, 300, 8);

    /* Subcarrier but no 30 Hz AM — the beacon is there, the bearing is not. */
    const auto no_variable = decode_composite(48000.0, 90.0, 0.0, 0.30);
    CHECK(!no_variable.valid);
    CHECK(no_variable.var_level < 150);
    CHECK_NEAR(no_variable.ref_level, 480, 10);
}

TEST(vor_decoder_handles_empty_and_short_input) {
    app::VorDecoder d;
    d.configure(48000.0f);
    app::VorStatus s{};

    d.process(nullptr, 0);
    CHECK(!d.take_status(s));

    /* One sample short of a window: still nothing. */
    std::vector<float> partial(d.window_samples() - 1, 1.0f);
    d.process(partial.data(), partial.size());
    CHECK(!d.take_status(s));

    /* The sample that completes it produces exactly one status. */
    const float last = 1.0f;
    d.process(&last, 1);
    CHECK(d.take_status(s));
    CHECK(!d.take_status(s));
}

TEST(vor_decoder_zero_sample_rate_falls_back) {
    app::VorDecoder d;
    d.configure(0.0f);
    CHECK_NEAR(d.sample_rate(), 48000.0, 1e-6);
    CHECK_EQ(d.window_samples(), 4800u);
}

/* --- Burst feeding, which is what the host tap actually delivers ------------- */

TEST(vor_decoder_is_unchanged_by_chunking_a_contiguous_stream) {
    /* Splitting the same samples into pieces, with a discontinuity marked
     * between them even though nothing was lost, must not move the bearing.
     * That is the property the host front end relies on. */
    constexpr double fs = 48000.0;
    const auto signal = make_vor_envelope(fs, 0.35, 77.0);

    app::VorDecoder whole;
    whole.configure(static_cast<float>(fs));
    whole.process(signal.data(), signal.size());
    app::VorStatus a{};
    CHECK(whole.take_status(a));

    app::VorDecoder chunked;
    chunked.configure(static_cast<float>(fs));
    size_t pos = 0;
    app::VorStatus b{};
    bool got = false;
    while (pos < signal.size() && !got) {
        const size_t n = std::min<size_t>(1000, signal.size() - pos);
        chunked.mark_discontinuity();
        chunked.process(signal.data() + pos, n);
        pos += n;
        if (chunked.take_status(b)) got = true;
    }
    CHECK(got);
    CHECK(a.valid);
    CHECK(b.valid);
    CHECK_NEAR(wrap_error(b.radial_deg, 77.0), 0.0, 2.0);
}

TEST(vor_decoder_survives_unknown_gaps_between_bursts) {
    /* Bursts separated by an unknown amount of discarded real time. The gap
     * rotates both 30 Hz tones equally, so combining bursts as
     * REFERENCE * conj(VARIABLE) leaves the bearing intact. Bursts here are one
     * whole 30 Hz cycle long. */
    constexpr double fs = 48000.0;
    constexpr double radial = 213.0;
    constexpr size_t burst = 1600;  /* exactly one cycle at 48 kHz */
    constexpr size_t gap = 977;     /* deliberately not a whole cycle */

    app::VorDecoder d;
    d.configure(static_cast<float>(fs));

    const double w30 = 2.0 * kPi * 30.0;
    const double wsc = 2.0 * kPi * 9960.0;
    const double beta = 480.0 / 30.0;
    const double theta = radial * kPi / 180.0;

    std::vector<float> chunk(burst);
    size_t origin = 0;
    int windows = 0;
    double last_error = 999.0;
    while (windows < 12) {
        for (size_t i = 0; i < burst; i++) {
            const double t = static_cast<double>(origin + i) / fs;
            chunk[i] = static_cast<float>(1.0 + 0.30 * std::cos(w30 * t - theta) +
                                          0.30 * std::cos(wsc * t + beta * std::sin(w30 * t)));
        }
        d.mark_discontinuity();
        d.process(chunk.data(), chunk.size());
        origin += burst + gap;

        app::VorStatus s{};
        if (d.take_status(s)) {
            windows++;
            /* Skip the first few while the carrier estimate converges. */
            if (windows >= 6) {
                CHECK(s.valid);
                last_error = wrap_error(s.radial_deg, radial);
                CHECK_NEAR(last_error, 0.0, 2.0);
            }
        }
    }
    CHECK(last_error < 900.0);
}

/* --- The host front end: raw IQ in, bearing out ----------------------------- */

TEST(vor_channelizer_picks_a_rate_that_keeps_the_subcarrier) {
    app::VorChannelizer c;
    const float rate = c.configure(250000.0, 0.0);
    CHECK(rate >= static_cast<float>(app::VorChannelizer::kMinChannelRateHz));
    CHECK_EQ(c.decimation(), 6u);
    CHECK_NEAR(rate, 250000.0 / 6.0, 1.0);
    /* 9960 + 480 Hz must sit well inside Nyquist. */
    CHECK(rate / 2.0f > 10440.0f * 1.5f);

    /* A rate too low to decimate at all still configures. */
    app::VorChannelizer slow;
    const float slow_rate = slow.configure(30000.0, 0.0);
    CHECK_EQ(slow.decimation(), 1u);
    CHECK_NEAR(slow_rate, 30000.0, 1.0);

    /* And a nonsensical rate does not divide by zero. */
    app::VorChannelizer none;
    CHECK_NEAR(none.configure(0.0, 0.0), 0.0, 1e-9);
    std::vector<float> env;
    CHECK_EQ(none.process(nullptr, 0, env), 0u);
}

TEST(vor_channelizer_and_decoder_recover_a_bearing_from_iq) {
    /* Full host pipeline minus the radio: an off-centre AM carrier in complex
     * baseband -> mix -> channel filter/decimate -> envelope -> decode. */
    constexpr double fs = 250000.0;
    constexpr double offset = 40000.0;  /* carrier this far above the LO */
    constexpr double radial = 311.0;

    const auto envelope = make_vor_envelope(fs, 0.40, radial);
    std::vector<dsp::cfloat> iq(envelope.size());
    for (size_t i = 0; i < envelope.size(); i++) {
        const double t = static_cast<double>(i) / fs;
        const auto rot = std::polar(static_cast<double>(envelope[i]),
                                    2.0 * kPi * offset * t);
        iq[i] = dsp::cfloat{static_cast<float>(rot.real()), static_cast<float>(rot.imag())};
    }

    app::VorChannelizer channelizer;
    const float channel_rate = channelizer.configure(fs, offset);
    app::VorDecoder decoder;
    decoder.configure(channel_rate);

    std::vector<float> demod;
    channelizer.process(iq.data(), iq.size(), demod);
    CHECK(demod.size() >= decoder.window_samples());

    decoder.process(demod.data(), demod.size());
    app::VorStatus s{};
    CHECK(decoder.take_status(s));
    CHECK(s.valid);
    CHECK_NEAR(wrap_error(s.radial_deg, radial), 0.0, 2.0);
    CHECK_NEAR(s.ref_level, 480, 15);
    CHECK_NEAR(s.var_level, 300, 15);
}

TEST(vor_channelizer_rejects_an_adjacent_channel) {
    /* A strong unmodulated carrier on the next 50 kHz channel must not reach
     * the envelope: if it did it would beat with the wanted carrier and swamp
     * the 30 Hz measurement. */
    constexpr double fs = 250000.0;
    constexpr double radial = 45.0;

    const auto envelope = make_vor_envelope(fs, 0.40, radial);
    std::vector<dsp::cfloat> iq(envelope.size());
    for (size_t i = 0; i < envelope.size(); i++) {
        const double t = static_cast<double>(i) / fs;
        const auto wanted = std::polar(static_cast<double>(envelope[i]), 0.0);
        const auto interferer = std::polar(2.0, 2.0 * kPi * 50000.0 * t);
        const auto sum = wanted + interferer;
        iq[i] = dsp::cfloat{static_cast<float>(sum.real()), static_cast<float>(sum.imag())};
    }

    app::VorChannelizer channelizer;
    const float channel_rate = channelizer.configure(fs, 0.0);
    app::VorDecoder decoder;
    decoder.configure(channel_rate);

    std::vector<float> demod;
    channelizer.process(iq.data(), iq.size(), demod);
    decoder.process(demod.data(), demod.size());

    app::VorStatus s{};
    CHECK(decoder.take_status(s));
    CHECK(s.valid);
    CHECK_NEAR(wrap_error(s.radial_deg, radial), 0.0, 2.0);
}

/* --- Band constants --------------------------------------------------------- */

TEST(vor_band_constants_match_the_allocation) {
    /* Read through volatile so the comparisons are not folded away between two
     * compile-time constants, which MSVC /W4 flags as a constant conditional. */
    volatile uint64_t low = app::kVorBandLowHz;
    volatile uint64_t high = app::kVorBandHighHz;
    volatile uint64_t step = app::kVorChannelStepHz;

    CHECK_EQ(low, 108'000'000ull);
    CHECK_EQ(high, 117'950'000ull);
    CHECK_EQ(step, 50'000ull);
    /* The band is a whole number of 50 kHz channels wide. */
    CHECK_EQ((high - low) % step, 0ull);
}
