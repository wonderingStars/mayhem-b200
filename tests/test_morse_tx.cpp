/*
 * mayhem-b200 — tests for the Morse TX encoder and CW keying.
 *
 * Expected symbol streams follow firmware/common/morse.cpp exactly. Symbol
 * kinds: 0=dot, 1=dash, 2=symbol-space, 3=letter-space, 4=word-space, with
 * durations 1,3,1,3,7 time units (morse_symbols). A letter's pattern is emitted
 * dot/space per code bit and the trailing space is promoted to a letter-space;
 * a non-code character promotes the previous space to a word-space.
 *
 *   SOS = S(...) O(---) S(...)
 *   PARIS = P(.--.) A(.-) R(.-.) I(..) S(...)
 *
 * time_unit_ms = 1200 / wpm; duration = units * time_unit_ms. Upstream ends
 * every letter (including the last) with a letter-space rather than a
 * word-space, so "PARIS" measures 46 units here, not the classic 50.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_morse_tx.hpp"

#include "../dsp/modulate.hpp"

#include <complex>
#include <string>
#include <vector>

using namespace app::morse_tx;

/* --- encoder --------------------------------------------------------------- */

TEST(morse_encodes_single_letters) {
    std::vector<uint8_t> s;
    CHECK_EQ(morse_encode("E", s), size_t{2});  // dot + letter-space
    CHECK_EQ(s, (std::vector<uint8_t>{DOT, LETTER_SPACE}));

    s.clear();
    CHECK_EQ(morse_encode("T", s), size_t{2});  // dash + letter-space
    CHECK_EQ(s, (std::vector<uint8_t>{DASH, LETTER_SPACE}));

    s.clear();
    CHECK_EQ(morse_encode("A", s), size_t{4});  // .-  -> dot ss dash ls
    CHECK_EQ(s, (std::vector<uint8_t>{DOT, SYMBOL_SPACE, DASH, LETTER_SPACE}));
}

TEST(morse_encodes_sos) {
    std::vector<uint8_t> s;
    const size_t n = morse_encode("SOS", s);
    CHECK_EQ(n, size_t{18});

    const std::vector<uint8_t> want{
        DOT, SYMBOL_SPACE, DOT, SYMBOL_SPACE, DOT, LETTER_SPACE,       // S
        DASH, SYMBOL_SPACE, DASH, SYMBOL_SPACE, DASH, LETTER_SPACE,    // O
        DOT, SYMBOL_SPACE, DOT, SYMBOL_SPACE, DOT, LETTER_SPACE};      // S
    CHECK_EQ(s, want);

    /* Units: S=8, O=14, S=8 = 30. */
    CHECK_EQ(morse_time_units(s), uint32_t{30});
}

TEST(morse_encodes_paris) {
    std::vector<uint8_t> s;
    const size_t n = morse_encode("PARIS", s);
    CHECK_EQ(n, size_t{28});

    const std::vector<uint8_t> want{
        DOT, SYMBOL_SPACE, DASH, SYMBOL_SPACE, DASH, SYMBOL_SPACE, DOT, LETTER_SPACE,  // P .--.
        DOT, SYMBOL_SPACE, DASH, LETTER_SPACE,                                         // A .-
        DOT, SYMBOL_SPACE, DASH, SYMBOL_SPACE, DOT, LETTER_SPACE,                      // R .-.
        DOT, SYMBOL_SPACE, DOT, LETTER_SPACE,                                          // I ..
        DOT, SYMBOL_SPACE, DOT, SYMBOL_SPACE, DOT, LETTER_SPACE};                      // S ...
    CHECK_EQ(s, want);

    /* Units: P=14, A=8, R=10, I=6, S=8 = 46. */
    CHECK_EQ(morse_time_units(s), uint32_t{46});
}

TEST(morse_word_space_between_words) {
    std::vector<uint8_t> s;
    /* "E E": dot, then the space promotes the letter-space to a word-space, then
     * dot + letter-space. */
    CHECK_EQ(morse_encode("E E", s), size_t{4});
    CHECK_EQ(s, (std::vector<uint8_t>{DOT, WORD_SPACE, DOT, LETTER_SPACE}));
    CHECK_EQ(morse_time_units(s), uint32_t{1 + 7 + 1 + 3});
}

TEST(morse_empty_message_is_zero) {
    std::vector<uint8_t> s;
    CHECK_EQ(morse_encode("", s), size_t{0});
    CHECK(s.empty());
}

TEST(morse_over_long_message_returns_zero) {
    /* 60 H's = 60*8 = 480 symbols, well over the 256 buffer. */
    std::vector<uint8_t> s;
    CHECK_EQ(morse_encode(std::string(60, 'H'), s), size_t{0});
    CHECK(s.empty());
}

/* --- timing ---------------------------------------------------------------- */

TEST(morse_time_unit_ms_is_paris_standard) {
    CHECK_EQ(morse_time_unit_ms(15), uint32_t{80});   // 1200 / 15
    CHECK_EQ(morse_time_unit_ms(20), uint32_t{60});
    CHECK_EQ(morse_time_unit_ms(12), uint32_t{100});
    CHECK_EQ(morse_time_unit_ms(0), uint32_t{0});      // guard
}

TEST(morse_sos_duration_at_15_wpm) {
    std::vector<uint8_t> s;
    morse_encode("SOS", s);
    const uint32_t ms = morse_time_units(s) * morse_time_unit_ms(15);
    CHECK_EQ(ms, uint32_t{30 * 80});  // 2400 ms
}

/* --- keying envelope ------------------------------------------------------- */

TEST(morse_expand_units_for_sos) {
    std::vector<uint8_t> s;
    morse_encode("SOS", s);
    const auto onoff = morse_expand_units(s);

    CHECK_EQ(onoff.size(), size_t{30});
    /* First letter S = dot ss dot ss dot ls = on off on off on off off off. */
    const std::vector<uint8_t> first_s{1, 0, 1, 0, 1, 0, 0, 0};
    for (size_t i = 0; i < first_s.size(); ++i) CHECK_EQ(onoff[i], first_s[i]);
}

TEST(morse_expand_units_for_E) {
    std::vector<uint8_t> s;
    morse_encode("E", s);
    const auto onoff = morse_expand_units(s);
    /* dot (1 unit on) + letter-space (3 units off). */
    CHECK_EQ(onoff, (std::vector<uint8_t>{1, 0, 0, 0}));
}

/* --- CW (OOK) rendering ---------------------------------------------------- */

TEST(morse_cw_ook_keys_carrier_on_and_off) {
    /* Render "E" (dot then 3 off units) at 15 wpm and check the carrier is on
     * during the dot and off during the following space. */
    constexpr double fs = 48000.0;
    std::vector<uint8_t> s;
    morse_encode("E", s);
    const auto onoff = morse_expand_units(s);
    const auto packed = pack_bits_msb(onoff);

    const double unit_ms = morse_time_unit_ms(15);       // 80 ms
    const double units_per_sec = 1000.0 / unit_ms;       // 12.5
    const double sps = fs / units_per_sec;               // 3840 samples/unit

    dsp::OokKeyer keyer;
    keyer.configure(static_cast<float>(fs), static_cast<float>(units_per_sec));
    keyer.set_data(packed.data(), onoff.size());

    std::vector<dsp::cfloat> out(keyer.total_samples());
    size_t got = 0;
    while (got < out.size() && !keyer.done())
        got += keyer.process(out.data() + got, out.size() - got);

    CHECK(out.size() >= static_cast<size_t>(sps * 3));
    /* Mid-dot: carrier present. */
    CHECK_NEAR(std::abs(out[static_cast<size_t>(sps * 0.5)]), 1.0, 1e-5);
    /* Well into the letter-space: carrier off. */
    CHECK_NEAR(std::abs(out[static_cast<size_t>(sps * 2.5)]), 0.0, 1e-5);
}
