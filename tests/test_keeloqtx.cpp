/*
 * mayhem-b200 — KeeLoq TX encoder tests.
 *
 * The load-bearing, hardware-free parts of the KeeLoq app are the cipher, the
 * per-manufacturer hop layout, and the OOK PWM framing. The cipher is anchored
 * to the published hadipourh/KeeLoq reference vectors; everything else is
 * checked against values derived by hand from the upstream algorithm.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_keeloqtx.hpp"

#include <cstdint>
#include <string>

using namespace mb200test;

TEST(keeloq_encrypt_known_vectors) {
    /* hadipourh/KeeLoq reference vectors (key 0x5cec6701b79fd949). */
    CHECK_EQ(app::keeloq_encrypt(0xf741e2dbu, 0x5cec6701b79fd949ull), 0xe44f4cdfu);
    CHECK_EQ(app::keeloq_encrypt(0x0ca69b92u, 0x5cec6701b79fd949ull), 0xa6ac0ea2u);
}

TEST(keeloq_decrypt_inverts_encrypt) {
    /* Same vectors, backwards. */
    CHECK_EQ(app::keeloq_decrypt(0xe44f4cdfu, 0x5cec6701b79fd949ull), 0xf741e2dbu);
    CHECK_EQ(app::keeloq_decrypt(0xa6ac0ea2u, 0x5cec6701b79fd949ull), 0x0ca69b92u);
}

TEST(keeloq_round_trip_many) {
    const uint64_t keys[] = {0x0ull, 0xFFFFFFFFFFFFFFFFull, 0x0123456789ABCDEFull,
                             0x5cec6701b79fd949ull};
    const uint32_t data[] = {0x0u, 0xFFFFFFFFu, 0xDEADBEEFu, 0x12345678u, 0x1u};
    for (uint64_t k : keys)
        for (uint32_t d : data)
            CHECK_EQ(app::keeloq_decrypt(app::keeloq_encrypt(d, k), k), d);
}

TEST(keeloq_reverse_key_bits) {
    /* Reverse of the low 8 bits of 0x01 is 0x80. */
    CHECK_EQ(app::keeloq_reverse_key(0x01ull, 8), 0x80ull);
    /* Reverse of a 64-bit value twice is the identity. */
    const uint64_t v = 0x0123456789ABCDEFull;
    CHECK_EQ(app::keeloq_reverse_key(app::keeloq_reverse_key(v, 64), 64), v);
    /* Reversing 0b1011 over 4 bits gives 0b1101. */
    CHECK_EQ(app::keeloq_reverse_key(0b1011ull, 4), 0b1101ull);
}

TEST(keeloq_normal_learning_composition) {
    /* keeloq_normal_learning must be exactly the two 0x2/0x6-seeded decrypts of
     * the 28-bit serial, packed k2:k1. */
    const uint64_t key = 0x5cec6701b79fd949ull;
    const uint32_t serial = 0x0ABCDEFu;

    uint32_t s = serial & 0x0FFFFFFFu;
    const uint32_t k1 = app::keeloq_decrypt((s & 0x0FFFFFFFu) | 0x20000000u, key);
    const uint32_t k2 = app::keeloq_decrypt((s & 0x0FFFFFFFu) | 0x60000000u, key);
    const uint64_t expected = (static_cast<uint64_t>(k2) << 32) | k1;

    CHECK_EQ(app::keeloq_normal_learning(serial, key), expected);
}

TEST(keeloq_build_hop_layouts) {
    /* Generic: btn<<28 | (serial & 0x3FF)<<16 | counter. */
    CHECK_EQ(app::keeloq_build_hop(app::KeeloqHop::Generic, 0x1, 0x2345u, 0x1122u),
             (0x1u << 28) | ((0x2345u & 0x3FFu) << 16) | 0x1122u);
    /* Serial12 masks to 12 bits. */
    CHECK_EQ(app::keeloq_build_hop(app::KeeloqHop::Serial12, 0x2, 0xABCDu, 0x0001u),
             (0x2u << 28) | ((0xABCDu & 0xFFFu) << 16) | 0x0001u);
    /* Serial8 masks to 8 bits. */
    CHECK_EQ(app::keeloq_build_hop(app::KeeloqHop::Serial8, 0x3, 0xABCDu, 0x0002u),
             (0x3u << 28) | ((0xABCDu & 0xFFu) << 16) | 0x0002u);
    /* Fixed-serial manufacturers. */
    CHECK_EQ(app::keeloq_build_hop(app::KeeloqHop::Merlin, 0x4, 0x9999u, 0x0003u),
             (0x4u << 28) | (0x000u << 16) | 0x0003u);
    CHECK_EQ(app::keeloq_build_hop(app::KeeloqHop::Centurion, 0x5, 0x9999u, 0x0004u),
             (0x5u << 28) | (0x1CEu << 16) | 0x0004u);
    CHECK_EQ(app::keeloq_build_hop(app::KeeloqHop::Monarch, 0x6, 0x9999u, 0x0005u),
             (0x6u << 28) | (0x100u << 16) | 0x0005u);
}

TEST(keeloq_build_hop_aprimatic_parity) {
    /* serial 0x000 -> low 10 bits have 0 set bits (even) -> parity fill added.
     * The fill is upstream's 0b110000000000 == 0xC00 (ui_keeloqtx.cpp:32), not
     * 0x300; the original expectation here miscomputed the binary constant. */
    const uint32_t hop = app::keeloq_build_hop(app::KeeloqHop::Aprimatic, 0x1, 0x000u, 0x0000u);
    CHECK_EQ(hop, (0x1u << 28) | (0xC00u << 16) | 0x0000u);
    /* serial 0x001 -> one set bit (odd) -> no fill, serial stays 0x001. */
    const uint32_t hop2 = app::keeloq_build_hop(app::KeeloqHop::Aprimatic, 0x1, 0x001u, 0x0000u);
    CHECK_EQ(hop2, (0x1u << 28) | (0x001u << 16) | 0x0000u);
}

TEST(keeloq_encode_fragments_structure) {
    const std::string header = "101010101010101010101010000000000";

    /* payload 0 -> every bit 0 -> "110" x 64. */
    std::string expected0 = header;
    for (int i = 0; i < 64; ++i) expected0 += "110";
    expected0 += "1001";
    CHECK_STR_EQ(app::keeloq_encode_fragments(0ull), expected0);

    /* Length is always 33 + 192 + 4 = 229 symbols. */
    CHECK_EQ(app::keeloq_encode_fragments(0xDEADBEEFCAFEBABEull).size(), size_t{229});

    /* payload LSB set -> the first data fragment is "100" (a 1 bit). */
    const std::string f = app::keeloq_encode_fragments(0x1ull);
    CHECK_STR_EQ(f.substr(header.size(), 3), std::string{"100"});
    /* The remaining 63 data bits are 0 -> "110". */
    CHECK_STR_EQ(f.substr(header.size() + 3, 3), std::string{"110"});
    /* Ends with the stop symbols. */
    CHECK_STR_EQ(f.substr(f.size() - 4), std::string{"1001"});
}

TEST(keeloq_full_payload_assembly) {
    /* fix = btn<<28 | serial(28); payload = fix<<32 | encrypt(hop, key). This is
     * the exact assembly the app transmits. */
    const uint64_t key = 0x5cec6701b79fd949ull;
    const uint8_t btn = 0x2;
    const uint32_t serial = 0x0345678u;
    const uint16_t counter = 0x0042u;

    const uint32_t fix = (static_cast<uint32_t>(btn) << 28) | (serial & 0x0FFFFFFFu);
    const uint32_t hop = app::keeloq_build_hop(app::KeeloqHop::Generic, btn, serial, counter);
    const uint32_t enc = app::keeloq_encrypt(hop, key);
    const uint64_t payload = (static_cast<uint64_t>(fix) << 32) | enc;

    /* Decrypting the hop back out proves the assembly's cipher half is sound. */
    CHECK_EQ(app::keeloq_decrypt(enc, key), hop);
    /* The high 32 bits of the payload are the fixed part. */
    CHECK_EQ(static_cast<uint32_t>(payload >> 32), fix);
}
