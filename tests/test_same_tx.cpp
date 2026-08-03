/*
 * mayhem-b200 — SAME / EAS transmit encoder tests.
 *
 * Checks the SAME header string, the 16-byte 0xAB preamble + ASCII byte
 * framing, the least-significant-bit-first on-air order, and an AFSK
 * modulate/demodulate round trip (mark 2083 Hz, space 1563 Hz, 520.83 baud)
 * that recovers the exact header through the Phase A AfskDemod.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "../src/apps/ui_same_tx.hpp"
#include "../src/dsp/demod_digital.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace mb200test;
namespace same = app::same_tx;

TEST(same_message_string) {
    /* WXR / RWT, Ohio (39) county 007, 30 minutes. */
    CHECK_STR_EQ(same::build_message(0, 0, 39, 7, 0, 30),
                 "ZCZC-WXR-RWT-390007+0030-0010000-SAMETX--");
    /* EAS / TOR, state 06 county 037, 1h00m. */
    CHECK_STR_EQ(same::build_message(1, 25, 6, 37, 1, 0),
                 "ZCZC-EAS-TOR-060037+0100-0010000-SAMETX--");
}

TEST(same_byte_framing_preamble) {
    auto bytes = same::build_bytes("ZCZC-WXR-RWT-390007+0030-0010000-SAMETX--");
    /* 16 preamble bytes 0xAB, then the ASCII header. */
    for (size_t i = 0; i < same::kPreambleCount; i++) CHECK_EQ(bytes[i], 0xABu);
    CHECK_EQ(bytes[same::kPreambleCount + 0], static_cast<uint8_t>('Z'));
    CHECK_EQ(bytes[same::kPreambleCount + 1], static_cast<uint8_t>('C'));
    CHECK_EQ(bytes.size(), same::kPreambleCount + 41u);
}

TEST(same_bits_lsb_first) {
    auto bits = same::build_bits("Z");  /* 16 preamble + one 'Z' */
    CHECK_EQ(bits.size(), (same::kPreambleCount + 1) * 8);

    /* Preamble byte 0xAB, least-significant-bit first: 1 1 0 1 0 1 0 1. */
    static const uint8_t ab_lsb[8] = {1, 1, 0, 1, 0, 1, 0, 1};
    for (int i = 0; i < 8; i++) CHECK_EQ(bits[i], ab_lsb[i]);

    /* 'Z' == 0x5A, least-significant-bit first: 0 1 0 1 1 0 1 0. */
    static const uint8_t z_lsb[8] = {0, 1, 0, 1, 1, 0, 1, 0};
    const size_t base = same::kPreambleCount * 8;
    for (int i = 0; i < 8; i++) CHECK_EQ(bits[base + i], z_lsb[i]);
}

TEST(same_afsk_round_trip) {
    const std::string msg = "ZCZC-WXR-RWT-390007+0030-0010000-SAMETX--";
    auto bits = same::build_bits(msg);

    /* Trailing bits so the demodulator flushes the last data bit. */
    std::vector<uint8_t> tx = bits;
    for (int i = 0; i < 8; i++) tx.push_back(1);

    auto audio = dsp::afsk_modulate(tx, 48'000.0f, same::kMarkHz, same::kSpaceHz,
                                    same::kBaud, 0.8f);
    CHECK(!audio.empty());

    dsp::AfskDemod dem;
    dem.configure(48'000.0f, same::kMarkHz, same::kSpaceHz, same::kBaud);
    std::vector<uint8_t> out;
    dem.process_audio(audio.data(), audio.size(), out);

    /* Best bit alignment within a few bits of startup slack. */
    int best = 0, best_off = 0;
    for (int off = 0; off <= 20 && static_cast<size_t>(off) < out.size(); off++) {
        const size_t n = std::min(bits.size(), out.size() - off);
        int m = 0;
        for (size_t i = 0; i < n; i++)
            if (out[off + i] == bits[i]) m++;
        if (m > best) { best = m; best_off = off; }
    }
    CHECK_EQ(static_cast<size_t>(best), bits.size());  /* every bit recovered */

    /* Recover the ASCII header from the payload region. */
    std::string decoded;
    for (size_t bi = same::kPreambleCount * 8;
         bi + 8 <= bits.size() && static_cast<size_t>(best_off) + bi + 8 <= out.size();
         bi += 8) {
        uint8_t v = 0;
        for (int k = 0; k < 8; k++)
            if (out[best_off + bi + k]) v |= static_cast<uint8_t>(1u << k);
        decoded += static_cast<char>(v);
    }
    CHECK_STR_EQ(decoded, msg);
}
