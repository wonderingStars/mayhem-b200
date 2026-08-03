/*
 * mayhem-b200 — OOK Brute tests.
 *
 * The hardware-free deliverable is generate_packet(): for each supported
 * remote protocol it must produce the exact framed fragment string and the
 * exact symbol timing a real fob does. Every expected value is built here from
 * the protocol template documented in the upstream OOKBruteView::generate_packet
 * (preamble length, start bit, per-bit zero/one patterns, trailer, and the
 * samples_per_bit expression), not from the code under test.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "modulate.hpp"      /* dsp::bit_at */
#include "ui_ookbrute.hpp"   /* app::ookbrute::* */

#include <string>
#include <vector>

using namespace app::ookbrute;

/* --- protocol geometry ------------------------------------------------------ */

TEST(ookbrute_protocol_bits_and_max_code) {
    CHECK_EQ(protocol_bits(Came12), uint8_t{12});
    CHECK_EQ(protocol_bits(Came24), uint8_t{24});
    CHECK_EQ(protocol_bits(Nice12), uint8_t{12});
    CHECK_EQ(protocol_bits(Nice24), uint8_t{24});
    CHECK_EQ(protocol_bits(Holtek12), uint8_t{12});
    CHECK_EQ(protocol_bits(Princeton24), uint8_t{24});

    CHECK_EQ(protocol_max_code(Came12), uint32_t{4095});
    CHECK_EQ(protocol_max_code(Came24), uint32_t{16777215});
    CHECK_EQ(protocol_max_code(Princeton24), uint32_t{16777215});
}

/* --- samples per bit (verbatim upstream expressions) ------------------------ */

TEST(ookbrute_samples_per_bit) {
    CHECK_EQ(generate_packet(Came12, 0).samples_per_bit, uint32_t{760});       // 2.28M / 3000
    CHECK_EQ(generate_packet(Came24, 0).samples_per_bit, uint32_t{760});
    CHECK_EQ(generate_packet(Nice12, 0).samples_per_bit, uint32_t{1550});      // 2.28M * 680us
    CHECK_EQ(generate_packet(Nice24, 0).samples_per_bit, uint32_t{1550});
    CHECK_EQ(generate_packet(Holtek12, 0).samples_per_bit, uint32_t{889});     // 2.28M * 390us
    CHECK_EQ(generate_packet(Princeton24, 0).samples_per_bit, uint32_t{1026}); // 2.28M * 450us
}

TEST(ookbrute_repeat_counts) {
    CHECK_EQ(generate_packet(Came12, 0).repeat, uint16_t{2});
    CHECK_EQ(generate_packet(Princeton24, 0).repeat, uint16_t{6});
}

/* --- exact frames ----------------------------------------------------------- */

TEST(ookbrute_came12_code0) {
    /* 36-zero preamble + start bit + 12 "zero" fragments (011) + 4-zero trailer. */
    std::string expected = std::string(36, '0') + "1";
    for (int i = 0; i < 12; i++) expected += "011";
    expected += "0000";

    const auto pkt = generate_packet(Came12, 0);
    CHECK_STR_EQ(pkt.fragments, expected);
    CHECK_EQ(pkt.fragments.size(), size_t{36 + 1 + 36 + 4});
    CHECK_EQ(pkt.databits, uint16_t{12});
}

TEST(ookbrute_came12_code1_bit_order_is_msb_first) {
    /* code == 1 sets only the least-significant bit, which is the LAST data
     * symbol, so 11 "zero" fragments then one "one" (001). */
    std::string expected = std::string(36, '0') + "1";
    for (int i = 0; i < 11; i++) expected += "011";
    expected += "001";
    expected += "0000";

    CHECK_STR_EQ(generate_packet(Came12, 1).fragments, expected);
}

TEST(ookbrute_came12_code_msb) {
    /* Setting bit 11 (value 0x800) sets the FIRST data symbol. */
    std::string expected = std::string(36, '0') + "1";
    expected += "001";  // first C is a "one"
    for (int i = 0; i < 11; i++) expected += "011";
    expected += "0000";

    CHECK_STR_EQ(generate_packet(Came12, 0x800).fragments, expected);
}

TEST(ookbrute_princeton24_code0) {
    /* 36-zero preamble + 24 "zero" fragments (1000) + trailer "10000000". */
    std::string expected = std::string(36, '0');
    for (int i = 0; i < 24; i++) expected += "1000";
    expected += "10000000";

    const auto pkt = generate_packet(Princeton24, 0);
    CHECK_STR_EQ(pkt.fragments, expected);
    CHECK_EQ(pkt.fragments.size(), size_t{36 + 96 + 8});
    CHECK_EQ(pkt.databits, uint16_t{24});
}

/* --- packing ---------------------------------------------------------------- */

TEST(ookbrute_pack_round_trips) {
    const auto pkt = generate_packet(Came12, 0xABC);
    std::vector<uint8_t> out;
    const size_t bits = pack_fragments(pkt.fragments, out);
    CHECK_EQ(bits, pkt.fragments.size());
    for (size_t i = 0; i < bits; i++)
        CHECK_EQ(dsp::bit_at(out.data(), i), pkt.fragments[i] == '1');
}

TEST(ookbrute_pack_empty) {
    std::vector<uint8_t> out;
    CHECK_EQ(pack_fragments("", out), size_t{0});
    CHECK_EQ(out.size(), size_t{0});
}
