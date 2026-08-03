/*
 * mayhem-b200 — tests for the Morse (CW) receiver.
 *
 * Expected values come from the Morse code itself and from upstream's
 * morsedecoder.hpp / proc_morse.cpp, never from what this port happens to
 * produce. The timing figures are worked out from upstream's own thresholds:
 * a default time unit of 119 ms gives an inter-element threshold of 95.2 ms, an
 * inter-character threshold of 297.5 ms and an inter-word threshold of 714 ms,
 * and the dot/dash decision sits at 2.0 units = 238 ms.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_morse_radio.hpp"

#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace app::morse;

namespace {

constexpr double kPi = 3.14159265358979323846;

/* Renders a key-down/key-up plan as an audio tone. Each entry is (key down?,
 * milliseconds). Phase runs continuously so a keyed tone has no discontinuity
 * beyond the gating itself. */
std::vector<float> render_tone(const std::vector<std::pair<bool, int>>& plan,
                               double sample_rate,
                               double tone_hz,
                               float amplitude) {
    std::vector<float> out;
    double phase = 0.0;
    const double dphi = 2.0 * kPi * tone_hz / sample_rate;
    for (const auto& step : plan) {
        const size_t n = static_cast<size_t>(sample_rate * step.second / 1000.0);
        for (size_t i = 0; i < n; i++) {
            out.push_back(step.first ? static_cast<float>(amplitude * std::sin(phase)) : 0.0f);
            phase += dphi;
            if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
        }
    }
    return out;
}

/* Feeds a duration stream through the accumulator + decoder exactly as the view
 * does, and returns everything the decoder emitted. */
std::string decode_durations(Decoder& d, const std::vector<int32_t>& durations_us) {
    SignalAccumulator acc;
    std::string text;
    for (int32_t us : durations_us) {
        const int32_t ms = acc.process(us, d.getInterWordThreshold());
        const auto r = d.handleInput(ms);
        if (r.isValid()) text += r.text;
    }
    return text;
}

}  // namespace

/* ---------------------------------------------------------------- table --- */

TEST(morse_table_is_upstreams) {
    /* Upstream ships 50 entries: 26 letters, 10 digits, 14 punctuation.
     * Counted rather than compared to the constant so the check survives
     * constant folding. */
    size_t counted = 0;
    for (size_t i = 0; i < table_size; i++) counted += (table[i].letter != nullptr) ? 1u : 0u;
    CHECK_EQ(counted, static_cast<size_t>(50));

    CHECK_STR_EQ(lookup("."), "E");
    CHECK_STR_EQ(lookup("-"), "T");
    CHECK_STR_EQ(lookup("..."), "S");
    CHECK_STR_EQ(lookup("---"), "O");
    CHECK_STR_EQ(lookup(".--."), "P");
    CHECK_STR_EQ(lookup(".-"), "A");
    CHECK_STR_EQ(lookup(".-."), "R");
    CHECK_STR_EQ(lookup(".."), "I");
    CHECK_STR_EQ(lookup("-----"), "0");
    CHECK_STR_EQ(lookup("....-"), "4");
    CHECK_STR_EQ(lookup("-...-"), "=");
    CHECK_STR_EQ(lookup(".-.-."), "+");

    /* Unknown sequences come back braced, which is what colours them white. */
    CHECK_STR_EQ(lookup("......."), "{.......}");
    CHECK_STR_EQ(lookup("--------"), "{--------}");
}

TEST(morse_table_codes_are_unique) {
    std::set<std::string> codes;
    for (size_t i = 0; i < table_size; i++) {
        const std::string c{table[i].code};
        CHECK(codes.insert(c).second);
    }
    CHECK_EQ(codes.size(), table_size);
}

/* ------------------------------------------------------------ ring buffer -- */

TEST(morse_ring_buffer_wraps_at_40) {
    RingBuffer40 r;
    CHECK(r.empty());

    for (uint32_t i = 0; i < 40; i++) r.push_back(i);
    CHECK_EQ(r.size(), static_cast<size_t>(40));
    CHECK_EQ(r[0], 0u);
    CHECK_EQ(r[39], 39u);

    /* Five more: the oldest five fall off the front. */
    for (uint32_t i = 40; i < 45; i++) r.push_back(i);
    CHECK_EQ(r.size(), static_cast<size_t>(40));
    CHECK_EQ(r.front(), 5u);
    CHECK_EQ(r[0], 5u);
    CHECK_EQ(r[39], 44u);

    uint32_t copy[40]{};
    r.copy_to_array(copy);
    CHECK_EQ(copy[0], 5u);
    CHECK_EQ(copy[39], 44u);

    r.pop_front();
    CHECK_EQ(r.size(), static_cast<size_t>(39));
    CHECK_EQ(r.front(), 6u);

    r.clear();
    CHECK(r.empty());
}

/* ----------------------------------------------------- symbol / gap timing - */

TEST(morse_default_thresholds) {
    Decoder d;
    CHECK_NEAR(d.getCurrentTimeUnit(), 119.0, 1e-9);
    CHECK_NEAR(d.getInterElementThreshold(), 95.2, 1e-9);
    CHECK_NEAR(d.getInterCharThreshold(), 297.5, 1e-9);
    CHECK_NEAR(d.getInterWordThreshold(), 714.0, 1e-9);
}

TEST(morse_dot_dash_classification) {
    Decoder d;
    /* Interpolation runs from 1.5 units (178.5 ms) to 2.5 units (297.5 ms). */
    CHECK_NEAR(d.getDahProbability(50), 0.0, 1e-9);
    CHECK_NEAR(d.getDahProbability(178), 0.0, 1e-9);
    CHECK_NEAR(d.getDahProbability(238), 0.5, 0.005);  /* 2.0 units: the fence */
    CHECK_NEAR(d.getDahProbability(298), 1.0, 1e-9);
    CHECK_NEAR(d.getDahProbability(1000), 1.0, 1e-9);

    /* And the same boundaries seen through handleInput: > 0.5 is a dash. */
    Decoder e;
    e.handleInput(100);
    CHECK_STR_EQ(e.getLastSequence(), ".");
    e.handleInput(300);
    CHECK_STR_EQ(e.getLastSequence(), ".-");
    e.handleInput(200);  /* 200 ms > 178.5 but < 238: still a dot */
    CHECK_STR_EQ(e.getLastSequence(), ".-.");
    e.handleInput(280);  /* 280 ms > 238: a dash */
    CHECK_STR_EQ(e.getLastSequence(), ".-.-");
}

TEST(morse_gap_classification) {
    /* An inter-element gap emits nothing; an inter-character gap emits the
     * letter; an inter-word gap emits the letter and a space. */
    {
        Decoder d;
        d.handleInput(100);
        auto r = d.handleInput(-90);  /* below 297.5 */
        CHECK(!r.isValid());
    }
    {
        Decoder d;
        d.handleInput(100);
        auto r = d.handleInput(-300);
        CHECK(r.isValid());
        CHECK_STR_EQ(r.text, "E");
    }
    {
        Decoder d;
        d.handleInput(100);
        auto r = d.handleInput(-800);  /* past 714 */
        CHECK(r.isValid());
        CHECK_STR_EQ(r.text, "E ");
    }
    {
        /* Noise inside +/-5 ms is discarded outright. */
        Decoder d;
        auto r = d.handleInput(4);
        CHECK(!r.isValid());
        CHECK_STR_EQ(d.getLastSequence(), "");
        r = d.handleInput(-4);
        CHECK(!r.isValid());
    }
}

TEST(morse_unmatched_sequence_has_zero_confidence) {
    Decoder d;
    for (int i = 0; i < 8; i++) d.handleInput(100);  /* eight dots: not a letter */
    const auto r = d.handleInput(-400);
    CHECK(r.isValid());
    CHECK_STR_EQ(r.text, "{........}");
    CHECK_NEAR(r.confidence, 0.0, 1e-12);
}

/* ------------------------------------------------------- adaptive timing --- */

TEST(morse_decision_boundary_splits_dots_from_dashes) {
    /* Six dots then four dashes: the split is at index 6. */
    uint32_t sorted[10] = {100, 100, 100, 100, 100, 100, 300, 300, 300, 300};
    CHECK_EQ(Decoder::findDecisionBoundary(sorted, 10), static_cast<size_t>(6));

    /* Fewer than four samples is never enough to call a boundary. */
    uint32_t few[3] = {100, 100, 300};
    CHECK_EQ(Decoder::findDecisionBoundary(few, 3), static_cast<size_t>(0));

    /* A flat list has no jump larger than half the preceding value. */
    uint32_t flat[6] = {100, 101, 102, 103, 104, 105};
    CHECK_EQ(Decoder::findDecisionBoundary(flat, 6), static_cast<size_t>(0));
}

TEST(morse_learning_pulls_time_unit_towards_the_dot_length) {
    Decoder d;
    CHECK_NEAR(d.getCurrentTimeUnit(), 119.0, 1e-9);

    /* Ten pulses, six dots of 100 ms and four dashes of 300 ms, is the first
     * point at which calculatePulseUnit() will run: split index 6, avg dit 100,
     * avg dah 300, ratio exactly 3.0 so confidence is 1.0 and the estimate is
     * taken whole. Rate limiting allows +/-25% (89.25..148.75), and the
     * learning factor at |100-160|/160 = 0.375 deviation is
     * 0.05 + 0.2*0.75 = 0.20, so the unit moves 119 -> 0.8*119 + 0.2*100. */
    for (int i = 0; i < 6; i++) d.handleInput(100);
    for (int i = 0; i < 3; i++) d.handleInput(300);
    CHECK_NEAR(d.getCurrentTimeUnit(), 119.0, 1e-9);  /* nine pulses: not yet */

    d.handleInput(300);
    CHECK_NEAR(d.getCurrentTimeUnit(), 115.2, 1e-6);

    /* It keeps converging towards 100 and never overshoots it. */
    for (int i = 0; i < 60; i++) d.handleInput(100);
    CHECK(d.getCurrentTimeUnit() > 99.0);
    CHECK(d.getCurrentTimeUnit() < 116.0);
}

TEST(morse_pulse_unit_rejects_implausible_ratios) {
    Decoder d;
    double unit = -1.0, conf = -1.0;

    /* Ten equal pulses: no boundary, so no estimate. */
    for (int i = 0; i < 10; i++) d.handleInput(100);
    CHECK(!d.calculatePulseUnit(unit, conf));

    /* A 10:1 spread is outside upstream's 1.5..5.0 acceptance window. */
    Decoder e;
    for (int i = 0; i < 6; i++) e.handleInput(20);
    for (int i = 0; i < 4; i++) e.handleInput(200);
    CHECK(!e.calculatePulseUnit(unit, conf));
}

/* ------------------------------------------------------ signal accumulator - */

TEST(morse_accumulator_merges_and_delays_by_one_state) {
    SignalAccumulator a;
    const double word_threshold_ms = 714.0;

    /* The first interval only primes the accumulator. */
    CHECK_EQ(a.process(-500000, word_threshold_ms), 0);
    /* Each state change flushes the interval that just ended, in ms. */
    CHECK_EQ(a.process(100000, word_threshold_ms), -500);
    CHECK_EQ(a.process(-100000, word_threshold_ms), 100);
    CHECK_EQ(a.process(100000, word_threshold_ms), -100);
}

TEST(morse_accumulator_sums_same_sign_intervals) {
    SignalAccumulator a;
    const double word_threshold_ms = 714.0;

    a.process(50000, word_threshold_ms);                   /* prime, tone */
    CHECK_EQ(a.process(30000, word_threshold_ms), 0);      /* still tone: merge */
    CHECK_EQ(a.process(20000, word_threshold_ms), 0);
    CHECK_EQ(a.process(-10000, word_threshold_ms), 100);   /* 50+30+20 ms */
}

TEST(morse_accumulator_emits_a_word_gap_early) {
    SignalAccumulator a;
    const double word_threshold_ms = 714.0;

    a.process(10000, word_threshold_ms);  /* prime with a tone */
    a.process(-300000, word_threshold_ms);
    /* Second silence chunk carries the total past 714 ms, so it is reported
     * without waiting for the next key-down. */
    CHECK_EQ(a.process(-500000, word_threshold_ms), -800);
    /* ...and only once. */
    CHECK_EQ(a.process(-500000, word_threshold_ms), 0);
    /* The key-down that follows reports nothing, the gap having been sent. */
    CHECK_EQ(a.process(10000, word_threshold_ms), 0);
}

/* ------------------------------------------------------------- decoding ---- */

TEST(morse_decodes_sos) {
    /* Durations in microseconds as the tone detector produces them: alternating
     * sign, one per key transition. Dot 100 ms, dash 300 ms, inter-element gap
     * 100 ms, inter-character gap 350 ms, word gap 800 ms. */
    const std::vector<int32_t> plan = {
        -500000,                                            /* lead-in silence */
        100000, -100000, 100000, -100000, 100000, -350000,  /* S */
        300000, -100000, 300000, -100000, 300000, -350000,  /* O */
        100000, -100000, 100000, -100000, 100000, -800000,  /* S + word gap */
        100000, -500000,                                    /* flush */
    };

    Decoder d;
    CHECK_STR_EQ(decode_durations(d, plan), "SOS ");
}

TEST(morse_decodes_paris) {
    /* PARIS: .--.  .-  .-.  ..  ...  — 14 elements, so the adaptive timing does
     * kick in part-way through. It converges towards the 100 ms dot, which
     * keeps every threshold on the right side of the injected timings. */
    const std::vector<int32_t> plan = {
        -500000,
        /* P  . - - . */
        100000, -100000, 300000, -100000, 300000, -100000, 100000, -350000,
        /* A  . - */
        100000, -100000, 300000, -350000,
        /* R  . - . */
        100000, -100000, 300000, -100000, 100000, -350000,
        /* I  . . */
        100000, -100000, 100000, -350000,
        /* S  . . . */
        100000, -100000, 100000, -100000, 100000, -800000,
        100000, -500000,
    };

    Decoder d;
    CHECK_STR_EQ(decode_durations(d, plan), "PARIS ");
}

TEST(morse_wpm_from_time_unit) {
    /* Upstream: wpm = round(3600 / (3*unit + 18)). At the 119 ms default that
     * is 3600 / 375 = 9.6 -> 10 wpm. */
    Decoder d;
    CHECK_EQ(d.wpm(), static_cast<uint16_t>(10));
}

/* -------------------------------------------------------- tone detector ---- */

TEST(morse_goertzel_coefficient_matches_upstream_formula) {
    ToneProcessor p;
    p.configure(Modulation::AM);  /* 12 kHz audio rate */

    /* coeff = 2 * (1 - w^2/2) * 16384 with w = 2*pi*f/fs, truncated to int32.
     * Upstream deliberately uses the small-angle cosine, so the port must too. */
    for (float f : {300.0f, 700.0f, 1000.0f, 2300.0f}) {
        p.update_goertzel_coeff(f);
        const double w = 2.0 * kPi * f / 12000.0;
        const double expected = 2.0 * (1.0 - w * w * 0.5) * 16384.0;
        CHECK_NEAR(p.goertzel_coefficient(), static_cast<int32_t>(expected), 1.0);
    }

    /* Out-of-range frequencies clamp to 300 and 2300 Hz. */
    p.update_goertzel_coeff(100.0f);
    const int32_t at_300 = p.goertzel_coefficient();
    p.update_goertzel_coeff(300.0f);
    CHECK_EQ(p.goertzel_coefficient(), at_300);

    p.update_goertzel_coeff(9000.0f);
    const int32_t at_2300 = p.goertzel_coefficient();
    p.update_goertzel_coeff(2300.0f);
    CHECK_EQ(p.goertzel_coefficient(), at_2300);
}

TEST(morse_tone_detector_times_key_intervals) {
    /* 700 Hz keyed tone at 12 kHz. The Goertzel evaluates every 60 samples, so
     * every reported interval is a multiple of 5 ms and each edge can land one
     * block late — hence the 15 ms tolerance. */
    const std::vector<std::pair<bool, int>> plan = {
        {false, 500}, {true, 200}, {false, 300}, {true, 400}, {false, 300}, {true, 100}, {false, 300}};
    const auto audio = render_tone(plan, 12000.0, 700.0, 0.03f);

    ToneProcessor p;
    p.configure(Modulation::AM);
    p.set_squelch_level(0);

    std::vector<int32_t> durations;
    p.process_audio(audio.data(), audio.size(), durations);

    /* Six edges in the plan produce six intervals; the trailing silence is only
     * reported when it ends or times out, so it is not among them. */
    CHECK_EQ(durations.size(), static_cast<size_t>(6));
    if (durations.size() == 6) {
        CHECK_NEAR(durations[0], -500000, 15000);
        CHECK_NEAR(durations[1], 200000, 15000);
        CHECK_NEAR(durations[2], -300000, 15000);
        CHECK_NEAR(durations[3], 400000, 15000);
        CHECK_NEAR(durations[4], -300000, 15000);
        CHECK_NEAR(durations[5], 100000, 15000);
    }
}

TEST(morse_tone_detector_ignores_silence) {
    std::vector<float> silence(12000 * 2, 0.0f);  /* 2 s */

    ToneProcessor p;
    p.configure(Modulation::AM);
    p.set_squelch_level(0);

    std::vector<int32_t> durations;
    p.process_audio(silence.data(), silence.size(), durations);

    /* No tone ever starts, so nothing is reported until the 2.4 s silence
     * timeout — which two seconds does not reach. */
    CHECK(durations.empty());
}

TEST(morse_tone_detector_reports_a_long_silence_on_timeout) {
    /* Upstream flushes a silence longer than 28800 samples (2.4 s at 12 kHz)
     * so a decoder waiting on a word break is not left hanging. */
    std::vector<float> silence(12000 * 3, 0.0f);

    ToneProcessor p;
    p.configure(Modulation::AM);
    p.set_squelch_level(0);

    std::vector<int32_t> durations;
    p.process_audio(silence.data(), silence.size(), durations);

    CHECK_EQ(durations.size(), static_cast<size_t>(1));
    if (!durations.empty()) {
        /* 28800 samples + one 60-sample block, times 250/3 us. */
        CHECK(durations[0] < 0);
        CHECK_NEAR(durations[0], -2400000, 30000);
    }
}

TEST(morse_end_to_end_sos_from_a_keyed_tone) {
    /* The whole chain the app runs, minus the radio: keyed 700 Hz audio ->
     * Goertzel tone detector -> interval accumulator -> decoder. */
    const std::vector<std::pair<bool, int>> plan = {
        {false, 500},
        {true, 100}, {false, 100}, {true, 100}, {false, 100}, {true, 100}, {false, 350},
        {true, 300}, {false, 100}, {true, 300}, {false, 100}, {true, 300}, {false, 350},
        {true, 100}, {false, 100}, {true, 100}, {false, 100}, {true, 100}, {false, 800},
        {true, 100}, {false, 500},
    };
    const auto audio = render_tone(plan, 12000.0, 700.0, 0.03f);

    ToneProcessor p;
    p.configure(Modulation::AM);
    p.set_squelch_level(0);

    std::vector<int32_t> durations;
    p.process_audio(audio.data(), audio.size(), durations);

    Decoder d;
    CHECK_STR_EQ(decode_durations(d, durations), "SOS ");
}

TEST(morse_am_path_clip_flag_tracks_the_last_sample) {
    /* Upstream recomputes `clipped` on every sample and never latches it, so
     * after a block the flag describes that block's last sample only — and only
     * positive excursions, because the negative limiter does not set it. The
     * AM path's gain is 16, so anything past 1/16 of full scale clips. */
    ToneProcessor p;
    p.configure(Modulation::AM);
    std::vector<int32_t> d;

    std::vector<float> loud(600, 0.9f);
    p.process_audio(loud.data(), loud.size(), d);
    CHECK(p.clipped());

    std::vector<float> quiet(600, 0.03f);
    p.process_audio(quiet.data(), quiet.size(), d);
    CHECK(!p.clipped());

    std::vector<float> loud_negative(600, -0.9f);
    p.process_audio(loud_negative.data(), loud_negative.size(), d);
    CHECK(!p.clipped());
}

TEST(morse_fm_mode_uses_the_24khz_rate) {
    ToneProcessor p;
    p.configure(Modulation::FM);
    CHECK_NEAR(p.audio_rate_hz(), 24000.0, 1e-6);

    p.configure(Modulation::AM);
    CHECK_NEAR(p.audio_rate_hz(), 12000.0, 1e-6);
    p.configure(Modulation::USB);
    CHECK_NEAR(p.audio_rate_hz(), 12000.0, 1e-6);
}

TEST(morse_tone_frequency_measurement) {
    /* The zero-crossing meter needs six consecutive stable periods and reports
     * about every 200 ms, rounded down to 5 Hz. 12000/1000 = 12 samples per
     * period exactly, so the measurement should land on 1000 Hz. */
    const auto audio = render_tone({{true, 1000}}, 12000.0, 1000.0, 0.03f);

    ToneProcessor p;
    p.configure(Modulation::AM);
    p.set_squelch_level(0);

    std::vector<int32_t> d;
    p.process_audio(audio.data(), audio.size(), d);

    uint32_t hz = 0;
    CHECK(p.take_frequency_update(hz));
    CHECK_NEAR(hz, 1000, 15);
    /* The flag is consumed. */
    uint32_t again = 0;
    CHECK(!p.take_frequency_update(again));
}
