/*
 * mayhem-b200 — AFSK receive terminal decode tests.
 *
 * Covers the word-to-character transform (against upstream's hard-coded 8-bit
 * reversal, over every input), the RS232-like word framer and its value-
 * triggered sibling, and the whole Bell 202 chain from a modulated signal back
 * to characters.
 *
 * No radio is attached; nothing here proves reception off the air.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_afsk_rx.hpp"

#include "../src/dsp/demod_digital.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace app;

namespace {

constexpr double kPi = 3.14159265358979323846;

/* Build the on-air bits of one asynchronous character: a start bit (0), the
 * data bits, an optional parity bit and the stop bits (1). This is the
 * transmit side of modems.cpp generate_data(). */
void push_uart_char(std::vector<uint8_t>& bits,
                    uint8_t ch,
                    const AfskSerialFormat& fmt) {
    bits.push_back(0); /* start */

    uint8_t ones = 0;
    if (fmt.bit_order == AfskSerialBitOrder::LsbFirst) {
        for (uint8_t i = 0; i < fmt.data_bits; i++) {
            const uint8_t b = static_cast<uint8_t>((ch >> i) & 1);
            ones = static_cast<uint8_t>(ones + b);
            bits.push_back(b);
        }
    } else {
        for (int i = fmt.data_bits - 1; i >= 0; i--) {
            const uint8_t b = static_cast<uint8_t>((ch >> i) & 1);
            ones = static_cast<uint8_t>(ones + b);
            bits.push_back(b);
        }
    }

    if (fmt.has_parity()) {
        const uint8_t init = (fmt.parity == AfskSerialParity::Odd) ? 1u : 0u;
        bits.push_back(static_cast<uint8_t>((ones + init) & 1));
    }

    for (uint8_t i = 0; i < fmt.stop_bits; i++) bits.push_back(1);
}

void push_uart_string(std::vector<uint8_t>& bits,
                      const std::string& text,
                      const AfskSerialFormat& fmt) {
    for (const char c : text) push_uart_char(bits, static_cast<uint8_t>(c), fmt);
}

}  // namespace

/* ===========================================================================
 * Word -> character
 * ===========================================================================*/

TEST(afsk_display_value_reverses_eight_bits_and_drops_parity) {
    /* 'A' is 0x41 = 100 0001, two bits set, so an even-parity line sends a 0
     * parity bit. LSB first that is 1,0,0,0,0,0,1,0; the framer shifts them in
     * most-significant-first, so the word is 0x82. */
    CHECK_EQ(afsk_display_value(0x82), 0x41u);

    /* 'F' is 0x46 = 100 0110, three bits set, so even parity sends a 1.
     * LSB first: 0,1,1,0,0,0,1,1 -> word 0x63. */
    CHECK_EQ(afsk_display_value(0x63), 0x46u);

    CHECK_EQ(afsk_display_value(0x00), 0x00u);
    /* 0x01 reverses to 0x80, whose only set bit is the parity bit. */
    CHECK_EQ(afsk_display_value(0x01), 0x00u);
    CHECK_EQ(afsk_display_value(0x02), 0x40u);
    /* Bits above the word are masked off before anything else happens. */
    CHECK_EQ(afsk_display_value(0xFF82u), 0x41u);
}

TEST(afsk_deframe_word_matches_upstream_for_every_byte) {
    /* The general form must be bit-for-bit upstream's hard-coded transform for
     * upstream's own configuration: seven data bits, a parity bit, LSB first. */
    for (uint32_t v = 0; v < 256; v++) {
        const uint32_t general = afsk_deframe_word(v, 7, true, AfskSerialBitOrder::LsbFirst);
        const uint32_t upstream = afsk_display_value(v);
        CHECK_EQ(general, upstream);
        if (general != upstream) break;
    }
}

TEST(afsk_deframe_word_handles_msb_first_and_no_parity) {
    /* The inputs go through mutable locals so the whole comparison is not a
     * compile-time constant, which /W4 reports as a constant conditional. */
    auto deframe = [](uint32_t word, uint8_t data_bits, bool parity, AfskSerialBitOrder order) {
        return afsk_deframe_word(word, data_bits, parity, order);
    };

    /* MSB first with parity: only the trailing parity bit is shifted off
     * (modems.cpp deframe_word). 'F' sent d6..d0 then parity 1 is
     * 1,0,0,0,1,1,0,1 -> word 0x8D. */
    uint32_t word = 0x8D;
    CHECK_EQ(deframe(word, 7, true, AfskSerialBitOrder::MsbFirst), 0x46u);

    /* MSB first without parity is the identity on the data bits. */
    word = 0x46;
    CHECK_EQ(deframe(word, 7, false, AfskSerialBitOrder::MsbFirst), 0x46u);

    /* Eight data bits, no parity, LSB first: a plain byte reversal. */
    const uint32_t inputs[] = {0x01, 0x80, 0xA5, 0x0F};
    const uint32_t expected[] = {0x80, 0x01, 0xA5, 0xF0};
    for (size_t i = 0; i < 4; i++)
        CHECK_EQ(deframe(inputs[i], 8, false, AfskSerialBitOrder::LsbFirst), expected[i]);
}

TEST(afsk_reverse_bits_is_its_own_inverse) {
    for (uint32_t v = 0; v < 256; v++)
        CHECK_EQ(afsk_reverse_bits(afsk_reverse_bits(v, 8), 8), v);
}

TEST(afsk_format_byte_prints_printables_and_escapes_the_rest) {
    CHECK_STR_EQ(afsk_format_byte('A'), "A");
    CHECK_STR_EQ(afsk_format_byte(' '), " ");
    CHECK_STR_EQ(afsk_format_byte('~'), "~");
    CHECK_STR_EQ(afsk_format_byte(0x00), "[00]");
    CHECK_STR_EQ(afsk_format_byte(0x0A), "[0A]");
    CHECK_STR_EQ(afsk_format_byte(0x7F), "[7F]");
}

TEST(afsk_serial_format_word_length_matches_upstreams_hard_coded_eight) {
    /* afsk_rx configures 7 data bits with even parity and then asks the
     * baseband for eight-bit words. */
    AfskSerialFormat fmt{7, AfskSerialParity::Even, 1, AfskSerialBitOrder::LsbFirst};
    CHECK_EQ(static_cast<int>(fmt.word_length()), 8);
    CHECK(fmt.has_parity());

    fmt.parity = AfskSerialParity::None;
    CHECK_EQ(static_cast<int>(fmt.word_length()), 7);
    CHECK(!fmt.has_parity());
}

TEST(afsk_modem_presets_match_upstream) {
    /* modem_defs[] from modems.hpp, entry for entry. Walked through a runtime
     * index so the comparisons are not compile-time constants. */
    struct Expected {
        const char* name;
        int mark;
        int space;
        int baud;
        AfskModulation modulation;
    };
    const Expected expected[] = {
        {"Bell202", 1200, 2200, 1200, AfskModulation::Afsk},
        {"Bell103", 1270, 1070, 300, AfskModulation::Afsk},
        {"V21", 980, 1180, 300, AfskModulation::Afsk},
        {"V23 M1", 1300, 1700, 600, AfskModulation::Afsk},
        {"V23 M2", 1300, 2100, 1200, AfskModulation::Afsk},
        {"RTTY US", 2295, 2125, 45, AfskModulation::Am},
        {"RTTY EU", 2125, 1955, 45, AfskModulation::Am},
    };

    static_assert(kAfskModemDefs.size() == kAfskModemDefCount, "preset table size");
    static_assert(sizeof(expected) / sizeof(expected[0]) == kAfskModemDefCount,
                  "expectation table size");

    for (size_t i = 0; i < kAfskModemDefCount; i++) {
        const AfskModemDef& def = kAfskModemDefs[i];
        CHECK_STR_EQ(def.name, expected[i].name);
        CHECK_EQ(static_cast<int>(def.mark_freq), expected[i].mark);
        CHECK_EQ(static_cast<int>(def.space_freq), expected[i].space);
        CHECK_EQ(static_cast<int>(def.baudrate), expected[i].baud);
        CHECK(def.modulation == expected[i].modulation);
    }
}

/* ===========================================================================
 * Word framer
 * ===========================================================================*/

TEST(afsk_word_framer_assembles_a_known_uart_frame) {
    const AfskSerialFormat fmt{7, AfskSerialParity::Even, 1, AfskSerialBitOrder::LsbFirst};

    std::vector<uint8_t> bits;
    /* Idle line, then two characters. */
    for (int i = 0; i < 8; i++) bits.push_back(1);
    push_uart_string(bits, "AF", fmt);

    std::vector<uint32_t> words;
    AfskWordFramer framer;
    framer.configure(fmt.word_length(), false, 0);
    framer.on_word = [&](uint32_t v) { words.push_back(v); };
    framer.feed_bits(bits);

    CHECK_EQ(words.size(), size_t{2});
    if (words.size() < 2) return;

    CHECK_EQ(words[0] & 0xFFu, 0x82u); /* 'A' with an even-parity 0 */
    CHECK_EQ(words[1] & 0xFFu, 0x63u); /* 'F' with an even-parity 1 */

    CHECK_EQ(afsk_deframe_word(words[0], 7, true, fmt.bit_order), static_cast<uint32_t>('A'));
    CHECK_EQ(afsk_deframe_word(words[1], 7, true, fmt.bit_order), static_cast<uint32_t>('F'));
}

TEST(afsk_word_framer_decodes_a_whole_string) {
    const AfskSerialFormat fmt{7, AfskSerialParity::Even, 1, AfskSerialBitOrder::LsbFirst};
    const std::string text = "MB200 ACARS/AFSK ok!";

    std::vector<uint8_t> bits;
    for (int i = 0; i < 4; i++) bits.push_back(1);
    push_uart_string(bits, text, fmt);

    std::string out;
    AfskWordFramer framer;
    framer.configure(fmt.word_length(), false, 0);
    framer.on_word = [&](uint32_t v) {
        out += afsk_format_byte(afsk_deframe_word(v, fmt.data_bits, fmt.has_parity(),
                                                  fmt.bit_order));
    };
    framer.feed_bits(bits);

    CHECK_STR_EQ(out, text);
}

TEST(afsk_word_framer_decodes_an_msb_first_line) {
    const AfskSerialFormat fmt{7, AfskSerialParity::Odd, 1, AfskSerialBitOrder::MsbFirst};
    const std::string text = "MSB first";

    std::vector<uint8_t> bits;
    for (int i = 0; i < 4; i++) bits.push_back(1);
    push_uart_string(bits, text, fmt);

    std::string out;
    AfskWordFramer framer;
    framer.configure(fmt.word_length(), false, 0);
    framer.on_word = [&](uint32_t v) {
        out += static_cast<char>(
            afsk_deframe_word(v, fmt.data_bits, fmt.has_parity(), fmt.bit_order));
    };
    framer.feed_bits(bits);

    CHECK_STR_EQ(out, text);
}

TEST(afsk_word_framer_waits_for_a_stop_bit_before_the_next_start) {
    /* Once a word is complete the framer sits in WAIT_STOP until the line goes
     * high. A stuck-low line where the stop bit should be must not be mistaken
     * for the next start bit — upstream's guard against one framing error
     * cascading into every following character. */
    const AfskSerialFormat fmt{7, AfskSerialParity::Even, 1, AfskSerialBitOrder::LsbFirst};
    const AfskSerialFormat no_stop{7, AfskSerialParity::Even, 0, AfskSerialBitOrder::LsbFirst};

    std::vector<uint8_t> bits;
    bits.push_back(1);                    /* idle                              */
    push_uart_char(bits, 'A', no_stop);   /* start + eight bits, no stop bit    */
    const size_t stuck_low_start = bits.size();
    for (int i = 0; i < 5; i++) bits.push_back(0); /* line stuck low            */
    bits.push_back(1);                    /* the stop bit finally arrives      */
    push_uart_char(bits, 'F', fmt);       /* a clean frame after the recovery  */

    std::vector<uint32_t> words;
    std::vector<size_t> word_positions;
    size_t fed = 0;

    AfskWordFramer framer;
    framer.configure(fmt.word_length(), false, 0);
    framer.on_word = [&](uint32_t v) {
        words.push_back(v & 0xFFu);
        word_positions.push_back(fed);
    };
    for (const uint8_t b : bits) {
        fed++;
        framer.feed_bit(b);
    }

    CHECK_EQ(words.size(), size_t{2});
    if (words.size() < 2) return;
    CHECK_EQ(words[0], 0x82u); /* 'A' */
    CHECK_EQ(words[1], 0x63u); /* 'F' */

    /* Nothing was emitted while the line was low. */
    CHECK(word_positions[0] <= stuck_low_start);
    CHECK(word_positions[1] > stuck_low_start + 6);
}

TEST(afsk_word_framer_ignores_an_idle_line) {
    AfskWordFramer framer;
    framer.configure(8, false, 0);
    size_t emitted = 0;
    framer.on_word = [&](uint32_t) { emitted++; };

    for (int i = 0; i < 200; i++) framer.feed_bit(1);
    CHECK_EQ(emitted, size_t{0});
    CHECK(framer.state() == AfskWordFramer::State::WaitStart);
}

TEST(afsk_word_framer_trigger_mode_starts_on_the_sync_value) {
    /* Upstream's continuous value-triggered path: nothing is emitted until the
     * trigger value appears in the shift register, then every word_length bits
     * produce a word. */
    AfskWordFramer framer;
    framer.configure(8, true, 0x7E);

    std::vector<uint32_t> words;
    framer.on_word = [&](uint32_t v) { words.push_back(v); };

    /* Sixteen zeros, then the flag, then two known bytes. */
    for (int i = 0; i < 16; i++) framer.feed_bit(0);
    const uint8_t stream[] = {0x7E, 0xA5, 0x3C};
    for (const uint8_t byte : stream)
        for (int i = 7; i >= 0; i--) framer.feed_bit(static_cast<uint8_t>((byte >> i) & 1));

    CHECK(framer.triggered());
    CHECK_EQ(words.size(), size_t{3});
    if (words.size() < 3) return;
    CHECK_EQ(words[0], 0x7Eu);
    CHECK_EQ(words[1], 0xA5u);
    CHECK_EQ(words[2], 0x3Cu);
}

TEST(afsk_word_framer_reset_returns_to_hunting) {
    AfskWordFramer framer;
    framer.configure(8, false, 0);
    framer.feed_bit(0); /* start bit consumed */
    CHECK(framer.state() == AfskWordFramer::State::Receive);
    framer.reset();
    CHECK(framer.state() == AfskWordFramer::State::WaitStart);
    CHECK_EQ(framer.words_emitted(), size_t{0});
}

/* ===========================================================================
 * Bell 202 signal chain
 * ===========================================================================*/

TEST(afsk_bell202_audio_decodes_known_characters) {
    constexpr float fs = 24000.0f;
    const AfskSerialFormat fmt{7, AfskSerialParity::Even, 1, AfskSerialBitOrder::LsbFirst};
    const std::string text = "MB200 AFSK OK";

    /* Lead in with DEL characters: a real framed pattern with a transition
     * every character, which is what the correlator and the bit clock need to
     * settle. It is also upstream's message separator. The same characters
     * follow the text, because the modulated signal stops dead on the last bit
     * and the demodulator needs a bit period beyond it to emit that bit. */
    std::vector<uint8_t> bits;
    for (int i = 0; i < 8; i++) push_uart_char(bits, 0x7F, fmt);
    push_uart_string(bits, text, fmt);
    for (int i = 0; i < 2; i++) push_uart_char(bits, 0x7F, fmt);

    const auto& def = kAfskModemDefs[0];
    const auto audio = dsp::afsk_modulate(bits, fs,
                                          static_cast<float>(def.mark_freq),
                                          static_cast<float>(def.space_freq),
                                          static_cast<float>(def.baudrate), 0.8f);
    CHECK_EQ(audio.size(), bits.size() * 20);

    std::string out;
    AfskAudioDecoder decoder;
    decoder.configure(fs,
                      static_cast<float>(def.mark_freq),
                      static_cast<float>(def.space_freq),
                      static_cast<float>(def.baudrate),
                      fmt.word_length());
    decoder.framer().on_word = [&](uint32_t v) {
        out += afsk_format_byte(
            afsk_deframe_word(v, fmt.data_bits, fmt.has_parity(), fmt.bit_order));
    };
    decoder.process_audio(audio.data(), audio.size());

    CHECK(out.find(text) != std::string::npos);
}

TEST(afsk_bell202_over_fm_baseband_decodes_known_characters) {
    /* The way it arrives from the air: the tone pair on an FM carrier, which is
     * the complex signal the app hands the decoder after mixing and
     * decimation. */
    constexpr float fs = 24000.0f;
    const AfskSerialFormat fmt{7, AfskSerialParity::Even, 1, AfskSerialBitOrder::LsbFirst};
    const std::string text = "FM PATH";

    std::vector<uint8_t> bits;
    for (int i = 0; i < 8; i++) push_uart_char(bits, 0x7F, fmt);
    push_uart_string(bits, text, fmt);
    for (int i = 0; i < 2; i++) push_uart_char(bits, 0x7F, fmt);

    const auto audio = dsp::afsk_modulate(bits, fs, 1200.0f, 2200.0f, 1200.0f, 1.0f);

    std::vector<dsp::cfloat> baseband(audio.size());
    double phase = 0.0;
    for (size_t i = 0; i < audio.size(); i++) {
        baseband[i] = dsp::cfloat{static_cast<float>(std::cos(phase)),
                                  static_cast<float>(std::sin(phase))};
        phase += 2.0 * kPi * 3000.0 * static_cast<double>(audio[i]) / static_cast<double>(fs);
    }

    std::string out;
    AfskAudioDecoder decoder;
    decoder.configure(fs, 1200.0f, 2200.0f, 1200.0f, fmt.word_length());
    decoder.framer().on_word = [&](uint32_t v) {
        out += afsk_format_byte(
            afsk_deframe_word(v, fmt.data_bits, fmt.has_parity(), fmt.bit_order));
    };
    decoder.process_baseband(baseband.data(), baseband.size());

    CHECK(out.find(text) != std::string::npos);
}

TEST(afsk_bell103_audio_decodes_known_characters) {
    /* A second preset, to show the modem table is actually wired through. */
    constexpr float fs = 24000.0f;
    const AfskSerialFormat fmt{7, AfskSerialParity::Even, 1, AfskSerialBitOrder::LsbFirst};
    const std::string text = "B103";

    std::vector<uint8_t> bits;
    for (int i = 0; i < 6; i++) push_uart_char(bits, 0x7F, fmt);
    push_uart_string(bits, text, fmt);
    for (int i = 0; i < 2; i++) push_uart_char(bits, 0x7F, fmt);

    const auto& def = kAfskModemDefs[1];
    const auto audio = dsp::afsk_modulate(bits, fs,
                                          static_cast<float>(def.mark_freq),
                                          static_cast<float>(def.space_freq),
                                          static_cast<float>(def.baudrate), 0.8f);

    std::string out;
    AfskAudioDecoder decoder;
    decoder.configure(fs,
                      static_cast<float>(def.mark_freq),
                      static_cast<float>(def.space_freq),
                      static_cast<float>(def.baudrate),
                      fmt.word_length());
    decoder.framer().on_word = [&](uint32_t v) {
        out += afsk_format_byte(
            afsk_deframe_word(v, fmt.data_bits, fmt.has_parity(), fmt.bit_order));
    };
    decoder.process_audio(audio.data(), audio.size());

    CHECK(out.find(text) != std::string::npos);
}
