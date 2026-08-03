/*
 * mayhem-b200 — BHT (Xhouse) encoder tests.
 *
 * Golden values are produced by the upstream gen_message_ep / gen_message_xy
 * (see scratchpad/verify_bht.cpp); the Xylos default case is also cross-checked
 * against a hand trace of the "repeat becomes E" substitution.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_bht_tx.hpp"

#include <array>
#include <cstdint>
#include <string>

using namespace mb200test;

TEST(bht_epar_um3750_fragments) {
    /* Sync "001" then 12 data symbols; a 0 bit is "011", a 1 bit "001". */
    CHECK_STR_EQ(app::bht_gen_message_ep(0, 0, 0, 0),
                 std::string{"001011011011011011011011011011011011011"});
    /* City 1 -> bit 0 set -> first data symbol "001". */
    CHECK_STR_EQ(app::bht_gen_message_ep(1, 0, 0, 0),
                 std::string{"001001011011011011011011011011011011011"});
    /* City 0xA5, group 2, relay# 1, relay-state 1. */
    CHECK_STR_EQ(app::bht_gen_message_ep(0xA5, 2, 1, 1),
                 std::string{"001001011001011011001011001001011001001"});
    /* Always 3 (sync) + 12*3 = 39 symbols. */
    CHECK_EQ(app::bht_gen_message_ep(0, 0, 0, 0).size(), size_t{39});
}

TEST(bht_xylos_default_message) {
    const auto msg = app::bht_gen_message_xy(0, 0, 10, 1, false, 1, false, 1, 0, 0, 0, 0);
    const std::array<uint8_t, 20> want = {0, 14, 0, 14, 1, 0, 1, 14, 0, 1,
                                          11, 0, 14, 0, 14, 11, 0, 14, 0, 14};
    for (size_t i = 0; i < 20; ++i) CHECK_EQ(msg[i], want[i]);
    CHECK_STR_EQ(app::bht_ccir_to_ascii(msg), std::string{"0E0E101E01B0E0EB0E0E"});
}

TEST(bht_xylos_wildcards_and_relays) {
    /* subfamily wildcard -> 0xA, id wildcard -> 0xA 0xA, relays 2/1/0/2. */
    const auto msg = app::bht_gen_message_xy(12, 34, 56, 7, true, 0, true, 0, 2, 1, 0, 2);
    const std::array<uint8_t, 20> want = {1, 2, 3, 4, 5, 6, 7, 10, 14, 10,
                                          11, 2, 1, 0, 2, 11, 0, 14, 0, 14};
    for (size_t i = 0; i < 20; ++i) CHECK_EQ(msg[i], want[i]);
    CHECK_STR_EQ(app::bht_ccir_to_ascii(msg), std::string{"1234567AEAB2102B0E0E"});
}

TEST(bht_ccir_ascii_mapping) {
    std::array<uint8_t, 20> msg{};
    for (size_t i = 0; i < 16 && i < 20; ++i) msg[i] = static_cast<uint8_t>(i);
    /* 0..9 -> '0'..'9', 10..15 -> 'A'..'F'. */
    const std::string s = app::bht_ccir_to_ascii(msg);
    CHECK_EQ(s[0], '0');
    CHECK_EQ(s[9], '9');
    CHECK_EQ(s[10], 'A');
    CHECK_EQ(s[15], 'F');
}
