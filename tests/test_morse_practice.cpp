/*
 * mayhem-b200 — Morse Practice decode + scoring tests.
 *
 * Expected values come from the upstream decoder
 * (firmware/application/external/morse_practice/morsedecoder.hpp), not from this
 * port's own output:
 *   - default time unit 119 ms, so the inter-element / inter-char / inter-word
 *     thresholds are 0.8 / 2.5 / 6.0 times that (95.2 / 297.5 / 714 ms);
 *   - a pulse is a dah when it exceeds 2.5*unit, a dit at or below 1.5*unit, and
 *     is interpolated (with a confidence) in between;
 *   - the adaptive timing learner only engages once at least 10 pulses AND 10
 *     gaps have been seen, so short attempts decode against the fixed 119 ms
 *     unit and are fully deterministic;
 *   - scoring (score_color_id) mirrors writeCharToConsole: 0 white for a space
 *     or a no-match, then 1/2/3 for confidence <0.8 / <0.9 / else.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_morse_practice.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace app::morse;
using namespace mb200test;

namespace {

/* Feed a list of keying events (positive = key-down ms, negative = key-up ms)
 * and collect every decoded token together with the colour it would score. */
struct Attempt {
    std::string text{};
    std::vector<uint8_t> colors{};
};

Attempt run(MorseDecoder& d, const std::vector<int32_t>& events) {
    Attempt a;
    for (int32_t e : events) {
        const auto r = d.handleInput(e);
        if (r.isValid()) {
            a.text += r.text;
            a.colors.push_back(score_color_id(r.text, r.confidence));
        }
    }
    return a;
}

}  // namespace

/* ---- thresholds at the default 119 ms unit -------------------------------- */

TEST(morse_default_thresholds) {
    MorseDecoder d;
    CHECK_NEAR(d.getCurrentTimeUnit(), 119.0, 1e-9);
    CHECK_NEAR(d.getInterElementThreshold(), 95.2, 1e-9);
    CHECK_NEAR(d.getInterCharThreshold(), 297.5, 1e-9);
    CHECK_NEAR(d.getInterWordThreshold(), 714.0, 1e-9);
}

/* ---- the Morse table, both directions ------------------------------------- */

TEST(morse_table_decode) {
    MorseDecoder d;
    CHECK_STR_EQ(d.decode(".-"), "A");
    CHECK_STR_EQ(d.decode("..."), "S");
    CHECK_STR_EQ(d.decode("---"), "O");
    CHECK_STR_EQ(d.decode("-----"), "0");
    CHECK_STR_EQ(d.decode(".----"), "1");
    CHECK_STR_EQ(d.decode(".-.-.-"), ".");
    CHECK_STR_EQ(d.decode("..--.."), "?");
}

TEST(morse_table_decode_unknown_is_bracketed) {
    MorseDecoder d;
    CHECK_STR_EQ(d.decode("........"), "{........}");
    CHECK_STR_EQ(d.decode("-.-.-.-."), "{-.-.-.-.}");
}

TEST(morse_table_encode) {
    MorseDecoder d;
    CHECK_STR_EQ(d.encode('A'), ".-");
    CHECK_STR_EQ(d.encode('S'), "...");
    CHECK_STR_EQ(d.encode('O'), "---");
    CHECK_STR_EQ(d.encode('0'), "-----");
    CHECK_STR_EQ(d.encode('?'), "..--..");
}

TEST(morse_table_encode_is_case_insensitive) {
    MorseDecoder d;
    CHECK_STR_EQ(d.encode('s'), "...");
    CHECK_STR_EQ(d.encode('z'), "--..");
}

TEST(morse_table_encode_unknown_is_empty) {
    MorseDecoder d;
    CHECK_STR_EQ(d.encode('#'), "");
    CHECK_STR_EQ(d.encode(' '), "");
}

/* ---- scoring (confidence -> colour id) ------------------------------------ */

TEST(morse_score_space_and_nomatch_are_white) {
    CHECK_EQ(score_color_id(" ", 1.0), (uint8_t)0);
    CHECK_EQ(score_color_id("{..--}", 0.95), (uint8_t)0);
    CHECK_EQ(score_color_id("", 0.99), (uint8_t)0);
}

TEST(morse_score_confidence_bands) {
    CHECK_EQ(score_color_id("A", 0.79), (uint8_t)1);  /* weak: red    */
    CHECK_EQ(score_color_id("A", 0.85), (uint8_t)2);  /* fair: yellow */
    CHECK_EQ(score_color_id("A", 0.95), (uint8_t)3);  /* good: green  */
}

TEST(morse_score_band_edges) {
    CHECK_EQ(score_color_id("A", 0.80), (uint8_t)2);  /* 0.8 is not < 0.8 */
    CHECK_EQ(score_color_id("A", 0.90), (uint8_t)3);  /* 0.9 is not < 0.9 */
}

/* ---- decoding a whole keyed attempt --------------------------------------- */

TEST(morse_decode_single_dit_is_E) {
    MorseDecoder d;
    /* one dit (119 ms) then a full inter-char gap (300 ms >= 297.5). */
    const auto a = run(d, {119, -300});
    CHECK_STR_EQ(a.text, "E");
    CHECK_EQ(a.colors.size(), (size_t)1);
    CHECK_EQ(a.colors[0], (uint8_t)3);  /* a clean dit is high confidence */
}

TEST(morse_decode_letter_A_dit_dah) {
    MorseDecoder d;
    /* '.' (119), intra-char gap (100 < 297.5, no decode), '-' (357), gap (300). */
    const auto a = run(d, {119, -100, 357, -300});
    CHECK_STR_EQ(a.text, "A");
    CHECK_EQ(a.colors.size(), (size_t)1);
    CHECK_EQ(a.colors[0], (uint8_t)3);
}

TEST(morse_decode_word_SOS_all_green) {
    MorseDecoder d;
    const std::vector<int32_t> events = {
        119, -100, 119, -100, 119, -300,  /* S */
        357, -100, 357, -100, 357, -300,  /* O */
        119, -100, 119, -100, 119, -300,  /* S */
    };
    const auto a = run(d, events);
    CHECK_STR_EQ(a.text, "SOS");
    CHECK_EQ(a.colors.size(), (size_t)3);
    for (uint8_t c : a.colors) CHECK_EQ(c, (uint8_t)3);
}

TEST(morse_decode_ambiguous_length_scores_low) {
    MorseDecoder d;
    /* 238 ms == 2.0*unit, exactly the midpoint of the dit/dah ramp: dah_prob
     * 0.5, which the decoder treats as a dit with confidence 0.5 -> red. */
    const auto a = run(d, {238, -300});
    CHECK_STR_EQ(a.text, "E");
    CHECK_EQ(a.colors.size(), (size_t)1);
    CHECK_EQ(a.colors[0], (uint8_t)1);
}

TEST(morse_inter_word_gap_appends_space) {
    MorseDecoder d;
    /* one dit then a gap past the inter-word threshold (800 >= 714). */
    const auto a = run(d, {119, -800});
    CHECK_STR_EQ(a.text, "E ");
}

TEST(morse_tiny_events_are_ignored) {
    MorseDecoder d;
    /* |duration| < 5 ms is debounced away, so nothing decodes. */
    const auto a = run(d, {3, -2, 119, -300});
    CHECK_STR_EQ(a.text, "E");
    CHECK_EQ(a.colors.size(), (size_t)1);
}
