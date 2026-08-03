/*
 * mayhem-b200 — OOK / Encoders (ooktx) tests.
 *
 * The load-bearing, hardware-free parts of the OOK TX app are the preset
 * encoder table, the frame generator, the MSB-first bit packing, the
 * samples-per-bit arithmetic and the de Bruijn sequence generator. Every value
 * here is checked against the PortaPack sources (application/protocols/
 * encoders.hpp and baseband/proc_ook.cpp) or against the de Bruijn coverage
 * property, never against whatever the code happens to emit.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "modulate.hpp"      /* dsp::OokKeyer, dsp::bit_at */
#include "ui_encoders.hpp"   /* app::ook::* */

#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace app::ook;

namespace {

/* Every cyclic n-window of a length-2^n sequence must be a distinct n-bit code,
 * which is the defining property of a binary de Bruijn sequence B(2,n). */
bool covers_every_code_once(const std::vector<uint8_t>& seq, unsigned n) {
    const size_t L = seq.size();
    if (L != (size_t{1} << n)) return false;
    std::set<uint32_t> seen;
    for (size_t i = 0; i < L; i++) {
        uint32_t w = 0;
        for (unsigned j = 0; j < n; j++)
            w = (w << 1) | seq[(i + j) % L];
        seen.insert(w);
    }
    return seen.size() == L;  // L distinct windows over [0,2^n) => all present
}

}  // namespace

/* --- preset table pinned to upstream encoders.hpp --------------------------- */

TEST(encoders_table_matches_upstream_pt2262) {
    const auto& def = encoder_defs[2];
    CHECK_STR_EQ(std::string{def.name}, "2262   ");
    CHECK_EQ(def.clk_per_symbol, uint16_t{32});
    CHECK_EQ(def.clk_per_fragment, uint16_t{4});
    CHECK_STR_EQ(std::string{def.bit_format[0]}, "10001000");
    CHECK_STR_EQ(std::string{def.bit_format[1]}, "11101110");
    CHECK_STR_EQ(std::string{def.bit_format[2]}, "10001110");
    CHECK_EQ(def.word_length, uint8_t{12});
    CHECK_STR_EQ(std::string{def.word_format}, "AAAAAAAAAAAAS");
    CHECK_STR_EQ(std::string{def.sync}, "10000000000000000000000000000000");
    CHECK_EQ(def.default_speed, uint32_t{20000});
    CHECK_EQ(def.repeat_min, uint8_t{4});
}

TEST(encoders_table_matches_upstream_pt2260r2) {
    const auto& def = encoder_defs[0];
    CHECK_STR_EQ(std::string{def.name}, "2260-R2");
    CHECK_EQ(def.clk_per_symbol, uint16_t{1024});
    CHECK_EQ(def.clk_per_fragment, uint16_t{128});
    CHECK_STR_EQ(std::string{def.word_format}, "AAAAAAAAAADDS");
    CHECK_STR_EQ(std::string{def.sync}, "10000000000000000000000000000000");
    CHECK_EQ(def.default_speed, uint32_t{150000});
    CHECK_EQ(def.repeat_min, uint8_t{2});
}

TEST(encoders_um3750_index_is_stable) {
    /* Upstream guards this: ENCODER_UM3750 must still point at UM3750. */
    CHECK_STR_EQ(std::string{encoder_defs[kEncoderUm3750].name}, "UM3750 ");
}

/* --- frame generation ------------------------------------------------------- */

TEST(generate_frame_pt2262_known_word) {
    const auto& def = encoder_defs[2];  // PT2262, 12 address symbols + sync

    /* Word "01F" repeated four times: offsets 0,1,2 into "01F". */
    const std::vector<size_t> offsets = {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2};
    const std::string frame = generate_frame(def, offsets);

    /* Independently written from the spec: 0->10001000, 1->11101110,
     * F->10001110, then the 32-bit sync word. */
    const std::string unit = "100010001110111010001110";
    CHECK_STR_EQ(frame.substr(0, 24), unit);
    CHECK_EQ(frame.size(), size_t{4 * 24 + 32});
    CHECK_STR_EQ(frame.substr(96), "10000000000000000000000000000000");

    /* And composed from the table fragments, in order. */
    const std::string expected =
        std::string{def.bit_format[0]} + def.bit_format[1] + def.bit_format[2] +
        def.bit_format[0] + def.bit_format[1] + def.bit_format[2] +
        def.bit_format[0] + def.bit_format[1] + def.bit_format[2] +
        def.bit_format[0] + def.bit_format[1] + def.bit_format[2] + def.sync;
    CHECK_STR_EQ(frame, expected);
}

TEST(generate_frame_pt2260r2_address_and_data) {
    const auto& def = encoder_defs[0];  // 10 address + 2 data + sync

    /* 10 address '0' (offset 0) then 2 data '1' (offset 1). */
    std::vector<size_t> offsets(12, 0);
    offsets[10] = 1;
    offsets[11] = 1;
    const std::string frame = generate_frame(def, offsets);

    std::string expected;
    for (int i = 0; i < 10; i++) expected += def.bit_format[0];  // "10001000"
    for (int i = 0; i < 2; i++) expected += def.bit_format[1];   // "11101110"
    expected += def.sync;
    CHECK_STR_EQ(frame, expected);
    CHECK_EQ(frame.size(), size_t{10 * 8 + 2 * 8 + 32});
}

/* --- bit packing (make_bitstream) ------------------------------------------ */

TEST(pack_fragments_full_byte) {
    std::vector<uint8_t> out;
    const size_t bits = pack_fragments("10110010", out);
    CHECK_EQ(bits, size_t{8});
    CHECK_EQ(out.size(), size_t{1});
    CHECK_EQ(out[0], uint8_t{0xB2});  // 10110010b
}

TEST(pack_fragments_partial_byte_left_justified) {
    std::vector<uint8_t> out;
    const size_t bits = pack_fragments("101", out);
    CHECK_EQ(bits, size_t{3});
    CHECK_EQ(out.size(), size_t{1});
    CHECK_EQ(out[0], uint8_t{0xA0});  // 101 shifted into the high bits
}

TEST(pack_fragments_empty) {
    std::vector<uint8_t> out;
    const size_t bits = pack_fragments("", out);
    CHECK_EQ(bits, size_t{0});
    CHECK_EQ(out.size(), size_t{0});
}

TEST(pack_fragments_round_trips_through_bit_at) {
    const std::string frags = "1000100011101110100011100000000110";
    std::vector<uint8_t> out;
    const size_t bits = pack_fragments(frags, out);
    CHECK_EQ(bits, frags.size());
    for (size_t i = 0; i < bits; i++) {
        const bool want = (frags[i] == '1');
        CHECK_EQ(dsp::bit_at(out.data(), i), want);
    }
}

/* --- samples per bit / keyer timing ---------------------------------------- */

TEST(samples_per_bit_pt2262) {
    /* PT2262: 20 kHz clock / 4 clocks-per-fragment => 5 kBd fragments; at
     * 2.28 Msps that is 456 samples a fragment. */
    CHECK_EQ(samples_per_bit(20, 4), uint32_t{456});
}

TEST(samples_per_bit_pt2260r2_integer_arithmetic) {
    /* 150 kHz / 128 = 1171 Bd (integer), 2.28M / 1171 = 1947 samples, matching
     * upstream's integer division exactly. */
    CHECK_EQ(samples_per_bit(150, 128), uint32_t{1947});
}

TEST(ook_keyer_samples_per_symbol_matches_pt2262) {
    dsp::OokKeyer k;
    k.configure(static_cast<float>(kOokSampleRate), 5000.0f);  // PT2262 fragment rate
    CHECK_NEAR(k.samples_per_symbol(), 456.0, 1e-9);
}

TEST(ook_keyer_emits_packed_bits_at_symbol_centres) {
    /* End-to-end: pack a frame, key it, and read the level back at each symbol
     * centre. This proves the packing bit order and the samples-per-symbol
     * timing agree. */
    const auto& def = encoder_defs[2];
    const std::vector<size_t> offsets = {0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::string frame = generate_frame(def, offsets);

    std::vector<uint8_t> bytes;
    const size_t bits = pack_fragments(frame, bytes);

    dsp::OokKeyer k;
    k.configure(static_cast<float>(kOokSampleRate), 5000.0f);
    k.set_data(bytes.data(), bits);

    const double sps = k.samples_per_symbol();
    std::vector<dsp::cfloat> out(static_cast<size_t>(k.total_samples()));
    const size_t written = k.process(out.data(), out.size());
    CHECK_EQ(written, out.size());

    for (size_t i = 0; i < bits; i++) {
        const size_t centre = static_cast<size_t>((static_cast<double>(i) + 0.5) * sps);
        const float want = (frame[i] == '1') ? 1.0f : 0.0f;
        CHECK_NEAR(out[centre].real(), want, 1e-6);
    }
}

/* --- de Bruijn -------------------------------------------------------------- */

TEST(de_bruijn_order3_exact_sequence) {
    const auto seq = de_bruijn_sequence(2, 3);
    const std::vector<uint8_t> expected = {0, 0, 0, 1, 0, 1, 1, 1};
    CHECK_EQ(seq.size(), size_t{8});
    CHECK(seq == expected);
}

TEST(de_bruijn_covers_every_code_once) {
    for (unsigned n = 3; n <= 8; n++) {
        const auto seq = de_bruijn_sequence(2, n);
        CHECK_EQ(seq.size(), (size_t{1} << n));
        CHECK(covers_every_code_once(seq, n));
    }
}

TEST(de_bruijn_ternary_covers_every_code_once) {
    /* proc_ook supports ternary encoders too (duval_symbols = 3). B(3,3) has
     * length 27 and every base-3 3-tuple appears once. */
    const auto seq = de_bruijn_sequence(3, 3);
    CHECK_EQ(seq.size(), size_t{27});
    std::set<uint32_t> seen;
    const size_t L = seq.size();
    for (size_t i = 0; i < L; i++) {
        uint32_t w = 0;
        for (unsigned j = 0; j < 3; j++)
            w = w * 3 + seq[(i + j) % L];
        seen.insert(w);
    }
    CHECK_EQ(seen.size(), size_t{27});
}

TEST(de_bruijn_fragments_order3) {
    /* Each symbol expands via proc_ook's encoding: 0 -> 1000, 1 -> 1110. */
    const std::string expected =
        std::string{"1000"} + "1000" + "1000" + "1110" + "1000" + "1110" + "1110" + "1110";
    CHECK_STR_EQ(de_bruijn_fragments(3), expected);
}
