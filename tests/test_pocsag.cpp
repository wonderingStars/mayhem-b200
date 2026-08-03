/*
 * mayhem-b200 — POCSAG decoder tests.
 *
 * Every expectation here is derived from the protocol or from upstream's
 * implementation, not from what this code happens to produce:
 *
 *   - The BCH vectors are the two codewords the standard fixes by name, the
 *     frame synchronisation codeword 0x7CD215D8 and the idle codeword
 *     0x7A89C197. Both are valid BCH(31,21)+parity words, so a correct
 *     decoder reports zero errors on them and a correct encoder reproduces
 *     them from their payload alone.
 *   - The biquad is checked against the literal coefficients in
 *     baseband/proc_pocsag2.hpp, which upstream generated with
 *     scipy.signal.butter(2, 1800, "lowpass", fs=24000).
 *   - Address, function and message expectations follow the codeword layout
 *     in the standard: bit 31 flags address vs message, an address codeword
 *     carries RIC bits 21..3 at bits 30..13 (i.e. (cw >> 10) & 0x1FFFF8) with
 *     the low 3 bits coming from the frame the codeword arrived in, and
 *     message codewords carry 20 payload bits holding 7-bit characters sent
 *     least-significant-bit first.
 *   - The end-to-end cases are decoded from signals built by upstream's own
 *     encoder (pocsag_encode) and by the Phase A 2FSK modulator.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "../src/apps/pocsag_app.hpp"
#include "../src/dsp/demod_digital.hpp"

#include <string>
#include <vector>

using namespace pocsag;

namespace {

const EccContainer& ecc() {
    static EccContainer e;
    return e;
}

/* An address codeword for the given RIC and function, per the standard. */
uint32_t address_codeword(uint32_t ric, uint32_t function) {
    return ecc().encode(((ric & 0x1FFFF8u) << 10) | (function << 11));
}

/* Reverse the low 7 bits — POCSAG sends characters LSB first. */
uint32_t rev7(uint8_t c) {
    uint32_t r = 0;
    for (int i = 0; i < 7; i++)
        if (c & (1u << i)) r |= 1u << (6 - i);
    return r;
}

POCSAGPacket batch_of(const std::vector<uint32_t>& words) {
    POCSAGPacket p;
    batch_t b{};
    for (size_t i = 0; i < batch_size; i++) b[i] = (i < words.size()) ? words[i] : idleword;
    p.set(b);
    return p;
}

/* The on-air bit sequence for a preamble plus one full batch. */
std::vector<uint8_t> batch_bitstream(const std::vector<uint32_t>& words, size_t preamble_bits) {
    std::vector<uint8_t> bits;
    for (size_t i = 0; i < preamble_bits; i++) bits.push_back((i & 1) ? uint8_t{0} : uint8_t{1});
    bits.reserve(bits.size() + 32 * (words.size() + 1));
    for (int b = 31; b >= 0; --b) bits.push_back(static_cast<uint8_t>((syncword >> b) & 1u));
    for (uint32_t w : words)
        for (int b = 31; b >= 0; --b) bits.push_back(static_cast<uint8_t>((w >> b) & 1u));
    return bits;
}

/* The +/-1 stream an FM discriminator produces for those bits. A POCSAG '1'
 * is the lower tone, so it comes out negative. */
std::vector<float> render_audio(const std::vector<uint8_t>& bits, double samples_per_bit,
                                size_t trailing) {
    std::vector<float> audio;
    double t = 0.0;
    for (uint8_t bit : bits) {
        const double next = t + samples_per_bit;
        while (t < next) {
            audio.push_back(bit ? -1.0f : 1.0f);
            t += 1.0;
        }
    }
    for (size_t i = 0; i < trailing; i++) audio.push_back(1.0f);
    return audio;
}

}  // namespace

/* --- BCH(31,21) + parity ------------------------------------------------- */

TEST(pocsag_bch_accepts_the_standard_codewords) {
    uint32_t v = syncword;
    CHECK_EQ(ecc().error_correct(v), 0);
    CHECK_EQ(v, syncword);

    v = idleword;
    CHECK_EQ(ecc().error_correct(v), 0);
    CHECK_EQ(v, idleword);
}

TEST(pocsag_bch_encoder_reproduces_the_standard_codewords) {
    /* Given only the 21 payload bits, the encoder must regenerate the 10 BCH
     * parity bits and the even parity bit exactly. */
    CHECK_EQ(ecc().encode(syncword & 0xFFFFF800u), syncword);
    CHECK_EQ(ecc().encode(idleword & 0xFFFFF800u), idleword);

    /* And anything it produces must decode clean. */
    for (uint32_t payload : {0x00000000u, 0x80000000u, 0x4B5A1800u, 0xFFFFF800u}) {
        const uint32_t cw = ecc().encode(payload);
        uint32_t t = cw;
        CHECK_EQ(ecc().error_correct(t), 0);
        CHECK_EQ(t, cw);
    }
}

TEST(pocsag_bch_corrects_one_bit_error_in_every_data_position) {
    for (int i = 11; i < 32; i++) {
        uint32_t v = syncword ^ (1u << i);
        CHECK_EQ(ecc().error_correct(v), 1);
        CHECK_EQ(v, syncword);
    }
}

TEST(pocsag_bch_corrects_two_bit_errors_in_every_data_pair) {
    int corrected = 0;
    for (int i = 12; i < 32; i++) {
        for (int j = 11; j < i; j++) {
            uint32_t v = syncword ^ (1u << i) ^ (1u << j);
            if (ecc().error_correct(v) == 2 && v == syncword) corrected++;
        }
    }
    CHECK_EQ(corrected, 21 * 20 / 2); /* every pair of the 21 data bits */
}

TEST(pocsag_bch_reports_three_bit_errors_as_uncorrectable) {
    uint32_t v = syncword ^ (1u << 22) ^ (1u << 21) ^ (1u << 20);
    const uint32_t corrupt = v;
    CHECK_EQ(ecc().error_correct(v), 3);
    /* Uncorrectable means "left alone", not "silently mangled". */
    CHECK_EQ(v, corrupt);
}

TEST(pocsag_bch_flags_but_does_not_repair_parity_field_errors) {
    /* Upstream only ever corrects the 21 data bits. An error inside the BCH
     * parity field is reported and the payload is left intact. */
    uint32_t v = syncword ^ (1u << 5);
    CHECK_EQ(ecc().error_correct(v), 1);
    CHECK_EQ(v & 0xFFFFF800u, syncword & 0xFFFFF800u);

    /* The final even-parity bit is computed and then discarded upstream, so a
     * codeword corrupted only there is reported clean. Pinned so a future
     * change to that behaviour is a deliberate one. */
    v = syncword ^ 1u;
    CHECK_EQ(ecc().error_correct(v), 0);
}

/* --- sync word correlation ------------------------------------------------ */

TEST(pocsag_codeword_extractor_locks_to_the_sync_word) {
    BitQueue q;
    batch_t got{};
    int fired = 0;
    bool inverted = true;
    CodewordExtractor cx{q, [&](CodewordExtractor& c) {
                             got = c.batch();
                             inverted = c.inverted();
                             fired++;
                         }};

    const uint32_t acw = address_codeword(1234567, 3);
    std::vector<uint32_t> words;
    for (int i = 0; i < 16; i++) words.push_back(i == 14 ? acw : idleword);

    for (uint8_t bit : batch_bitstream(words, 64)) {
        q.push(bit != 0);
        cx.process_bits();
    }

    CHECK_EQ(fired, 1);
    CHECK_EQ(inverted, false);
    CHECK_EQ(got[14], acw);
    CHECK_EQ(got[0], idleword);
}

TEST(pocsag_codeword_extractor_locks_to_the_inverted_sync_word) {
    /* An FM receiver with the opposite discriminator polarity sees the
     * complement of everything; the extractor must notice and un-invert. */
    BitQueue q;
    batch_t got{};
    int fired = 0;
    bool inverted = false;
    CodewordExtractor cx{q, [&](CodewordExtractor& c) {
                             got = c.batch();
                             inverted = c.inverted();
                             fired++;
                         }};

    const uint32_t acw = address_codeword(1234567, 3);
    std::vector<uint32_t> words;
    for (int i = 0; i < 16; i++) words.push_back(i == 14 ? acw : idleword);

    for (uint8_t bit : batch_bitstream(words, 64)) {
        q.push(bit == 0); /* inverted */
        cx.process_bits();
    }

    CHECK_EQ(fired, 1);
    CHECK_EQ(inverted, true);
    CHECK_EQ(got[14], acw);
}

TEST(pocsag_codeword_extractor_tolerates_two_bit_errors_in_the_sync_word) {
    BitQueue q;
    int fired = 0;
    batch_t got{};
    CodewordExtractor cx{q, [&](CodewordExtractor& c) {
                             got = c.batch();
                             fired++;
                         }};

    const uint32_t acw = address_codeword(1234567, 3);
    std::vector<uint32_t> words;
    for (int i = 0; i < 16; i++) words.push_back(i == 14 ? acw : idleword);
    auto bits = batch_bitstream(words, 64);

    /* Corrupt two bits of the sync word itself. */
    bits[64 + 3] ^= 1;
    bits[64 + 17] ^= 1;

    for (uint8_t bit : bits) {
        q.push(bit != 0);
        cx.process_bits();
    }

    CHECK_EQ(fired, 1);
    CHECK_EQ(got[14], acw);
}

TEST(pocsag_codeword_extractor_rejects_three_bit_errors_in_the_sync_word) {
    BitQueue q;
    int fired = 0;
    CodewordExtractor cx{q, [&](CodewordExtractor&) { fired++; }};

    std::vector<uint32_t> words;
    for (int i = 0; i < 16; i++) words.push_back(idleword);
    auto bits = batch_bitstream(words, 64);
    bits[64 + 3] ^= 1;
    bits[64 + 17] ^= 1;
    bits[64 + 29] ^= 1;

    for (uint8_t bit : bits) {
        q.push(bit != 0);
        cx.process_bits();
    }

    CHECK_EQ(fired, 0);
}

/* --- address and function decode ------------------------------------------ */

TEST(pocsag_decodes_address_and_function) {
    /* RIC 1234567 = 0x12D687. Its low 3 bits (7) are the frame number, so the
     * codeword must sit in frame 7, i.e. batch index 14. */
    std::vector<uint32_t> words(16, idleword);
    words[14] = address_codeword(1234567, 3);

    POCSAGState st{&ecc()};
    const auto packet = batch_of(words);
    /* Returns true: the idle codeword in the next slot ends this page, and
     * there is still batch left to walk. */
    CHECK(pocsag_decode_batch(packet, st));

    CHECK_EQ(st.address, 1234567u);
    CHECK_EQ(st.function, 3u);
    CHECK_EQ(st.out_type, OUT_ADDRESS);
    CHECK_EQ(st.new_message, true);
    CHECK_EQ(st.errors, 0u);
}

TEST(pocsag_frame_number_comes_from_the_slot_not_the_codeword) {
    /* The same 21 payload bits in a different frame decode to a different
     * RIC: the low 3 bits are positional. Frame 2 == batch index 4. */
    std::vector<uint32_t> words(16, idleword);
    words[4] = address_codeword(1234567, 3);

    POCSAGState st{&ecc()};
    CHECK(pocsag_decode_batch(batch_of(words), st));

    CHECK_EQ(st.address, (1234567u & 0x1FFFF8u) | 2u);
    CHECK_EQ(st.function, 3u);
}

TEST(pocsag_idle_codewords_alone_produce_no_address) {
    std::vector<uint32_t> words(16, idleword);
    POCSAGState st{&ecc()};
    CHECK(!pocsag_decode_batch(batch_of(words), st));

    CHECK_EQ(st.out_type, OUT_IDLE);
    CHECK_EQ(st.mode, STATE_CLEAR);
    CHECK_EQ(st.address, 0u);
}

/* --- 7-bit alpha message assembly ----------------------------------------- */

TEST(pocsag_assembles_seven_bit_characters_from_a_message_codeword) {
    /* One message codeword carries 20 payload bits: two whole 7-bit
     * characters and 6 bits of a third, which has to wait for the next
     * codeword. Build the payload by hand from the standard's layout. */
    std::vector<uint32_t> words(16, idleword);
    words[14] = address_codeword(1234567, 3);
    const uint32_t payload = (rev7('A') << 13) | (rev7('B') << 6) | (rev7('C') >> 1);
    words[15] = ecc().encode(0x80000000u | (payload << 11));

    POCSAGState st{&ecc()};
    CHECK(!pocsag_decode_batch(batch_of(words), st));

    CHECK_EQ(st.out_type, OUT_MESSAGE);
    CHECK_STR_EQ(st.output, "AB");
    CHECK_EQ(st.detected, DET_ALPHA);
    CHECK_EQ(st.address, 1234567u);
}

TEST(pocsag_assembles_characters_across_codeword_boundaries) {
    /* Three message codewords hold 60 bits = eight whole characters plus 4
     * spare bits, so "ABCDEFGH" comes out complete. */
    const char* text = "ABCDEFGH";
    uint64_t acc = 0;
    int nbits = 0;
    std::vector<uint32_t> payloads;
    for (int i = 0; i < 8; i++) {
        acc = (acc << 7) | rev7(static_cast<uint8_t>(text[i]));
        nbits += 7;
        while (nbits >= 20) {
            payloads.push_back(static_cast<uint32_t>((acc >> (nbits - 20)) & 0xFFFFFu));
            nbits -= 20;
        }
    }
    CHECK_EQ(payloads.size(), size_t{2});
    payloads.push_back(static_cast<uint32_t>((acc & 0xFFFFu) << 4)); /* last 16 bits + pad */

    std::vector<uint32_t> words(16, idleword);
    words[10] = address_codeword(1234560, 0); /* frame 5 -> batch index 10 */
    for (size_t i = 0; i < payloads.size(); i++)
        words[11 + i] = ecc().encode(0x80000000u | (payloads[i] << 11));

    POCSAGState st{&ecc()};
    /* Ends at the idle codeword in slot 14, with batch still to walk. */
    CHECK(pocsag_decode_batch(batch_of(words), st));

    CHECK_EQ(st.address, 1234565u);
    CHECK_STR_EQ(st.output.substr(0, 8), "ABCDEFGH");
}

TEST(pocsag_marks_characters_from_uncorrectable_codewords) {
    std::vector<uint32_t> words(16, idleword);
    words[14] = address_codeword(1234567, 3);
    const uint32_t payload = (rev7('A') << 13) | (rev7('B') << 6) | (rev7('C') >> 1);
    /* Three bit errors in the data field: beyond what BCH can repair. */
    words[15] = ecc().encode(0x80000000u | (payload << 11)) ^ 0x00700000u;

    POCSAGState st{&ecc()};
    CHECK(!pocsag_decode_batch(batch_of(words), st));

    CHECK_STR_EQ(st.output, "??");
    CHECK(st.errors >= 3u);
}

/* --- numeric decode ------------------------------------------------------- */

TEST(pocsag_decodes_numeric_pages) {
    /* Upstream's encoder produces the codewords; the decoder has to get the
     * digits back. */
    std::vector<uint32_t> cws;
    pocsag_encode(NUMERIC_ONLY, ecc(), 0, "1234567890", 98765, cws);

    const size_t start = preamble_length / 32;
    CHECK_EQ(cws[start], syncword);

    batch_t b{};
    for (size_t i = 0; i < 16; i++) b[i] = cws[start + 1 + i];
    POCSAGPacket p;
    p.set(b);

    POCSAGState st{&ecc()};
    std::string numeric;
    while (pocsag_decode_batch(p, st)) {
        if (st.numeric_len) numeric.assign(st.numeric_buf, st.numeric_len);
    }
    if (st.numeric_len) numeric.assign(st.numeric_buf, st.numeric_len);

    CHECK_EQ(st.address, 98765u);
    CHECK_EQ(st.detected, DET_NUMERIC);
    CHECK_STR_EQ(numeric, "1234567890");
}

TEST(pocsag_numeric_nibbles_are_bit_reversed) {
    /* Digits go out LSB first, so bit 30 of the codeword is the digit's least
     * significant bit. decode_nibble has to undo that. */
    const uint32_t cw = 0x80000000u | (0x1u << 30); /* first nibble = 0b0001 sent LSB-first */
    CHECK_EQ(decode_nibble(cw, 0), 1);

    /* Bit 27 is the last transmitted bit of the first nibble, so it is the
     * digit's most significant bit. */
    const uint32_t cw2 = 0x80000000u | (1u << 27);
    CHECK_EQ(decode_nibble(cw2, 0), 8);

    /* Second nibble occupies bits 26..23. */
    const uint32_t cw3 = 0x80000000u | (1u << 26) | (1u << 24);
    CHECK_EQ(decode_nibble(cw3, 1), 1 | 4);
}

/* --- round trip through upstream's encoder -------------------------------- */

TEST(pocsag_alphanumeric_round_trip) {
    std::vector<uint32_t> cws;
    pocsag_encode(ALPHANUMERIC, ecc(), 3, "HELLO", 1234567, cws);

    POCSAGState st{&ecc()};
    std::string text;
    uint32_t address = 0;
    uint32_t function = 99;

    const size_t start = preamble_length / 32;
    for (size_t p = start; p + 16 < cws.size() + 1; p += 17) {
        if (cws[p] != syncword) break;
        batch_t b{};
        for (size_t i = 0; i < 16; i++) b[i] = cws[p + 1 + i];
        POCSAGPacket pk;
        pk.set(b);

        st.codeword_index = 0;
        st.errors = 0;
        while (pocsag_decode_batch(pk, st)) {
            if (st.out_type == OUT_MESSAGE) {
                text += st.output;
                address = st.address;
                function = st.function;
            }
        }
        if (st.mode != STATE_HAVE_ADDRESS && st.out_type == OUT_MESSAGE) {
            text += st.output;
            address = st.address;
            function = st.function;
        }
    }

    CHECK_EQ(address, 1234567u);
    CHECK_EQ(function, 3u);
    CHECK_STR_EQ(text, "HELLO");
}

TEST(pocsag_address_only_round_trip) {
    std::vector<uint32_t> cws;
    pocsag_encode(ADDRESS_ONLY, ecc(), 2, "", 42, cws);

    const size_t start = preamble_length / 32;
    batch_t b{};
    for (size_t i = 0; i < 16; i++) b[i] = cws[start + 1 + i];
    POCSAGPacket p;
    p.set(b);

    POCSAGState st{&ecc()};
    CHECK(pocsag_decode_batch(p, st)); /* address, then more of the batch */
    CHECK_EQ(st.address, 42u);
    CHECK_EQ(st.function, 2u);
    CHECK_EQ(st.out_type, OUT_ADDRESS);
}

/* --- audio filter --------------------------------------------------------- */

TEST(pocsag_lowpass_matches_upstreams_coefficients) {
    /* baseband/proc_pocsag2.hpp hard-codes these from
     * scipy.signal.butter(2, 1800, "lowpass", fs=24000). */
    const auto c = design_butterworth_lowpass(1800.0, 24000.0);
    CHECK_NEAR(c.b[0], 0.04125354f, 1e-6);
    CHECK_NEAR(c.b[1], 0.08250707f, 1e-6);
    CHECK_NEAR(c.b[2], 0.04125354f, 1e-6);
    CHECK_NEAR(c.a[0], 1.0f, 1e-9);
    CHECK_NEAR(c.a[1], -1.34896775f, 1e-6);
    CHECK_NEAR(c.a[2], 0.51398189f, 1e-6);
}

TEST(pocsag_lowpass_passes_dc_and_stops_nyquist) {
    Biquad f;
    f.configure(design_butterworth_lowpass(1800.0, 48000.0));
    float dc = 0.0f;
    for (int i = 0; i < 4000; i++) dc = f.process(1.0f);
    CHECK_NEAR(dc, 1.0f, 1e-3);

    f.reset();
    float peak = 0.0f;
    for (int i = 0; i < 4000; i++) {
        const float y = f.process((i & 1) ? 1.0f : -1.0f);
        if (i > 200 && std::fabs(y) > peak) peak = std::fabs(y);
    }
    CHECK(peak < 0.02f);
}

/* --- audio-level decode --------------------------------------------------- */

namespace {

/* Runs a synthesised preamble+batch through the full audio decoder and
 * returns the codeword recovered from frame 7. */
uint32_t decode_from_audio(float audio_rate, int baud, int8_t baud_config, double clock_error) {
    AudioDecoder dec;
    dec.configure(audio_rate, baud_config);
    dec.set_squelch_power(0.0f);

    uint32_t got = 0;
    int packets = 0;
    dec.set_packet_handler([&](const POCSAGPacket& p) {
        got = p[14];
        packets++;
    });

    const uint32_t acw = address_codeword(1234567, 3);
    std::vector<uint32_t> words(16, idleword);
    words[14] = acw;

    const auto bits = batch_bitstream(words, preamble_length);
    const double sps = static_cast<double>(audio_rate) / (baud * (1.0 + clock_error));
    const auto audio = render_audio(bits, sps, 400);

    dec.process(audio.data(), audio.size());
    return packets ? got : 0;
}

}  // namespace

TEST(pocsag_decodes_audio_at_all_three_bit_rates_forced) {
    const uint32_t expect = address_codeword(1234567, 3);
    CHECK_EQ(decode_from_audio(48000.0f, 512, 0, 0.0), expect);
    CHECK_EQ(decode_from_audio(48000.0f, 1200, 1, 0.0), expect);
    CHECK_EQ(decode_from_audio(48000.0f, 2400, 2, 0.0), expect);
}

TEST(pocsag_detects_the_bit_rate_from_the_preamble) {
    const uint32_t expect = address_codeword(1234567, 3);
    CHECK_EQ(decode_from_audio(48000.0f, 512, -1, 0.0), expect);
    CHECK_EQ(decode_from_audio(48000.0f, 1200, -1, 0.0), expect);
    CHECK_EQ(decode_from_audio(48000.0f, 2400, -1, 0.0), expect);
}

TEST(pocsag_reports_the_detected_bit_rate) {
    AudioDecoder dec;
    dec.configure(48000.0f, -1);
    dec.set_squelch_power(0.0f);

    uint16_t rate = 0;
    dec.set_packet_handler([&](const POCSAGPacket& p) { rate = p.bitrate(); });

    std::vector<uint32_t> words(16, idleword);
    words[14] = address_codeword(1234567, 3);
    const auto audio = render_audio(batch_bitstream(words, preamble_length), 48000.0 / 1200.0, 400);
    dec.process(audio.data(), audio.size());

    CHECK_EQ(rate, uint16_t{1200});
}

TEST(pocsag_tolerates_transmitter_clock_error) {
    /* A transmitter whose clock is 0.5% off in either direction must still
     * decode; upstream's free-running sampler loses the batch at a tenth of
     * this (see the note on BitExtractor::RateInfo). */
    const uint32_t expect = address_codeword(1234567, 3);
    for (double err : {0.005, -0.005}) {
        CHECK_EQ(decode_from_audio(48000.0f, 512, 0, err), expect);
        CHECK_EQ(decode_from_audio(48000.0f, 1200, 1, err), expect);
        CHECK_EQ(decode_from_audio(48000.0f, 2400, 2, err), expect);
    }
}

TEST(pocsag_decodes_at_awkward_audio_rates) {
    /* The B200's decimation will not always land on a tidy multiple of the
     * baud rate, so the decoder must not depend on one. */
    const uint32_t expect = address_codeword(1234567, 3);
    for (float rate : {22050.0f, 25000.0f, 44100.0f, 50000.0f})
        CHECK_EQ(decode_from_audio(rate, 1200, 1, 0.0), expect);
}

TEST(pocsag_ignores_silence) {
    AudioDecoder dec;
    dec.configure(48000.0f, 1);
    int packets = 0;
    dec.set_packet_handler([&](const POCSAGPacket&) { packets++; });

    std::vector<float> quiet(48000, 0.0f);
    dec.process(quiet.data(), quiet.size());
    CHECK_EQ(packets, 0);
}

/* --- full RF chain -------------------------------------------------------- */

TEST(pocsag_decodes_a_modulated_2fsk_signal) {
    /* Modulate with the Phase A 2FSK modulator, run it through the app's own
     * mix/decimate/discriminate front end, and decode. This exercises every
     * stage the app uses on a real capture except the capture itself. */
    const double src_rate = 240000.0;
    const int baud = 1200;

    const uint32_t acw = address_codeword(1234567, 3);
    std::vector<uint32_t> words(16, idleword);
    words[14] = acw;

    auto bits = batch_bitstream(words, preamble_length);
    for (int i = 0; i < 64; i++) bits.push_back(static_cast<uint8_t>(i & 1));

    /* A POCSAG '1' is the lower tone, so invert before modulating. */
    std::vector<uint8_t> tx;
    tx.reserve(bits.size());
    for (uint8_t b : bits) tx.push_back(b ? uint8_t{0} : uint8_t{1});
    const auto iq = dsp::fsk_modulate(tx, static_cast<float>(src_rate),
                                      static_cast<float>(baud), 4500.0f);

    ChannelFrontEnd fe;
    fe.set_deviation(4500.0);
    fe.configure(src_rate, 48000.0, 7000.0);
    fe.set_offset(0.0);
    CHECK_NEAR(fe.audio_rate(), 48000.0, 1.0);

    std::vector<float> audio;
    fe.process(iq.data(), iq.size(), audio);
    CHECK(audio.size() > 1000);

    AudioDecoder dec;
    dec.configure(static_cast<float>(fe.audio_rate()), -1);
    dec.set_squelch_power(0.0f);
    uint32_t got = 0;
    uint16_t rate = 0;
    dec.set_packet_handler([&](const POCSAGPacket& p) {
        got = p[14];
        rate = p.bitrate();
    });
    dec.process(audio.data(), audio.size());

    CHECK_EQ(got, acw);
    CHECK_EQ(rate, uint16_t{1200});
}

TEST(pocsag_front_end_decimates_to_the_requested_rate) {
    ChannelFrontEnd fe;
    fe.configure(2400000.0, 48000.0, 7000.0);
    CHECK_EQ(fe.decimation(), size_t{50});
    CHECK_NEAR(fe.audio_rate(), 48000.0, 1.0);

    fe.configure(1000000.0, 48000.0, 7000.0);
    CHECK_EQ(fe.decimation(), size_t{21});
    CHECK_NEAR(fe.audio_rate(), 1000000.0 / 21.0, 1.0);
}

/* --- malformed input ------------------------------------------------------ */

TEST(pocsag_decoder_survives_garbage) {
    /* Random-looking codewords must not hang, overrun or crash the state
     * machine; whatever they decode to is allowed, as long as it terminates
     * and the buffers stay in range. */
    std::vector<uint32_t> words;
    uint32_t x = 0x12345678u;
    for (int i = 0; i < 16; i++) {
        x = x * 1664525u + 1013904223u;
        words.push_back(x);
    }

    POCSAGState st{&ecc()};
    int guard = 0;
    const auto p = batch_of(words);
    while (pocsag_decode_batch(p, st) && guard < 32) guard++;
    CHECK(guard < 32);
    CHECK(st.numeric_len <= sizeof(st.numeric_buf));
    CHECK(st.codeword_index <= 16);
}

TEST(pocsag_decoder_without_ecc_does_not_crash) {
    /* POCSAGState is default-constructible with a null ecc pointer; the
     * decoder must treat that as "no correction" rather than dereference it. */
    std::vector<uint32_t> words(16, idleword);
    words[14] = address_codeword(1234567, 3);

    POCSAGState st{};
    CHECK(pocsag_decode_batch(batch_of(words), st));
    CHECK_EQ(st.address, 1234567u);
}

TEST(pocsag_packet_index_is_bounds_checked) {
    POCSAGPacket p;
    p.set(0, 0xDEADBEEFu);
    CHECK_EQ(p[0], 0xDEADBEEFu);
    CHECK_EQ(p[16], 0u);
    CHECK_EQ(p[1000], 0u);
    p.set(99, 0x12345678u); /* out of range: must be ignored */
    CHECK_EQ(p[0], 0xDEADBEEFu);
}

TEST(pocsag_empty_audio_block_is_a_no_op) {
    AudioDecoder dec;
    dec.configure(48000.0f, -1);
    int packets = 0;
    dec.set_packet_handler([&](const POCSAGPacket&) { packets++; });
    dec.process(nullptr, 0);
    CHECK_EQ(packets, 0);
}

TEST(pocsag_bitrate_and_flag_strings) {
    CHECK_STR_EQ(bitrate_str(512), "512bps ");
    CHECK_STR_EQ(bitrate_str(1200), "1200bps");
    CHECK_STR_EQ(bitrate_str(2400), "2400bps");
    CHECK_STR_EQ(bitrate_str(9600), "????");
    CHECK_STR_EQ(flag_str(FLAG_NORMAL), "OK");
    CHECK_STR_EQ(flag_str(FLAG_TIMED_OUT), "TIMED OUT");
}

/* --- message type heuristic ----------------------------------------------- */

TEST(pocsag_heuristic_calls_plain_text_alpha) {
    const uint8_t nibbles[5] = {0x0A, 0x0B, 0x0A, 0x0B, 0x0A};
    CHECK_EQ(detect_message_type("HELLO", nibbles, 5, 2), DET_ALPHA);
}

TEST(pocsag_heuristic_calls_digits_numeric) {
    /* Payload that reads as a phone number under the numeric interpretation
     * and as control characters under the alpha one. */
    const uint8_t nibbles[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0};
    const std::string alpha{"\x01\x02\x03\x04"};
    CHECK_EQ(detect_message_type(alpha, nibbles, 10, 2), DET_NUMERIC);
}

TEST(pocsag_heuristic_calls_an_empty_page_tone_only) {
    CHECK_EQ(detect_message_type("", nullptr, 0, 0), DET_TONE);
}

TEST(pocsag_heuristic_calls_long_messages_alpha) {
    /* Eight or more message codewords cannot be a phone number. */
    const uint8_t nibbles[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0};
    CHECK_EQ(detect_message_type("", nibbles, 10, 8), DET_ALPHA);
}
