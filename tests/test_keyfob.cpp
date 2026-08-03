/*
 * mayhem-b200 — Key fob (Subaru) encoder tests.
 *
 * Checks the checksum, the frame assembly, and the OOK framing against values
 * derived by hand from firmware/application/external/keyfob/ui_keyfob.cpp.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_keyfob.hpp"

#include <string>

using namespace mb200test;

TEST(subaru_checksum_known) {
    /* {0x55, 0...}: nibbles of 0x55 are 5,5 -> 0; running XOR stays 0; ++ -> 1. */
    app::SubaruFrame f{};
    f[0] = 0x55;
    CHECK_EQ(app::subaru_checksum(f), uint8_t{1});

    /* {0x55, 0x01, 0x02, 0...}: XOR of nibbles = 0^1^2 = 3; ++ -> 4. */
    app::SubaruFrame g{};
    g[0] = 0x55;
    g[1] = 0x01;
    g[2] = 0x02;
    CHECK_EQ(app::subaru_checksum(g), uint8_t{4});
}

TEST(subaru_build_frame_layout) {
    /* half_a big-endian into frame[0..4], half_b into frame[5..9]. */
    const app::SubaruFrame f = app::subaru_build_frame(0x5511223344ull, 0x5566778899ull);
    CHECK_EQ(f[0], uint8_t{0x55});
    CHECK_EQ(f[1], uint8_t{0x11});
    CHECK_EQ(f[2], uint8_t{0x22});
    CHECK_EQ(f[3], uint8_t{0x33});
    CHECK_EQ(f[4], uint8_t{0x44});
    CHECK_EQ(f[5], uint8_t{0x55});
    CHECK_EQ(f[6], uint8_t{0x66});
    CHECK_EQ(f[7], uint8_t{0x77});
    CHECK_EQ(f[8], uint8_t{0x88});
    /* frame[9] high nibble from half_b (0x9), low nibble = checksum. */
    CHECK_EQ(f[9] >> 4, uint8_t{0x9});
    CHECK_EQ(f[9] & 0x0F, app::subaru_checksum(f));
    CHECK(app::subaru_is_valid(f));
}

TEST(subaru_build_frame_checksum_valid) {
    /* Default frame: header only. */
    const app::SubaruFrame f = app::subaru_build_frame(0x5500000000ull, 0x0000000000ull);
    CHECK_EQ(f[0], uint8_t{0x55});
    CHECK_EQ(f[9] & 0x0F, uint8_t{1});  // checksum of the 0x55-only frame is 1
    CHECK(app::subaru_is_valid(f));
}

TEST(subaru_set_command) {
    app::SubaruFrame f{};
    f[0] = 0x55;
    f[5] = 0xA0;
    f[6] = 0xB0;
    app::subaru_set_command(f, 2);  // Unlock
    CHECK_EQ(f[5], uint8_t{0xA2});
    CHECK_EQ(f[6], uint8_t{0xB2});
}

TEST(keyfob_bitstream_structure) {
    app::SubaruFrame f{};
    f[0] = 0x55;  // 0101 0101

    const std::string s = app::keyfob_encode_bitstream(f);

    /* 256 preamble + 4 space + 160 payload + 8 space + 160 payload. */
    CHECK_EQ(s.size(), size_t{588});

    std::string preamble;
    for (int i = 0; i < 128; ++i) preamble += "01";
    CHECK_STR_EQ(s.substr(0, 256), preamble);

    /* space after preamble. */
    CHECK_STR_EQ(s.substr(256, 4), std::string{"0000"});

    /* frame[0] = 0x55 -> bits 0,1,0,1,0,1,0,1 -> "01 10 01 10 01 10 01 10". */
    CHECK_STR_EQ(s.substr(260, 16), std::string{"0110011001100110"});

    /* The 8x space sits between the two payload copies. */
    CHECK_STR_EQ(s.substr(256 + 4 + 160, 8), std::string{"00000000"});

    /* The second payload copy repeats the first. */
    CHECK_STR_EQ(s.substr(256 + 4 + 160 + 8, 16), std::string{"0110011001100110"});
}
