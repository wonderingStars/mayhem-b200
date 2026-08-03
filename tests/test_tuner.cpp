/*
 * mayhem-b200 — Tuner app logic tests.
 *
 * Covers the pure, hardware-independent logic ported from upstream
 * firmware/application/external/tuner/: the sample-rate enum mapping, the
 * octave-shift "real frequency" computation from TunerView::paint(), and the
 * verbatim instrument note tables. Expected values are taken from upstream's
 * ui_tuner.hpp, not from this port's output.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_tuner.hpp"

using namespace app;

/* --- protected_sample_rate (tuner_rate_hz) ------------------------------- */

TEST(tuner_rate_hz_maps_each_enum) {
    CHECK_EQ(tuner_rate_hz(TunerRate::Hz_12000), 12000u);
    CHECK_EQ(tuner_rate_hz(TunerRate::Hz_24000), 24000u);
    CHECK_EQ(tuner_rate_hz(TunerRate::Hz_48000), 48000u);
}

/* --- real_note_frequency (upstream paint() octave-shift math) ------------ */

TEST(real_frequency_zero_shift_is_identity) {
    CHECK_EQ(real_note_frequency(440, 0), 440);
    CHECK_EQ(real_note_frequency(196, 0), 196);
    CHECK_EQ(real_note_frequency(1, 0), 1);
}

TEST(real_frequency_negative_shift_halves_per_octave) {
    /* Guitar E2: stored 165, shift -1 -> 165/2 = 82 (upstream integer div). */
    CHECK_EQ(real_note_frequency(165, -1), 82);
    /* Two octaves down. */
    CHECK_EQ(real_note_frequency(100, -2), 25);
    CHECK_EQ(real_note_frequency(200, -3), 25);
}

TEST(real_frequency_positive_shift_doubles_per_octave) {
    CHECK_EQ(real_note_frequency(100, 1), 200);
    CHECK_EQ(real_note_frequency(100, 2), 400);
    CHECK_EQ(real_note_frequency(55, 3), 440);
}

/* --- Instrument tables (verbatim from upstream) -------------------------- */

TEST(guitar_table_matches_upstream) {
    const Instrument& g = tuner_guitar();
    CHECK_EQ(g.notes.size(), 6u);

    CHECK_EQ(g.notes.at("E2").frequency, 165);
    CHECK_EQ(g.notes.at("E2").octave_shift, -1);
    CHECK_EQ(g.notes.at("A2").frequency, 110);
    CHECK_EQ(g.notes.at("D3").frequency, 147);
    CHECK_EQ(g.notes.at("G3").frequency, 196);
    CHECK_EQ(g.notes.at("B3").frequency, 247);
    CHECK_EQ(g.notes.at("E4").frequency, 330);

    /* Every guitar note but the low E is played un-shifted. */
    CHECK_EQ(g.notes.at("A2").octave_shift, 0);
    CHECK_EQ(g.notes.at("E4").octave_shift, 0);

    /* Sample rates per upstream: low three at 12k, upper three at 24k. */
    CHECK(g.notes.at("E2").sample_rate == TunerRate::Hz_12000);
    CHECK(g.notes.at("G3").sample_rate == TunerRate::Hz_24000);
}

TEST(guitar_low_e_real_frequency_is_e2) {
    const Instrument& g = tuner_guitar();
    const auto& e2 = g.notes.at("E2");
    /* The host plays the real string frequency, not the PP-shifted 165. */
    CHECK_EQ(real_note_frequency(e2.frequency, e2.octave_shift), 82);
}

TEST(violin_table_matches_upstream) {
    const Instrument& v = tuner_violin();
    CHECK_EQ(v.notes.size(), 4u);
    CHECK_EQ(v.notes.at("G3").frequency, 196);
    CHECK_EQ(v.notes.at("D4").frequency, 294);
    CHECK_EQ(v.notes.at("A4").frequency, 440);
    CHECK_EQ(v.notes.at("E5").frequency, 659);
    CHECK(v.notes.at("A4").sample_rate == TunerRate::Hz_48000);
    /* No violin note is octave-shifted. */
    for (const auto& n : v.notes) CHECK_EQ(n.second.octave_shift, 0);
}

TEST(pitch_fork_table_matches_upstream) {
    const Instrument& p = tuner_pitch_fork();
    CHECK_EQ(p.notes.size(), 4u);
    CHECK_EQ(p.notes.at("12ET A4").frequency, 440);
    CHECK_EQ(p.notes.at("Sarti's A4").frequency, 436);
    CHECK_EQ(p.notes.at("1858 A4").frequency, 435);
    CHECK_EQ(p.notes.at("Verdi's A4").frequency, 432);
    /* All standard-pitch references sample at 48k and are un-shifted. */
    for (const auto& n : p.notes) {
        CHECK(n.second.sample_rate == TunerRate::Hz_48000);
        CHECK_EQ(n.second.octave_shift, 0);
    }
}

/* --- instrument selection by OptionsField index ------------------------- */

TEST(instrument_by_index_selects_expected_table) {
    CHECK(tuner_instrument_by_index(0) == &tuner_guitar());
    CHECK(tuner_instrument_by_index(1) == &tuner_violin());
    CHECK(tuner_instrument_by_index(2) == &tuner_pitch_fork());
}

TEST(instrument_by_index_rejects_out_of_range) {
    CHECK(tuner_instrument_by_index(-1) == nullptr);
    CHECK(tuner_instrument_by_index(3) == nullptr);
    CHECK(tuner_instrument_by_index(99) == nullptr);
}
