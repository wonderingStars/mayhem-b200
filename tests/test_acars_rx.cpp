/*
 * mayhem-b200 — ACARS receiver decode tests.
 *
 * Everything below the antenna connector is exercised here: the odd-parity
 * character check, the CRC-16/CCITT (anchored on the published CRC-16/XMODEM
 * check value), the frame field parse, the block framing state machine in both
 * bit orders and on both of its terminators, and the whole MSK chain from an
 * AM-modulated complex baseband down to a decoded block.
 *
 * No radio is attached, so nothing here proves reception off the air; it proves
 * the decoder is correct when it is fed samples.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_acars_rx.hpp"

#include "../src/dsp/demod_digital.hpp"
#include "../src/dsp/protocol.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace app;

namespace {

constexpr double kPi = 3.14159265358979323846;

void push_char_lsb_first(std::vector<uint8_t>& bits, uint8_t ch) {
    for (int i = 0; i < 8; i++) bits.push_back(static_cast<uint8_t>((ch >> i) & 1));
}

void push_char_msb_first(std::vector<uint8_t>& bits, uint8_t ch) {
    for (int i = 7; i >= 0; i--) bits.push_back(static_cast<uint8_t>((ch >> i) & 1));
}

void push_preamble(std::vector<uint8_t>& bits, size_t count) {
    /* ARINC 618 pre-key: an alternating bit pattern. It can never be mistaken
     * for SYN (0x16) at any window offset, so the sliding sync search walks
     * straight through it. */
    for (size_t i = 0; i < count; i++) bits.push_back(static_cast<uint8_t>(i & 1));
}

/* The body of a plausible downlink block, every character chosen to already
 * carry odd parity so the byte values are plain ASCII and the expected strings
 * below are readable. Offsets follow acars_decode()'s documented layout. */
std::vector<uint8_t> sample_block_body() {
    const std::string body =
        "2"          /* [0]      mode character                    */
        "CEFIJLO"    /* [1..7]   aircraft registration             */
        "\x02"       /* [8]      ack / STX                         */
        "Q1"         /* [9..10]  label                             */
        "R"          /* [11]     block id                          */
        "T24"        /* [12..14] message number                    */
        "WX1278"     /* [15..20] flight id                         */
        "OIL 1278";  /* [21..]   free text                         */

    std::vector<uint8_t> out(body.begin(), body.end());
    out.push_back(kAcarsEtx);
    return out;
}

/* A complete on-air block: pre-key, SYN SYN SOH, the body, the two CRC bytes
 * and the closing DEL. */
std::vector<uint8_t> sample_block_bits(const std::vector<uint8_t>& body,
                                       bool msb_first,
                                       size_t preamble_bits = 32) {
    const uint16_t crc = acars_crc16(body.data(), body.size());

    std::vector<uint8_t> bits;
    push_preamble(bits, preamble_bits);

    auto push = [&](uint8_t ch) {
        if (msb_first)
            push_char_msb_first(bits, ch);
        else
            push_char_lsb_first(bits, ch);
    };

    push(kAcarsSyn);
    push(kAcarsSyn);
    push(kAcarsSoh);
    for (const uint8_t ch : body) push(ch);
    push(static_cast<uint8_t>(crc >> 8));
    push(static_cast<uint8_t>(crc & 0xFF));
    push(kAcarsDle);
    return bits;
}

std::string to_string(const std::vector<uint8_t>& v) {
    return std::string(v.begin(), v.end());
}

}  // namespace

/* ===========================================================================
 * Parity
 * ===========================================================================*/

TEST(acars_parity_is_odd_over_the_whole_character) {
    /* Upstream's own framing constants are the proof that the parity bit lives
     * in bit 7 and that the check is for an odd number of set bits: ETX is
     * ASCII 0x03 with bit 7 set, ETB is 0x17 with bit 7 set, and SOH, STX, SYN
     * and DEL already have an odd population count. */
    CHECK(acars_parity_ok(kAcarsSoh));
    CHECK(acars_parity_ok(kAcarsStx));
    CHECK(acars_parity_ok(kAcarsSyn));
    CHECK(acars_parity_ok(kAcarsEtx));
    CHECK(acars_parity_ok(kAcarsEtb));
    CHECK(acars_parity_ok(kAcarsDle));

    /* 'A' is 0x41, two bits set — even, so it fails until the parity bit is on. */
    CHECK(!acars_parity_ok(0x41));
    CHECK(acars_parity_ok(0xC1));

    CHECK(!acars_parity_ok(0x00));
    CHECK(acars_parity_ok(0xFF) == false); /* eight bits set: even */
}

TEST(acars_add_parity_reproduces_the_framing_constants) {
    CHECK_EQ(static_cast<int>(acars_add_parity(0x03)), static_cast<int>(kAcarsEtx));
    CHECK_EQ(static_cast<int>(acars_add_parity(0x17)), static_cast<int>(kAcarsEtb));
    CHECK_EQ(static_cast<int>(acars_add_parity(0x01)), static_cast<int>(kAcarsSoh));
    CHECK_EQ(static_cast<int>(acars_add_parity(0x02)), static_cast<int>(kAcarsStx));
    CHECK_EQ(static_cast<int>(acars_add_parity(0x16)), static_cast<int>(kAcarsSyn));
    CHECK_EQ(static_cast<int>(acars_add_parity(0x7F)), static_cast<int>(kAcarsDle));

    /* Every result must pass the check, and only bit 7 may ever change. */
    for (int ch = 0; ch < 128; ch++) {
        const uint8_t withp = acars_add_parity(static_cast<uint8_t>(ch));
        CHECK(acars_parity_ok(withp));
        CHECK_EQ(static_cast<int>(withp & 0x7F), ch);
    }
}

/* ===========================================================================
 * CRC
 * ===========================================================================*/

TEST(acars_crc16_matches_the_published_xmodem_check_value) {
    /* ACARS uses CRC-16/CCITT with poly 0x1021, init 0x0000, no reflection and
     * no final XOR — the CRC-16/XMODEM parameter set, whose catalogue check
     * value (the CRC of the nine ASCII bytes "123456789") is 0x31C3. */
    const std::string check = "123456789";
    CHECK_EQ(static_cast<int>(acars_crc16(check, check.size())), 0x31C3);
}

TEST(acars_crc16_agrees_with_the_generic_crc_engine) {
    /* Same parameters through the Phase A bit-at-a-time engine. Two independent
     * implementations agreeing over a non-trivial buffer. */
    std::vector<uint8_t> data(257);
    uint32_t x = 12345u;
    for (auto& b : data) {
        x = x * 1103515245u + 12345u;
        b = static_cast<uint8_t>((x >> 16) & 0xFF);
    }

    dsp::Crc<16> engine{0x1021, 0x0000, 0x0000};
    engine.process_bytes(data.data(), data.size());

    CHECK_EQ(static_cast<uint32_t>(acars_crc16(data.data(), data.size())), engine.checksum());
}

TEST(acars_crc16_of_nothing_is_the_initial_value) {
    CHECK_EQ(static_cast<int>(acars_crc16(nullptr, 0)), 0x0000);
}

/* ===========================================================================
 * Frame parse (acars_decode / acars_format)
 * ===========================================================================*/

TEST(acars_decode_extracts_every_field_and_verifies_the_crc) {
    const auto body = sample_block_body();
    const uint16_t crc = acars_crc16(body.data(), body.size());

    std::string raw = to_string(body);
    raw.push_back(static_cast<char>(crc >> 8));
    raw.push_back(static_cast<char>(crc & 0xFF));

    const AcarsDecoded d = acars_decode(raw);

    CHECK(d.crc_ok);
    CHECK_STR_EQ(d.reg, "CEFIJLO");
    CHECK_STR_EQ(d.label, "Q1");
    CHECK_EQ(d.block_id, 'R');
    CHECK_STR_EQ(d.msg_num, "T24");
    CHECK_STR_EQ(d.flight_id, "WX1278");
    /* Free text runs from offset 21 to the byte before the CRC, so it takes the
     * ETX terminator with it — upstream's layout, not an artefact here. */
    CHECK_STR_EQ(d.txt, std::string("OIL 1278") + static_cast<char>(kAcarsEtx));
}

TEST(acars_decode_reports_a_bad_crc) {
    const auto body = sample_block_body();
    const uint16_t crc = acars_crc16(body.data(), body.size());

    std::string raw = to_string(body);
    raw.push_back(static_cast<char>(crc >> 8));
    raw.push_back(static_cast<char>(crc & 0xFF));

    /* One bit flipped anywhere in the covered bytes must fail. */
    raw[5] = static_cast<char>(raw[5] ^ 0x01);
    const AcarsDecoded d = acars_decode(raw);
    CHECK(!d.crc_ok);
    /* The fields are still extracted — upstream reports them either way. */
    CHECK_STR_EQ(d.label, "Q1");
}

TEST(acars_decode_rejects_a_frame_shorter_than_the_fixed_header) {
    const std::string raw(kAcarsMinLen - 1, 'X');
    const AcarsDecoded d = acars_decode(raw);
    CHECK(!d.crc_ok);
    CHECK(d.reg.empty());
    CHECK(d.txt.find("too short") != std::string::npos);
}

TEST(acars_format_lays_out_every_field) {
    AcarsDecoded d;
    d.crc_ok = true;
    d.reg = "CEFIJLO";
    d.label = "Q1";
    d.block_id = 'R';
    d.msg_num = "T24";
    d.flight_id = "WX1278";
    d.txt = "OIL";

    CHECK_STR_EQ(acars_format(d),
                 "ACARS Decoded Result\nCRC: OK"
                 "\nRegistration: CEFIJLO"
                 "\nLabel: Q1"
                 "\nBlockID: R"
                 "\nMsgNum: T24"
                 "\nFlightID: WX1278"
                 "\nMessage: OIL");

    d.crc_ok = false;
    CHECK(acars_format(d).find("CRC: FAIL") != std::string::npos);
}

/* ===========================================================================
 * Block framing state machine
 * ===========================================================================*/

TEST(acars_bit_decoder_frames_a_known_block_lsb_first) {
    const auto body = sample_block_body();
    const auto bits = sample_block_bits(body, /*msb_first=*/false);

    std::vector<AcarsBlock> blocks;
    AcarsBitDecoder dec;
    dec.on_block = [&](const AcarsBlock& b) { blocks.push_back(b); };
    dec.feed_bits(bits);

    CHECK_EQ(blocks.size(), size_t{1});
    if (blocks.empty()) return;

    CHECK_EQ(blocks[0].message.size(), body.size());
    CHECK(blocks[0].message == body);
    CHECK_EQ(static_cast<int>(blocks[0].parity_errors), 0);

    const uint16_t crc = acars_crc16(body.data(), body.size());
    CHECK_EQ(static_cast<int>(blocks[0].crc_high), static_cast<int>(crc >> 8));
    CHECK_EQ(static_cast<int>(blocks[0].crc_low), static_cast<int>(crc & 0xFF));

    /* The block hands acars_decode() message||crc, which is exactly the ARINC
     * 618 block-check coverage, so the CRC must verify. */
    const AcarsDecoded d = acars_decode(blocks[0].raw());
    CHECK(d.crc_ok);
    CHECK_STR_EQ(d.reg, "CEFIJLO");
    CHECK_STR_EQ(d.flight_id, "WX1278");
}

TEST(acars_bit_decoder_frames_a_known_block_msb_first) {
    /* Upstream's own bit order, still selectable. */
    const auto body = sample_block_body();
    const auto bits = sample_block_bits(body, /*msb_first=*/true);

    std::vector<AcarsBlock> blocks;
    AcarsBitDecoder dec;
    dec.set_bit_order(AcarsBitDecoder::BitOrder::MsbFirst);
    dec.on_block = [&](const AcarsBlock& b) { blocks.push_back(b); };
    dec.feed_bits(bits);

    CHECK_EQ(blocks.size(), size_t{1});
    if (blocks.empty()) return;
    CHECK(blocks[0].message == body);
    CHECK(acars_decode(blocks[0].raw()).crc_ok);
}

TEST(acars_bit_decoder_ignores_a_stream_in_the_wrong_bit_order) {
    /* The mismatch has to be visible, otherwise the bit-order option would be
     * cosmetic: an MSB-first stream must not frame in the LSB-first decoder. */
    const auto body = sample_block_body();
    const auto bits = sample_block_bits(body, /*msb_first=*/true);

    size_t emitted = 0;
    AcarsBitDecoder dec;
    dec.on_block = [&](const AcarsBlock&) { emitted++; };
    dec.feed_bits(bits);

    CHECK_EQ(emitted, size_t{0});
}

TEST(acars_bit_decoder_resyncs_after_leading_rubbish) {
    /* The WSYN window slides one bit at a time, so any amount of junk before
     * the pre-key must be walked through. */
    const auto body = sample_block_body();
    auto bits = sample_block_bits(body, false, /*preamble_bits=*/0);

    std::vector<uint8_t> junk;
    uint32_t x = 99u;
    for (int i = 0; i < 137; i++) {
        x = x * 1103515245u + 12345u;
        junk.push_back(static_cast<uint8_t>((x >> 20) & 1));
    }
    junk.insert(junk.end(), bits.begin(), bits.end());

    std::vector<AcarsBlock> blocks;
    AcarsBitDecoder dec;
    dec.on_block = [&](const AcarsBlock& b) { blocks.push_back(b); };
    dec.feed_bits(junk);

    CHECK_EQ(blocks.size(), size_t{1});
    if (!blocks.empty()) CHECK(blocks[0].message == body);
}

TEST(acars_bit_decoder_uses_the_dle_shortcut_when_etx_is_missing) {
    /* A block whose ETX was lost still terminates: past twenty characters, a
     * DEL means the two characters before it were the CRC. Upstream reaches the
     * CRC2 branch inside the same call by reloading the bit register — this is
     * the test that the shortcut still works after the port. */
    std::vector<uint8_t> body;
    const std::string text = "2CEFIJLO\x02Q1RT24WX1278OIL 1278EXTRA";
    for (const char c : text) body.push_back(acars_add_parity(static_cast<uint8_t>(c)));
    CHECK(body.size() > 20);

    const uint16_t crc = acars_crc16(body.data(), body.size());

    std::vector<uint8_t> bits;
    push_preamble(bits, 32);
    push_char_lsb_first(bits, kAcarsSyn);
    push_char_lsb_first(bits, kAcarsSyn);
    push_char_lsb_first(bits, kAcarsSoh);
    for (const uint8_t ch : body) push_char_lsb_first(bits, ch);
    push_char_lsb_first(bits, static_cast<uint8_t>(crc >> 8));
    push_char_lsb_first(bits, static_cast<uint8_t>(crc & 0xFF));
    push_char_lsb_first(bits, kAcarsDle);

    std::vector<AcarsBlock> blocks;
    AcarsBitDecoder dec;
    dec.on_block = [&](const AcarsBlock& b) { blocks.push_back(b); };
    dec.feed_bits(bits);

    CHECK_EQ(blocks.size(), size_t{1});
    if (blocks.empty()) return;

    /* DEL and the two CRC characters must have been lifted back out of the
     * message. */
    CHECK_EQ(blocks[0].message.size(), body.size());
    CHECK(blocks[0].message == body);
    CHECK_EQ(static_cast<int>(blocks[0].crc_high), static_cast<int>(crc >> 8));
    CHECK_EQ(static_cast<int>(blocks[0].crc_low), static_cast<int>(crc & 0xFF));
    CHECK(acars_decode(blocks[0].raw()).crc_ok);
}

TEST(acars_bit_decoder_accepts_four_parity_errors_and_drops_the_fifth) {
    auto body_with_bad = [](int bad_count) {
        auto body = sample_block_body();
        /* Clear bit 7 on the first `bad_count` free-text characters, which
         * turns each of them from odd to even parity. */
        for (int i = 0; i < bad_count; i++)
            body[21 + static_cast<size_t>(i)] =
                static_cast<uint8_t>(body[21 + static_cast<size_t>(i)] ^ 0x01);
        return body;
    };

    for (int bad = 0; bad <= 4; bad++) {
        const auto body = body_with_bad(bad);
        const auto bits = sample_block_bits(body, false);

        std::vector<AcarsBlock> blocks;
        AcarsBitDecoder dec;
        dec.on_block = [&](const AcarsBlock& b) { blocks.push_back(b); };
        dec.feed_bits(bits);

        CHECK_EQ(blocks.size(), size_t{1});
        if (!blocks.empty()) CHECK_EQ(static_cast<int>(blocks[0].parity_errors), bad);
    }

    /* Five is one too many: upstream abandons the block. */
    const auto body = body_with_bad(5);
    const auto bits = sample_block_bits(body, false);

    size_t emitted = 0;
    AcarsBitDecoder dec;
    dec.on_block = [&](const AcarsBlock&) { emitted++; };
    dec.feed_bits(bits);
    CHECK_EQ(emitted, size_t{0});
}

TEST(acars_bit_decoder_abandons_an_overlong_block) {
    /* No terminator ever arrives: past 240 characters upstream resets rather
     * than running off the end of its 250-byte buffer. */
    std::vector<uint8_t> bits;
    push_preamble(bits, 32);
    push_char_lsb_first(bits, kAcarsSyn);
    push_char_lsb_first(bits, kAcarsSyn);
    push_char_lsb_first(bits, kAcarsSoh);
    for (int i = 0; i < 300; i++) push_char_lsb_first(bits, acars_add_parity('X'));

    size_t emitted = 0;
    AcarsBitDecoder dec;
    dec.on_block = [&](const AcarsBlock&) { emitted++; };
    dec.feed_bits(bits);

    CHECK_EQ(emitted, size_t{0});
    CHECK(dec.message_length() <= kAcarsMessageCapacity);
}

TEST(acars_bit_decoder_requires_two_syns_then_soh) {
    /* SYN, SYN, then something that is not SOH: upstream throws the frame away
     * and goes back to hunting. */
    std::vector<uint8_t> bits;
    push_preamble(bits, 16);
    push_char_lsb_first(bits, kAcarsSyn);
    push_char_lsb_first(bits, kAcarsSyn);
    push_char_lsb_first(bits, acars_add_parity('Z'));

    std::vector<AcarsState> states;
    AcarsBitDecoder dec;
    dec.on_state = [&](AcarsState s, uint8_t) { states.push_back(s); };
    dec.feed_bits(bits);

    CHECK_EQ(dec.state(), AcarsState::WaitSyn);
    /* Reaching SOH1 is reported; being kicked back to WSYN is reported too. */
    CHECK(states.size() >= 2);
    CHECK_EQ(states.front(), AcarsState::Soh1);
    CHECK_EQ(states.back(), AcarsState::WaitSyn);
}

TEST(acars_bit_decoder_terminates_on_etb_as_well_as_etx) {
    auto body = sample_block_body();
    body.back() = kAcarsEtb;
    const auto bits = sample_block_bits(body, false);

    std::vector<AcarsBlock> blocks;
    AcarsBitDecoder dec;
    dec.on_block = [&](const AcarsBlock& b) { blocks.push_back(b); };
    dec.feed_bits(bits);

    CHECK_EQ(blocks.size(), size_t{1});
    if (!blocks.empty()) CHECK_EQ(static_cast<int>(blocks[0].message.back()),
                                  static_cast<int>(kAcarsEtb));
}

TEST(acars_bit_decoder_slices_soft_symbols_at_zero) {
    /* consume_symbol()'s slicer: >= 0 is a one. */
    const auto body = sample_block_body();
    const auto bits = sample_block_bits(body, false);

    std::vector<AcarsBlock> blocks;
    AcarsBitDecoder dec;
    dec.on_block = [&](const AcarsBlock& b) { blocks.push_back(b); };
    for (const uint8_t b : bits) dec.feed_symbol(b ? 0.7f : -0.7f);

    CHECK_EQ(blocks.size(), size_t{1});
    if (!blocks.empty()) CHECK(blocks[0].message == body);
}

/* ===========================================================================
 * MSK front end
 * ===========================================================================*/

TEST(acars_msk_audio_round_trips_through_the_demodulator) {
    /* ACARS is MSK at 2400 bit/s: a one is the 2400 Hz tone, a zero the
     * 1200 Hz tone. Generate exactly that with the Phase A modulator and
     * decode it back into a block. */
    constexpr float fs = 24000.0f; /* ten samples per bit */

    const auto body = sample_block_body();
    const auto bits = sample_block_bits(body, false, /*preamble_bits=*/96);

    const auto audio = dsp::afsk_modulate(bits, fs, kAcarsMarkHz, kAcarsSpaceHz, kAcarsBaud, 0.8f);
    CHECK_EQ(audio.size(), bits.size() * 10);

    std::vector<AcarsBlock> blocks;
    AcarsAudioDecoder decoder;
    decoder.configure(fs);
    decoder.decoder().on_block = [&](const AcarsBlock& b) { blocks.push_back(b); };
    decoder.process_audio(audio.data(), audio.size());

    CHECK_EQ(blocks.size(), size_t{1});
    if (blocks.empty()) return;
    CHECK(blocks[0].message == body);
    CHECK(acars_decode(blocks[0].raw()).crc_ok);
}

TEST(acars_am_baseband_round_trips_through_the_whole_chain) {
    /* The same MSK audio amplitude-modulated onto a carrier at DC, which is
     * what the channel path hands the app after mixing and decimation. */
    constexpr float fs = 24000.0f;

    const auto body = sample_block_body();
    const auto bits = sample_block_bits(body, false, /*preamble_bits=*/96);
    const auto audio = dsp::afsk_modulate(bits, fs, kAcarsMarkHz, kAcarsSpaceHz, kAcarsBaud, 0.8f);

    std::vector<dsp::cfloat> baseband(audio.size());
    for (size_t i = 0; i < audio.size(); i++) {
        const float envelope = 1.0f + 0.8f * audio[i];
        baseband[i] = dsp::cfloat{envelope, 0.0f};
    }

    std::vector<AcarsBlock> blocks;
    AcarsAudioDecoder decoder;
    decoder.configure(fs);
    decoder.decoder().on_block = [&](const AcarsBlock& b) { blocks.push_back(b); };
    decoder.process_baseband(baseband.data(), baseband.size());

    CHECK_EQ(blocks.size(), size_t{1});
    if (blocks.empty()) return;
    CHECK(blocks[0].message == body);
    CHECK(acars_decode(blocks[0].raw()).crc_ok);
}

TEST(acars_msk_survives_a_carrier_offset_and_noise) {
    /* A residual tuning error rotates the baseband; AM envelope detection is
     * blind to it, which is the point of detecting rather than discriminating.
     * A little additive noise on top. */
    constexpr float fs = 24000.0f;

    const auto body = sample_block_body();
    const auto bits = sample_block_bits(body, false, /*preamble_bits=*/96);
    const auto audio = dsp::afsk_modulate(bits, fs, kAcarsMarkHz, kAcarsSpaceHz, kAcarsBaud, 0.8f);

    std::vector<dsp::cfloat> baseband(audio.size());
    uint32_t rng = 4242u;
    for (size_t i = 0; i < audio.size(); i++) {
        const double phase = 2.0 * kPi * 300.0 * static_cast<double>(i) / fs;
        const float envelope = 1.0f + 0.8f * audio[i];
        rng = rng * 1103515245u + 12345u;
        const float noise = (static_cast<float>((rng >> 16) & 0xFFFF) / 65535.0f - 0.5f) * 0.05f;
        baseband[i] = dsp::cfloat{static_cast<float>(envelope * std::cos(phase)) + noise,
                                  static_cast<float>(envelope * std::sin(phase))};
    }

    std::vector<AcarsBlock> blocks;
    AcarsAudioDecoder decoder;
    decoder.configure(fs);
    decoder.decoder().on_block = [&](const AcarsBlock& b) { blocks.push_back(b); };
    decoder.process_baseband(baseband.data(), baseband.size());

    CHECK_EQ(blocks.size(), size_t{1});
    if (!blocks.empty()) CHECK(acars_decode(blocks[0].raw()).crc_ok);
}

TEST(acars_state_names_cover_every_state) {
    CHECK_STR_EQ(acars_state_name(AcarsState::WaitSyn), "WSYN");
    CHECK_STR_EQ(acars_state_name(AcarsState::Syn2), "SYN2");
    CHECK_STR_EQ(acars_state_name(AcarsState::Soh1), "SOH1");
    CHECK_STR_EQ(acars_state_name(AcarsState::Text), "TXT");
    CHECK_STR_EQ(acars_state_name(AcarsState::Crc1), "CRC1");
    CHECK_STR_EQ(acars_state_name(AcarsState::Crc2), "CRC2");
    CHECK_STR_EQ(acars_state_name(AcarsState::End), "END");
}
