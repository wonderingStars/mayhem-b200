/*
 * mayhem-b200 — BLE advertising transmit encoder tests.
 *
 * The deliverable is the encoder, so it is checked against hard upstream data:
 *
 *   - whitening sequence vs the literal per-channel scramble_table from
 *     baseband/proc_btlerx.hpp (a genuine known-output vector);
 *   - the advertising access address value 0x8E89BED6;
 *   - CRC-24 cross-checked against the canonical table-driven BLE CRC (the
 *     crc_table + crc_init_reorder from proc_btlerx.hpp), which is an
 *     independent reference implementation of the same CRC, with the result
 *     pinned; plus a dewhiten round trip that recovers the PDU.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "../src/apps/ui_ble_tx.hpp"

#include <array>
#include <cstdint>
#include <vector>

using namespace mb200test;
namespace ble = app::ble_tx;

/* --- Canonical BLE CRC-24 reference (upstream proc_btlerx.hpp) ------------- */

static const uint32_t kBleCrcTable[256] = {
    0x000000, 0x01b4c0, 0x036980, 0x02dd40, 0x06d300, 0x0767c0, 0x05ba80, 0x040e40,
    0x0da600, 0x0c12c0, 0x0ecf80, 0x0f7b40, 0x0b7500, 0x0ac1c0, 0x081c80, 0x09a840,
    0x1b4c00, 0x1af8c0, 0x182580, 0x199140, 0x1d9f00, 0x1c2bc0, 0x1ef680, 0x1f4240,
    0x16ea00, 0x175ec0, 0x158380, 0x143740, 0x103900, 0x118dc0, 0x135080, 0x12e440,
    0x369800, 0x372cc0, 0x35f180, 0x344540, 0x304b00, 0x31ffc0, 0x332280, 0x329640,
    0x3b3e00, 0x3a8ac0, 0x385780, 0x39e340, 0x3ded00, 0x3c59c0, 0x3e8480, 0x3f3040,
    0x2dd400, 0x2c60c0, 0x2ebd80, 0x2f0940, 0x2b0700, 0x2ab3c0, 0x286e80, 0x29da40,
    0x207200, 0x21c6c0, 0x231b80, 0x22af40, 0x26a100, 0x2715c0, 0x25c880, 0x247c40,
    0x6d3000, 0x6c84c0, 0x6e5980, 0x6fed40, 0x6be300, 0x6a57c0, 0x688a80, 0x693e40,
    0x609600, 0x6122c0, 0x63ff80, 0x624b40, 0x664500, 0x67f1c0, 0x652c80, 0x649840,
    0x767c00, 0x77c8c0, 0x751580, 0x74a140, 0x70af00, 0x711bc0, 0x73c680, 0x727240,
    0x7bda00, 0x7a6ec0, 0x78b380, 0x790740, 0x7d0900, 0x7cbdc0, 0x7e6080, 0x7fd440,
    0x5ba800, 0x5a1cc0, 0x58c180, 0x597540, 0x5d7b00, 0x5ccfc0, 0x5e1280, 0x5fa640,
    0x560e00, 0x57bac0, 0x556780, 0x54d340, 0x50dd00, 0x5169c0, 0x53b480, 0x520040,
    0x40e400, 0x4150c0, 0x438d80, 0x423940, 0x463700, 0x4783c0, 0x455e80, 0x44ea40,
    0x4d4200, 0x4cf6c0, 0x4e2b80, 0x4f9f40, 0x4b9100, 0x4a25c0, 0x48f880, 0x494c40,
    0xda6000, 0xdbd4c0, 0xd90980, 0xd8bd40, 0xdcb300, 0xdd07c0, 0xdfda80, 0xde6e40,
    0xd7c600, 0xd672c0, 0xd4af80, 0xd51b40, 0xd11500, 0xd0a1c0, 0xd27c80, 0xd3c840,
    0xc12c00, 0xc098c0, 0xc24580, 0xc3f140, 0xc7ff00, 0xc64bc0, 0xc49680, 0xc52240,
    0xcc8a00, 0xcd3ec0, 0xcfe380, 0xce5740, 0xca5900, 0xcbedc0, 0xc93080, 0xc88440,
    0xecf800, 0xed4cc0, 0xef9180, 0xee2540, 0xea2b00, 0xeb9fc0, 0xe94280, 0xe8f640,
    0xe15e00, 0xe0eac0, 0xe23780, 0xe38340, 0xe78d00, 0xe639c0, 0xe4e480, 0xe55040,
    0xf7b400, 0xf600c0, 0xf4dd80, 0xf56940, 0xf16700, 0xf0d3c0, 0xf20e80, 0xf3ba40,
    0xfa1200, 0xfba6c0, 0xf97b80, 0xf8cf40, 0xfcc100, 0xfd75c0, 0xffa880, 0xfe1c40,
    0xb75000, 0xb6e4c0, 0xb43980, 0xb58d40, 0xb18300, 0xb037c0, 0xb2ea80, 0xb35e40,
    0xbaf600, 0xbb42c0, 0xb99f80, 0xb82b40, 0xbc2500, 0xbd91c0, 0xbf4c80, 0xbef840,
    0xac1c00, 0xada8c0, 0xaf7580, 0xaec140, 0xaacf00, 0xab7bc0, 0xa9a680, 0xa81240,
    0xa1ba00, 0xa00ec0, 0xa2d380, 0xa36740, 0xa76900, 0xa6ddc0, 0xa40080, 0xa5b440,
    0x81c800, 0x807cc0, 0x82a180, 0x831540, 0x871b00, 0x86afc0, 0x847280, 0x85c640,
    0x8c6e00, 0x8ddac0, 0x8f0780, 0x8eb340, 0x8abd00, 0x8b09c0, 0x89d480, 0x886040,
    0x9a8400, 0x9b30c0, 0x99ed80, 0x985940, 0x9c5700, 0x9de3c0, 0x9f3e80, 0x9e8a40,
    0x972200, 0x9696c0, 0x944b80, 0x95ff40, 0x91f100, 0x9045c0, 0x929880, 0x932c40};

static uint32_t ref_crc24(const std::vector<uint8_t>& bytes, uint32_t init) {
    uint32_t crc = init;
    for (uint8_t d : bytes) {
        const uint32_t idx = (crc ^ d) & 0xFF;
        crc = (kBleCrcTable[idx] ^ (crc >> 8)) & 0xFFFFFF;
    }
    return crc;
}

static uint32_t crc_init_reorder(uint32_t crc_init) {
    uint32_t tmp = crc_init;
    uint32_t input = tmp & 0xFF;
    tmp >>= 8;
    input = (input << 8) | (tmp & 0xFF);
    tmp >>= 8;
    input = (input << 8) | (tmp & 0xFF);
    input <<= 1;
    uint32_t out = 0;
    for (int i = 0; i < 24; i++) {
        input >>= 1;
        out = (out << 1) | (input & 1);
    }
    return out;
}

/* Packs a 0/1 bit vector least-significant-bit first into bytes, the on-air
 * BLE octet order a receiver reassembles. */
static std::vector<uint8_t> pack_lsb(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> bytes((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); i++)
        if (bits[i] & 1) bytes[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
    return bytes;
}

/* --- whitening vs upstream scramble_table --------------------------------- */

TEST(ble_tx_whitening_matches_scramble_table_ch37) {
    /* proc_btlerx.hpp scramble_table[37], the first bytes. */
    static const uint8_t expected[] = {141, 210, 87, 161, 61, 167, 102, 176,
                                       117, 49,  17, 72,  150, 119, 248, 227};
    auto seq = ble::whitening_sequence(37, sizeof(expected) * 8);
    auto bytes = pack_lsb(seq);
    for (size_t i = 0; i < sizeof(expected); i++)
        CHECK_EQ(bytes[i], expected[i]);
}

TEST(ble_tx_whitening_matches_scramble_table_ch39) {
    /* proc_btlerx.hpp scramble_table[39]. */
    static const uint8_t expected[] = {31, 55, 74, 95, 133, 246, 156, 154,
                                       193, 214, 197, 68, 32, 89, 222, 225};
    auto seq = ble::whitening_sequence(39, sizeof(expected) * 8);
    auto bytes = pack_lsb(seq);
    for (size_t i = 0; i < sizeof(expected); i++)
        CHECK_EQ(bytes[i], expected[i]);
}

TEST(ble_tx_whitening_is_its_own_inverse) {
    std::vector<uint8_t> data;
    for (int i = 0; i < 200; i++) data.push_back(static_cast<uint8_t>((i * 37 + 5) & 1));
    auto once = ble::whiten(data, 37);
    auto twice = ble::whiten(once, 37);
    CHECK_EQ(twice.size(), data.size());
    for (size_t i = 0; i < data.size(); i++) CHECK_EQ(twice[i], data[i]);
}

/* --- access address ------------------------------------------------------- */

TEST(ble_tx_access_address_value) {
    /* preamble(8) + access address(32); the receiver assembles the AA
     * least-significant-bit first into 0x8E89BED6. */
    ble::AdvPacket pkt{};
    auto info = ble::build_info_bits(pkt);
    CHECK(info.size() >= 40);

    uint32_t aa = 0;
    for (int i = 0; i < 32; i++)
        if (info[8 + i] & 1) aa |= (1u << i);
    CHECK_EQ(aa, 0x8E89BED6u);

    /* Preamble byte is 0xAA. */
    uint8_t preamble = 0;
    for (int i = 0; i < 8; i++)
        if (info[i] & 1) preamble |= static_cast<uint8_t>(1u << i);
    CHECK_EQ(preamble, 0xAAu);
}

/* --- MAC byte-order reversal ---------------------------------------------- */

TEST(ble_tx_mac_is_reversed) {
    ble::AdvPacket pkt{};
    pkt.mac = "AABBCCDDEEFF";
    auto info = ble::build_info_bits(pkt);
    CHECK(!info.empty());
    /* AdvA starts after preamble(8)+AA(32)+header(16) = bit 56, 6 octets,
     * least-significant octet first: FF EE DD CC BB AA. */
    static const uint8_t on_air[] = {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    for (int o = 0; o < 6; o++) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++)
            if (info[56 + o * 8 + b] & 1) byte |= static_cast<uint8_t>(1u << b);
        CHECK_EQ(byte, on_air[o]);
    }
}

/* --- CRC-24 vs canonical table + pinned value ----------------------------- */

TEST(ble_tx_crc24_matches_canonical_and_pinned) {
    ble::AdvPacket pkt{};
    pkt.channel = 37;
    pkt.type = ble::PduType::ADV_IND;
    pkt.mac = "010203040506";
    pkt.adv_data = "02011A";

    auto info = ble::build_info_bits(pkt);
    CHECK(!info.empty());

    /* PDU region = header + AdvA + AdvData = everything after bit 40. */
    std::vector<uint8_t> pdu_bits(info.begin() + 40, info.end());
    auto pdu_bytes = pack_lsb(pdu_bits);

    /* Table-driven reference CRC over the same bytes. */
    const uint32_t ref = ref_crc24(pdu_bytes, crc_init_reorder(0x555555));

    /* Encoder CRC (bit LFSR), reassembled least-significant-bit first into a
     * 24-bit word as a receiver would. */
    auto crc_bits = ble::crc24(pdu_bits, ble::kCrcInit);
    uint32_t enc = 0;
    for (int i = 0; i < 24; i++)
        if (crc_bits[i] & 1) enc |= (1u << i);

    CHECK_EQ(enc, ref);
    /* Regression pin (cross-validated by the canonical table above). */
    CHECK_EQ(enc, 0x0ACF1Fu);
}

/* --- full dewhiten round trip --------------------------------------------- */

TEST(ble_tx_phy_dewhiten_round_trip) {
    ble::AdvPacket pkt{};
    pkt.channel = 39;
    pkt.type = ble::PduType::ADV_NONCONN_IND;
    pkt.mac = "C1C2C3C4C5C6";
    pkt.adv_data = "0201060909484F53540102";

    auto info = ble::build_info_bits(pkt);
    auto phy = ble::build_phy_bits(pkt);
    CHECK(!info.empty());
    CHECK(!phy.empty());

    /* Preamble + AA (first 40 bits) are unwhitened and identical. */
    for (size_t i = 0; i < 40; i++) CHECK_EQ(phy[i], info[i]);

    /* Dewhiten the rest and confirm it is PDU followed by a CRC that checks. */
    std::vector<uint8_t> whitened(phy.begin() + 40, phy.end());
    auto dewhitened = ble::whiten(whitened, pkt.channel);  /* XOR is its own inverse */

    const size_t pdu_bit_len = info.size() - 40;
    CHECK_EQ(dewhitened.size(), pdu_bit_len + 24);

    /* Recovered PDU bits equal the original PDU bits. */
    for (size_t i = 0; i < pdu_bit_len; i++) CHECK_EQ(dewhitened[i], info[40 + i]);

    /* CRC over the recovered PDU matches the trailing 24 bits. */
    std::vector<uint8_t> pdu_bits(dewhitened.begin(), dewhitened.begin() + pdu_bit_len);
    auto pdu_bytes = pack_lsb(pdu_bits);
    std::vector<uint8_t> crc_bits(dewhitened.begin() + pdu_bit_len, dewhitened.end());
    auto crc_bytes = pack_lsb(crc_bits);
    uint32_t rx_crc = crc_bytes[0] | (crc_bytes[1] << 8) | (crc_bytes[2] << 16);

    const uint32_t ref = ref_crc24(pdu_bytes, crc_init_reorder(0x555555));
    CHECK_EQ(rx_crc, ref);
}

/* --- boundary: invalid hex ------------------------------------------------ */

TEST(ble_tx_invalid_mac_rejected) {
    ble::AdvPacket pkt{};
    pkt.mac = "0102030405";  /* 10 hex chars, not 12 */
    CHECK(ble::build_info_bits(pkt).empty());
    CHECK(ble::build_phy_bits(pkt).empty());
}

TEST(ble_tx_channel_frequencies) {
    CHECK_EQ(ble::channel_frequency(37), 2'402'000'000ULL);
    CHECK_EQ(ble::channel_frequency(38), 2'426'000'000ULL);
    CHECK_EQ(ble::channel_frequency(39), 2'480'000'000ULL);
    CHECK_EQ(ble::channel_frequency(0), 2'404'000'000ULL);
    CHECK_EQ(ble::channel_frequency(10), 2'424'000'000ULL);
    CHECK_EQ(ble::channel_frequency(11), 2'428'000'000ULL);
}
