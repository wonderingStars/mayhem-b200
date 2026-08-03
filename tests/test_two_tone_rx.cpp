/*
 * mayhem-b200 — tests for the Two-Tone (Motorola Quik-Call II) RX decoder.
 *
 * The tone table and the two matching tolerances come from upstream
 * (baseband/proc_tonedetect.cpp, application/external/two_tone_rx/), so the
 * expected values below are derived from the protocol table, not from whatever
 * this implementation happens to produce.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_two_tone_rx.hpp"

#include <cmath>
#include <vector>

using namespace app::two_tone;

namespace {

constexpr float kRate = 24000.0f;
constexpr float kAmp = 0.5f;

/* Appends `windows` * 960 samples of a pure tone, continuing the phase. */
void append_tone(std::vector<float>& out, double freq_hz, size_t samples, double& phase) {
    const double inc = 2.0 * 3.14159265358979323846 * freq_hz / kRate;
    for (size_t i = 0; i < samples; i++) {
        out.push_back(kAmp * static_cast<float>(std::sin(phase)));
        phase += inc;
    }
}

/* Two real Quik-Call II tones, far enough apart in the table that leakage into
 * the neighbouring Goertzel bins stays under the detection threshold:
 *   index 20 = 855.5 Hz (group A), index 27 = 1153.4 Hz (group B). */
constexpr size_t kToneAIndex = 20;
constexpr size_t kToneBIndex = 27;
constexpr double kToneAHz = 855.5;
constexpr double kToneBHz = 1153.4;

}  // namespace

TEST(two_tone_tone_tables_match_upstream) {
    /* Spot-check the ends and the two entries the other tests use. */
    CHECK_EQ(kMotoFreqsX10[0], 2885u);
    CHECK_EQ(kMotoFreqsX10[kMotoToneCount - 1], 20275u);
    CHECK_EQ(kMotoFreqsX10[kToneAIndex], 8555u);
    CHECK_EQ(kMotoFreqsX10[kToneBIndex], 11534u);

    /* CTCSS index 0 is "None"; 1 is 67.0 Hz; 50 is 254.1 Hz. */
    CHECK_EQ(kCtcssFreqsX10[0], 0u);
    CHECK_EQ(kCtcssFreqsX10[1], 670u);
    CHECK_EQ(kCtcssFreqsX10[kCtcssOptionCount - 1], 2541u);

    CHECK_STR_EQ(ctcss_name(0), "None");
    CHECK_STR_EQ(ctcss_name(1738), "173.8Hz");
}

TEST(two_tone_moto_index_snaps_within_tolerance) {
    /* Exact table value. */
    CHECK_EQ(moto_index(855, kMotoTransitionSnapHz), kToneAIndex);
    /* Inside the 30 Hz live tolerance. */
    CHECK_EQ(moto_index(875, kMotoTransitionSnapHz), kToneAIndex);
    /* Outside 30 Hz but inside the 60 Hz logging tolerance; the nearest entry
     * at 903.2 Hz is 48 Hz away, 855.5 Hz is 40 Hz away, so it stays on A. */
    CHECK_EQ(moto_index(895, kMotoTransitionSnapHz), 21u);
    /* Nothing within tolerance of a frequency far below the table. */
    CHECK_EQ(moto_index(100, kMotoTransitionSnapHz), kMotoNone);
    /* Zero is "no candidate". */
    CHECK_EQ(moto_index(0, kMotoFinalMatchHz), kMotoNone);
}

TEST(two_tone_format_tone_snaps_and_formats) {
    CHECK_STR_EQ(format_tone(856, 1000), "855.5Hz 1000ms");
    CHECK_STR_EQ(format_tone(1154, 3000), "1153.4Hz 3000ms");
    /* Far from every entry: reported raw. */
    CHECK_STR_EQ(format_tone(100, 40), "100Hz 40ms");
}

TEST(two_tone_goertzel_detects_a_known_tone) {
    ToneDetector det;
    det.configure(kRate, 0);
    det.set_carrier_open(true);

    CHECK_EQ(det.window_samples(), 960u);
    CHECK_EQ(det.window_ms(), 40u);

    std::vector<float> audio;
    double phase = 0.0;
    append_tone(audio, kToneAHz, 5 * 960, phase);

    std::vector<ToneWindow> windows;
    det.process(audio.data(), audio.size(), windows);

    CHECK_EQ(windows.size(), 5u);
    for (size_t i = 0; i < windows.size(); i++) {
        CHECK(!windows[i].tone_end);
        /* The centroid estimate must land on the right table entry at both the
         * live (30 Hz) and the logging (60 Hz) tolerance. */
        CHECK_EQ(moto_index(windows[i].freq_hz, kMotoTransitionSnapHz), kToneAIndex);
        CHECK_NEAR(windows[i].freq_hz, 855.5, 10.0);
        /* duration_ms counts windows since the gate opened. */
        CHECK_EQ(windows[i].duration_ms, static_cast<uint32_t>((i + 1) * 40));
    }

    /* The winning Goertzel bin is the tone's own, and it beats both neighbours
     * by a wide margin. */
    const auto& e = det.last_energies();
    CHECK(e[kToneAIndex] > e[kToneAIndex - 1] * 10.0f);
    CHECK(e[kToneAIndex] > e[kToneAIndex + 1] * 10.0f);
}

TEST(two_tone_goertzel_rejects_noise_free_silence) {
    ToneDetector det;
    det.configure(kRate, 0);
    det.set_carrier_open(true);

    std::vector<float> audio(3 * 960, 0.0f);
    std::vector<ToneWindow> windows;
    det.process(audio.data(), audio.size(), windows);

    CHECK_EQ(windows.size(), 3u);
    for (const auto& w : windows) {
        /* Gate is open (carrier present) but no tone exceeds the energy
         * threshold, so the window reports "no candidate". */
        CHECK_EQ(w.freq_hz, 0u);
        CHECK(!w.tone_end);
    }
}

TEST(two_tone_closed_carrier_emits_tone_end_once) {
    ToneDetector det;
    det.configure(kRate, 0);
    det.set_carrier_open(true);

    std::vector<float> audio;
    double phase = 0.0;
    append_tone(audio, kToneAHz, 2 * 960, phase);

    std::vector<ToneWindow> windows;
    det.process(audio.data(), audio.size(), windows);
    CHECK_EQ(windows.size(), 2u);

    windows.clear();
    det.set_carrier_open(false);
    std::vector<float> silence(2 * 960, 0.0f);
    det.process(silence.data(), silence.size(), windows);

    /* One tone_end when the gate closes, then nothing. */
    CHECK_EQ(windows.size(), 1u);
    CHECK(windows[0].tone_end);
    CHECK_EQ(windows[0].freq_hz, 0u);
    CHECK_EQ(windows[0].duration_ms, 80u);
}

TEST(two_tone_ctcss_gate_requires_the_sub_audible_tone) {
    /* CTCSS index 12 = 100.0 Hz. */
    CHECK_EQ(kCtcssFreqsX10[12], 1000u);

    std::vector<float> audio;
    double phase = 0.0;
    append_tone(audio, kToneAHz, 3 * 960, phase);

    {
        ToneDetector det;
        det.configure(kRate, kCtcssFreqsX10[12]);
        det.set_carrier_open(true);
        std::vector<ToneWindow> windows;
        det.process(audio.data(), audio.size(), windows);
        /* Carrier present but no 100 Hz CTCSS: gate stays shut, so no windows
         * are reported at all (and no tone_end, since it never opened). */
        CHECK_EQ(windows.size(), 0u);
    }

    /* Add the CTCSS tone and the same audio now passes the gate. */
    std::vector<float> with_ctcss;
    double p1 = 0.0;
    double p2 = 0.0;
    append_tone(with_ctcss, kToneAHz, 3 * 960, p1);
    {
        std::vector<float> sub;
        append_tone(sub, 100.0, 3 * 960, p2);
        for (size_t i = 0; i < with_ctcss.size(); i++) with_ctcss[i] += sub[i];
    }

    {
        ToneDetector det;
        det.configure(kRate, kCtcssFreqsX10[12]);
        det.set_carrier_open(true);
        std::vector<ToneWindow> windows;
        det.process(with_ctcss.data(), with_ctcss.size(), windows);
        CHECK_EQ(windows.size(), 3u);
        for (const auto& w : windows) {
            CHECK_EQ(moto_index(w.freq_hz, kMotoFinalMatchHz), kToneAIndex);
        }
    }
}

TEST(two_tone_sequencer_logs_a_known_pair) {
    /* End-to-end through the real Goertzel front end: a 1 s A tone followed by
     * a 3 s B tone, then the carrier drops. That is a textbook Quik-Call II
     * page (1 s "A", 3 s "B"). */
    ToneDetector det;
    det.configure(kRate, 0);
    det.set_carrier_open(true);

    TwoToneSequencer seq;
    seq.set_window_ms(det.window_ms());

    std::vector<float> audio;
    double phase = 0.0;
    append_tone(audio, kToneAHz, 25 * 960, phase);  /* 1000 ms */
    phase = 0.0;
    append_tone(audio, kToneBHz, 75 * 960, phase);  /* 3000 ms */

    std::vector<ToneWindow> windows;
    det.process(audio.data(), audio.size(), windows);
    CHECK_EQ(windows.size(), 100u);

    det.set_carrier_open(false);
    std::vector<float> silence(960, 0.0f);
    det.process(silence.data(), silence.size(), windows);
    CHECK_EQ(windows.size(), 101u);
    CHECK(windows.back().tone_end);

    DetectedPair pair{};
    int pairs = 0;
    for (const auto& w : windows) {
        if (seq.feed(w, pair)) pairs++;
    }

    CHECK_EQ(pairs, 1);
    CHECK_EQ(moto_index(pair.t1_freq_hz, kMotoFinalMatchHz), kToneAIndex);
    CHECK_EQ(moto_index(pair.t2_freq_hz, kMotoFinalMatchHz), kToneBIndex);
    CHECK_EQ(pair.t1_duration_ms, 1000u);
    CHECK_EQ(pair.t2_duration_ms, 3000u);

    CHECK_STR_EQ(format_tone(pair.t1_freq_hz, pair.t1_duration_ms) + " " +
                     format_tone(pair.t2_freq_hz, pair.t2_duration_ms),
                 "855.5Hz 1000ms 1153.4Hz 3000ms");
}

TEST(two_tone_sequencer_rejects_a_short_b_tone) {
    /* Same A tone, but B lasts only 400 ms — under the 500 ms minimum, so the
     * pair must be dropped. */
    ToneDetector det;
    det.configure(kRate, 0);
    det.set_carrier_open(true);

    TwoToneSequencer seq;
    seq.set_window_ms(det.window_ms());

    std::vector<float> audio;
    double phase = 0.0;
    append_tone(audio, kToneAHz, 25 * 960, phase);
    phase = 0.0;
    append_tone(audio, kToneBHz, 10 * 960, phase);  /* 400 ms */

    std::vector<ToneWindow> windows;
    det.process(audio.data(), audio.size(), windows);
    det.set_carrier_open(false);
    std::vector<float> silence(960, 0.0f);
    det.process(silence.data(), silence.size(), windows);

    DetectedPair pair{};
    int pairs = 0;
    for (const auto& w : windows) {
        if (seq.feed(w, pair)) pairs++;
    }
    CHECK_EQ(pairs, 0);
}

TEST(two_tone_sequencer_rejects_a_single_tone) {
    /* A carrier with one tone and no transition never reaches T2, so nothing
     * is logged when the gate closes. */
    TwoToneSequencer seq;
    seq.set_window_ms(40);

    DetectedPair pair{};
    int pairs = 0;
    for (int i = 0; i < 50; i++) {
        if (seq.feed(ToneWindow{856, static_cast<uint32_t>((i + 1) * 40), false}, pair)) pairs++;
    }
    CHECK(seq.state() == TwoToneSequencer::State::T1Collecting);
    if (seq.feed(ToneWindow{0, 2000, true}, pair)) pairs++;
    CHECK_EQ(pairs, 0);
    CHECK(seq.state() == TwoToneSequencer::State::Idle);
}

TEST(two_tone_sequencer_tolerates_one_dropout_in_b) {
    /* A single zero-frequency window during B is forgiven; a second one breaks
     * the sequence. */
    TwoToneSequencer seq;
    seq.set_window_ms(40);
    DetectedPair pair{};

    for (int i = 0; i < 25; i++) seq.feed(ToneWindow{856, 0, false}, pair);
    for (int i = 0; i < 12; i++) seq.feed(ToneWindow{1154, 0, false}, pair);
    CHECK(seq.state() == TwoToneSequencer::State::T2Collecting);

    /* First dropout: still collecting B. */
    CHECK(!seq.feed(ToneWindow{0, 0, false}, pair));
    CHECK(seq.state() == TwoToneSequencer::State::T2Collecting);

    /* Second consecutive dropout at only 480 ms of B: below the 2000 ms
     * t2_break threshold, so the sequence is abandoned, not logged. */
    CHECK(!seq.feed(ToneWindow{0, 0, false}, pair));
    CHECK(seq.state() == TwoToneSequencer::State::Idle);
}

TEST(two_tone_sequencer_logs_on_a_long_b_break) {
    /* Upstream's t2_break path: B has run past 2000 ms when the estimate drops
     * out, so the pair is logged immediately rather than waiting for the gate
     * to close. */
    TwoToneSequencer seq;
    seq.set_window_ms(40);
    DetectedPair pair{};

    for (int i = 0; i < 25; i++) seq.feed(ToneWindow{856, 0, false}, pair);   /* A, 1000 ms */
    for (int i = 0; i < 60; i++) seq.feed(ToneWindow{1154, 0, false}, pair);  /* B, 2400 ms */

    CHECK(seq.feed(ToneWindow{0, 0, false}, pair));
    CHECK_EQ(pair.t1_duration_ms, 1000u);
    CHECK_EQ(pair.t2_duration_ms, 2400u);
    CHECK_EQ(pair.t1_freq_hz, 856u);
    CHECK_EQ(pair.t2_freq_hz, 1154u);
    CHECK(seq.state() == TwoToneSequencer::State::Idle);
}

TEST(two_tone_fm_noise_squelch_opens_on_a_clean_tone) {
    FmNoiseSquelch sq;
    sq.set_threshold(0.20f);
    CHECK(sq.enabled());

    /* A clean 1 kHz audio tone has almost no energy above 8 kHz. */
    std::vector<float> tone;
    double phase = 0.0;
    append_tone(tone, 1000.0, 960, phase);
    CHECK(sq.execute(tone.data(), tone.size()));

    /* Full-scale alternating samples are the worst case at Nyquist: the HPF
     * passes them and the gate closes. */
    sq.reset();
    std::vector<float> noise(960);
    for (size_t i = 0; i < noise.size(); i++) noise[i] = (i & 1) ? 1.0f : -1.0f;
    CHECK(!sq.execute(noise.data(), noise.size()));

    /* Threshold zero disables the gate entirely, as upstream does. */
    FmNoiseSquelch off;
    CHECK(!off.enabled());
    CHECK(off.execute(noise.data(), noise.size()));
}
