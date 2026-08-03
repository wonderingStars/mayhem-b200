/*
 * mayhem-b200 — APRS TX encoder tests.
 *
 * The encoder (app::AprsTxFrame) is the deliverable, so everything here is
 * checked against the protocol and against upstream's transmitter, never against
 * whatever the implementation happens to emit:
 *
 *  - The FCS is cross-checked three ways: the published CRC-16/X-25 check value
 *    for "123456789" (0x906E), an independent reflected-0x8408 reference (the
 *    algorithm firmware/baseband/proc_aprsrx.hpp uses), and the residue the
 *    receiver tests over data + FCS.
 *  - The shifted address octets are re-derived by hand from the AX.25 rule
 *    (callsign << 1, SSID octet 0x60 | ssid<<1 | ext) and compared to
 *    frame_bytes().
 *  - The full NRZI + bit-stuffed line-level stream is compared to an independent
 *    transcription of firmware/application/protocols/ax25.cpp — same LSB-first
 *    order, same stuffed zero after five ones, same NRZI sense.
 *  - Encoder output is fed back through the Phase A AFSK demodulator
 *    (dsp::AfskDemod), the AX.25 HDLC decoder (app::Ax25Decoder) and an FM
 *    channel round trip (app::AprsChannelDecoder), and the payload must survive.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_registry.hpp"
#include "bitmaps.hpp"
#include "demod_digital.hpp"
#include "ui_aprs_rx.hpp"  /* Ax25Decoder, AprsPacket, ax25_fcs, AprsChannelDecoder */
#include "ui_aprs_tx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using app::AprsChannelDecoder;
using app::AprsPacket;
using app::AprsPosition;
using app::AprsTxFrame;
using app::Ax25Decoder;

namespace {

constexpr double kPi = 3.14159265358979323846;

/* --- independent reference FCS (upstream's reflected-0x8408 table) --------- */

struct ReferenceCrc {
    uint16_t tab[256];
    ReferenceCrc() {
        for (int i = 0; i < 256; i++) {
            uint16_t crc = static_cast<uint16_t>(i);
            for (int b = 0; b < 8; b++)
                crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0x8408)
                                : static_cast<uint16_t>(crc >> 1);
            tab[i] = crc;
        }
    }
    uint16_t running(const uint8_t* data, size_t length) const {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < length; i++)
            crc = static_cast<uint16_t>(((crc >> 8) ^ tab[(crc ^ data[i]) & 0xFF]) & 0xFFFF);
        return crc;
    }
    uint16_t fcs(const uint8_t* data, size_t length) const {
        return static_cast<uint16_t>(running(data, length) ^ 0xFFFF);
    }
};

const ReferenceCrc& reference_crc() {
    static const ReferenceCrc r;
    return r;
}

/* --- independent AX.25 address / body construction ------------------------- */

/* One shifted AX.25 address octet group, re-derived from the spec:
 * callsign << 1, SSID octet 0x60 | (ssid << 1) | ext. */
std::vector<uint8_t> address(const std::string& call, uint8_t ssid, bool last) {
    std::vector<uint8_t> out;
    std::string c = call;
    c.resize(6, ' ');
    for (size_t i = 0; i < 6; i++) out.push_back(static_cast<uint8_t>(c[i] << 1));
    out.push_back(static_cast<uint8_t>(0x60 | ((ssid & 0x0F) << 1) | (last ? 1 : 0)));
    return out;
}

void append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}
void append(std::vector<uint8_t>& dst, const std::string& src) {
    for (char c : src) dst.push_back(static_cast<uint8_t>(c));
}

std::vector<uint8_t> with_fcs(std::vector<uint8_t> body) {
    const uint16_t fcs = reference_crc().fcs(body.data(), body.size());
    body.push_back(static_cast<uint8_t>(fcs & 0xFF));
    body.push_back(static_cast<uint8_t>((fcs >> 8) & 0xFF));
    return body;
}

/* --- independent NRZI + stuffing reference (transcribed from ax25.cpp) ----- */

struct RefBits {
    std::vector<uint8_t> bits;
    uint8_t level = 0;
    uint8_t ones = 0;

    void emit(uint8_t bit) {
        if (!bit) level ^= 1;
        bits.push_back(level);
    }
    void byte(uint8_t b, bool is_flag) {
        for (int i = 0; i < 8; i++) {
            const uint8_t bit = static_cast<uint8_t>((b >> i) & 1);
            emit(bit);
            if (bit) {
                ones++;
                if (ones == 5 && !is_flag) {
                    emit(0);
                    ones = 0;
                }
            } else {
                ones = 0;
            }
        }
    }
};

/* Independently produce the whole line-level stream for a body + flag counts. */
std::vector<uint8_t> reference_bits(const std::vector<uint8_t>& body,
                                    size_t lead_flags, size_t trail_flags) {
    RefBits r;
    for (size_t i = 0; i < lead_flags; i++) r.byte(0x7E, true);
    for (uint8_t b : body) r.byte(b, false);
    const uint16_t fcs = reference_crc().fcs(body.data(), body.size());
    r.byte(static_cast<uint8_t>(fcs & 0xFF), false);
    r.byte(static_cast<uint8_t>((fcs >> 8) & 0xFF), false);
    for (size_t i = 0; i < trail_flags; i++) r.byte(0x7E, true);
    return r.bits;
}

std::string hex(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    for (uint8_t b : v) {
        s += d[b >> 4];
        s += d[b & 0x0F];
    }
    return s;
}

struct FrameSink {
    std::vector<std::vector<uint8_t>> frames;
    void attach(Ax25Decoder& dec) {
        dec.set_frame_handler(
            [this](const uint8_t* p, size_t n) { frames.emplace_back(p, p + n); });
    }
};

/* The reference frame used throughout, matching ui_aprs_rx.hpp's test frame:
 *   N0CALL-9>APRS,WIDE1-1:!4903.50N/07201.75W-Test */
std::vector<uint8_t> reference_body() {
    std::vector<uint8_t> f;
    append(f, address("APRS", 0, false));
    append(f, address("N0CALL", 9, false));
    append(f, address("WIDE1", 1, true));
    f.push_back(0x03);
    f.push_back(0xF0);
    append(f, "!4903.50N/07201.75W-Test");
    return f;
}

}  // namespace

/* ===========================================================================
 * Frame-byte layout (address shift + SSID octet + control/PID/info)
 * ===========================================================================*/

TEST(aprstx_frame_bytes_match_upstream_layout) {
    AprsTxFrame f;
    f.build("N0CALL", 9, "APRS", 0, "!4903.50N/07201.75W-Test", "WIDE1-1");

    CHECK_STR_EQ(hex(f.frame_bytes()), hex(reference_body()));
    /* 3 x 7 address bytes + control + PID + 24 info bytes = 47. */
    CHECK_EQ(f.frame_bytes().size(), 47u);
}

TEST(aprstx_address_octet_encoding) {
    /* SSID 9, not last: 0x60 | (9<<1) | 0 = 0x72. SSID 1, last: 0x63. */
    AprsTxFrame f;
    f.build("N0CALL", 9, "APRS", 0, "x", "WIDE1-1");
    const auto& b = f.frame_bytes();

    /* Destination "APRS  " shifted, then its SSID octet (0, not last). */
    CHECK_EQ(b[0], static_cast<uint8_t>('A' << 1));
    CHECK_EQ(b[1], static_cast<uint8_t>('P' << 1));
    CHECK_EQ(b[2], static_cast<uint8_t>('R' << 1));
    CHECK_EQ(b[3], static_cast<uint8_t>('S' << 1));
    CHECK_EQ(b[4], static_cast<uint8_t>(' ' << 1));
    CHECK_EQ(b[5], static_cast<uint8_t>(' ' << 1));
    CHECK_EQ(b[6], 0x60u);  /* ssid 0, not last */
    /* Source "N0CALL" shifted, SSID 9, not last (a path follows). */
    CHECK_EQ(b[7], static_cast<uint8_t>('N' << 1));
    CHECK_EQ(b[8], static_cast<uint8_t>('0' << 1));
    CHECK_EQ(b[13], static_cast<uint8_t>(0x60 | (9 << 1)));  /* 0x72 */
    /* Path "WIDE1" SSID 1, last -> extension bit set. */
    CHECK_EQ(b[20], static_cast<uint8_t>(0x60 | (1 << 1) | 1));  /* 0x63 */
    CHECK_EQ(b[21], 0x03u);  /* control */
    CHECK_EQ(b[22], 0xF0u);  /* PID */
}

TEST(aprstx_no_path_sets_extension_on_source) {
    AprsTxFrame f;
    f.build("N0CALL", 0, "APRS", 0, "=test", "");

    std::vector<uint8_t> expect;
    append(expect, address("APRS", 0, false));
    append(expect, address("N0CALL", 0, true));  /* source is last when no path */
    expect.push_back(0x03);
    expect.push_back(0xF0);
    append(expect, "=test");

    CHECK_STR_EQ(hex(f.frame_bytes()), hex(expect));
    /* 14 address + control + PID + 5 info. */
    CHECK_EQ(f.frame_bytes().size(), 21u);
}

/* ===========================================================================
 * FCS
 * ===========================================================================*/

TEST(aprstx_fcs_check_vector) {
    /* CRC-16/X-25 published check value for "123456789". */
    AprsTxFrame f;
    f.build("N0CALL", 0, "123456", 0, "", "");
    (void)f;

    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK_EQ(app::ax25_fcs(check, sizeof(check)), 0x906Eu);
    CHECK_EQ(app::ax25_fcs(check, sizeof(check)), reference_crc().fcs(check, sizeof(check)));
}

TEST(aprstx_fcs_matches_reference_and_residue) {
    AprsTxFrame f;
    f.build("N0CALL", 9, "APRS", 0, "!4903.50N/07201.75W-Test", "WIDE1-1");

    const auto& body = f.frame_bytes();
    const uint16_t theirs = reference_crc().fcs(body.data(), body.size());
    CHECK_EQ(f.fcs(), theirs);
    CHECK_EQ(f.fcs(), app::ax25_fcs(body.data(), body.size()));

    /* data + FCS must give the receiver's residue and pass ax25_frame_valid. */
    auto framed = body;
    framed.push_back(static_cast<uint8_t>(f.fcs() & 0xFF));
    framed.push_back(static_cast<uint8_t>((f.fcs() >> 8) & 0xFF));
    CHECK_EQ(reference_crc().running(framed.data(), framed.size()), 0xF0B8u);
    CHECK(app::ax25_frame_valid(framed.data(), framed.size()));
}

/* ===========================================================================
 * NRZI + bit stuffing (whole line-level stream vs independent reference)
 * ===========================================================================*/

TEST(aprstx_bitstream_matches_ax25_spec) {
    AprsTxFrame f;
    f.build("N0CALL", 9, "APRS", 0, "!4903.50N/07201.75W-Test", "WIDE1-1",
            /*lead_flags=*/4, /*trail_flags=*/2);

    const auto expected = reference_bits(reference_body(), 4, 2);
    CHECK_STR_EQ(hex(f.bits()), hex(expected));
}

TEST(aprstx_bit_stuffing_inserts_zero_after_five_ones) {
    /* 0xFF sends eight ones LSB-first, forcing a stuffed zero after every five.
     * A payload of ones must produce more bits than one with none, for equal
     * length, and the extra count must match the reference's stuffing. */
    AprsTxFrame ones;
    ones.build("N0CALL", 0, "APRS", 0, std::string(4, static_cast<char>(0xFF)), "");
    AprsTxFrame zeros;
    zeros.build("N0CALL", 0, "APRS", 0, std::string(4, static_cast<char>(0x00)), "");

    CHECK(ones.bits().size() > zeros.bits().size());

    /* And the exact stream still matches the spec transcription. */
    std::vector<uint8_t> body;
    append(body, address("APRS", 0, false));
    append(body, address("N0CALL", 0, true));
    body.push_back(0x03);
    body.push_back(0xF0);
    for (int i = 0; i < 4; i++) body.push_back(0xFF);

    const auto expected = reference_bits(body, 4, 2);
    AprsTxFrame f;
    f.build("N0CALL", 0, "APRS", 0, std::string(4, static_cast<char>(0xFF)), "",
            /*lead_flags=*/4, /*trail_flags=*/2);
    CHECK_STR_EQ(hex(f.bits()), hex(expected));

    /* A lone flag (0x7E) is never stuffed: eight bits, no more. A frame with an
     * empty payload begins with lead_flags flags of exactly 8 bits each. */
    AprsTxFrame g;
    g.build("N0CALL", 0, "APRS", 0, "", "", /*lead_flags=*/3, /*trail_flags=*/0);
    /* The first 24 line levels are three unstuffed flags. */
    RefBits r;
    r.byte(0x7E, true);
    r.byte(0x7E, true);
    r.byte(0x7E, true);
    for (size_t i = 0; i < r.bits.size(); i++) CHECK_EQ(g.bits()[i], r.bits[i]);
}

TEST(aprstx_nrzi_sense_zero_flips_one_holds) {
    /* Feed a controlled body and verify the NRZI rule directly: relative to the
     * previous line level, a data 0 changes it and a data 1 keeps it. Decoding
     * the emitted stream back must reproduce the pre-NRZI, pre-stuff data bits
     * of the flags+body. */
    AprsTxFrame f;
    f.build("N0CALL", 0, "APRS", 0, "AB", "", /*lead_flags=*/1, /*trail_flags=*/1);

    /* Re-derive the expected physical stream and compare bit for bit. */
    std::vector<uint8_t> body;
    append(body, address("APRS", 0, false));
    append(body, address("N0CALL", 0, true));
    body.push_back(0x03);
    body.push_back(0xF0);
    append(body, "AB");
    const auto expected = reference_bits(body, 1, 1);

    CHECK_EQ(f.bits().size(), expected.size());
    CHECK_STR_EQ(hex(f.bits()), hex(expected));
}

/* ===========================================================================
 * Round trips
 * ===========================================================================*/

TEST(aprstx_bitstream_decodes_back_to_frame) {
    AprsTxFrame f;
    f.build("N0CALL", 9, "APRS", 0, "!4903.50N/07201.75W-Test", "WIDE1-1", 8, 2);

    Ax25Decoder dec;
    FrameSink sink;
    sink.attach(dec);
    dec.feed_bits(f.bits());

    CHECK_EQ(sink.frames.size(), 1u);
    CHECK_EQ(dec.frames_valid(), 1u);
    CHECK_EQ(dec.frames_bad_fcs(), 0u);

    if (!sink.frames.empty()) {
        auto expected = f.frame_bytes();
        expected.push_back(static_cast<uint8_t>(f.fcs() & 0xFF));
        expected.push_back(static_cast<uint8_t>((f.fcs() >> 8) & 0xFF));
        CHECK_STR_EQ(hex(sink.frames[0]), hex(expected));

        AprsPacket p;
        p.set_bytes(sink.frames[0].data(), sink.frames[0].size());
        CHECK_STR_EQ(p.get_stream_text(),
                     "N0CALL-9>APRS,WIDE1-1:!4903.50N/07201.75W-Test");
        CHECK_NEAR(p.get_position().latitude, 49.0 + 3.50 / 60.0, 1e-5);
    }
}

TEST(aprstx_afsk_roundtrip_bell202) {
    AprsTxFrame f;
    /* A real TXDelay preamble so the slicer and bit clock settle. */
    f.build("N0CALL", 9, "APRS", 0, "!4903.50N/07201.75W-Test", "WIDE1-1", 24, 4);

    constexpr float kAudioRate = 24000.0f;
    const auto audio = dsp::afsk_modulate(f.bits(), kAudioRate, 1200.0f, 2200.0f, 1200.0f, 0.8f);
    CHECK(audio.size() > 1000u);

    dsp::AfskDemod demod;
    demod.configure(kAudioRate, dsp::AfskDemod::Standard::Bell202);

    std::vector<uint8_t> bits;
    demod.process_audio(audio.data(), audio.size(), bits);

    Ax25Decoder dec;
    FrameSink sink;
    sink.attach(dec);
    dec.feed_bits(bits);

    CHECK_EQ(sink.frames.size(), 1u);
    if (!sink.frames.empty()) {
        AprsPacket p;
        p.set_bytes(sink.frames[0].data(), sink.frames[0].size());
        CHECK_STR_EQ(p.get_stream_text(),
                     "N0CALL-9>APRS,WIDE1-1:!4903.50N/07201.75W-Test");
        CHECK_NEAR(p.get_position().latitude, 49.0 + 3.50 / 60.0, 1e-5);
        CHECK_NEAR(p.get_position().longitude, -(72.0 + 1.75 / 60.0), 1e-5);
    }
}

TEST(aprstx_channel_roundtrip_from_iq) {
    AprsTxFrame f;
    f.build("M0ABC", 3, "APRS", 0, "=5132.07N/00007.42W-London", "WIDE1-1,WIDE2-1", 24, 4);

    constexpr double kInputRate = 480000.0;  /* decimates by 20 to 24 kHz */
    constexpr double kOffsetHz = 40000.0;
    constexpr double kDeviationHz = 3000.0;

    const auto audio = dsp::afsk_modulate(f.bits(), static_cast<float>(kInputRate),
                                          1200.0f, 2200.0f, 1200.0f, 1.0f);

    std::vector<dsp::cfloat> iq(audio.size());
    double phase = 0.0;
    for (size_t i = 0; i < audio.size(); i++) {
        iq[i] = dsp::cfloat{static_cast<float>(std::cos(phase)),
                            static_cast<float>(std::sin(phase))};
        const double fr = kOffsetHz + kDeviationHz * static_cast<double>(audio[i]);
        phase += 2.0 * kPi * fr / kInputRate;
        if (phase > kPi) phase -= 2.0 * kPi;
    }

    AprsChannelDecoder decoder;
    decoder.configure(kInputRate, kOffsetHz);
    CHECK(decoder.configured());

    std::vector<AprsPacket> packets;
    decoder.on_packet = [&packets](const AprsPacket& p) { packets.push_back(p); };

    constexpr size_t kBlock = 4096;
    for (size_t i = 0; i < iq.size(); i += kBlock)
        decoder.process(iq.data() + i, std::min(kBlock, iq.size() - i));

    CHECK_EQ(packets.size(), 1u);
    if (!packets.empty()) {
        CHECK(packets[0].is_valid_checksum());
        CHECK_STR_EQ(packets[0].get_stream_text(),
                     "M0ABC-3>APRS,WIDE1-1,WIDE2-1:=5132.07N/00007.42W-London");
        CHECK_NEAR(packets[0].get_position().latitude, 51.0 + 32.07 / 60.0, 1e-4);
        CHECK_NEAR(packets[0].get_position().longitude, -(0.0 + 7.42 / 60.0), 1e-4);
    }
}

/* ===========================================================================
 * Path parsing and boundary payloads
 * ===========================================================================*/

TEST(aprstx_fix_path_matches_upstream_parser) {
    /* "WIDE1-1,WIDE2-1" -> two 7-byte groups: "WIDE1 " + '1', "WIDE2 " + '1'. */
    const std::string fixed = AprsTxFrame::fix_path("WIDE1-1,WIDE2-1");
    CHECK_EQ(fixed.size(), 14u);
    CHECK_STR_EQ(fixed.substr(0, 6), "WIDE1 ");
    CHECK_EQ(static_cast<uint8_t>(fixed[6]), static_cast<uint8_t>('1'));  /* 1 | 0x30 */
    CHECK_STR_EQ(fixed.substr(7, 6), "WIDE2 ");
    CHECK_EQ(static_cast<uint8_t>(fixed[13]), static_cast<uint8_t>('1'));

    /* Lower case is upper-cased; SSID 0 becomes '0'; empty path stays empty. */
    const std::string one = AprsTxFrame::fix_path("relay");
    CHECK_EQ(one.size(), 7u);
    CHECK_STR_EQ(one.substr(0, 6), "RELAY ");
    CHECK_EQ(static_cast<uint8_t>(one[6]), static_cast<uint8_t>('0'));

    CHECK_EQ(AprsTxFrame::fix_path("").size(), 0u);
}

TEST(aprstx_path_with_many_hops_still_decodes) {
    AprsTxFrame f;
    f.build("N0CALL", 0, "APRS", 0, ">status only", "WIDE1-1,WIDE2-2,WIDE3-3", 16, 2);

    Ax25Decoder dec;
    FrameSink sink;
    sink.attach(dec);
    dec.feed_bits(f.bits());

    CHECK_EQ(sink.frames.size(), 1u);
    if (!sink.frames.empty()) {
        AprsPacket p;
        p.set_bytes(sink.frames[0].data(), sink.frames[0].size());
        CHECK_STR_EQ(p.get_source_formatted(), "N0CALL");
        CHECK_STR_EQ(p.get_digipeaters_formatted(), ",WIDE1-1,WIDE2-2,WIDE3-3");
        CHECK_STR_EQ(p.get_information_text_formatted(), ">status only");
    }
}

TEST(aprstx_empty_payload_is_valid) {
    AprsTxFrame f;
    f.build("N0CALL", 0, "APRS", 0, "", "", 8, 2);

    /* Body is 14 address + control + PID = 16 bytes, and its FCS still checks. */
    CHECK_EQ(f.frame_bytes().size(), 16u);
    CHECK_EQ(f.fcs(), reference_crc().fcs(f.frame_bytes().data(), f.frame_bytes().size()));

    Ax25Decoder dec;
    FrameSink sink;
    sink.attach(dec);
    dec.feed_bits(f.bits());

    CHECK_EQ(sink.frames.size(), 1u);
    if (!sink.frames.empty()) {
        AprsPacket p;
        p.set_bytes(sink.frames[0].data(), sink.frames[0].size());
        CHECK_STR_EQ(p.get_source_formatted(), "N0CALL");
        CHECK_STR_EQ(p.get_information_text_formatted(), "");
    }
}

TEST(aprstx_long_callsign_and_ssid_are_clamped) {
    /* Callsigns longer than 6 truncate; SSID is masked to 4 bits. */
    AprsTxFrame f;
    f.build("VERYLONGCALL", 15, "APRS", 0, "x", "");
    const auto& b = f.frame_bytes();

    /* Source occupies bytes 7..13; first six are "VERYLO" shifted, SSID 15 last. */
    CHECK_EQ(b[7], static_cast<uint8_t>('V' << 1));
    CHECK_EQ(b[8], static_cast<uint8_t>('E' << 1));
    CHECK_EQ(b[12], static_cast<uint8_t>('O' << 1));
    CHECK_EQ(b[13], static_cast<uint8_t>(0x60 | (15 << 1) | 1));  /* 0x7F */
}

/* ===========================================================================
 * Position formatting (port of process_coordinates)
 * ===========================================================================*/

TEST(aprstx_format_coordinates) {
    /* 49 deg 03.50' N, 72 deg 01.75' W -> "4903.50N/07201.75W". */
    const std::string s = AprsTxFrame::format_coordinates(49.0f + 3.50f / 60.0f,
                                                          -(72.0f + 1.75f / 60.0f));
    CHECK_STR_EQ(s, "4903.50N/07201.75W");

    /* Southern / eastern hemisphere and zero. */
    const std::string s2 =
        AprsTxFrame::format_coordinates(-(33.0f + 52.10f / 60.0f), 151.0f + 12.90f / 60.0f);
    CHECK_STR_EQ(s2, "3352.10S/15112.90E");

    const std::string s3 = AprsTxFrame::format_coordinates(0.0f, 0.0f);
    CHECK_STR_EQ(s3, "0000.00N/00000.00E");
}

TEST(aprstx_gps_placeholder_expands_and_decodes) {
    /* A payload built with the formatted position (as the ?GPS? substitution
     * produces) parses back to that position through the decoder. */
    const std::string coord = AprsTxFrame::format_coordinates(49.0f + 3.50f / 60.0f,
                                                             -(72.0f + 1.75f / 60.0f));
    const std::string info = "!" + coord + "-Test";

    AprsTxFrame f;
    f.build("N0CALL", 0, "APRS", 0, info, "", 8, 2);

    Ax25Decoder dec;
    FrameSink sink;
    sink.attach(dec);
    dec.feed_bits(f.bits());

    CHECK_EQ(sink.frames.size(), 1u);
    if (!sink.frames.empty()) {
        AprsPacket p;
        p.set_bytes(sink.frames[0].data(), sink.frames[0].size());
        CHECK(p.has_position());
        CHECK_NEAR(p.get_position().latitude, 49.0 + 3.50 / 60.0, 1e-4);
        CHECK_NEAR(p.get_position().longitude, -(72.0 + 1.75 / 60.0), 1e-4);
    }
}

/* ===========================================================================
 * App registration
 * ===========================================================================*/

TEST(aprstx_app_is_registered) {
    const auto* entry = app::AppRegistry::instance().by_id("aprstx");
    CHECK(entry != nullptr);
    if (entry != nullptr) {
        CHECK_STR_EQ(entry->display_name, "APRS TX");
        CHECK(entry->category == app::Category::Transmit);
        CHECK(entry->icon == &ui::bitmap_icon_aprs);
        CHECK(!entry->hardware_limited);
    }
}
