/*
 * mayhem-b200 — tests for the Nordic nRF24L01+ (Enhanced ShockBurst) decoder.
 *
 * The bit layout, the CRC and the 0x3C18 initial remainder all come from
 * upstream (firmware/baseband/proc_nrfrx.cpp) and from the nRF24L01+ frame
 * format it implements. The expected values below are computed from the
 * protocol definition, not read back out of this implementation.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "modulate.hpp"
#include "protocol.hpp"
#include "ui_nrf_rx.hpp"

#include <cmath>
#include <vector>

using namespace app::nrf;

namespace {

/* The nRF24L01+ power-on default pipe address, and a payload with an obvious
 * bit pattern. */
constexpr uint64_t kAddress = 0xE7E7E7E7E7ULL;
constexpr uint8_t kPayload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
constexpr uint8_t kPayloadLength = 4;
constexpr uint8_t kPid = 2;
constexpr bool kNoAck = false;

uint16_t make_pcf(uint8_t length, uint8_t pid, bool no_ack) {
    return static_cast<uint16_t>((length << 3) | (pid << 1) | (no_ack ? 1 : 0));
}

/* Builds the on-air bit stream: preamble, address, PCF, payload, CRC. */
std::vector<uint8_t> build_frame_bits(uint64_t address, uint8_t length,
                                      uint8_t pid, bool no_ack,
                                      const uint8_t* payload, uint16_t* crc_out) {
    const uint16_t pcf = make_pcf(length, pid, no_ack);
    const uint16_t crc = esb_crc(address, pcf, payload, length);
    if (crc_out) *crc_out = crc;

    dsp::BitStreamWriter w{dsp::BitOrder::MsbFirst};
    /* Preamble is 0xAA when the address MSB is 1, 0x55 otherwise. */
    const bool msb_set = ((address >> 39) & 1ULL) != 0;
    w.write(msb_set ? 0xAAu : 0x55u, 8);
    w.write(address, 40);
    w.write(pcf, 9);
    for (uint8_t i = 0; i < length; i++) w.write(payload[i], 8);
    w.write(crc, 16);

    /* One bit per byte, which is what the keyer's bit_at() walks after we pack
     * it — here we return the packed bytes plus the bit count via size. */
    std::vector<uint8_t> bytes = w.bytes();
    bytes.push_back(static_cast<uint8_t>(w.bit_count() & 0xFF));
    bytes.push_back(static_cast<uint8_t>((w.bit_count() >> 8) & 0xFF));
    return bytes;
}

/* Modulates `bit_count` bits of `bits` as GFSK and returns the FM
 * discriminator output, padded front and back with unmodulated carrier so the
 * decoder's sliding window can walk across the burst. */
std::vector<float> gfsk_discriminator(const uint8_t* bits, size_t bit_count,
                                      double sample_rate, double bit_rate,
                                      double deviation, size_t pad_samples) {
    dsp::FskKeyer keyer;
    keyer.configure(static_cast<float>(sample_rate), static_cast<float>(bit_rate),
                    static_cast<float>(deviation));
    keyer.set_gaussian(0.5f, 4);
    keyer.set_data(bits, bit_count);
    keyer.set_repeat(1, 0);

    const size_t burst = static_cast<size_t>(
        std::lround(bit_count * sample_rate / bit_rate)) + 64;

    std::vector<dsp::cfloat> iq(pad_samples + burst + pad_samples, dsp::cfloat{1.0f, 0.0f});
    const size_t got = keyer.process(iq.data() + pad_samples, burst);
    /* Anything the keyer did not fill stays as unmodulated carrier. */
    for (size_t i = pad_samples + got; i < iq.size(); i++) iq[i] = dsp::cfloat{1.0f, 0.0f};

    dsp::FmDemod fm;
    fm.configure(static_cast<float>(sample_rate), static_cast<float>(deviation));
    std::vector<float> out;
    fm.process(iq.data(), iq.size(), out);
    return out;
}

}  // namespace

TEST(nrf_crc16_matches_the_ccitt_check_vector) {
    /* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final
     * XOR. The standard Rocksoft check value over "123456789" is 0x29B1. */
    const uint8_t check[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK_EQ(crc16(check, sizeof(check), 0xFFFF), 0x29B1u);
}

TEST(nrf_0x3c18_is_0xffff_after_seven_zero_bits) {
    /* This is the whole justification for upstream's byte-wise CRC over a
     * left-padded header. Feed 0x3C18 seven zero bits and the register must
     * arrive at the datasheet's 0xFFFF initial value. */
    dsp::Crc<16> crc{0x1021, 0x3C18, 0x0000};
    for (int i = 0; i < 7; i++) crc.process_bit(false);
    CHECK_EQ(crc.checksum(), 0xFFFFu);
}

TEST(nrf_padded_byte_crc_equals_the_bitwise_esb_crc) {
    const uint16_t pcf = make_pcf(kPayloadLength, kPid, kNoAck);

    /* Upstream's form: 49 header bits left-padded into 7 bytes, init 0x3C18. */
    uint8_t packed[7 + 4]{};
    uint64_t header = kAddress;
    header <<= 9;
    header |= pcf;
    for (size_t c = 0; c < 7; c++) packed[c] = static_cast<uint8_t>((header >> ((6 - c) * 8)) & 0xFF);
    for (size_t c = 0; c < kPayloadLength; c++) packed[c + 7] = kPayload[c];
    const uint16_t byte_form = crc16(packed, 7 + kPayloadLength);

    /* The datasheet's form: init 0xFFFF over 40 + 9 + payload bits. */
    const uint16_t bit_form = esb_crc(kAddress, pcf, kPayload, kPayloadLength);

    CHECK_EQ(byte_form, bit_form);
    /* And the padding really is seven zero bits and not, say, eight. */
    CHECK(byte_form != crc16(packed, 7 + kPayloadLength, 0xFFFF));
}

TEST(nrf_pcf_field_split_matches_the_frame_format) {
    /* 6-bit length, 2-bit PID, 1-bit NO_ACK, packed most significant first. */
    const uint16_t pcf = make_pcf(32, 3, true);
    CHECK_EQ(pcf, static_cast<uint16_t>((32u << 3) | (3u << 1) | 1u));
    CHECK_EQ(static_cast<uint8_t>(pcf >> 3), 32u);
    CHECK_EQ(static_cast<uint8_t>((pcf >> 1) & 3), 3u);
    CHECK_EQ(static_cast<uint8_t>(pcf & 1), 1u);
    /* Nine bits, so it fits in the field upstream reads. */
    CHECK(pcf < 512u);
}

TEST(nrf_decoder_recovers_a_synthesised_gfsk_burst) {
    /* 1 Msps / 250 kbps = 4 samples per bit, which is proc_nrfrx's g_srate.
     * Deviation +/-160 kHz is the nRF24L01+ 250 kbps figure. */
    constexpr double kSampleRate = 1'000'000.0;
    constexpr double kBitRate = 250'000.0;
    constexpr double kDeviation = 160'000.0;

    uint16_t expected_crc = 0;
    std::vector<uint8_t> packed =
        build_frame_bits(kAddress, kPayloadLength, kPid, kNoAck, kPayload, &expected_crc);
    const size_t bit_count = static_cast<size_t>(packed[packed.size() - 2]) |
                             (static_cast<size_t>(packed[packed.size() - 1]) << 8);
    packed.resize(packed.size() - 2);
    CHECK_EQ(bit_count, 8u + 40u + 9u + 32u + 16u);

    Decoder decoder;
    decoder.configure(4);
    /* The ring must hold a maximum-length frame: 329 bits * 4 samples. */
    CHECK_EQ(decoder.ring_size(), kMaxFrameBits * 4 + 1);

    std::vector<Packet> got;
    decoder.set_on_packet([&got](const Packet& p) { got.push_back(p); });

    const std::vector<float> disc = gfsk_discriminator(
        packed.data(), bit_count, kSampleRate, kBitRate, kDeviation,
        decoder.ring_size() + 64);
    decoder.process(disc.data(), disc.size());

    CHECK(!got.empty());
    if (got.empty()) return;

    const Packet& p = got.front();
    CHECK_EQ(p.address, kAddress);
    CHECK_EQ(p.payload_length, kPayloadLength);
    CHECK_EQ(p.pid, kPid);
    CHECK_EQ(p.no_ack, kNoAck);
    CHECK_EQ(p.crc, expected_crc);
    CHECK_EQ(p.computed_crc, expected_crc);
    for (uint8_t i = 0; i < kPayloadLength; i++) CHECK_EQ(p.payload[i], kPayload[i]);

    CHECK_STR_EQ(p.address_string(), "E7E7E7E7E7");
    CHECK_STR_EQ(p.payload_string(), "DE AD BE EF");
}

TEST(nrf_decoder_rejects_a_corrupted_crc) {
    constexpr double kSampleRate = 1'000'000.0;
    constexpr double kBitRate = 250'000.0;
    constexpr double kDeviation = 160'000.0;

    uint16_t expected_crc = 0;
    std::vector<uint8_t> packed =
        build_frame_bits(kAddress, kPayloadLength, kPid, kNoAck, kPayload, &expected_crc);
    const size_t bit_count = static_cast<size_t>(packed[packed.size() - 2]) |
                             (static_cast<size_t>(packed[packed.size() - 1]) << 8);
    packed.resize(packed.size() - 2);

    /* Flip one payload bit and leave the CRC alone. Bit 57 is the first
     * payload bit. */
    const size_t bad_bit = 57;
    packed[bad_bit >> 3] = static_cast<uint8_t>(packed[bad_bit >> 3] ^ (0x80u >> (bad_bit & 7)));

    Decoder decoder;
    decoder.configure(4);
    size_t accepted = 0;
    decoder.set_on_packet([&accepted](const Packet&) { accepted++; });

    const std::vector<float> disc = gfsk_discriminator(
        packed.data(), bit_count, kSampleRate, kBitRate, kDeviation,
        decoder.ring_size() + 64);
    decoder.process(disc.data(), disc.size());

    CHECK_EQ(accepted, 0u);
}

TEST(nrf_decoder_ignores_noise_and_dc) {
    Decoder decoder;
    decoder.configure(4);
    size_t accepted = 0;
    decoder.set_on_packet([&accepted](const Packet&) { accepted++; });

    /* A hard DC offset well past the sanity limit: nothing is ever attempted. */
    std::vector<float> dc(decoder.ring_size() * 3, 0.9f);
    decoder.process(dc.data(), dc.size());
    CHECK_EQ(accepted, 0u);

    /* Deterministic pseudo-noise centred on zero: passes the DC check, but no
     * alignment produces a valid CRC. */
    decoder.reset();
    std::vector<float> noise(decoder.ring_size() * 3);
    uint32_t state = 0x12345678u;
    for (size_t i = 0; i < noise.size(); i++) {
        state = state * 1664525u + 1013904223u;
        noise[i] = (static_cast<float>((state >> 8) & 0xFFFF) / 32767.5f) - 1.0f;
    }
    decoder.process(noise.data(), noise.size());
    CHECK_EQ(accepted, 0u);
}

TEST(nrf_decoder_rejects_an_over_long_payload_length) {
    /* A PCF claiming 63 bytes is impossible on an nRF24L01+ (32 is the
     * maximum) and is what overruns upstream's 50-byte CRC scratch. Build the
     * bit pattern by hand, straight into the decoder's slicer, so the frame
     * reaches the length check. */
    constexpr double kSampleRate = 1'000'000.0;
    constexpr double kBitRate = 250'000.0;
    constexpr double kDeviation = 160'000.0;

    dsp::BitStreamWriter w{dsp::BitOrder::MsbFirst};
    w.write(0xAAu, 8);
    w.write(kAddress, 40);
    w.write(make_pcf(63, 0, false), 9);
    for (int i = 0; i < 63; i++) w.write(0x00u, 8);
    w.write(0x0000u, 16);

    std::vector<uint8_t> bytes = w.bytes();

    Decoder decoder;
    decoder.configure(4);
    size_t accepted = 0;
    decoder.set_on_packet([&accepted](const Packet&) { accepted++; });

    const std::vector<float> disc = gfsk_discriminator(
        bytes.data(), w.bit_count(), kSampleRate, kBitRate, kDeviation,
        decoder.ring_size() + 64);
    decoder.process(disc.data(), disc.size());

    CHECK_EQ(accepted, 0u);
}

TEST(nrf_decoder_handles_a_maximum_length_payload) {
    constexpr double kSampleRate = 1'000'000.0;
    constexpr double kBitRate = 250'000.0;
    constexpr double kDeviation = 160'000.0;

    uint8_t payload[kMaxPayloadBytes];
    for (size_t i = 0; i < kMaxPayloadBytes; i++) payload[i] = static_cast<uint8_t>(i * 7 + 1);

    uint16_t expected_crc = 0;
    std::vector<uint8_t> packed = build_frame_bits(
        kAddress, static_cast<uint8_t>(kMaxPayloadBytes), 1, true, payload, &expected_crc);
    const size_t bit_count = static_cast<size_t>(packed[packed.size() - 2]) |
                             (static_cast<size_t>(packed[packed.size() - 1]) << 8);
    packed.resize(packed.size() - 2);
    CHECK_EQ(bit_count, kMaxFrameBits);

    Decoder decoder;
    decoder.configure(4);
    std::vector<Packet> got;
    decoder.set_on_packet([&got](const Packet& p) { got.push_back(p); });

    const std::vector<float> disc = gfsk_discriminator(
        packed.data(), bit_count, kSampleRate, kBitRate, kDeviation,
        decoder.ring_size() + 64);
    decoder.process(disc.data(), disc.size());

    CHECK(!got.empty());
    if (got.empty()) return;
    const Packet& p = got.front();
    CHECK_EQ(p.payload_length, static_cast<uint8_t>(kMaxPayloadBytes));
    CHECK_EQ(p.pid, 1u);
    CHECK(p.no_ack);
    CHECK_EQ(p.crc, expected_crc);
    for (size_t i = 0; i < kMaxPayloadBytes; i++) CHECK_EQ(p.payload[i], payload[i]);
}
