/*
 * mayhem-b200 — POCSAG transmitter (encoder) tests.
 *
 * The deliverable is the encoder, so every expectation here is derived from the
 * POCSAG standard or from an independent re-derivation in this file, never from
 * what the code under test happens to emit:
 *
 *   - The preamble is 576 alternating bits = 18 codewords of 0xAAAAAAAA; the
 *     batch begins with the frame sync codeword 0x7CD215D8.
 *   - BCH generation is checked by feeding each produced codeword back through
 *     the decoder's error_correct(): a correctly generated word must report
 *     zero errors, which is exactly the check a receiver applies.
 *   - The address codeword places RIC bits 21..3 at bits 30..13 and a 2-bit
 *     function at bits 12..11, and sits in the batch slot (RIC & 7) * 2.
 *   - 7-bit alphanumeric packing is checked against a hand-computed codeword
 *     ('A' -> 0xC1000000 payload) and against an independent LSB-first bit
 *     packer (ref_alpha_msg_codewords), then round-tripped through the Phase A
 *     decoder.
 *   - Numeric and address-only pages round-trip through the decoder too.
 *   - Polarity inversion is the bitwise complement; byte packing is MSB-first.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "../src/apps/ui_pocsag_tx.hpp"

#include <string>
#include <vector>

using namespace pocsag;
using namespace app::pocsag_tx;

namespace {

const EccContainer& ecc() {
    static EccContainer e;
    return e;
}

/* Split a full codeword stream (preamble + sync + 16-word batches) into the
 * 16-codeword batches a decoder consumes. */
std::vector<POCSAGPacket> batches_of(const std::vector<uint32_t>& cws) {
    std::vector<POCSAGPacket> out;
    const size_t start = preamble_length / 32;
    for (size_t p = start; p + 16 < cws.size() + 1; p += 17) {
        if (cws[p] != syncword) break;
        batch_t b{};
        for (size_t i = 0; i < 16; i++) b[i] = cws[p + 1 + i];
        POCSAGPacket pk;
        pk.set(b);
        out.push_back(pk);
    }
    return out;
}

/* Decode a whole page back to (address, function, text). */
struct Decoded {
    uint32_t address = 0;
    uint32_t function = 99;
    std::string text{};
    std::string numeric{};
    DetectedType detected = DET_UNKNOWN;
};

Decoded decode_page(const std::vector<uint32_t>& cws) {
    Decoded d;
    POCSAGState st{&ecc()};
    for (auto& pk : batches_of(cws)) {
        st.codeword_index = 0;
        st.errors = 0;
        while (pocsag_decode_batch(pk, st)) {
            if (st.out_type == OUT_MESSAGE) {
                d.text += st.output;
                d.address = st.address;
                d.function = st.function;
            } else if (st.out_type == OUT_ADDRESS) {
                d.address = st.address;
                d.function = st.function;
            }
        }
        if (st.mode != STATE_HAVE_ADDRESS && st.out_type == OUT_MESSAGE) {
            d.text += st.output;
            d.address = st.address;
            d.function = st.function;
        }
        if (st.numeric_len) d.numeric.assign(st.numeric_buf, st.numeric_len);
        if (st.detected != DET_UNKNOWN) d.detected = st.detected;
    }
    return d;
}

/* Independent 7-bit LSB-first alpha packer: 20 payload bits/codeword at bits
 * 30..11, flag bit 31 set, then BCH via the decoder's encode(). */
std::vector<uint32_t> ref_alpha_msg_codewords(const std::string& msg) {
    std::vector<int> bits;
    for (char ch : msg) {
        const uint8_t c = static_cast<uint8_t>(ch) & 0x7F;
        for (int b = 0; b < 7; b++) bits.push_back((c >> b) & 1);
    }
    std::vector<uint32_t> cws;
    size_t i = 0;
    while (i < bits.size()) {
        uint32_t cw = 0x80000000u;
        for (int p = 0; p < 20; p++) {
            const int v = (i < bits.size()) ? bits[i] : 0;
            cw |= static_cast<uint32_t>(v) << (30 - p);
            i++;
        }
        cws.push_back(ecc().encode(cw));
    }
    return cws;
}

/* The message codewords (flag bit set) of the first batch, in order. */
std::vector<uint32_t> first_batch_message_codewords(const std::vector<uint32_t>& cws) {
    std::vector<uint32_t> out;
    const size_t start = preamble_length / 32;  // sync index
    for (size_t i = start + 1; i < start + 1 + 16 && i < cws.size(); i++)
        if (cws[i] & 0x80000000u) out.push_back(cws[i]);
    return out;
}

}  // namespace

/* --- preamble + sync structure ------------------------------------------- */

TEST(pocsag_tx_preamble_is_576_alternating_bits) {
    const auto cws = encode_codewords(ADDRESS_ONLY, ecc(), 0, "", 42);
    const size_t preamble_words = preamble_length / 32;  // 576 / 32 = 18
    for (size_t i = 0; i < preamble_words; i++)
        CHECK_EQ(cws[i], 0xAAAAAAAAu);
    /* The batch opens with the frame sync codeword, so word 18 is the sync and
     * word 17 is still preamble — pinning the preamble at exactly 18 words. */
    CHECK_EQ(cws[preamble_words], syncword);       // 0x7CD215D8
    CHECK_EQ(cws[preamble_words - 1], 0xAAAAAAAAu);
}

TEST(pocsag_tx_batch_has_sync_plus_sixteen_codewords) {
    const auto cws = encode_codewords(ADDRESS_ONLY, ecc(), 0, "", 42);
    /* preamble(18) + sync(1) + 16 codewords = 35 for an address-only page. */
    CHECK_EQ(cws.size(), 18u + 1u + 16u);
}

/* --- BCH generation, checked with the decoder's own error_correct --------- */

TEST(pocsag_tx_address_codeword_passes_the_decoder_bch_check) {
    /* RIC 1337007, function 1 (B). */
    const uint32_t ric = 1337007;
    const uint32_t func = 1;
    const auto cws = encode_codewords(ADDRESS_ONLY, ecc(), func, "", ric);

    const size_t sync_idx = preamble_length / 32;
    const uint32_t slot = (ric & 7u) * 2;
    const uint32_t addr_cw = cws[sync_idx + 1 + slot];

    /* An address codeword has flag bit 0. */
    CHECK_EQ(addr_cw & 0x80000000u, 0u);

    /* The check a receiver applies: a correctly generated codeword reports zero
     * BCH errors and is returned unchanged. */
    uint32_t v = addr_cw;
    CHECK_EQ(ecc().error_correct(v), 0);
    CHECK_EQ(v, addr_cw);

    /* And it must equal the standard's layout: RIC bits 21..3 at 30..13, the
     * function at bits 12..11, BCH+parity below. */
    const uint32_t expected = ecc().encode(((ric & 0x1FFFF8u) << 10) | (func << 11));
    CHECK_EQ(addr_cw, expected);

    /* Every other slot in the batch is idle. */
    for (uint32_t i = 0; i < 16; i++)
        if (i != slot)
            CHECK_EQ(cws[sync_idx + 1 + i], idleword);
}

TEST(pocsag_tx_all_generated_codewords_decode_clean) {
    const auto cws = encode_codewords(ALPHANUMERIC, ecc(), 3, "PORTAPACK", 1234567);
    const size_t sync_idx = preamble_length / 32;
    /* Every codeword after the sync must be a valid BCH word. */
    for (size_t i = sync_idx + 1; i < cws.size(); i++) {
        if (cws[i] == syncword) continue;  // batch boundaries
        uint32_t v = cws[i];
        CHECK_EQ(ecc().error_correct(v), 0);
        CHECK_EQ(v, cws[i]);
    }
}

/* --- 7-bit alphanumeric packing ------------------------------------------ */

TEST(pocsag_tx_single_char_alpha_matches_hand_computed_codeword) {
    /* 'A' = 0x41. Sent LSB-first, the payload lands as bit 30 and bit 24 set,
     * i.e. 0x41000000, with the message flag bit 31 -> 0xC1000000. */
    const auto cws = encode_codewords(ALPHANUMERIC, ecc(), 3, "A", 8 /* slot 0 */);
    const size_t sync_idx = preamble_length / 32;
    const uint32_t msg_cw = cws[sync_idx + 1 + 1];  // slot 0 = address, slot 1 = message
    CHECK_EQ(msg_cw, ecc().encode(0xC1000000u));
}

TEST(pocsag_tx_alpha_packing_matches_independent_bit_packer) {
    const auto cws = encode_codewords(ALPHANUMERIC, ecc(), 3, "HELLO", 8 /* slot 0 */);
    const auto got = first_batch_message_codewords(cws);
    const auto ref = ref_alpha_msg_codewords("HELLO");
    CHECK_EQ(got.size(), ref.size());
    for (size_t i = 0; i < got.size() && i < ref.size(); i++)
        CHECK_EQ(got[i], ref[i]);
}

/* --- round trips through the Phase A decoder ------------------------------ */

TEST(pocsag_tx_alphanumeric_round_trip) {
    const auto cws = encode_codewords(ALPHANUMERIC, ecc(), 3, "HELLO WORLD", 1234567);
    const auto d = decode_page(cws);
    CHECK_EQ(d.address, 1234567u);
    CHECK_EQ(d.function, 3u);
    CHECK_STR_EQ(d.text.substr(0, 11), "HELLO WORLD");
}

TEST(pocsag_tx_numeric_round_trip) {
    const auto cws = encode_codewords(NUMERIC_ONLY, ecc(), 0, "1234567890", 98765);
    const auto d = decode_page(cws);
    CHECK_EQ(d.address, 98765u);
    CHECK_EQ(d.detected, DET_NUMERIC);
    CHECK_STR_EQ(d.numeric, "1234567890");
}

TEST(pocsag_tx_address_only_round_trip) {
    const auto cws = encode_codewords(ADDRESS_ONLY, ecc(), 2, "", 42);
    const auto d = decode_page(cws);
    CHECK_EQ(d.address, 42u);
    CHECK_EQ(d.function, 2u);
}

/* --- boundary payloads --------------------------------------------------- */

TEST(pocsag_tx_empty_alpha_is_address_plus_idle) {
    const auto cws = encode_codewords(ALPHANUMERIC, ecc(), 3, "", 4242);
    /* preamble + sync + exactly one 16-word batch: address then idle fill. */
    CHECK_EQ(cws.size(), 18u + 1u + 16u);
    const auto d = decode_page(cws);
    CHECK_EQ(d.address, 4242u);
    /* No message content survived (idle fill only). */
    CHECK(d.text.empty());
}

TEST(pocsag_tx_max_address_encodes_without_overflow) {
    const uint32_t ric = max_address;  // 0x1FFFFF = 2097151
    const auto cws = encode_codewords(ADDRESS_ONLY, ecc(), 0, "", ric);
    const size_t sync_idx = preamble_length / 32;
    const uint32_t slot = (ric & 7u) * 2;
    uint32_t v = cws[sync_idx + 1 + slot];
    CHECK_EQ(ecc().error_correct(v), 0);
    const auto d = decode_page(cws);
    CHECK_EQ(d.address, ric);
}

/* --- byte packing + polarity --------------------------------------------- */

TEST(pocsag_tx_bytes_are_msb_first) {
    const auto cws = encode_codewords(ADDRESS_ONLY, ecc(), 0, "", 42);
    const auto bytes = codewords_to_bytes(cws, /*invert=*/false);
    CHECK_EQ(bytes.size(), cws.size() * 4);
    /* Preamble byte 0. */
    CHECK_EQ(bytes[0], 0xAAu);
    /* Sync word bytes, big-endian, at codeword index 18. */
    const size_t o = (preamble_length / 32) * 4;
    CHECK_EQ(bytes[o + 0], 0x7Cu);
    CHECK_EQ(bytes[o + 1], 0xD2u);
    CHECK_EQ(bytes[o + 2], 0x15u);
    CHECK_EQ(bytes[o + 3], 0xD8u);
}

TEST(pocsag_tx_standard_polarity_inverts_every_bit) {
    const auto cws = encode_codewords(ADDRESS_ONLY, ecc(), 0, "", 42);
    const auto normal = codewords_to_bytes(cws, /*invert=*/false);
    const auto inverted = codewords_to_bytes(cws, /*invert=*/true);
    CHECK_EQ(normal.size(), inverted.size());
    for (size_t i = 0; i < normal.size(); i++)
        CHECK_EQ(static_cast<uint8_t>(~normal[i]), inverted[i]);
    /* Preamble 0xAA -> 0x55 under inversion. */
    CHECK_EQ(inverted[0], 0x55u);
}
