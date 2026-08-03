/*
 * mayhem-b200 — TouchTunes OOK encoder tests.
 *
 * The frame word and the OOK fragment stream are the deliverable, checked
 * against the worked examples in the upstream ui_touchtunes.hpp header:
 * PIN 0 / On/Off gives frame 0x5D007887 and a fragment stream that begins with
 * the "S L S L L L S L" pattern of the 0x5D sync byte.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "../src/apps/ui_touchtunes.hpp"

#include <string>

using namespace mb200test;
namespace tt = app::touchtunes;

TEST(touchtunes_button_codes) {
    CHECK_EQ(tt::button_code(0), 0x32u);  /* Pause */
    CHECK_EQ(tt::button_code(1), 0x78u);  /* On/Off */
    CHECK_EQ(tt::button_code(2), 0x70u);  /* P1 */
    CHECK_EQ(tt::button_code(31), 0x40u); /* Z3 Vol- */
}

TEST(touchtunes_frame_word_pin0_onoff) {
    /* Upstream: sync 0x5D, PIN 0, code 0x78 then complement 0x87. */
    CHECK_EQ(tt::frame_word(0, 1), 0x5D007887u);
}

TEST(touchtunes_frame_word_pin_and_complement) {
    /* PIN 255 fills the 8-bit PIN field; P1 code 0x70, complement 0x8F. */
    CHECK_EQ(tt::frame_word(255, 2), 0x5DFF708Fu);

    /* The low byte is always the one's complement of the code byte. */
    for (size_t i = 0; i < tt::kButtonCount; i++) {
        const uint32_t f = tt::frame_word(0, i);
        const uint8_t code = static_cast<uint8_t>((f >> 8) & 0xFF);
        const uint8_t comp = static_cast<uint8_t>(f & 0xFF);
        CHECK_EQ(static_cast<uint8_t>(code ^ 0xFF), comp);
        CHECK_EQ(code, tt::button_code(i));
    }
}

TEST(touchtunes_pin_is_bit_reversed) {
    /* PIN inserted least significant bit first: PIN 0x01 lands in the PIN
     * field's MSB, i.e. 0x80. */
    const uint32_t f = tt::frame_word(0x01, 1);
    const uint8_t pin_field = static_cast<uint8_t>((f >> 16) & 0xFF);
    CHECK_EQ(pin_field, 0x80u);
}

TEST(touchtunes_fragments_full_stream) {
    const std::string expected =
        "11111111111111110000000010100010100010001000101000101010101010101010"
        "10001000100010001010101000101010101000100010001000";
    /* 24-char lead-in + 90-char data + 4-char terminator = 118 symbols. */
    CHECK_EQ(expected.size(), size_t{118});
    CHECK_STR_EQ(tt::build_fragments(0, 1), expected);
}

TEST(touchtunes_fragments_structure) {
    const std::string frag = tt::build_fragments(123, 5);
    /* Lead-in: 16 ones then 8 zeros. */
    CHECK_STR_EQ(frag.substr(0, 24), "111111111111111100000000");
    /* Terminating pulse. */
    CHECK_STR_EQ(frag.substr(frag.size() - 4), "1000");
    /* Only OOK symbols. */
    for (char c : frag) CHECK(c == '0' || c == '1');
}
