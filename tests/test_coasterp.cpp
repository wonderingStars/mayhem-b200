/*
 * mayhem-b200 — Coaster / Syscall pager encoder tests.
 *
 * Checks the 19-byte frame layout and bit order against the upstream
 * generate_frame(), then closes the loop through the real Phase A FSK modulator
 * and demodulator: the frame is 2-FSK-modulated, demodulated, and the sync word
 * plus the eight data bytes are recovered intact.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "demod_digital.hpp"
#include "ui_coasterp.hpp"

#include <array>
#include <cstdint>
#include <vector>

using namespace mb200test;

TEST(coasterp_frame_layout) {
    const auto frame = app::coasterp_build_frame(0x44013B30303034BCull);

    for (int i = 0; i < 8; ++i) CHECK_EQ(frame[i], uint8_t{0x55});  // preamble
    CHECK_EQ(frame[8], uint8_t{0x2D});                              // sync
    CHECK_EQ(frame[9], uint8_t{0xD4});
    CHECK_EQ(frame[10], uint8_t{0x08});                            // data length
    /* Data, big-endian. */
    CHECK_EQ(frame[11], uint8_t{0x44});
    CHECK_EQ(frame[12], uint8_t{0x01});
    CHECK_EQ(frame[13], uint8_t{0x3B});
    CHECK_EQ(frame[14], uint8_t{0x30});
    CHECK_EQ(frame[15], uint8_t{0x30});
    CHECK_EQ(frame[16], uint8_t{0x30});
    CHECK_EQ(frame[17], uint8_t{0x34});
    CHECK_EQ(frame[18], uint8_t{0xBC});
}

TEST(coasterp_frame_bits_order) {
    const auto frame = app::coasterp_build_frame(0x0000000000000000ull);
    const auto bits = app::coasterp_frame_bits(frame);

    CHECK_EQ(bits.size(), size_t{152});
    /* First byte 0x55 -> 0,1,0,1,0,1,0,1, MSB first. */
    const uint8_t want[8] = {0, 1, 0, 1, 0, 1, 0, 1};
    for (int i = 0; i < 8; ++i) CHECK_EQ(bits[i], want[i]);
    /* Sync high byte 0x2D at bits 64..71 -> 0,0,1,0,1,1,0,1. */
    const uint8_t sync_hi[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    for (int i = 0; i < 8; ++i) CHECK_EQ(bits[64 + i], sync_hi[i]);
}

TEST(coasterp_fsk_round_trip) {
    const uint64_t data = 0x44013B30303034BCull;
    const auto frame = app::coasterp_build_frame(data);
    const auto bits = app::coasterp_frame_bits(frame);

    constexpr float fs = 500000.0f, baud = 1000.0f, dev = 5000.0f;
    const auto iq = dsp::fsk_modulate(bits, fs, baud, dev);
    CHECK(iq.size() > 1000);

    dsp::FskDemod demod;
    demod.configure(fs, baud, dev);
    std::vector<uint8_t> rx;
    demod.process(iq.data(), iq.size(), rx);

    /* Find the 16-bit sync 0x2DD4, skip the 8-bit length byte, read 64 data
     * bits. */
    bool found = false;
    uint64_t recovered = 0;
    for (size_t i = 0; i + 16 + 8 + 64 <= rx.size(); ++i) {
        uint16_t win = 0;
        for (int k = 0; k < 16; ++k) win = static_cast<uint16_t>((win << 1) | rx[i + k]);
        if (win == 0x2DD4) {
            found = true;
            for (int k = 0; k < 64; ++k)
                recovered = (recovered << 1) | rx[i + 16 + 8 + k];
            break;
        }
    }

    CHECK(found);
    CHECK_EQ(recovered, data);
}
