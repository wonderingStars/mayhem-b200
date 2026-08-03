/*
 * mayhem-b200 — Shopping-cart lock tone generation tests.
 *
 * Expected values come from the documented Gatekeeper Smart Wheel protocol
 * (cra0/Gatekeeper-Systems-SmartWheel), not from this port's own output:
 *   LOCK   = 0x8E = 1000 1110      UNLOCK  = 0x71 = 0111 0001
 *   LOCK2  = 0xC7 = 1100 0111      UNLOCK2 = 0x78 = 0111 1000
 *   carrier ~7.8 kHz, on/off keyed, MSB first.
 * The bit period is this port's labelled assumption (kBitSeconds); the tests
 * that depend on it use kBitSeconds so they track the constant rather than
 * pinning an undocumented value.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_shoppingcart_lock.hpp"

#include <cmath>
#include <vector>

using namespace app::cartlock;
using namespace mb200test;

/* ---- protocol constants --------------------------------------------------- */

TEST(cart_codes_match_teardown) {
    /* Copy to runtime locals so the assertion isn't a constant expression
     * (avoids /W4 C4127 on comparing two compile-time constants). */
    uint8_t lock = kLockCode, unlock = kUnlockCode;
    uint8_t lock2 = kLock2Code, unlock2 = kUnlock2Code;
    CHECK_EQ(lock, (uint8_t)0x8E);
    CHECK_EQ(unlock, (uint8_t)0x71);
    CHECK_EQ(lock2, (uint8_t)0xC7);
    CHECK_EQ(unlock2, (uint8_t)0x78);
}

TEST(cart_code_bit_is_msb_first) {
    /* 0x8E = 1000 1110 */
    const bool lock[8] = {true, false, false, false, true, true, true, false};
    for (size_t i = 0; i < 8; ++i) CHECK_EQ(code_bit(kLockCode, i), lock[i]);

    /* 0x71 = 0111 0001 */
    const bool unlock[8] = {false, true, true, true, false, false, false, true};
    for (size_t i = 0; i < 8; ++i) CHECK_EQ(code_bit(kUnlockCode, i), unlock[i]);
}

/* ---- samples per bit ------------------------------------------------------ */

TEST(cart_samples_per_bit) {
    CHECK_EQ(samples_per_bit(48000.0f, 0.010f), (size_t)480);
    CHECK_EQ(samples_per_bit(48000.0f, 0.001f), (size_t)48);
    CHECK_EQ(samples_per_bit(44100.0f, 0.010f), (size_t)441);
}

TEST(cart_samples_per_bit_floor_is_one) {
    CHECK_EQ(samples_per_bit(48000.0f, 0.0000001f), (size_t)1);
    CHECK_EQ(samples_per_bit(0.0f, 0.010f), (size_t)1);
}

/* ---- overall waveform length --------------------------------------------- */

TEST(cart_ook_length_is_eight_bits) {
    const auto wf = generate_ook(kLockCode, kCarrierHz, 48000.0f, 0.010f);
    CHECK_EQ(wf.size(), (size_t)(8 * 480));
}

/* ---- OOK structure: energy on '1' bits, silence on '0' bits --------------- */

namespace {

double rms(const std::vector<float>& v, size_t begin, size_t end) {
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = begin; i < end && i < v.size(); ++i) {
        acc += static_cast<double>(v[i]) * v[i];
        ++n;
    }
    return n ? std::sqrt(acc / n) : 0.0;
}

int sign_of(float x) { return x > 0.0f ? 1 : (x < 0.0f ? -1 : 0); }

int zero_crossings(const std::vector<float>& v, size_t begin, size_t end) {
    int count = 0;
    int prev = 0;
    for (size_t i = begin; i < end && i < v.size(); ++i) {
        const int s = sign_of(v[i]);
        if (s != 0) {
            if (prev != 0 && s != prev) ++count;
            prev = s;
        }
    }
    return count;
}

}  // namespace

TEST(cart_ook_gates_carrier_by_code) {
    const size_t spb = samples_per_bit(48000.0f, kBitSeconds);
    const auto wf = generate_ook(kLockCode, kCarrierHz, 48000.0f, kBitSeconds);

    for (size_t bit = 0; bit < 8; ++bit) {
        const double e = rms(wf, bit * spb, (bit + 1) * spb);
        if (code_bit(kLockCode, bit)) {
            CHECK(e > 0.1);  /* '1' -> carrier present */
        } else {
            CHECK_EQ(e, 0.0);  /* '0' -> exact silence */
        }
    }
}

TEST(cart_ook_unlock_pattern) {
    const size_t spb = samples_per_bit(48000.0f, kBitSeconds);
    const auto wf = generate_ook(kUnlockCode, kCarrierHz, 48000.0f, kBitSeconds);
    for (size_t bit = 0; bit < 8; ++bit) {
        const double e = rms(wf, bit * spb, (bit + 1) * spb);
        if (code_bit(kUnlockCode, bit))
            CHECK(e > 0.1);
        else
            CHECK_EQ(e, 0.0);
    }
}

/* ---- carrier frequency via zero crossings --------------------------------- */

TEST(cart_carrier_frequency) {
    const float rate = 48000.0f;
    const size_t spb = samples_per_bit(rate, kBitSeconds);
    const auto wf = generate_ook(kLockCode, 7800.0f, rate, kBitSeconds);

    /* Bit 0 (MSB) of 0x8E is '1', so its segment is a clean 7800 Hz sine.
     * cycles = 7800 * bit_seconds; zero crossings ~= 2 * cycles. */
    const double cycles = 7800.0 * kBitSeconds;
    const int expected = static_cast<int>(2.0 * cycles + 0.5);  /* ~156 */
    const int cross = zero_crossings(wf, 0, spb);
    CHECK(cross >= expected - 2 && cross <= expected + 2);
}

TEST(cart_carrier_frequency_scales) {
    const float rate = 48000.0f;
    const size_t spb = samples_per_bit(rate, kBitSeconds);
    const auto wf = generate_ook(kLockCode, 6000.0f, rate, kBitSeconds);

    const double cycles = 6000.0 * kBitSeconds;  /* 60 */
    const int expected = static_cast<int>(2.0 * cycles + 0.5);  /* ~120 */
    const int cross = zero_crossings(wf, 0, spb);
    CHECK(cross >= expected - 2 && cross <= expected + 2);
}

/* ---- amplitude ------------------------------------------------------------ */

TEST(cart_amplitude_bounded) {
    const auto wf = generate_ook(kLockCode, kCarrierHz, 48000.0f, kBitSeconds, 0.9f);
    float peak = 0.0f;
    for (float s : wf) peak = std::max(peak, std::fabs(s));
    CHECK(peak <= 0.9f + 1e-4f);
    CHECK(peak > 0.5f);  /* the on-bits are actually audible */
}
