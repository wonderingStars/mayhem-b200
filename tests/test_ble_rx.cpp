/*
 * mayhem-b200 — BLE advertising receive decoder tests.
 *
 * A synthesised GFSK advertising burst (built with the same CRC + whitening the
 * decoder verifies, then modulated with dsp::fsk_modulate) is fed through the
 * decoder and the recovered PDU is checked. Covers the 24-bit CRC (pinned),
 * whitening, the access-address match, and rejection when the access address is
 * corrupted.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "../src/apps/ui_ble_rx.hpp"
#include "../src/dsp/demod_digital.hpp"

#include <cstdint>
#include <vector>

using namespace mb200test;
namespace ble = app::ble_rx;

static std::vector<uint8_t> bytes_to_bits_lsb(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (uint8_t b : bytes)
        for (int k = 0; k < 8; k++) bits.push_back(static_cast<uint8_t>((b >> k) & 1));
    return bits;
}

/* An ADV_IND PDU: header {type/flags, length}, 6 AdvA octets (on-air order),
 * then AdvData. rb_buf[2..7] carry the MAC least-significant octet first, so
 * these bytes decode to display MAC AA:BB:CC:DD:EE:FF. */
static std::vector<uint8_t> sample_pdu() {
    return {0x40, 0x09, 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x02, 0x01, 0x1A};
}

static std::vector<dsp::cfloat> build_burst(int channel, uint8_t corrupt_aa_bit) {
    auto pdu = sample_pdu();
    const uint32_t crc = ble::crc24(pdu.data(), pdu.size(), ble::crc_init_reorder(ble::kCrcInit));

    std::vector<uint8_t> full = pdu;
    full.push_back(static_cast<uint8_t>(crc & 0xFF));
    full.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    full.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));

    auto whitened = ble::whiten(bytes_to_bits_lsb(full), channel);

    /* Preamble 0xAA then access address D6 BE 89 8E, on-air (LSB-first) order. */
    std::vector<uint8_t> pre_bytes = {0xAA, 0xD6, 0xBE, 0x89, 0x8E};
    auto bits = bytes_to_bits_lsb(pre_bytes);
    if (corrupt_aa_bit) bits[10] ^= 1;  /* damage the access address */
    bits.insert(bits.end(), whitened.begin(), whitened.end());
    for (int i = 0; i < 4; i++) bits.push_back(0);  /* trailing readable symbols */

    return dsp::fsk_modulate(bits, 4'000'000.0f, 1'000'000.0f, 250'000.0f, 0.5f);
}

TEST(ble_rx_crc_init_reorder) {
    /* proc_btlerx uses crc_init_reorder(0x555555) as the table preset. */
    const uint32_t r = ble::crc_init_reorder(0x555555);
    CHECK_EQ(r, ble::crc_init_reorder(0x555555));  /* deterministic */
    CHECK_EQ(r & 0xFF000000u, 0u);                 /* 24-bit */
}

TEST(ble_rx_crc24_pinned) {
    auto pdu = sample_pdu();
    const uint32_t crc = ble::crc24(pdu.data(), pdu.size(), ble::crc_init_reorder(ble::kCrcInit));
    CHECK_EQ(crc, 0x095E5Eu);
}

TEST(ble_rx_whiten_self_inverse) {
    std::vector<uint8_t> data;
    for (int i = 0; i < 160; i++) data.push_back(static_cast<uint8_t>((i * 13 + 1) & 1));
    auto back = ble::whiten(ble::whiten(data, 39), 39);
    CHECK_EQ(back.size(), data.size());
    for (size_t i = 0; i < data.size(); i++) CHECK_EQ(back[i], data[i]);
}

TEST(ble_rx_decode_round_trip) {
    auto iq = build_burst(37, /*corrupt*/ 0);
    CHECK(!iq.empty());

    ble::Decoder dec;
    dec.configure(4, 37);
    ble::Packet got{};
    int hits = 0;
    dec.set_on_packet([&](const ble::Packet& p) { got = p; hits++; });
    dec.process(iq.data(), iq.size());

    CHECK_EQ(hits, 1);
    CHECK_EQ(dec.packets_decoded(), size_t{1});
    CHECK_EQ(got.type, 0u);          /* ADV_IND */
    CHECK_EQ(got.payload_len, 9u);   /* 6 MAC + 3 data */
    CHECK_EQ(got.crc, got.computed_crc);

    static const uint8_t expect_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    for (int i = 0; i < 6; i++) CHECK_EQ(got.mac[i], expect_mac[i]);

    CHECK_EQ(got.data.size(), size_t{3});
    CHECK_EQ(got.data[0], 0x02u);
    CHECK_EQ(got.data[1], 0x01u);
    CHECK_EQ(got.data[2], 0x1Au);

    CHECK_STR_EQ(got.mac_string(), "AA:BB:CC:DD:EE:FF");
}

TEST(ble_rx_rejects_corrupt_access_address) {
    auto iq = build_burst(37, /*corrupt*/ 1);
    ble::Decoder dec;
    dec.configure(4, 37);
    dec.process(iq.data(), iq.size());
    CHECK_EQ(dec.packets_decoded(), size_t{0});
}

TEST(ble_rx_wrong_channel_fails_crc) {
    /* Built for channel 37 but decoded assuming channel 39: the dewhitening
     * differs, so the CRC fails and nothing is reported. */
    auto iq = build_burst(37, 0);
    ble::Decoder dec;
    dec.configure(4, 39);
    dec.process(iq.data(), iq.size());
    CHECK_EQ(dec.packets_decoded(), size_t{0});
}
