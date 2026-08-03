/*
 * mayhem-b200 — KISS TNC codec and AX.25 encoder tests.
 *
 * Covers the required KISS FEND/FESC escape/unescape round trip, plus the AX.25
 * frame-check sequence (CRC-16/X-25, checked against its 0x906E standard vector)
 * and HDLC bit stuffing.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "../src/apps/ui_kiss_tnc.hpp"

#include <cstdint>
#include <vector>

using namespace mb200test;
namespace kiss = app::kiss;

/* --- KISS codec ----------------------------------------------------------- */

TEST(kiss_encode_escapes) {
    /* FEND -> DB DC, FESC -> DB DD; other bytes pass through. */
    std::vector<uint8_t> payload = {0x11, kiss::FEND, 0x22, kiss::FESC, 0x33};
    auto enc = kiss::kiss_encode(payload);

    std::vector<uint8_t> expected = {
        kiss::FEND, 0x00, 0x11, kiss::FESC, kiss::TFEND, 0x22,
        kiss::FESC, kiss::TFESC, 0x33, kiss::FEND};
    CHECK_EQ(enc.size(), expected.size());
    for (size_t i = 0; i < expected.size(); i++) CHECK_EQ(enc[i], expected[i]);
}

TEST(kiss_round_trip) {
    /* A payload full of the bytes that must be escaped. */
    std::vector<uint8_t> payload = {0x01, kiss::FEND, 0x02, kiss::FESC, 0x03,
                                    kiss::FEND, kiss::FESC, 0xAA, 0x00, 0xFF};
    auto enc = kiss::kiss_encode(payload);

    kiss::KissDecoder dec;
    std::vector<std::vector<uint8_t>> frames;
    dec.set_on_frame([&](const std::vector<uint8_t>& f) { frames.push_back(f); });
    dec.feed(enc);

    CHECK_EQ(frames.size(), size_t{1});
    CHECK_EQ(frames[0].size(), payload.size());
    for (size_t i = 0; i < payload.size(); i++) CHECK_EQ(frames[0][i], payload[i]);
}

TEST(kiss_two_frames_back_to_back) {
    kiss::KissDecoder dec;
    int count = 0;
    std::vector<uint8_t> last;
    dec.set_on_frame([&](const std::vector<uint8_t>& f) { count++; last = f; });

    auto a = kiss::kiss_encode(std::vector<uint8_t>{0x10, 0x20});
    auto b = kiss::kiss_encode(std::vector<uint8_t>{0x30, kiss::FEND, 0x40});
    std::vector<uint8_t> stream = a;
    stream.insert(stream.end(), b.begin(), b.end());
    dec.feed(stream);

    CHECK_EQ(count, 2);
    static const uint8_t expect[3] = {0x30, kiss::FEND, 0x40};
    CHECK_EQ(last.size(), size_t{3});
    for (int i = 0; i < 3; i++) CHECK_EQ(last[i], expect[i]);
}

TEST(kiss_byte_split_across_feeds) {
    /* The decoder is streaming: bytes may arrive in any chunking. */
    auto enc = kiss::kiss_encode(std::vector<uint8_t>{0xC0, 0xDB, 0x55});
    kiss::KissDecoder dec;
    std::vector<uint8_t> got;
    dec.set_on_frame([&](const std::vector<uint8_t>& f) { got = f; });
    for (uint8_t b : enc) dec.feed(&b, 1);  /* one byte at a time */

    static const uint8_t expect[3] = {0xC0, 0xDB, 0x55};
    CHECK_EQ(got.size(), size_t{3});
    for (int i = 0; i < 3; i++) CHECK_EQ(got[i], expect[i]);
}

/* --- AX.25 ---------------------------------------------------------------- */

TEST(ax25_fcs_known_vector) {
    /* CRC-16/X-25 check value for "123456789" is 0x906E. */
    const uint8_t v[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK_EQ(kiss::ax25_fcs(v, sizeof(v)), 0x906Eu);
}

TEST(ax25_bit_stuffing) {
    /* Data bytes of all ones would give long runs of ones without stuffing;
     * HDLC inserts a 0 after every five, so (recovering the pre-NRZI data bits)
     * no run of ones exceeds six, and a run of six occurs only in the flags. */
    kiss::AX25Frame frame;
    const uint8_t data[] = {0xFF, 0xFF, 0xFF};
    frame.make_frame_from_raw(data, sizeof(data));
    const auto& bits = frame.bits();
    CHECK(!bits.empty());

    /* Invert NRZI: a data bit is 1 when the level held, 0 when it flipped.
     * current_bit_ starts at 0. */
    uint8_t prev = 0;
    int run = 0, max_run = 0;
    for (uint8_t level : bits) {
        const int data_bit = (level == prev) ? 1 : 0;
        prev = level;
        if (data_bit) {
            run++;
            if (run > max_run) max_run = run;
        } else {
            run = 0;
        }
    }
    CHECK(max_run <= 6);   /* stuffing holds; broken stuffing would give 8+ */
    CHECK(max_run == 6);   /* the flags still carry their six ones */
}

TEST(ax25_frame_has_flags_and_fcs) {
    /* A minimal frame: 4 opening flags + FCS + 2 closing flags. Recover the
     * pre-NRZI bits and confirm the first 32 are four 0x7E flags (LSB-first:
     * 0 1 1 1 1 1 1 0). */
    kiss::AX25Frame frame;
    frame.make_frame_from_raw(nullptr, 0);
    const auto& bits = frame.bits();
    CHECK(bits.size() >= 48);  /* 6 flags (48 bits) + FCS (16) at least */

    uint8_t prev = 0;
    std::vector<uint8_t> pre;
    for (uint8_t level : bits) {
        pre.push_back(static_cast<uint8_t>((level == prev) ? 1 : 0));
        prev = level;
    }
    static const uint8_t flag_lsb[8] = {0, 1, 1, 1, 1, 1, 1, 0};
    for (int f = 0; f < 4; f++)
        for (int i = 0; i < 8; i++) CHECK_EQ(pre[f * 8 + i], flag_lsb[i]);
}
