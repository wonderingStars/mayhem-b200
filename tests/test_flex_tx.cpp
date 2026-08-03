/*
 * mayhem-b200 — FLEX transmitter (encoder) tests.
 *
 * The encoder is the deliverable, so expectations come from the FLEX standard,
 * from hand computation, or from the independent Phase A FLEX decoder's own
 * BCH/bit-order code (flex::BchEcc in ui_flex_rx.hpp) — never from what the
 * encoder happens to emit:
 *
 *   - BCH(31,21): the TX encoder builds a word by systematic division by the
 *     generator 0x769 over the bit-reversed 21-bit info field. The decoder
 *     builds the same word a completely different way (a syndrome table). They
 *     must agree: flex_enc(x) == bit_reverse_32(make_word(x)) for all x, and
 *     every produced word must pass the decoder's fix_errors() with 0 errors.
 *   - The frame-info / BIW nibble checksum makes the 21-bit word's nibble sum
 *     0xF, which the standard requires and the decoder checks.
 *   - Short address = flex_enc(capcode + 0x8000); the long-address arithmetic
 *     is hand-computed at the 2101249 boundary.
 *   - Block interleave is a bit transpose, verified by de-interleaving.
 *   - Sync/FIW/S2 framing bytes and lengths are the standard's.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "../src/apps/ui_flex_tx.hpp"
#include "../src/apps/ui_flex_rx.hpp"  /* flex::BchEcc, flex::bit_reverse_32 */

#include <cstdint>
#include <string>
#include <vector>

using namespace app::flex_tx;

namespace {

const flex::BchEcc& bch() {
    static flex::BchEcc e;
    return e;
}

/* De-interleave 32 bytes back into 8 words (inverse of the transpose). */
void deinterleave(const uint8_t* bytes, uint32_t out8[8]) {
    for (int j = 0; j < 8; j++) out8[j] = 0;
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 8; j++)
            if ((bytes[i] >> (7 - j)) & 1u)
                out8[j] |= 1u << (31 - i);
}

/* Simple deterministic PRNG so the sweep is reproducible. */
uint32_t lcg(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s;
}

}  // namespace

/* --- bit reversal -------------------------------------------------------- */

TEST(flex_tx_reverse_bits32_known_values) {
    CHECK_EQ(reverse_bits32(0x00000001u), 0x80000000u);
    CHECK_EQ(reverse_bits32(0x80000000u), 0x00000001u);
    CHECK_EQ(reverse_bits32(0xFFFFFFFFu), 0xFFFFFFFFu);
    CHECK_EQ(reverse_bits32(0x00000000u), 0x00000000u);
    CHECK_EQ(reverse_bits32(0x0000000Fu), 0xF0000000u);
    /* Involution. */
    CHECK_EQ(reverse_bits32(reverse_bits32(0x12345678u)), 0x12345678u);
}

/* --- BCH generation against the independent decoder ----------------------- */

TEST(flex_tx_bch_matches_decoder_make_word) {
    /* The TX polynomial encoder and the RX syndrome encoder are unrelated
     * implementations of the same code; they must produce identical words.
     * flex_enc yields the transmit-order (MSB-first) word, make_word the
     * FLEX-order (LSB-first) word, so one is the bit reverse of the other. */
    uint32_t s = 0xC0FFEE01u;
    for (int n = 0; n < 2000; n++) {
        const uint32_t x = lcg(s) & kDataMask;  // 21-bit info field
        const uint32_t tx = flex_enc(x);
        const uint32_t rx = bch().make_word(x);
        CHECK_EQ(tx, flex::bit_reverse_32(rx));
    }
    /* Edges. */
    for (uint32_t x : {0x000000u, 0x1FFFFFu, 0x000001u, 0x100000u, 0x0AAAAAu, 0x155555u}) {
        CHECK_EQ(flex_enc(x), flex::bit_reverse_32(bch().make_word(x)));
    }
}

TEST(flex_tx_every_generated_word_is_bch_valid) {
    /* fix_errors() expects a FLEX-order (LSB-first) word; flex_enc gives the
     * transmit-order word, so reverse it first. A valid word reports 0 errors. */
    uint32_t s = 0x1234ABCDu;
    for (int n = 0; n < 500; n++) {
        const uint32_t x = lcg(s) & kDataMask;
        uint32_t w = flex::bit_reverse_32(flex_enc(x));
        CHECK_EQ(bch().fix_errors(w), 0);
    }
}

TEST(flex_tx_bch_even_parity_bit) {
    /* Bit 0 of a transmit-order word is even parity over the whole 32 bits. */
    uint32_t s = 0x99887766u;
    for (int n = 0; n < 200; n++) {
        const uint32_t w = flex_enc(lcg(s) & kDataMask);
        int ones = 0;
        for (int i = 0; i < 32; i++) ones += (w >> i) & 1;
        CHECK_EQ(ones & 1, 0);
    }
}

/* --- nibble checksum ----------------------------------------------------- */

TEST(flex_tx_checksum_makes_nibble_sum_0xF) {
    /* The FLEX BIW/FIW checksum sets the low nibble so the sum of the five
     * nibbles plus bit 20 comes to 0xF (mod 16). */
    uint32_t s = 0x5A5A0001u;
    for (int n = 0; n < 500; n++) {
        const uint32_t d = lcg(s) & 0x1FFFF0u;  // leave low nibble clear
        const uint32_t cs = flex_checksum(d);
        const uint32_t sum = (cs & 0xF) + ((cs >> 4) & 0xF) + ((cs >> 8) & 0xF) +
                             ((cs >> 12) & 0xF) + ((cs >> 16) & 0xF) + ((cs >> 20) & 0x1);
        CHECK_EQ(sum & 0xF, 0xFu);
        /* Only the low nibble is altered. */
        CHECK_EQ(cs & ~0xFu, d & ~0xFu);
    }
}

TEST(flex_tx_fiw_is_valid_and_checksummed) {
    const uint32_t fiw = flex_fiw(/*cycle=*/3, /*frame=*/17, /*roaming=*/0);
    uint32_t w = flex::bit_reverse_32(fiw);
    CHECK_EQ(bch().fix_errors(w), 0);
}

/* --- addresses ----------------------------------------------------------- */

TEST(flex_tx_short_address_is_capcode_plus_0x8000) {
    const uint64_t capcode = 1234567;
    const uint32_t expected_info = (uint32_t)((capcode + 0x8000) & kDataMask);
    CHECK_EQ(flex_short_addr(capcode), flex_enc(expected_info));
    /* Independent decoder agreement. */
    CHECK_EQ(flex_short_addr(capcode),
             flex::bit_reverse_32(bch().make_word(expected_info)));
}

TEST(flex_tx_long_address_boundary_hand_computed) {
    /* First long capcode: 2101249. result = 2101249 - 2068481 = 32768.
     *   w1 = 32768 % 32768 + 1 = 1
     *   w2 = 2097151 - 32768/32768 = 2097151 - 1 = 2097150 */
    uint32_t out[2];
    CHECK_EQ(flex_long_addr(2101249ULL, out), 0);
    CHECK_EQ(out[0], flex_enc(1u));
    CHECK_EQ(out[1], flex_enc(2097150u & kDataMask));

    /* Below the long range fails. */
    uint32_t dummy[2];
    CHECK_EQ(flex_long_addr(2101248ULL, dummy), -1);
    /* Above the top range fails. */
    CHECK_EQ(flex_long_addr(4297068543ULL, dummy), -1);
}

/* --- alpha / numeric packing -------------------------------------------- */

TEST(flex_tx_alpha_words_are_valid_and_counted) {
    uint32_t words[84];
    const int wc = flex_encode_alpha("HELLO", words, 84, /*seq=*/1, /*msg_r=*/1);
    /* Header word + at least one data word for a 5-char message. */
    CHECK(wc >= 2);
    for (int i = 0; i < wc; i++) {
        uint32_t w = flex::bit_reverse_32(words[i]);
        CHECK_EQ(bch().fix_errors(w), 0);
    }
}

TEST(flex_tx_alpha_empty_still_has_header) {
    uint32_t words[84];
    const int wc = flex_encode_alpha("", words, 84, 0, 0);
    CHECK(wc >= 2);  // header + one (padded) data word
    for (int i = 0; i < wc; i++) {
        uint32_t w = flex::bit_reverse_32(words[i]);
        CHECK_EQ(bch().fix_errors(w), 0);
    }
}

TEST(flex_tx_numeric_words_are_valid) {
    uint32_t words[84];
    uint32_t k = 0;
    const int wc = flex_encode_numeric("1234567890", words, 84, &k);
    CHECK(wc >= 1);
    CHECK(k <= 0xF);
    for (int i = 0; i < wc; i++) {
        uint32_t w = flex::bit_reverse_32(words[i]);
        CHECK_EQ(bch().fix_errors(w), 0);
    }
}

/* --- block interleave ---------------------------------------------------- */

TEST(flex_tx_interleave_is_a_reversible_transpose) {
    uint32_t words[8];
    uint32_t s = 0xDEADBEEFu;
    for (int j = 0; j < 8; j++) words[j] = lcg(s);

    std::vector<uint8_t> bytes;
    flex_interleave_block_to_bytes(words, bytes);
    CHECK_EQ(bytes.size(), 32u);

    uint32_t rec[8];
    deinterleave(bytes.data(), rec);
    for (int j = 0; j < 8; j++)
        CHECK_EQ(rec[j], words[j]);
}

TEST(flex_tx_interleave_first_byte_is_msb_of_all_words) {
    /* dst[0] bit(7-j) = word[j] bit31. Set only bit31 of word 0. */
    uint32_t words[8] = {0x80000000u, 0, 0, 0, 0, 0, 0, 0};
    std::vector<uint8_t> bytes;
    flex_interleave_block_to_bytes(words, bytes);
    CHECK_EQ(bytes[0], 0x80u);  // word0 -> bit 7
    /* And word 7's MSB would land in bit 0. */
    uint32_t words2[8] = {0, 0, 0, 0, 0, 0, 0, 0x80000000u};
    std::vector<uint8_t> bytes2;
    flex_interleave_block_to_bytes(words2, bytes2);
    CHECK_EQ(bytes2[0], 0x01u);
}

/* --- framing ------------------------------------------------------------- */

TEST(flex_tx_frame_sync_and_length) {
    FlexBIWParams bp{};
    bp.send_date = 0;
    bp.send_time = 0;
    bp.send_tz = 0;
    bp.send_ssid1 = 0;
    bp.send_ssid2 = 0;

    std::vector<uint8_t> out;
    const size_t len = flex_build_frame(out, /*capcode=*/1000, /*type=*/2 /*short/tone*/,
                                        "", 0, 0, /*msg_r=*/1, bp);
    /* 14 (S1) + 4 (FIW) + 5 (S2) + 352 (11*32 interleaved) + 4 (trailing). */
    CHECK_EQ(len, 14u + 4u + 5u + 352u + 4u);
    CHECK_EQ(out.size(), len);

    /* S1: BS1 (4x0xAA), A1, B, then ~A1. */
    CHECK_EQ(out[0], 0xAAu);
    CHECK_EQ(out[1], 0xAAu);
    CHECK_EQ(out[2], 0xAAu);
    CHECK_EQ(out[3], 0xAAu);
    CHECK_EQ(out[4], 0x78u);
    CHECK_EQ(out[5], 0xF3u);
    CHECK_EQ(out[6], 0x59u);
    CHECK_EQ(out[7], 0x39u);
    CHECK_EQ(out[8], 0x55u);
    CHECK_EQ(out[9], 0x55u);
    CHECK_EQ(out[10], static_cast<uint8_t>(~0x78u));  // 0x87
    CHECK_EQ(out[11], static_cast<uint8_t>(~0xF3u));  // 0x0C
    CHECK_EQ(out[12], static_cast<uint8_t>(~0x59u));  // 0xA6
    CHECK_EQ(out[13], static_cast<uint8_t>(~0x39u));  // 0xC6
}

TEST(flex_tx_ers_pattern) {
    std::vector<uint8_t> out;
    const size_t len = flex_build_ers(out, 1);
    CHECK_EQ(len, 12u);
    CHECK_EQ(out[0], 0xAAu);
    CHECK_EQ(out[1], 0xAAu);
    CHECK_EQ(out[2], 0xCBu);  // AR
    CHECK_EQ(out[3], 0x20u);
    CHECK_EQ(out[4], 0x59u);
    CHECK_EQ(out[5], 0x39u);
    CHECK_EQ(out[6], 0x55u);
    CHECK_EQ(out[7], 0x55u);
    CHECK_EQ(out[8], static_cast<uint8_t>(~0xCBu));  // 0x34
    CHECK_EQ(out[9], static_cast<uint8_t>(~0x20u));  // 0xDF
    CHECK_EQ(out[10], static_cast<uint8_t>(~0x59u)); // 0xA6
    CHECK_EQ(out[11], static_cast<uint8_t>(~0x39u)); // 0xC6
}

TEST(flex_tx_full_transmission_short_and_long) {
    FlexBIWParams bp{};
    bp.send_ers = 1;
    bp.ers_count = 1;

    /* Short address. */
    std::vector<uint8_t> a;
    CHECK(flex_build_transmission(a, /*capcode=*/1000, /*type=*/0, "HI", bp));
    /* 42-cycle ERS run + two frames each prefixed by 8-cycle ERS. */
    const size_t ers42 = 42u * 12u;
    const size_t ers8 = 8u * 12u;
    const size_t frame = 14u + 4u + 5u + 352u + 4u;
    CHECK_EQ(a.size(), ers42 + 2u * (ers8 + frame));

    /* Long address encodes without error. */
    std::vector<uint8_t> b;
    CHECK(flex_build_transmission(b, /*capcode=*/3000000, /*type=*/1, "42", bp));
    CHECK(b.size() > 0);

    /* Out-of-range long capcode is rejected. */
    std::vector<uint8_t> c;
    CHECK(!flex_build_transmission(c, /*capcode=*/5000000000ULL, /*type=*/0, "X", bp));
}

/* --- year equivalence ---------------------------------------------------- */

TEST(flex_tx_equiv_year_in_range_and_matching) {
    /* In-range years pass through. */
    CHECK_EQ(flex_equiv_year(2015), 2015);
    CHECK_EQ(flex_equiv_year(1994), 1994);
    CHECK_EQ(flex_equiv_year(2025), 2025);
    /* Out-of-range maps to a 1994-2025 year with the same leap-ness and Jan-1
     * weekday. */
    for (int y : {2030, 2050, 2099, 1980}) {
        const int e = flex_equiv_year(y);
        CHECK(e >= 1994 && e <= 2025);
        CHECK_EQ(flex_is_leap(e), flex_is_leap(y));
        CHECK_EQ(flex_jan1_dow(e), flex_jan1_dow(y));
    }
}
