/*
 * mayhem-b200 — tests for the RTTY TX Baudot/ITA2 encoder and framing.
 *
 * The expected 5-bit codes are read straight off upstream's ITA2 tables
 * (external/rtty_tx/baudot.cpp): LETTERS index for R=10, T=16, Y=21, A=3, and
 * FIGURES index for '1'=23. FIGS=0x1B, LTRS=0x1F, SPACE=0x04. The framing bit
 * order is proc_rtty_tx.cpp: 15x LTRS + CR + LF preamble, each code sent as
 * start(space) + d0..d4 (LSB first) + stop(mark), trailing CR + LF.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_rtty_tx.hpp"

#include "../dsp/modulate.hpp"

#include <cmath>
#include <complex>
#include <string>
#include <vector>

using namespace app::rtty_tx;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

/* Instantaneous frequency in Hz between two complex samples. */
double inst_freq(const std::vector<dsp::cfloat>& x, size_t i, double fs) {
    const dsp::cfloat d = x[i] * std::conj(x[i - 1]);
    return std::atan2(static_cast<double>(d.imag()), static_cast<double>(d.real())) *
           fs / kTwoPi;
}

}  // namespace

/* --- Baudot encoder -------------------------------------------------------- */

TEST(rtty_baudot_encodes_letters) {
    /* "RTTY" is all in the LETTERS shift, so no shift codes appear. */
    const auto codes = baudot_encode("RTTY");
    const std::vector<uint8_t> want{10, 16, 16, 21};
    CHECK_EQ(codes.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i) CHECK_EQ(codes[i], want[i]);
}

TEST(rtty_baudot_letters_to_figures_shift) {
    /* "RTTY 1": the digit forces a LETTERS->FIGURES shift (FIGS=0x1B) before the
     * '1' code (FIGURES index 23). Space (0x04) needs no shift. */
    const auto codes = baudot_encode("RTTY 1");
    const std::vector<uint8_t> want{10, 16, 16, 21, CODE_SPACE, CODE_FIGS, 23};
    CHECK_EQ(codes.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i) CHECK_EQ(codes[i], want[i]);
}

TEST(rtty_baudot_figures_to_letters_shift) {
    /* "1A": FIGS then '1'(23), then LTRS then 'A'(3) — both shift directions. */
    const auto codes = baudot_encode("1A");
    const std::vector<uint8_t> want{CODE_FIGS, 23, CODE_LTRS, 3};
    CHECK_EQ(codes.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i) CHECK_EQ(codes[i], want[i]);
}

TEST(rtty_baudot_lowercase_is_uppercased) {
    CHECK_EQ(baudot_encode("rtty"), baudot_encode("RTTY"));
}

TEST(rtty_baudot_empty_is_empty) {
    CHECK(baudot_encode("").empty());
}

TEST(rtty_baudot_round_trip_through_decoder) {
    /* Encode, then decode with a fresh coder: shift codes vanish and the text
     * (upper-cased) comes back. */
    const std::string src = "RTTY 1A";
    const auto codes = baudot_encode(src);

    BaudotCoder dec;
    dec.set_usos(false);
    std::string got;
    for (uint8_t c : codes) {
        char ch = dec.decode(c);
        if (ch) got.push_back(ch);
    }
    CHECK_STR_EQ(got, "RTTY 1A");
}

/* --- Framing (proc_rtty_tx.cpp) -------------------------------------------- */

TEST(rtty_frame_char_layout) {
    /* T = code 16 = 0b10000: start(0) d0..d4=0,0,0,0,1 stop(1) with 1 stop bit. */
    std::vector<uint8_t> bits;
    rtty_frame_char(bits, 16, 1);
    const std::vector<uint8_t> want{0, 0, 0, 0, 0, 1, 1};
    CHECK_EQ(bits.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i) CHECK_EQ(bits[i], want[i]);
}

TEST(rtty_frame_char_two_stop_bits) {
    /* LTRS = 0x1F = 0b11111: start(0) d0..d4=1,1,1,1,1 stop(1)(1). */
    std::vector<uint8_t> bits;
    rtty_frame_char(bits, CODE_LTRS, 2);
    const std::vector<uint8_t> want{0, 1, 1, 1, 1, 1, 1, 1};
    CHECK_EQ(bits.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i) CHECK_EQ(bits[i], want[i]);
}

TEST(rtty_frame_preamble_and_length) {
    /* One data code, 2 stop bits: 15 LTRS + CR + LF + 1 data + CR + LF = 20
     * characters, each 1 start + 5 data + 2 stop = 8 bits -> 160 bits. */
    const std::vector<uint8_t> data{16};  // T
    const auto bits = rtty_build_frame_bits(data, 2);
    CHECK_EQ(bits.size(), size_t{20 * 8});

    /* First character is a LTRS (idle): start 0, then five 1s, then two stop 1s. */
    const std::vector<uint8_t> first{0, 1, 1, 1, 1, 1, 1, 1};
    for (size_t i = 0; i < first.size(); ++i) CHECK_EQ(bits[i], first[i]);

    /* The data character sits after 15 LTRS + CR + LF = 17 chars = 136 bits. */
    const size_t off = 17 * 8;
    const std::vector<uint8_t> tchar{0, 0, 0, 0, 0, 1, 1, 1};  // T=16, LSB first
    for (size_t i = 0; i < tchar.size(); ++i) CHECK_EQ(bits[off + i], tchar[i]);
}

TEST(rtty_frame_stop_bit_count_changes_length) {
    const std::vector<uint8_t> data{16};
    CHECK_EQ(rtty_build_frame_bits(data, 1).size(), size_t{20 * 7});
    CHECK_EQ(rtty_build_frame_bits(data, 2).size(), size_t{20 * 8});
}

TEST(rtty_pack_bits_msb_first) {
    /* 1,0,1,1,0,0,1,0 -> 0xB2. */
    const std::vector<uint8_t> bits{1, 0, 1, 1, 0, 0, 1, 0};
    const auto bytes = pack_bits_msb(bits);
    CHECK_EQ(bytes.size(), size_t{1});
    CHECK_EQ(bytes[0], uint8_t{0xB2});
}

/* --- FSK keyer mark/space output ------------------------------------------- */

TEST(rtty_fsk_keyer_emits_mark_and_space) {
    /* With deviation = shift/2, a mark bit (1) sits at +shift/2 and a space bit
     * (0) at -shift/2 — the two RTTY tones, `shift` apart, centred on the
     * carrier. Feed a 1,0,1,0 pattern and measure the mid-symbol frequency. */
    constexpr double fs = 48000.0;
    constexpr double shift = 850.0;
    constexpr double deviation = shift / 2.0;  // 425 Hz
    constexpr double baud = 100.0;             // 480 samples per symbol

    const std::vector<uint8_t> pattern{1, 0, 1, 0};
    const auto packed = pack_bits_msb(pattern);

    dsp::FskKeyer keyer;
    keyer.configure(static_cast<float>(fs), static_cast<float>(baud),
                    static_cast<float>(deviation));
    keyer.set_data(packed.data(), pattern.size());

    std::vector<dsp::cfloat> out(4 * 480);
    keyer.process(out.data(), out.size());

    const size_t spb = 480;
    for (size_t bit = 0; bit < pattern.size(); ++bit) {
        const double want = (pattern[bit] == 1) ? +deviation : -deviation;
        CHECK_NEAR(inst_freq(out, bit * spb + spb / 2, fs), want, 1.0);
    }
}

TEST(rtty_fsk_inverted_swaps_tones) {
    /* Negating the deviation (the app's "Inverted" path) swaps mark and space. */
    constexpr double fs = 48000.0;
    constexpr double deviation = -425.0;
    constexpr double baud = 100.0;

    const std::vector<uint8_t> pattern{1, 0};
    const auto packed = pack_bits_msb(pattern);

    dsp::FskKeyer keyer;
    keyer.configure(static_cast<float>(fs), static_cast<float>(baud),
                    static_cast<float>(deviation));
    keyer.set_data(packed.data(), pattern.size());

    std::vector<dsp::cfloat> out(2 * 480);
    keyer.process(out.data(), out.size());

    /* Now a 1 bit is the LOW tone and a 0 bit is the HIGH tone. */
    CHECK_NEAR(inst_freq(out, 240, fs), -425.0, 1.0);
    CHECK_NEAR(inst_freq(out, 720, fs), +425.0, 1.0);
}
