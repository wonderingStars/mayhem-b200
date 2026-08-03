/*
 * mayhem-b200 — FLEX decoder tests.
 *
 * Expectations come from the protocol (TIA/EIA-STD-43A) and from upstream's
 * proc_flex.cpp, not from what this code happens to emit:
 *
 *   - The sync codes 0x870C / 0xB068 / 0x7B18 / 0xDEA0 / 0x4C7C and the
 *     32-bit marker 0xA6C6AAAA are the mode codes the standard defines, with
 *     the speeds and level counts upstream maps them to.
 *   - The BCH is the same (31,21) code POCSAG uses, applied to bit-reversed
 *     words because FLEX transmits least-significant-bit first.
 *   - The frame information word packs a 4-bit checksum, a 4-bit cycle, a
 *     7-bit frame, a roaming bit, a repeat bit and 4 traffic bits, and its
 *     nibble sum must come to 0xF.
 *   - Block Information Word 1 gives the address-field and vector-field
 *     offsets; alphanumeric vectors are K(4) V(3) w1(7) n(7); numeric vectors
 *     put a 3-bit word count at bits 14-16; message words carry three 7-bit
 *     characters or five BCD digits, sent least-significant-bit first.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "../src/apps/ui_flex_rx.hpp"

#include <string>
#include <utility>
#include <vector>

using namespace flex;

namespace {

/* A capcode's address word is the capcode plus 0x8000. */
constexpr uint32_t kAddr1234567 = 1234567u + 0x8000u;

/* BIW1 with the vector field at word 2, the address field at word 1 (the
 * 2-bit field holds aoffset-1) and no priority addresses. */
constexpr uint32_t kBiw1 = (2u << 10);

/* Alphanumeric vector: K=5, V=5 (ALN), w1=3, n=3. K is chosen so the nibble
 * checksum comes to 0xF, which is what marks a vector word as valid. */
constexpr uint32_t kAlphaVector = 0xC1D5u;

/* Standard numeric vector: K=3, V=3 (NUM), w1=3, n=0 (one word). */
constexpr uint32_t kNumericVector = 0x1B3u;

/* Message header with F=3 (first fragment), N=5, R=1. */
constexpr uint32_t kAlphaHeader = 0x8B800u;

/* Three 7-bit characters per word, low bits first. */
constexpr uint32_t chars3(uint32_t a, uint32_t b, uint32_t c) {
    return (a & 0x7Fu) | ((b & 0x7Fu) << 7) | ((c & 0x7Fu) << 14);
}

struct PhaseRun {
    std::vector<FlexPacket> packets;

    /* Decodes a phase built from (index, 21-bit information word) pairs. */
    void run(const std::vector<std::pair<int, uint32_t>>& words, unsigned int baud = 1600,
             unsigned int levels = 2, char phase_no = 'A') {
        Decoder d;
        d.sync.baud = baud;
        d.sync.levels = levels;
        d.fiw.cycleno = 3;
        d.fiw.frameno = 42;
        d.set_packet_handler([this](const FlexPacket& p) { packets.push_back(p); });

        uint32_t buf[phase_words] = {};
        for (const auto& kv : words) buf[kv.first] = d.bch.make_word(kv.second);
        d.decode_phase_buffer(buf, phase_no);
    }

    /* The first packet that is not the per-frame BIW announcement. */
    const FlexPacket* page() const {
        for (const auto& p : packets)
            if (p.type != 9) return &p;
        return nullptr;
    }
};

/* The 64-bit sync buffer for a mode code: A : marker : ~A. */
uint64_t sync_buffer(uint16_t code) {
    return (static_cast<uint64_t>(code) << 48) | (static_cast<uint64_t>(sync_marker) << 16) |
           static_cast<uint64_t>(static_cast<uint16_t>(~code));
}

}  // namespace

/* --- BCH ------------------------------------------------------------------ */

TEST(flex_bch_round_trips_information_words) {
    Decoder d;
    for (uint32_t info : {0x000800u, 0x135807u, 0x00C1D5u, 0x08B800u, 0x1FFFFFu, 0x000001u}) {
        const uint32_t word = d.bch.make_word(info);
        uint32_t t = word;
        CHECK_EQ(d.bch.fix_errors(t), 0);
        CHECK_EQ(t & 0x001FFFFFu, info);
    }
}

TEST(flex_bch_words_are_bit_reversed_relative_to_pocsag) {
    /* FLEX sends least-significant-bit first, so the information bits land in
     * bits 0..20 of the word and the tables' MSB-first layout is reached by
     * reversing. An all-ones payload is the fixed point that shows the
     * reversal is total, not partial. */
    Decoder d;
    CHECK_EQ(d.bch.make_word(0x1FFFFFu), 0xFFFFFFFFu);
    CHECK_EQ(bit_reverse_32(0x00000001u), 0x80000000u);
    CHECK_EQ(bit_reverse_32(0xA6C6AAAAu), bit_reverse_32(0xA6C6AAAAu));
    CHECK_EQ(bit_reverse_32(bit_reverse_32(0x12345678u)), 0x12345678u);
}

TEST(flex_bch_corrects_one_bit_errors) {
    Decoder d;
    const uint32_t base = d.bch.make_word(kAddr1234567);
    for (int i = 0; i < 21; i++) {
        uint32_t t = base ^ (1u << i);
        CHECK_EQ(d.bch.fix_errors(t), 1);
        CHECK_EQ(t & 0x001FFFFFu, kAddr1234567);
    }
}

TEST(flex_bch_corrects_two_bit_errors) {
    Decoder d;
    const uint32_t base = d.bch.make_word(kAddr1234567);
    int corrected = 0;
    for (int i = 1; i < 21; i++) {
        for (int j = 0; j < i; j++) {
            uint32_t t = base ^ (1u << i) ^ (1u << j);
            if (d.bch.fix_errors(t) == 2 && (t & 0x001FFFFFu) == kAddr1234567) corrected++;
        }
    }
    CHECK_EQ(corrected, 21 * 20 / 2);
}

TEST(flex_bch_rejects_three_adjacent_bit_errors) {
    Decoder d;
    const uint32_t base = d.bch.make_word(kAddr1234567);
    uint32_t t = base ^ 0x7u;
    CHECK_EQ(d.bch.fix_errors(t), 3);
}

/* --- sync ----------------------------------------------------------------- */

TEST(flex_sync_check_recognises_every_mode_code) {
    Decoder d;
    for (uint16_t code : {uint16_t{0x870C}, uint16_t{0xB068}, uint16_t{0x7B18}, uint16_t{0xDEA0},
                          uint16_t{0x4C7C}})
        CHECK_EQ(d.sync_check(sync_buffer(code)), static_cast<unsigned int>(code));
}

TEST(flex_sync_check_tolerates_three_marker_errors) {
    Decoder d;
    uint64_t buf = sync_buffer(0x870C);
    /* Three bits wrong in the marker is still inside the Hamming window. */
    buf ^= (static_cast<uint64_t>(0x00000007u) << 16);
    CHECK_EQ(d.sync_check(buf), 0x870Cu);
}

TEST(flex_sync_check_rejects_four_marker_errors) {
    Decoder d;
    uint64_t buf = sync_buffer(0x870C);
    buf ^= (static_cast<uint64_t>(0x0000000Fu) << 16);
    CHECK_EQ(d.sync_check(buf), 0u);
}

TEST(flex_sync_check_rejects_a_mode_code_that_is_not_its_own_complement) {
    Decoder d;
    /* A (16) and ~A (16) must agree; four disagreeing bits fails. */
    uint64_t buf = sync_buffer(0x870C);
    buf ^= 0x000Full;
    CHECK_EQ(d.sync_check(buf), 0u);
}

TEST(flex_feed_sync_finds_the_pattern_in_a_symbol_stream) {
    Decoder d;
    const uint64_t buf = sync_buffer(0x870C);
    unsigned int got = 0;
    for (int i = 63; i >= 0; --i) {
        const int bit = static_cast<int>((buf >> i) & 1u);
        /* feed_sync() treats symbols 0 and 1 as a 1 bit. */
        got = d.feed_sync(static_cast<unsigned char>(bit ? 0 : 3));
    }
    CHECK_EQ(got, 0x870Cu);
    CHECK_EQ(d.sync.polarity, 0u);
}

TEST(flex_feed_sync_detects_inverted_polarity) {
    Decoder d;
    const uint64_t buf = sync_buffer(0x870C);
    unsigned int got = 0;
    for (int i = 63; i >= 0; --i) {
        const int bit = static_cast<int>((buf >> i) & 1u);
        got = d.feed_sync(static_cast<unsigned char>(bit ? 3 : 0));
    }
    CHECK_EQ(got, 0x870Cu);
    CHECK_EQ(d.sync.polarity, 1u);
}

TEST(flex_decode_mode_maps_sync_codes_to_speeds) {
    Decoder d;
    struct Expect {
        unsigned int code, baud, levels;
    };
    const Expect expected[] = {
        {0x870C, 1600, 2},
        {0xB068, 1600, 4},
        {0x7B18, 3200, 2},
        {0xDEA0, 3200, 4},
        {0x4C7C, 3200, 4},
    };
    for (const auto& e : expected) {
        d.decode_mode(e.code);
        CHECK_EQ(d.sync.baud, e.baud);
        CHECK_EQ(d.sync.levels, e.levels);
    }

    /* An unrecognised code falls back to the slowest mode. */
    d.decode_mode(0x1234);
    CHECK_EQ(d.sync.baud, 1600u);
    CHECK_EQ(d.sync.levels, 2u);
}

/* --- frame information word ----------------------------------------------- */

TEST(flex_decodes_the_frame_information_word) {
    Decoder d;
    /* checksum 8, cycle 3, frame 42, roaming 1 -> nibble sum 8+3+10+10 = 0x1F. */
    d.fiw.rawdata = d.bch.make_word(0xAA38u);
    CHECK_EQ(d.decode_fiw(), 0);
    CHECK_EQ(d.fiw.cycleno, 3u);
    CHECK_EQ(d.fiw.frameno, 42u);
    CHECK_EQ(d.fiw.roaming, 1u);
    CHECK_EQ(d.fiw.repeat, 0u);
    CHECK_EQ(d.fiw.traffic, 0u);
    CHECK_EQ(d.fiw.checksum, 8u);
}

TEST(flex_rejects_a_frame_information_word_with_a_bad_checksum) {
    Decoder d;
    /* Same fields, checksum nibble 0 instead of 8. */
    d.fiw.rawdata = d.bch.make_word(0xAA30u);
    CHECK_EQ(d.decode_fiw(), 1);
}

TEST(flex_rejects_an_uncorrectable_frame_information_word) {
    Decoder d;
    d.fiw.rawdata = d.bch.make_word(0xAA38u) ^ 0x7u;
    CHECK_EQ(d.decode_fiw(), 1);
}

TEST(flex_fiw_survives_a_correctable_bit_error) {
    Decoder d;
    d.fiw.rawdata = d.bch.make_word(0xAA38u) ^ (1u << 9);
    CHECK_EQ(d.decode_fiw(), 0);
    CHECK_EQ(d.fiw.cycleno, 3u);
    CHECK_EQ(d.fiw.frameno, 42u);
}

/* --- vector checksum ------------------------------------------------------ */

TEST(flex_vector_nibble_checksum) {
    /* The nibble sum of a vector word, plus bit 20, must be 0xF. */
    CHECK(Decoder::nibble_checksum_ok(kAlphaVector));
    CHECK(Decoder::nibble_checksum_ok(kNumericVector));
    CHECK(!Decoder::nibble_checksum_ok(kNumericVector & ~0xFu));
    CHECK(!Decoder::nibble_checksum_ok(kAlphaVector ^ 1u));
}

/* --- address classification ----------------------------------------------- */

TEST(flex_classifies_address_ranges) {
    Decoder d;

    d.parse_capcode(kAddr1234567);
    CHECK_EQ(d.decode.long_address, 0);
    CHECK_EQ(static_cast<int>(d.decode.addr_type), static_cast<int>(AddrType::SHORT));
    CHECK_EQ(d.decode.capcode, int64_t{1234567});

    d.parse_capcode(0x000100u); /* long address set 1 */
    CHECK_EQ(d.decode.long_address, 1);
    CHECK_EQ(static_cast<int>(d.decode.addr_type), static_cast<int>(AddrType::LONG));

    d.parse_capcode(0x1F7805u); /* temporary group slot 5 */
    CHECK_EQ(static_cast<int>(d.decode.addr_type), static_cast<int>(AddrType::TEMPORARY));

    d.parse_capcode(0x1F7815u);
    CHECK_EQ(static_cast<int>(d.decode.addr_type), static_cast<int>(AddrType::OPERATOR));

    d.parse_capcode(0x1F7000u);
    CHECK_EQ(static_cast<int>(d.decode.addr_type), static_cast<int>(AddrType::NETWORK));

    d.parse_capcode(0x1F3000u);
    CHECK_EQ(static_cast<int>(d.decode.addr_type), static_cast<int>(AddrType::INFO_SVC));

    d.parse_capcode(0x1F0100u);
    CHECK_EQ(static_cast<int>(d.decode.addr_type), static_cast<int>(AddrType::RESERVED));

    d.parse_capcode(0x000000u);
    CHECK_EQ(static_cast<int>(d.decode.addr_type), static_cast<int>(AddrType::UNKNOWN));
}

/* --- whole-phase decode --------------------------------------------------- */

TEST(flex_decodes_an_alphanumeric_page) {
    PhaseRun r;
    r.run({{0, kBiw1},
           {1, kAddr1234567},
           {2, kAlphaVector},
           {3, kAlphaHeader},
           {4, chars3(0x00, 'H', 'E')}, /* first data word: low 7 bits are the signature */
           {5, chars3('L', 'L', 'O')}});

    CHECK_EQ(r.packets.size(), size_t{2}); /* the BIW announcement plus the page */
    CHECK_EQ(r.packets[0].type, 9u);
    CHECK_EQ(r.packets[0].biw_field, 0xFF);

    const FlexPacket* p = r.page();
    CHECK(p != nullptr);
    if (!p) return;
    CHECK_EQ(p->type, 5u); /* ALN */
    CHECK_EQ(p->capcode, int64_t{1234567});
    CHECK_STR_EQ(p->message, "HELLO");
    CHECK_EQ(p->bitrate, 1600u);
    CHECK_EQ(p->phase, 'A');
    CHECK_EQ(p->cycle, 3);
    CHECK_EQ(p->frame, 42);

    /* Header fields: F=3 first fragment, N=5, R=1 new. */
    CHECK_EQ(p->has_flags, 1);
    CHECK_EQ(p->frag, 3);
    CHECK_EQ(p->seq, 5);
    CHECK_EQ(p->is_new, 1);
    CHECK_EQ(p->maildrop, 0);
    CHECK_EQ(p->more_frag, 0);
}

TEST(flex_reports_six_thousand_four_hundred_bps_for_four_level_thirty_two_hundred) {
    PhaseRun r;
    r.run({{0, kBiw1},
           {1, kAddr1234567},
           {2, kAlphaVector},
           {3, kAlphaHeader},
           {4, chars3(0x00, 'H', 'E')},
           {5, chars3('L', 'L', 'O')}},
          3200, 4);

    const FlexPacket* p = r.page();
    CHECK(p != nullptr);
    if (p) CHECK_EQ(p->bitrate, 6400u);
}

TEST(flex_decodes_a_numeric_page) {
    /* One message word: two skipped K bits then four BCD digits, each sent
     * least-significant-bit first. */
    const uint32_t digits = (1u << 2) | (2u << 6) | (3u << 10) | (4u << 14);

    PhaseRun r;
    r.run({{0, kBiw1}, {1, kAddr1234567}, {2, kNumericVector}, {3, digits}});

    const FlexPacket* p = r.page();
    CHECK(p != nullptr);
    if (!p) return;
    CHECK_EQ(p->type, 3u); /* NUM */
    CHECK_EQ(p->capcode, int64_t{1234567});
    CHECK_STR_EQ(p->message, "1234");
}

TEST(flex_treats_an_address_without_a_valid_vector_as_tone_only) {
    /* The vector word's checksum fails, so the address sits past the end of
     * the valid vector run: a tone-only page. */
    PhaseRun r;
    r.run({{0, kBiw1}, {1, kAddr1234567}, {2, kNumericVector & ~0xFu}});

    const FlexPacket* p = r.page();
    CHECK(p != nullptr);
    if (!p) return;
    CHECK_EQ(p->type, 2u); /* TON */
    CHECK_EQ(p->capcode, int64_t{1234567});
    CHECK_STR_EQ(p->message, "");
}

TEST(flex_marks_characters_from_uncorrectable_words) {
    /* Find a three-bit corruption the BCH cannot repair, then check the
     * characters from that word come out as '?'. */
    Decoder probe;
    const uint32_t good = probe.bch.make_word(chars3('L', 'L', 'O'));
    uint32_t bad_word = 0;
    bool found = false;
    for (int i = 2; i < 21 && !found; i++) {
        for (int j = 1; j < i && !found; j++) {
            for (int k = 0; k < j && !found; k++) {
                uint32_t t = good ^ (1u << i) ^ (1u << j) ^ (1u << k);
                const uint32_t candidate = t;
                if (probe.bch.fix_errors(t) > 2) {
                    bad_word = candidate;
                    found = true;
                }
            }
        }
    }
    CHECK(found);
    if (!found) return;

    Decoder d;
    d.sync.baud = 1600;
    d.sync.levels = 2;
    std::vector<FlexPacket> packets;
    d.set_packet_handler([&](const FlexPacket& p) { packets.push_back(p); });

    uint32_t buf[phase_words] = {};
    buf[0] = d.bch.make_word(kBiw1);
    buf[1] = d.bch.make_word(kAddr1234567);
    buf[2] = d.bch.make_word(kAlphaVector);
    buf[3] = d.bch.make_word(kAlphaHeader);
    buf[4] = d.bch.make_word(chars3(0x00, 'H', 'E'));
    buf[5] = bad_word;
    d.decode_phase_buffer(buf, 'A');

    const FlexPacket* p = nullptr;
    for (const auto& q : packets)
        if (q.type != 9) p = &q;
    CHECK(p != nullptr);
    if (!p) return;
    CHECK_STR_EQ(std::string(p->message), "HE???");
}

TEST(flex_ignores_an_idle_phase) {
    /* Idle fill alternates all-ones and all-zeros words; BCH must not be let
     * loose on it. */
    Decoder d;
    int packets = 0;
    d.set_packet_handler([&](const FlexPacket&) { packets++; });

    uint32_t buf[phase_words];
    for (int i = 0; i < phase_words; i++) buf[i] = (i & 1) ? 0xFFFFFFFFu : 0x00000000u;
    d.decode_phase_buffer(buf, 'A');
    CHECK_EQ(packets, 0);
}

TEST(flex_ignores_a_phase_with_an_empty_block_information_word) {
    Decoder d;
    int packets = 0;
    d.set_packet_handler([&](const FlexPacket&) { packets++; });

    uint32_t buf[phase_words] = {};
    buf[1] = d.bch.make_word(kAddr1234567);
    d.decode_phase_buffer(buf, 'A');
    CHECK_EQ(packets, 0);
}

TEST(flex_ignores_a_phase_whose_offsets_are_impossible) {
    /* voffset (2) below aoffset (4) cannot be parsed. */
    Decoder d;
    int packets = 0;
    d.set_packet_handler([&](const FlexPacket&) { packets++; });

    uint32_t buf[phase_words] = {};
    buf[0] = d.bch.make_word((2u << 10) | (3u << 8));
    buf[1] = d.bch.make_word(kAddr1234567);
    d.decode_phase_buffer(buf, 'A');
    CHECK_EQ(packets, 0);
}

TEST(flex_emits_block_information_words) {
    /* aoffset = 2 puts one BIW word at index 1. Type 2 is the time-of-day
     * word: hour at bits 7-11, minute at 12-17, seconds unit at 18-20. */
    const uint32_t biw1 = (3u << 10) | (1u << 8); /* voffset 3, aoffset 2 */
    const uint32_t time_word = (2u << 4) | (13u << 7) | (45u << 12) | (4u << 18);

    Decoder d;
    d.sync.baud = 1600;
    d.sync.levels = 2;
    std::vector<FlexPacket> packets;
    d.set_packet_handler([&](const FlexPacket& p) { packets.push_back(p); });

    uint32_t buf[phase_words] = {};
    buf[0] = d.bch.make_word(biw1);
    buf[1] = d.bch.make_word(time_word);
    buf[2] = d.bch.make_word(kAddr1234567);
    buf[3] = d.bch.make_word(kNumericVector & ~0xFu); /* tone-only, keeps it simple */
    d.decode_phase_buffer(buf, 'A');

    const FlexPacket* biw = nullptr;
    for (const auto& p : packets)
        if (p.type == 9 && p.biw_field == 2) biw = &p;
    CHECK(biw != nullptr);
    if (!biw) return;
    CHECK_EQ(biw->biw_v1, 13); /* hour */
    CHECK_EQ(biw->biw_v2, 45); /* minute */
    CHECK_EQ(biw->biw_v3, 4);  /* seconds unit, x7.5 s */
}

/* --- symbol clock --------------------------------------------------------- */

TEST(flex_symbol_clock_locks_to_an_alternating_stream) {
    /* The FLEX lock pattern is an alternation of the two outer levels, which
     * is what the preamble carries. */
    Decoder d;
    d.configure(48000.0f);

    std::vector<float> audio;
    const double sps = 48000.0 / 1600.0;
    double t = 0.0;
    for (int s = 0; s < 200; s++) {
        const double next = t + sps;
        const float level = (s & 1) ? 1.0f : -1.0f;
        while (t < next) {
            audio.push_back(level);
            t += 1.0;
        }
    }

    d.process(audio.data(), audio.size());
    CHECK_EQ(d.demod.locked, 1);
    CHECK(d.demod.symbol_count > 100u);
}

TEST(flex_symbol_clock_does_not_lock_to_silence) {
    Decoder d;
    d.configure(48000.0f);
    std::vector<float> quiet(48000, 0.0f);
    d.process(quiet.data(), quiet.size());
    CHECK_EQ(d.demod.locked, 0);
}

/* --- de-interleaving ------------------------------------------------------ */

TEST(flex_data_bits_are_interleaved_across_eight_words) {
    /* idx = ((counter >> 5) & 0xFFF8) | (counter & 7): consecutive bits land
     * in eight different words, so it takes 8*32 = 256 bits to fill the first
     * group of words. That spread is what makes a burst error survivable. */
    Decoder d;
    d.sync.baud = 1600;
    d.sync.levels = 2;

    /* Ones in the bits that belong to word 0, zeros everywhere else. */
    for (int i = 0; i < 256; i++) d.read_data(((i & 7) == 0) ? 3 : 0);

    CHECK_EQ(d.data.a.buf[0], 0xFFFFFFFFu);
    CHECK_EQ(d.data.a.buf[1], 0x00000000u);
    CHECK_EQ(d.data.a.buf[7], 0x00000000u);
    /* 2-level mode leaves the B phase empty. */
    CHECK_EQ(d.data.b.buf[0], 0x00000000u);
}

TEST(flex_read_data_fills_phase_a_at_1600_two_level) {
    Decoder d;
    d.sync.baud = 1600;
    d.sync.levels = 2;
    for (int i = 0; i < 256; i++) d.read_data(3);
    for (int w = 0; w < 8; w++) CHECK_EQ(d.data.a.buf[w], 0xFFFFFFFFu);
    CHECK_EQ(d.data.b.buf[0], 0x00000000u);
    CHECK_EQ(d.data.c.buf[0], 0x00000000u);
}

TEST(flex_read_data_interleaves_phases_a_and_c_at_3200) {
    Decoder d;
    d.sync.baud = 3200;
    d.sync.levels = 2;
    /* At 3200 symbols/s the symbols alternate between the A and C phases. */
    for (int i = 0; i < 512; i++) d.read_data((i & 1) ? 0 : 3);
    for (int w = 0; w < 8; w++) {
        CHECK_EQ(d.data.a.buf[w], 0xFFFFFFFFu);
        CHECK_EQ(d.data.c.buf[w], 0x00000000u);
    }
}

/* --- console formatting --------------------------------------------------- */

TEST(flex_formats_a_page_line) {
    FlexPacket p{};
    p.cycle = 3;
    p.frame = 42;
    p.bitrate = 1600;
    p.phase = 'A';
    p.capcode = 1234567;
    p.type = 5;
    std::snprintf(p.message, sizeof(p.message), "%s", "HELLO");

    CHECK_STR_EQ(format_packet_line(p), "3/42 1600 N A 1234567 ALN HELLO");

    p.is_inverted = 1;
    p.is_priority = 1;
    CHECK_STR_EQ(format_packet_line(p), "3/42 1600 I A 1234567 P ALN HELLO");
}

TEST(flex_formats_a_temporary_group_line) {
    FlexPacket p{};
    p.cycle = 0;
    p.frame = 1;
    p.bitrate = 3200;
    p.phase = 'B';
    p.addr_type = 2; /* temporary */
    p.capcode = 0x1F7805 - 0x8000;
    p.type = 5;
    std::snprintf(p.message, sizeof(p.message), "%s", "HI");

    CHECK_STR_EQ(format_packet_line(p), "0/1 3200 N B TG5 ALN HI");
}

TEST(flex_type_tags) {
    CHECK_STR_EQ(type_tag(0), "SEC");
    CHECK_STR_EQ(type_tag(1), "INS");
    CHECK_STR_EQ(type_tag(2), "TON");
    CHECK_STR_EQ(type_tag(3), "NUM");
    CHECK_STR_EQ(type_tag(4), "SNUM");
    CHECK_STR_EQ(type_tag(5), "ALN");
    CHECK_STR_EQ(type_tag(6), "HEX");
    CHECK_STR_EQ(type_tag(7), "NNUM");
    CHECK_STR_EQ(type_tag(8), "SHORT");
    CHECK_STR_EQ(type_tag(9), "BIW");
    CHECK_STR_EQ(type_tag(42), "UNK");
}

TEST(flex_timezone_table) {
    CHECK_EQ(timezone_offset_minutes(0), 0);
    CHECK_EQ(timezone_offset_minutes(1), 60);
    CHECK_EQ(timezone_offset_minutes(12), 720);
    CHECK_EQ(timezone_offset_minutes(17), 345);
    CHECK_EQ(timezone_offset_minutes(20), -210);
    CHECK_EQ(timezone_offset_minutes(31), -60);
    CHECK_EQ(timezone_offset_minutes(99), 0); /* out of range */
}

/* --- front end ------------------------------------------------------------ */

TEST(flex_front_end_decimates_to_the_requested_rate) {
    ChannelFrontEnd fe;
    fe.configure(2400000.0, 48000.0, 8000.0);
    CHECK_EQ(fe.decimation(), size_t{50});
    CHECK_NEAR(fe.audio_rate(), 48000.0, 1.0);
}

TEST(flex_front_end_handles_an_unconfigured_source) {
    ChannelFrontEnd fe;
    std::vector<dsp::cfloat> in(64, dsp::cfloat{1.0f, 0.0f});
    std::vector<float> out;
    fe.process(in.data(), in.size(), out);
    CHECK(out.empty());
}

/* --- malformed input ------------------------------------------------------ */

TEST(flex_decoder_survives_a_random_phase_buffer) {
    Decoder d;
    int packets = 0;
    d.set_packet_handler([&](const FlexPacket&) { packets++; });

    uint32_t buf[phase_words];
    uint32_t x = 0x12345678u;
    for (int i = 0; i < phase_words; i++) {
        x = x * 1664525u + 1013904223u;
        buf[i] = x;
    }
    d.decode_phase_buffer(buf, 'A');
    /* Whatever it decides, it must terminate and stay in bounds. */
    CHECK(packets >= 0);
}

TEST(flex_decoder_survives_random_audio) {
    Decoder d;
    d.configure(48000.0f);
    int packets = 0;
    d.set_packet_handler([&](const FlexPacket&) { packets++; });

    std::vector<float> noise(48000);
    uint32_t x = 1u;
    for (auto& s : noise) {
        x = x * 1103515245u + 12345u;
        s = static_cast<float>(static_cast<int32_t>(x >> 16) % 2000) / 1000.0f;
    }
    d.process(noise.data(), noise.size());
    CHECK(packets >= 0);
}

TEST(flex_decoder_reset_clears_state) {
    Decoder d;
    d.configure(48000.0f);
    d.sync.baud = 3200;
    d.sync.levels = 4;
    d.fiw.cycleno = 9;
    d.demod.locked = 1;
    d.reset();
    CHECK_EQ(d.demod.locked, 0);
    CHECK_EQ(d.fiw.cycleno, 0u);
    CHECK_EQ(static_cast<int>(d.state.current), static_cast<int>(State::SYNC1));
    CHECK_EQ(d.demod.sample_freq, 48000u);
}

TEST(flex_empty_audio_block_is_a_no_op) {
    Decoder d;
    d.configure(48000.0f);
    d.process(nullptr, 0);
    CHECK_EQ(d.demod.sample_count, 0u);
}
