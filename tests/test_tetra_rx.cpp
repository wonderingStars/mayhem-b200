/*
 * mayhem-b200 — TETRA receiver tests.
 *
 * Every stage of the channel-decoding chain is exercised against values taken
 * from the specification or derived by hand, and then the whole chain is run
 * end to end on a burst this file builds with the *inverse* of each stage:
 *
 *   type-1 bits -> CRC16 -> tail -> rate-1/4 K=5 convolutional encode
 *               -> 2/3 puncture -> block interleave -> scramble -> burst
 *
 * and then handed to ChannelDecoder, which must return CRC-OK and the original
 * type-1 bits. The demodulator is driven with a synthesised pi/4-DQPSK signal
 * carrying a burst whose bits are known, including the inverted-polarity case.
 *
 * Nothing here needs a radio. Live reception is unverified — see the app.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_tetra_rx.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

using namespace app::tetra;

namespace {

constexpr double kPi = 3.14159265358979323846;

/* --- packed-bit helpers for building test vectors ------------------------- */

std::vector<uint8_t> pack_bits(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> out((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); i++) bits_set(out.data(), i, bits[i] != 0);
    return out;
}

std::vector<uint8_t> unpack_bits(const uint8_t* packed, size_t n_bits) {
    std::vector<uint8_t> out(n_bits, 0);
    for (size_t i = 0; i < n_bits; i++) out[i] = bits_get(packed, i);
    return out;
}

/* A repeatable pseudo-random bit stream (xorshift32), so a failure is
 * reproducible. */
struct Rng {
    uint32_t s{0x1234567u};
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    uint8_t bit() { return static_cast<uint8_t>(next() & 1u); }
};

/* --- the transmit side of one type-2 block -------------------------------- */

/* Builds the len_t5 channel bits for a block, i.e. the exact inverse of
 * ChannelDecoder::decode_block(). Writes them into `burst` at `offset`. */
void build_block(const std::vector<uint8_t>& type1_bits,
                 size_t type2_bits,
                 size_t len_t5,
                 size_t interleave_a,
                 uint32_t scramb_init,
                 uint8_t* burst,
                 size_t offset) {
    const size_t type1_len = type1_bits.size();

    /* 1. type-2 block = type-1 || ~CRC16 || 4 tail zeros. Appending the ones
     * complement of the remainder is what makes the receiver's CRC settle on
     * the fixed residue 0x1D0F. */
    std::vector<uint8_t> t2(type1_len + 16 + 4, 0);
    for (size_t i = 0; i < type1_len; i++) t2[i] = type1_bits[i];

    auto t1_packed = pack_bits(type1_bits);
    const uint16_t crc = crc16_itut_bits(t1_packed.data(), static_cast<uint32_t>(type1_len));
    const uint16_t parity = static_cast<uint16_t>(~crc);
    for (size_t i = 0; i < 16; i++) {
        t2[type1_len + i] = static_cast<uint8_t>((parity >> (15 - i)) & 1);
    }
    /* tail bits already zero */

    auto t2_packed = pack_bits(t2);

    /* 2. rate-1/4 mother code */
    std::vector<uint8_t> mother(4 * type2_bits + 64, 0);
    convolutional_encode(t2_packed.data(), static_cast<uint32_t>(type2_bits), mother.data());

    /* 3. puncture to rate 2/3 */
    std::vector<uint8_t> t3((len_t5 + 7) / 8, 0);
    puncture_2_3(mother.data(), t3.data(), static_cast<uint32_t>(len_t5));

    /* 4. interleave */
    std::vector<uint8_t> t5((len_t5 + 7) / 8, 0);
    block_interleave(t3.data(), t5.data(), static_cast<uint32_t>(len_t5),
                     static_cast<uint32_t>(interleave_a));

    /* 5. scramble (the scrambler is its own inverse) */
    descramble(t5.data(), len_t5, scramb_init);

    for (size_t i = 0; i < len_t5; i++) {
        bits_set(burst, offset + i, bits_get(t5.data(), i) != 0);
    }
}

/* --- pi/4-DQPSK modulator ------------------------------------------------- */

/* Phase change per dibit, the inverse of slice_dibit()'s quadrant map:
 *   00 -> +pi/4,  01 -> +3pi/4,  10 -> -pi/4,  11 -> -3pi/4 */
const double kDibitPhase[4] = {kPi / 4.0, 3.0 * kPi / 4.0, -kPi / 4.0, -3.0 * kPi / 4.0};

/* `bits` is one bit per element, most significant bit of each dibit first —
 * the order the demodulator emits them in. One extra leading symbol is sent so
 * the demodulator's first (reference-less) symbol does not eat a data dibit. */
std::vector<dsp::cfloat> modulate_pi4_dqpsk(const std::vector<uint8_t>& bits, size_t sps) {
    std::vector<dsp::cfloat> out;
    out.reserve((bits.size() / 2 + 1) * sps);

    double phase = 0.0;
    const auto emit = [&](uint8_t dibit) {
        phase += kDibitPhase[dibit & 3];
        for (size_t s = 0; s < sps; s++) {
            out.push_back(dsp::cfloat{static_cast<float>(std::cos(phase)),
                                      static_cast<float>(std::sin(phase))});
        }
    };

    emit(0);  /* leading reference symbol */
    for (size_t i = 0; i + 1 < bits.size(); i += 2) {
        emit(static_cast<uint8_t>((bits[i] << 1) | bits[i + 1]));
    }
    return out;
}

/* Writes a value MSB-first into a per-bit vector. */
void put_field(std::vector<uint8_t>& bits, size_t start, size_t length, uint32_t value) {
    for (size_t i = 0; i < length; i++) {
        bits[start + i] = static_cast<uint8_t>((value >> (length - 1 - i)) & 1);
    }
}

}  // namespace

/* ===========================================================================
 * CRC
 * ===========================================================================*/

TEST(tetra_crc_matches_ccitt_false_check_vector) {
    /* The Rocksoft check value: CRC-16/CCITT-FALSE("123456789") == 0x29B1. */
    const char* msg = "123456789";
    const uint16_t crc = crc16_itut_bits(reinterpret_cast<const uint8_t*>(msg), 9 * 8);
    CHECK_EQ(crc, static_cast<uint16_t>(0x29B1));
}

TEST(tetra_crc_ok_residue_is_ffff_through_16_zeros) {
    /* 0x1D0F is CRC-CCITT(init 0xFFFF) over sixteen zero bits — which is
     * exactly what the register holds once data + complemented parity have
     * been fed in. */
    const uint8_t zeros[2] = {0, 0};
    CHECK_EQ(crc16_itut_bits(zeros, 16), kTetraCrcOk);
}

TEST(tetra_crc_complemented_parity_gives_the_ok_residue) {
    Rng rng;
    for (int trial = 0; trial < 8; trial++) {
        std::vector<uint8_t> bits(60);
        for (auto& b : bits) b = rng.bit();

        auto packed = pack_bits(bits);
        const uint16_t crc = crc16_itut_bits(packed.data(), 60);

        /* data || ~crc */
        std::vector<uint8_t> with_parity = bits;
        for (size_t i = 0; i < 16; i++) {
            with_parity.push_back(static_cast<uint8_t>(((~crc) >> (15 - i)) & 1));
        }
        auto full = pack_bits(with_parity);
        CHECK_EQ(crc16_itut_bits(full.data(), 76), kTetraCrcOk);
    }
}

TEST(tetra_crc_detects_a_single_bit_error) {
    std::vector<uint8_t> bits(60, 0);
    bits[3] = bits[17] = bits[41] = 1;
    auto packed = pack_bits(bits);
    const uint16_t crc = crc16_itut_bits(packed.data(), 60);

    std::vector<uint8_t> with_parity = bits;
    for (size_t i = 0; i < 16; i++) {
        with_parity.push_back(static_cast<uint8_t>(((~crc) >> (15 - i)) & 1));
    }
    with_parity[25] ^= 1;  /* corrupt one data bit */
    auto full = pack_bits(with_parity);
    CHECK(crc16_itut_bits(full.data(), 76) != kTetraCrcOk);
}

/* ===========================================================================
 * Scrambler
 * ===========================================================================*/

TEST(tetra_scrambler_first_bits_match_the_lfsr_by_hand) {
    /* Stepping the 32-bit LFSR by hand from the BSCH seed 0x00000003 gives
     * 1,0,1,1,1,1,1,1 for the first eight bits (the taps are all in the low
     * bits, so the seed's two set bits walk up through them). */
    uint8_t seq[8]{};
    descramble_generate(kTetraScrambInitBsch, seq, 8);
    const uint8_t expected[8] = {1, 0, 1, 1, 1, 1, 1, 1};
    for (size_t i = 0; i < 8; i++) CHECK_EQ(static_cast<int>(seq[i]), static_cast<int>(expected[i]));
}

TEST(tetra_scrambler_is_its_own_inverse) {
    Rng rng;
    std::vector<uint8_t> bits(432);
    for (auto& b : bits) b = rng.bit();
    auto packed = pack_bits(bits);
    const auto original = packed;

    descramble(packed.data(), 432, kTetraScrambInitBsch);
    CHECK(std::memcmp(packed.data(), original.data(), packed.size()) != 0);

    descramble(packed.data(), 432, kTetraScrambInitBsch);
    CHECK_EQ(std::memcmp(packed.data(), original.data(), packed.size()), 0);
}

TEST(tetra_scrambler_seed_depends_on_the_network) {
    std::vector<uint8_t> zeros((432 + 7) / 8, 0);
    auto a = zeros;
    auto b = zeros;
    descramble(a.data(), 432, kTetraScrambInitBsch);
    descramble(b.data(), 432, 0x00ABCDEF);
    CHECK(std::memcmp(a.data(), b.data(), a.size()) != 0);
}

TEST(tetra_dmo_scramb_init_packs_mni_and_source) {
    /* (src & 0xFFFFFF) | ((mni & 0x3F) << 24), then <<2 | 3. */
    const uint32_t init = dmo_scramb_init(0x2A, 0x123456);
    CHECK_EQ(init, static_cast<uint32_t>((((0x2Au & 0x3Fu) << 24) | 0x123456u) << 2 | 3u));
}

TEST(tetra_dnb_seed_matches_upstream_layout) {
    ChannelDecoder d;
    d.set_network(234, 30, 21);
    const uint32_t expected = ((static_cast<uint32_t>(234) << 20) |
                               (static_cast<uint32_t>(30) << 6) | 21u)
                              << 2 |
                              3u;
    CHECK_EQ(d.tmo_dnb_seed(), expected);
}

/* ===========================================================================
 * Interleaver
 * ===========================================================================*/

TEST(tetra_deinterleave_matches_the_index_formula) {
    /* out[i-1] = in[((a*i) % K)], with i one-based. For K=120, a=11, i=1 the
     * source index is 1 + 11 = 12, i.e. in[11]. */
    std::vector<uint8_t> in_bits(120, 0);
    in_bits[11] = 1;
    auto in = pack_bits(in_bits);
    std::vector<uint8_t> out(15, 0);
    block_deinterleave(in.data(), out.data(), 120, 11);
    CHECK_EQ(static_cast<int>(bits_get(out.data(), 0)), 1);
    CHECK_EQ(static_cast<int>(bits_get(out.data(), 1)), 0);
}

TEST(tetra_interleave_round_trips_for_every_block_size) {
    struct Case { uint32_t k; uint32_t a; };
    const Case cases[] = {{120, 11}, {216, 101}, {432, 103}};

    Rng rng;
    for (const auto& c : cases) {
        std::vector<uint8_t> bits(c.k);
        for (auto& b : bits) b = rng.bit();
        auto src = pack_bits(bits);

        std::vector<uint8_t> woven((c.k + 7) / 8, 0);
        std::vector<uint8_t> back((c.k + 7) / 8, 0);
        block_interleave(src.data(), woven.data(), c.k, c.a);
        block_deinterleave(woven.data(), back.data(), c.k, c.a);

        for (uint32_t i = 0; i < c.k; i++) {
            CHECK_EQ(static_cast<int>(bits_get(back.data(), i)), static_cast<int>(bits[i]));
        }
    }
}

/* ===========================================================================
 * RCPC puncturing
 * ===========================================================================*/

TEST(tetra_depuncture_keeps_positions_1_2_and_5_of_each_period) {
    std::vector<uint8_t> in_bits(9, 1);
    auto in = pack_bits(in_bits);
    std::vector<uint8_t> mother(64, 0);
    depuncture_2_3(in.data(), mother.data(), 9);

    /* Period 1 covers mother[0..7]: positions 1,2,5 (1-based) carry data. */
    const int expect_first_period[8] = {1, 1, -1, -1, 1, -1, -1, -1};
    for (int i = 0; i < 8; i++) {
        if (expect_first_period[i] < 0) {
            CHECK_EQ(static_cast<int>(mother[i]), static_cast<int>(kTetraErased));
        } else {
            CHECK_EQ(static_cast<int>(mother[i]), 1);
        }
    }
    /* Same pattern one period along. */
    CHECK_EQ(static_cast<int>(mother[8]), 1);
    CHECK_EQ(static_cast<int>(mother[9]), 1);
    CHECK_EQ(static_cast<int>(mother[10]), static_cast<int>(kTetraErased));
    CHECK_EQ(static_cast<int>(mother[12]), 1);
}

TEST(tetra_mother_length_follows_upstream_formula) {
    CHECK_EQ(depuncture_2_3_mother_bits(120), 8u * 40u + 8u);
    CHECK_EQ(depuncture_2_3_mother_bits(216), 8u * 72u + 8u);
    CHECK_EQ(depuncture_2_3_mother_bits(432), 8u * 144u + 8u);
}

TEST(tetra_puncture_and_depuncture_are_inverses) {
    Rng rng;
    std::vector<uint8_t> mother(1200, kTetraErased);
    for (size_t i = 0; i < 400; i++) mother[i] = rng.bit();

    std::vector<uint8_t> punctured(64, 0);
    puncture_2_3(mother.data(), punctured.data(), 120);

    std::vector<uint8_t> back(1200, 0);
    depuncture_2_3(punctured.data(), back.data(), 120);

    /* Every non-erased position must survive the round trip. */
    for (size_t i = 0; i < depuncture_2_3_mother_bits(120); i++) {
        if (back[i] != kTetraErased) CHECK_EQ(static_cast<int>(back[i]), static_cast<int>(mother[i]));
    }
}

/* ===========================================================================
 * Convolutional code + Viterbi
 * ===========================================================================*/

TEST(tetra_viterbi_recovers_an_unpunctured_codeword) {
    Rng rng;
    const uint32_t n_info = 80;
    std::vector<uint8_t> bits(n_info);
    for (auto& b : bits) b = rng.bit();
    auto packed = pack_bits(bits);

    std::vector<uint8_t> mother(4 * n_info, 0);
    convolutional_encode(packed.data(), n_info, mother.data());

    std::vector<uint8_t> out(16, 0);
    std::vector<uint8_t> trace(9216, 0);
    const int cost = viterbi_decode_cch(mother.data(), n_info, out.data(), trace.data());

    CHECK_EQ(cost, 0);
    for (uint32_t i = 0; i < n_info; i++) {
        CHECK_EQ(static_cast<int>(bits_get(out.data(), i)), static_cast<int>(bits[i]));
    }
}

TEST(tetra_viterbi_recovers_a_punctured_codeword) {
    Rng rng;
    const uint32_t n_info = 80;   /* SB1 type-2 length */
    const uint32_t len_t5 = 120;  /* the punctured channel bits */

    std::vector<uint8_t> bits(n_info);
    for (auto& b : bits) b = rng.bit();
    auto packed = pack_bits(bits);

    std::vector<uint8_t> mother(4 * n_info + 64, 0);
    convolutional_encode(packed.data(), n_info, mother.data());

    std::vector<uint8_t> punctured(16, 0);
    puncture_2_3(mother.data(), punctured.data(), len_t5);

    std::vector<uint8_t> depunctured(1200, 0);
    depuncture_2_3(punctured.data(), depunctured.data(), len_t5);

    std::vector<uint8_t> out(16, 0);
    std::vector<uint8_t> trace(9216, 0);
    const int cost = viterbi_decode_cch(depunctured.data(), n_info, out.data(), trace.data());

    CHECK_EQ(cost, 0);
    for (uint32_t i = 0; i < n_info; i++) {
        CHECK_EQ(static_cast<int>(bits_get(out.data(), i)), static_cast<int>(bits[i]));
    }
}

TEST(tetra_viterbi_corrects_a_single_channel_error) {
    Rng rng;
    const uint32_t n_info = 80;
    std::vector<uint8_t> bits(n_info);
    for (auto& b : bits) b = rng.bit();
    auto packed = pack_bits(bits);

    std::vector<uint8_t> mother(4 * n_info, 0);
    convolutional_encode(packed.data(), n_info, mother.data());
    mother[37] ^= 1;  /* one hard-decision error */

    std::vector<uint8_t> out(16, 0);
    std::vector<uint8_t> trace(9216, 0);
    const int cost = viterbi_decode_cch(mother.data(), n_info, out.data(), trace.data());

    CHECK_EQ(cost, 1);
    for (uint32_t i = 0; i < n_info; i++) {
        CHECK_EQ(static_cast<int>(bits_get(out.data(), i)), static_cast<int>(bits[i]));
    }
}

/* ===========================================================================
 * decode_block, the whole per-block chain
 * ===========================================================================*/

TEST(tetra_decode_block_round_trips_sch_s) {
    Rng rng;
    std::vector<uint8_t> type1(ChannelDecoder::SB1_TYPE1_BITS);
    for (auto& b : type1) b = rng.bit();

    std::array<uint8_t, 63> burst{};
    build_block(type1, ChannelDecoder::SB1_TYPE2_BITS, ChannelDecoder::BLK1_LEN_BITS,
                ChannelDecoder::SB1_INTERLEAVE_A, kTetraScrambInitBsch,
                burst.data(), ChannelDecoder::BLK1_OFFSET_BITS);

    ChannelDecoder decoder;
    std::array<uint8_t, 34> out{};
    uint16_t crc = 0;
    int cost = -1;
    const bool ok = decoder.decode_block(
        burst.data(), ChannelDecoder::BLK1_OFFSET_BITS, ChannelDecoder::BLK1_LEN_BITS,
        ChannelDecoder::SB1_TYPE1_BITS, ChannelDecoder::SB1_TYPE2_BITS,
        ChannelDecoder::SB1_INTERLEAVE_A, out.data(), out.size(), crc, cost,
        kTetraScrambInitBsch);

    CHECK(ok);
    CHECK_EQ(crc, kTetraCrcOk);
    CHECK_EQ(cost, 0);
    for (size_t i = 0; i < type1.size(); i++) {
        CHECK_EQ(static_cast<int>(bits_get(out.data(), i)), static_cast<int>(type1[i]));
    }
}

TEST(tetra_decode_block_round_trips_sch_hd_and_sch_f) {
    Rng rng;

    /* SCH/HD: 124 type-1 bits carried in 216 channel bits. */
    {
        std::vector<uint8_t> type1(ChannelDecoder::SB2_TYPE1_BITS);
        for (auto& b : type1) b = rng.bit();

        std::array<uint8_t, 63> burst{};
        build_block(type1, ChannelDecoder::SB2_TYPE2_BITS, ChannelDecoder::BLK2_LEN_BITS,
                    ChannelDecoder::SB2_INTERLEAVE_A, kTetraScrambInitBsch, burst.data(),
                    ChannelDecoder::TMO_BLK2_OFFSET_BITS);

        ChannelDecoder decoder;
        std::array<uint8_t, 34> out{};
        uint16_t crc = 0;
        int cost = -1;
        CHECK(decoder.decode_block(burst.data(), ChannelDecoder::TMO_BLK2_OFFSET_BITS,
                                   ChannelDecoder::BLK2_LEN_BITS, ChannelDecoder::SB2_TYPE1_BITS,
                                   ChannelDecoder::SB2_TYPE2_BITS,
                                   ChannelDecoder::SB2_INTERLEAVE_A, out.data(), out.size(), crc,
                                   cost, kTetraScrambInitBsch));
        CHECK_EQ(crc, kTetraCrcOk);
        for (size_t i = 0; i < type1.size(); i++) {
            CHECK_EQ(static_cast<int>(bits_get(out.data(), i)), static_cast<int>(type1[i]));
        }
    }

    /* SCH/F: 268 type-1 bits over the whole 432-bit slot, with a traffic
     * scrambling seed rather than the BSCH one. */
    {
        std::vector<uint8_t> type1(ChannelDecoder::SCH_F_TYPE1_BITS);
        for (auto& b : type1) b = rng.bit();

        ChannelDecoder decoder;
        decoder.set_network(234, 30, 21);
        const uint32_t seed = decoder.tmo_dnb_seed();

        std::array<uint8_t, 54> burst{};
        build_block(type1, ChannelDecoder::SCH_F_TYPE2_BITS, ChannelDecoder::SCH_F_LEN_BITS,
                    ChannelDecoder::SCH_F_INTERLEAVE_A, seed, burst.data(), 0);

        std::array<uint8_t, 34> out{};
        uint16_t crc = 0;
        int cost = -1;
        CHECK(decoder.decode_block(burst.data(), 0, ChannelDecoder::SCH_F_LEN_BITS,
                                   ChannelDecoder::SCH_F_TYPE1_BITS,
                                   ChannelDecoder::SCH_F_TYPE2_BITS,
                                   ChannelDecoder::SCH_F_INTERLEAVE_A, out.data(), out.size(), crc,
                                   cost, seed));
        CHECK_EQ(crc, kTetraCrcOk);
        for (size_t i = 0; i < type1.size(); i++) {
            CHECK_EQ(static_cast<int>(bits_get(out.data(), i)), static_cast<int>(type1[i]));
        }
    }
}

TEST(tetra_decode_block_rejects_the_wrong_scrambling_seed) {
    Rng rng;
    std::vector<uint8_t> type1(ChannelDecoder::SB1_TYPE1_BITS);
    for (auto& b : type1) b = rng.bit();

    std::array<uint8_t, 63> burst{};
    build_block(type1, ChannelDecoder::SB1_TYPE2_BITS, ChannelDecoder::BLK1_LEN_BITS,
                ChannelDecoder::SB1_INTERLEAVE_A, kTetraScrambInitBsch, burst.data(),
                ChannelDecoder::BLK1_OFFSET_BITS);

    ChannelDecoder decoder;
    std::array<uint8_t, 34> out{};
    uint16_t crc = 0;
    int cost = -1;
    CHECK(!decoder.decode_block(burst.data(), ChannelDecoder::BLK1_OFFSET_BITS,
                                ChannelDecoder::BLK1_LEN_BITS, ChannelDecoder::SB1_TYPE1_BITS,
                                ChannelDecoder::SB1_TYPE2_BITS, ChannelDecoder::SB1_INTERLEAVE_A,
                                out.data(), out.size(), crc, cost, 0x00ABCDEF));
    CHECK(crc != kTetraCrcOk);
}

TEST(tetra_decode_block_rejects_a_corrupted_burst) {
    Rng rng;
    std::vector<uint8_t> type1(ChannelDecoder::SB1_TYPE1_BITS);
    for (auto& b : type1) b = rng.bit();

    std::array<uint8_t, 63> burst{};
    build_block(type1, ChannelDecoder::SB1_TYPE2_BITS, ChannelDecoder::BLK1_LEN_BITS,
                ChannelDecoder::SB1_INTERLEAVE_A, kTetraScrambInitBsch, burst.data(),
                ChannelDecoder::BLK1_OFFSET_BITS);

    /* Twelve flipped channel bits is well past what a rate-2/3 K=5 code can
     * carry through a 120-bit block. */
    for (size_t i = 0; i < 12; i++) {
        const size_t bit = ChannelDecoder::BLK1_OFFSET_BITS + i * 7;
        bits_set(burst.data(), bit, bits_get(burst.data(), bit) == 0);
    }

    ChannelDecoder decoder;
    std::array<uint8_t, 34> out{};
    uint16_t crc = 0;
    int cost = -1;
    CHECK(!decoder.decode_block(burst.data(), ChannelDecoder::BLK1_OFFSET_BITS,
                                ChannelDecoder::BLK1_LEN_BITS, ChannelDecoder::SB1_TYPE1_BITS,
                                ChannelDecoder::SB1_TYPE2_BITS, ChannelDecoder::SB1_INTERLEAVE_A,
                                out.data(), out.size(), crc, cost, kTetraScrambInitBsch));
    CHECK(cost > 0);
}

/* ===========================================================================
 * decode_burst: SYNC PDU and MAC PDU parsing
 * ===========================================================================*/

namespace {

/* A SYNC PDU carrying the given identity, laid out per parse_sync_pdu(). */
std::vector<uint8_t> make_sync_pdu(uint8_t bcc, uint8_t timeslot, uint8_t frame_number,
                                   uint8_t encryption, uint16_t mcc, uint16_t mnc) {
    std::vector<uint8_t> bits(ChannelDecoder::SB1_TYPE1_BITS, 0);
    put_field(bits, 0, 4, 0);  /* system code */
    put_field(bits, 4, 6, bcc);
    put_field(bits, 10, 2, timeslot);
    put_field(bits, 12, 5, frame_number);
    put_field(bits, 30, 1, encryption);
    put_field(bits, 31, 10, mcc);
    put_field(bits, 41, 14, mnc);
    return bits;
}

/* A SYSINFO/BCAST MAC PDU (pdu_type 2, bcast_type 0). */
std::vector<uint8_t> make_sysinfo_pdu(size_t length_bits, uint16_t la, uint8_t encryption) {
    std::vector<uint8_t> bits(length_bits, 0);
    put_field(bits, 0, 2, 2);  /* pdu_type = SYSINFO/BCAST */
    put_field(bits, 2, 2, 0);  /* bcast_type = SYSINFO     */
    put_field(bits, 82, 14, la);
    put_field(bits, 122, 1, encryption);
    return bits;
}

}  // namespace

TEST(tetra_decode_burst_reads_the_sync_pdu) {
    const auto sync_pdu = make_sync_pdu(21, 2, 17, 0, 234, 30);

    SyncBurst burst{};
    build_block(sync_pdu, ChannelDecoder::SB1_TYPE2_BITS, ChannelDecoder::BLK1_LEN_BITS,
                ChannelDecoder::SB1_INTERLEAVE_A, kTetraScrambInitBsch, burst.payload.data(),
                ChannelDecoder::BLK1_OFFSET_BITS);

    ChannelDecoder decoder;
    const auto result = decoder.decode_burst(burst);

    CHECK(result.is_ok);
    /* The SCH/H half is not present, so this stops at Sync, not SyncFull. */
    CHECK(result.type == ChannelDecoder::Result::Type::Sync);
    CHECK_EQ(static_cast<int>(result.bcc), 21);
    CHECK_EQ(static_cast<int>(result.timeslot), 2);
    CHECK_EQ(static_cast<int>(result.frame_number), 17);
    CHECK_EQ(static_cast<int>(result.mcc), 234);
    CHECK_EQ(static_cast<int>(result.mnc), 30);
    CHECK_EQ(static_cast<int>(result.encryption), 0);

    /* The identity is latched so DNB bursts can be descrambled. */
    CHECK(decoder.network_synced());
    CHECK_EQ(static_cast<int>(decoder.last_mcc()), 234);
    CHECK_EQ(static_cast<int>(decoder.last_mnc()), 30);
    CHECK_EQ(static_cast<int>(decoder.last_bcc()), 21);
}

TEST(tetra_decode_burst_reads_sync_plus_sysinfo) {
    const auto sync_pdu = make_sync_pdu(9, 1, 3, 1, 262, 1);
    const auto sysinfo = make_sysinfo_pdu(ChannelDecoder::SB2_TYPE1_BITS, 4660, 1);

    SyncBurst burst{};
    build_block(sync_pdu, ChannelDecoder::SB1_TYPE2_BITS, ChannelDecoder::BLK1_LEN_BITS,
                ChannelDecoder::SB1_INTERLEAVE_A, kTetraScrambInitBsch, burst.payload.data(),
                ChannelDecoder::BLK1_OFFSET_BITS);
    build_block(sysinfo, ChannelDecoder::SB2_TYPE2_BITS, ChannelDecoder::BLK2_LEN_BITS,
                ChannelDecoder::SB2_INTERLEAVE_A, kTetraScrambInitBsch, burst.payload.data(),
                ChannelDecoder::TMO_BLK2_OFFSET_BITS);

    ChannelDecoder decoder;
    const auto result = decoder.decode_burst(burst);

    CHECK(result.is_ok);
    CHECK(result.type == ChannelDecoder::Result::Type::SyncFull);
    /* Identity survives the second block's decode. */
    CHECK_EQ(static_cast<int>(result.mcc), 262);
    CHECK_EQ(static_cast<int>(result.mnc), 1);
    CHECK_EQ(static_cast<int>(result.bcc), 9);
    /* ...and the MAC PDU is parsed on top. */
    CHECK_EQ(static_cast<int>(result.pdu_type), 2);
    CHECK_STR_EQ(ChannelDecoder::pdu_name(result.pdu_type), "SYSINFO/BCAST");
    CHECK_EQ(static_cast<int>(result.la), 4660);
    CHECK_EQ(static_cast<int>(result.encryption), 1);
}

TEST(tetra_decode_burst_rejects_noise) {
    Rng rng;
    SyncBurst burst{};
    for (auto& byte : burst.payload) byte = static_cast<uint8_t>(rng.next() & 0xFF);

    ChannelDecoder decoder;
    const auto result = decoder.decode_burst(burst);
    CHECK(!result.is_ok);
    CHECK(result.type == ChannelDecoder::Result::Type::None);
    CHECK(!decoder.network_synced());
}

TEST(tetra_decode_dnb_needs_network_sync_first) {
    const auto sysinfo = make_sysinfo_pdu(ChannelDecoder::SCH_F_TYPE1_BITS, 777, 0);

    ChannelDecoder ref;
    ref.set_network(234, 30, 21);

    NormalBurst burst{};
    build_block(sysinfo, ChannelDecoder::SCH_F_TYPE2_BITS, ChannelDecoder::SCH_F_LEN_BITS,
                ChannelDecoder::SCH_F_INTERLEAVE_A, ref.tmo_dnb_seed(), burst.payload.data(), 0);

    /* A decoder that has not seen a SYNC PDU has no seed and must not guess. */
    ChannelDecoder cold;
    CHECK(!cold.decode_dnb(burst).is_ok);

    /* Once synced to the same network the same burst decodes. */
    ChannelDecoder warm;
    warm.set_network(234, 30, 21);
    const auto result = warm.decode_dnb(burst);
    CHECK(result.is_ok);
    CHECK(result.type == ChannelDecoder::Result::Type::Dnb);
    CHECK_EQ(static_cast<int>(result.pdu_type), 2);
    CHECK_EQ(static_cast<int>(result.la), 777);
}

TEST(tetra_decode_dnb_rejects_the_wrong_network) {
    const auto sysinfo = make_sysinfo_pdu(ChannelDecoder::SCH_F_TYPE1_BITS, 777, 0);

    ChannelDecoder ref;
    ref.set_network(234, 30, 21);

    NormalBurst burst{};
    build_block(sysinfo, ChannelDecoder::SCH_F_TYPE2_BITS, ChannelDecoder::SCH_F_LEN_BITS,
                ChannelDecoder::SCH_F_INTERLEAVE_A, ref.tmo_dnb_seed(), burst.payload.data(), 0);

    ChannelDecoder other;
    other.set_network(234, 30, 22);  /* one colour code out */
    CHECK(!other.decode_dnb(burst).is_ok);
}

TEST(tetra_mac_resource_pdu_yields_cmce_and_ssi) {
    /* Laid out exactly as parse_mac_pdu() walks it: address type 1 carries a
     * 24-bit SSI, both optional-field flags are clear, no channel allocation,
     * LLC type 0 (2 further bits), MLE type 1 = CMCE, then a 5-bit CMCE type
     * and a 14-bit call identifier. */
    ChannelDecoder::Result result{};
    std::vector<uint8_t> bits(ChannelDecoder::SCH_F_TYPE1_BITS, 0);
    put_field(bits, 0, 2, 0);        /* MAC-RESOURCE      */
    put_field(bits, 4, 2, 0);        /* not encrypted     */
    put_field(bits, 7, 6, 20);       /* length indication */
    put_field(bits, 13, 3, 1);       /* address type SSI  */
    put_field(bits, 16, 24, 0x123456);
    put_field(bits, 40, 1, 0);       /* no power control  */
    put_field(bits, 41, 1, 0);       /* no slot granting  */
    put_field(bits, 42, 1, 0);       /* no channel alloc  */
    put_field(bits, 43, 4, 0);       /* LLC type 0        */
    put_field(bits, 49, 3, 1);       /* MLE = CMCE        */
    put_field(bits, 52, 5, 7);       /* CMCE SETUP        */
    put_field(bits, 57, 14, 1234);   /* call identifier   */

    auto packed = pack_bits(bits);
    std::memcpy(result.payload.data(), packed.data(),
                std::min(result.payload.size(), packed.size()));

    ChannelDecoder::parse_mac_pdu(result);

    CHECK_EQ(static_cast<int>(result.pdu_type), 0);
    CHECK_STR_EQ(ChannelDecoder::pdu_name(result.pdu_type), "MAC-RESOURCE");
    CHECK_EQ(result.calling_ssi, 0x123456u);
    CHECK_EQ(static_cast<int>(result.cmce_type), 7);
    CHECK_STR_EQ(ChannelDecoder::cmce_name(result.cmce_type), "SETUP");
    CHECK_EQ(static_cast<int>(result.call_id), 1234);
}

TEST(tetra_mac_resource_pdu_stops_at_encryption) {
    /* An encrypted MAC-RESOURCE carries nothing readable, so the parser must
     * leave the CMCE fields alone rather than decode ciphertext. */
    ChannelDecoder::Result result{};
    std::vector<uint8_t> bits(ChannelDecoder::SCH_F_TYPE1_BITS, 0);
    put_field(bits, 0, 2, 0);
    put_field(bits, 4, 2, 1);   /* encrypted */
    put_field(bits, 7, 6, 20);
    put_field(bits, 13, 3, 1);
    put_field(bits, 16, 24, 0x123456);
    put_field(bits, 52, 5, 7);

    auto packed = pack_bits(bits);
    std::memcpy(result.payload.data(), packed.data(),
                std::min(result.payload.size(), packed.size()));

    ChannelDecoder::parse_mac_pdu(result);
    CHECK_EQ(static_cast<int>(result.encryption), 1);
    CHECK_EQ(static_cast<int>(result.cmce_type), 0xFF);
    CHECK_EQ(result.calling_ssi, 0u);
}

TEST(tetra_pdu_and_cmce_names) {
    CHECK_STR_EQ(ChannelDecoder::pdu_name(0), "MAC-RESOURCE");
    CHECK_STR_EQ(ChannelDecoder::pdu_name(1), "MAC-FRAG");
    CHECK_STR_EQ(ChannelDecoder::pdu_name(2), "SYSINFO/BCAST");
    CHECK_STR_EQ(ChannelDecoder::pdu_name(3), "MAC-U-SIGNAL");
    CHECK_STR_EQ(ChannelDecoder::pdu_name(9), "UNK_9");

    CHECK_STR_EQ(ChannelDecoder::cmce_name(11), "TX GRANTED");
    CHECK_STR_EQ(ChannelDecoder::cmce_name(14), "SDS DATA");
    CHECK_STR_EQ(ChannelDecoder::cmce_name(31), "NOT SUPPORTED");
    CHECK_STR_EQ(ChannelDecoder::cmce_name(20), "CMCE:20");
}

/* ===========================================================================
 * pi/4-DQPSK slicing and burst synchronisation
 * ===========================================================================*/

TEST(tetra_dibit_slicer_quadrant_map) {
    /* Each quadrant of the differential product, at the exact pi/4-DQPSK
     * constellation points. */
    const float c = 0.70710678f;
    CHECK_EQ(static_cast<int>(slice_dibit(c, c)), 0);    /* +pi/4  */
    CHECK_EQ(static_cast<int>(slice_dibit(-c, c)), 1);   /* +3pi/4 */
    CHECK_EQ(static_cast<int>(slice_dibit(-c, -c)), 3);  /* -3pi/4 */
    CHECK_EQ(static_cast<int>(slice_dibit(c, -c)), 2);   /* -pi/4  */
}

TEST(tetra_carrier_detector_is_zero_on_constellation_points) {
    const float c = 0.70710678f;
    CHECK_NEAR(carrier_phase_error(c, c), 0.0f, 1e-6);
    CHECK_NEAR(carrier_phase_error(-c, c), 0.0f, 1e-6);
    CHECK_NEAR(carrier_phase_error(-c, -c), 0.0f, 1e-6);
    CHECK_NEAR(carrier_phase_error(c, -c), 0.0f, 1e-6);

    /* ...and pushes the right way when the constellation is rotated. */
    const float a = std::cos(0.3f);
    const float b = std::sin(0.3f);  /* +0.3 rad, inside quadrant I */
    CHECK(carrier_phase_error(a, b) < 0.0f);
}

namespace {

/* The 500 bits of a synchronisation burst: the SCH/S block at bit 94, the
 * 38-bit Y frame-synchronisation word at bit 214 (which is what makes
 * burst_start = sync_start - 214 land on bit 0), and the SCH/H block at 282. */
std::vector<uint8_t> make_sync_burst_bits(const std::vector<uint8_t>& sync_pdu,
                                          const std::vector<uint8_t>& sysinfo) {
    std::array<uint8_t, 63> packed{};
    build_block(sync_pdu, ChannelDecoder::SB1_TYPE2_BITS, ChannelDecoder::BLK1_LEN_BITS,
                ChannelDecoder::SB1_INTERLEAVE_A, kTetraScrambInitBsch, packed.data(),
                ChannelDecoder::BLK1_OFFSET_BITS);
    build_block(sysinfo, ChannelDecoder::SB2_TYPE2_BITS, ChannelDecoder::BLK2_LEN_BITS,
                ChannelDecoder::SB2_INTERLEAVE_A, kTetraScrambInitBsch, packed.data(),
                ChannelDecoder::TMO_BLK2_OFFSET_BITS);

    auto bits = unpack_bits(packed.data(), kBurstBits);
    for (size_t i = 0; i < kYSyncBits; i++) {
        bits[kSyncOffset + i] = static_cast<uint8_t>((kYSync >> (kYSyncBits - 1 - i)) & 1ULL);
    }
    return bits;
}

}  // namespace

TEST(tetra_demodulator_recovers_a_sync_burst_from_a_synthesised_signal) {
    const auto sync_pdu = make_sync_pdu(21, 2, 17, 0, 234, 30);
    const auto sysinfo = make_sysinfo_pdu(ChannelDecoder::SB2_TYPE1_BITS, 4660, 0);
    const auto burst_bits = make_sync_burst_bits(sync_pdu, sysinfo);

    /* Pseudo-random lead-in and tail so the correlator has to find the sync
     * word rather than tripping on a run of zeros. */
    Rng rng;
    std::vector<uint8_t> stream;
    for (int i = 0; i < 300; i++) stream.push_back(rng.bit());
    const size_t burst_offset = stream.size();
    stream.insert(stream.end(), burst_bits.begin(), burst_bits.end());
    for (int i = 0; i < 120; i++) stream.push_back(rng.bit());

    /* 4 samples per symbol: 18 kBd at 72 kHz. Upstream runs 48 kHz (2.67
     * samples/symbol); the code is rate-agnostic, and an integer ratio keeps
     * this test's expectations exact. */
    const auto signal = modulate_pi4_dqpsk(stream, 4);

    Demodulator demod;
    demod.configure(72000.0f);
    demod.set_dnb_enabled(false);

    std::vector<SyncBurst> caught;
    demod.set_sync_handler([&caught](const SyncBurst& b) { caught.push_back(b); });
    demod.process(signal.data(), signal.size());

    CHECK_EQ(caught.size(), 1u);
    if (caught.empty()) return;

    CHECK(!caught[0].inverted);
    CHECK_EQ(static_cast<int>(caught[0].sync_errors), 0);
    for (size_t i = 0; i < kBurstBits; i++) {
        CHECK_EQ(static_cast<int>(bits_get(caught[0].payload.data(), i)),
                 static_cast<int>(burst_bits[i]));
    }
    (void)burst_offset;

    /* ...and the burst the demodulator handed over decodes. */
    ChannelDecoder decoder;
    const auto result = decoder.decode_burst(caught[0]);
    CHECK(result.type == ChannelDecoder::Result::Type::SyncFull);
    CHECK_EQ(static_cast<int>(result.mcc), 234);
    CHECK_EQ(static_cast<int>(result.mnc), 30);
    CHECK_EQ(static_cast<int>(result.bcc), 21);
    CHECK_EQ(static_cast<int>(result.la), 4660);
}

TEST(tetra_demodulator_handles_inverted_polarity) {
    const auto sync_pdu = make_sync_pdu(9, 3, 1, 0, 262, 7);
    const auto sysinfo = make_sysinfo_pdu(ChannelDecoder::SB2_TYPE1_BITS, 1000, 0);
    const auto burst_bits = make_sync_burst_bits(sync_pdu, sysinfo);

    Rng rng;
    std::vector<uint8_t> stream;
    for (int i = 0; i < 300; i++) stream.push_back(rng.bit());
    stream.insert(stream.end(), burst_bits.begin(), burst_bits.end());
    for (int i = 0; i < 120; i++) stream.push_back(rng.bit());

    /* Complementing every bit maps each dibit onto the constellation point
     * 180 degrees away — the polarity ambiguity a differential receiver has to
     * resolve from the sync word. */
    for (auto& b : stream) b = static_cast<uint8_t>(b ^ 1);

    const auto signal = modulate_pi4_dqpsk(stream, 4);

    Demodulator demod;
    demod.configure(72000.0f);
    demod.set_dnb_enabled(false);

    std::vector<SyncBurst> caught;
    demod.set_sync_handler([&caught](const SyncBurst& b) { caught.push_back(b); });
    demod.process(signal.data(), signal.size());

    CHECK_EQ(caught.size(), 1u);
    if (caught.empty()) return;

    CHECK(caught[0].inverted);
    /* The payload comes back the right way up. */
    for (size_t i = 0; i < kBurstBits; i++) {
        CHECK_EQ(static_cast<int>(bits_get(caught[0].payload.data(), i)),
                 static_cast<int>(burst_bits[i]));
    }

    ChannelDecoder decoder;
    const auto result = decoder.decode_burst(caught[0]);
    CHECK(result.type == ChannelDecoder::Result::Type::SyncFull);
    CHECK_EQ(static_cast<int>(result.mcc), 262);
    CHECK_EQ(static_cast<int>(result.mnc), 7);
}

TEST(tetra_demodulator_finds_a_normal_burst_by_its_training_sequence) {
    /* A downlink normal burst: block 1, then the 22-bit N training sequence
     * 16 bits later, then block 2 38 bits after the sequence starts. */
    ChannelDecoder ref;
    ref.set_network(234, 30, 21);
    const auto sysinfo = make_sysinfo_pdu(ChannelDecoder::SB2_TYPE1_BITS, 321, 0);

    std::array<uint8_t, 27> block1{};
    build_block(sysinfo, ChannelDecoder::SB2_TYPE2_BITS, ChannelDecoder::BLK2_LEN_BITS,
                ChannelDecoder::SB2_INTERLEAVE_A, ref.tmo_dnb_seed(), block1.data(), 0);
    const auto block1_bits = unpack_bits(block1.data(), kDnbBlockBits);

    Rng rng;
    std::vector<uint8_t> stream;
    for (int i = 0; i < 300; i++) stream.push_back(rng.bit());

    const size_t block1_start = stream.size();
    stream.insert(stream.end(), block1_bits.begin(), block1_bits.end());
    /* Fill from the end of block 1 up to the training sequence. */
    const size_t train_start = block1_start + kDnbBlock1Back;
    while (stream.size() < train_start) stream.push_back(rng.bit());
    for (size_t i = 0; i < kDnbTrainBits; i++) {
        stream.push_back(static_cast<uint8_t>((kNSync >> (kDnbTrainBits - 1 - i)) & 1u));
    }
    const size_t block2_start = train_start + kDnbBlock2Forward;
    while (stream.size() < block2_start) stream.push_back(rng.bit());

    std::vector<uint8_t> block2_bits;
    for (size_t i = 0; i < kDnbBlockBits; i++) block2_bits.push_back(rng.bit());
    stream.insert(stream.end(), block2_bits.begin(), block2_bits.end());
    for (int i = 0; i < 60; i++) stream.push_back(rng.bit());

    if (stream.size() % 2) stream.push_back(0);

    const auto signal = modulate_pi4_dqpsk(stream, 4);

    Demodulator demod;
    demod.configure(72000.0f);

    std::vector<NormalBurst> caught;
    demod.set_dnb_handler([&caught](const NormalBurst& b) { caught.push_back(b); });
    demod.process(signal.data(), signal.size());

    CHECK(!caught.empty());
    if (caught.empty()) return;

    /* The first detection is the one placed here; later ones would be the
     * correlator firing on the random tail, which is what the distance limit
     * of 1 makes unlikely but not impossible. */
    const auto& b = caught[0];
    CHECK(!b.inverted);
    CHECK(!b.p_train);
    CHECK_EQ(static_cast<int>(b.errors), 0);

    for (size_t i = 0; i < kDnbBlockBits; i++) {
        CHECK_EQ(static_cast<int>(bits_get(b.payload.data(), i)),
                 static_cast<int>(block1_bits[i]));
    }
    for (size_t i = 0; i < kDnbBlockBits; i++) {
        CHECK_EQ(static_cast<int>(bits_get(b.payload.data(), kDnbBlockBits + i)),
                 static_cast<int>(block2_bits[i]));
    }

    /* Block 1 carries a decodable SCH/HD. */
    ChannelDecoder decoder;
    decoder.set_network(234, 30, 21);
    const auto result = decoder.decode_dnb(b);
    CHECK(result.is_ok);
    CHECK_EQ(static_cast<int>(result.la), 321);
}

TEST(tetra_demodulator_ignores_noise) {
    Rng rng;
    std::vector<uint8_t> stream;
    for (int i = 0; i < 4000; i++) stream.push_back(rng.bit());
    const auto signal = modulate_pi4_dqpsk(stream, 4);

    Demodulator demod;
    demod.configure(72000.0f);
    demod.set_dnb_enabled(false);

    size_t hits = 0;
    demod.set_sync_handler([&hits](const SyncBurst&) { hits++; });
    demod.process(signal.data(), signal.size());

    /* A 38-bit word at distance <= 4 has probability ~ 2 * C(38,<=4) / 2^38
     * per bit, i.e. about 5e-7; 4000 bits should see none. */
    CHECK_EQ(hits, 0u);
}

TEST(tetra_demodulator_symbol_clock_stays_near_nominal) {
    Rng rng;
    std::vector<uint8_t> stream;
    for (int i = 0; i < 2000; i++) stream.push_back(rng.bit());
    const auto signal = modulate_pi4_dqpsk(stream, 4);

    Demodulator demod;
    demod.configure(72000.0f);
    demod.process(signal.data(), signal.size());

    /* 18 kBd at 72 kHz is exactly a quarter turn per sample. */
    const double nominal = 18000.0 * 4294967296.0 / 72000.0;
    const double actual = static_cast<double>(demod.symbol_phase_inc());
    /* Upstream's clamp is roughly +-0.6% of nominal; a clean signal must stay
     * well inside it. */
    CHECK(actual > nominal * 0.994);
    CHECK(actual < nominal * 1.006);

    /* Every transmitted bit came out. */
    CHECK_EQ(demod.bit_count(), static_cast<uint64_t>(stream.size() + 2));
}
