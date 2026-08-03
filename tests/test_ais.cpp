/*
 * mayhem-b200 — AIS decoder tests.
 *
 * Everything here is checked against something outside the code under test:
 *
 *  - the FCS against the Rocksoft check value for CRC-16/X-25 (0x906E over
 *    "123456789"), against an independent bit-serial implementation written in
 *    the reflected form RFC 1662 Appendix C specifies for the PPP FCS-16, and
 *    against RFC 1662's "good FCS" residue 0xF0B8;
 *  - the message field offsets against ITU-R M.1371 as implemented in
 *    firmware/application/apps/ais_app.cpp (message 1 at bits 38/42/50/61/89/
 *    116/128, message 5's call sign at 70, name at 112, destination at 302);
 *  - the six-bit ASCII map against ITU-R M.1371 Table 47 (0..31 -> '@'..'_',
 *    32..63 -> ' '..'?');
 *  - the framing (NRZI, five-ones bit stuffing, 0x7E flags, LSB-first octets)
 *    against firmware/baseband/proc_ais.hpp's packet builder configuration;
 *  - the whole chain against a GMSK burst synthesised with dsp::FskKeyer at the
 *    AIS parameters (9600 baud, +/-2400 Hz, BT 0.4).
 *
 * NOT covered: reception over the air. No radio is attached, so nothing here
 * proves the front end delivers usable samples.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ais_app.hpp"
#include "app_registry.hpp"
#include "bitmaps.hpp"
#include "modulate.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

/* --- An independent FCS ----------------------------------------------------
 *
 * RFC 1662 Appendix C's FCS-16, written bit-serially so it shares no code with
 * dsp::Crc: the register is shifted right through the reversed polynomial
 * 0x8408, seeded 0xFFFF, complemented at the end. */
uint16_t reference_fcs16(const uint8_t* data, size_t length) {
    uint16_t fcs = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        fcs = static_cast<uint16_t>(fcs ^ data[i]);
        for (int b = 0; b < 8; b++) {
            fcs = static_cast<uint16_t>((fcs & 1) ? ((fcs >> 1) ^ 0x8408) : (fcs >> 1));
        }
    }
    return static_cast<uint16_t>(~fcs);
}

uint16_t reference_fcs16(const std::vector<uint8_t>& data) {
    return reference_fcs16(data.data(), data.size());
}

/* --- AIS payload construction ---------------------------------------------
 *
 * ITU-R M.1371 numbers payload bits MSB-first across the octets, which is what
 * ais::Packet::read() returns (its i^7 remap undoes the LSB-first transmission
 * order). So a payload is built here as an MSB-first bit array and packed
 * MSB-first into octets. */
class PayloadBuilder {
   public:
    void put(uint64_t value, size_t nbits) {
        for (size_t i = nbits; i > 0; --i)
            bits_.push_back(static_cast<uint8_t>((value >> (i - 1)) & 1));
    }

    /* Six-bit ASCII, '@'-padded to `chars` characters, the inverse of
     * ais::six_bit_to_ascii. */
    void put_text(const std::string& s, size_t chars) {
        for (size_t i = 0; i < chars; i++) {
            const uint8_t c = (i < s.size()) ? static_cast<uint8_t>(s[i]) : uint8_t{'@'};
            put(static_cast<uint64_t>(((c - 32) ^ 32) & 0x3F), 6);
        }
    }

    size_t bit_count() const { return bits_.size(); }

    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out((bits_.size() + 7) / 8, 0);
        for (size_t i = 0; i < bits_.size(); i++)
            if (bits_[i]) out[i / 8] = static_cast<uint8_t>(out[i / 8] | (0x80u >> (i % 8)));
        return out;
    }

   private:
    std::vector<uint8_t> bits_{};
};

/* Message 1, position report. Field offsets from upstream's
 * AISRecentEntry::update. `radio_status` defaults to all ones so the wire
 * stream is guaranteed to need bit stuffing. */
std::vector<uint8_t> make_message_1(uint32_t mmsi,
                                    uint32_t nav_status,
                                    int32_t rate_of_turn,
                                    uint32_t sog,
                                    int32_t lon_raw,
                                    int32_t lat_raw,
                                    uint32_t cog,
                                    uint32_t heading,
                                    uint32_t radio_status = 0x7FFFF) {
    PayloadBuilder b;
    b.put(1, 6);                                            /*   0 message id      */
    b.put(0, 2);                                            /*   6 repeat          */
    b.put(mmsi, 30);                                        /*   8 MMSI            */
    b.put(nav_status, 4);                                   /*  38 nav status      */
    b.put(static_cast<uint64_t>(rate_of_turn) & 0xFF, 8);   /*  42 rate of turn    */
    b.put(sog, 10);                                         /*  50 speed over grnd */
    b.put(1, 1);                                            /*  60 position acc.   */
    b.put(static_cast<uint64_t>(lon_raw) & 0xFFFFFFF, 28);  /*  61 longitude       */
    b.put(static_cast<uint64_t>(lat_raw) & 0x7FFFFFF, 27);  /*  89 latitude        */
    b.put(cog, 12);                                         /* 116 course over grnd*/
    b.put(heading, 9);                                      /* 128 true heading    */
    b.put(30, 6);                                           /* 137 UTC second      */
    b.put(0, 2);                                            /* 143 manoeuvre       */
    b.put(0, 3);                                            /* 145 spare           */
    b.put(0, 1);                                            /* 148 RAIM            */
    b.put(radio_status, 19);                                /* 149 radio status    */
    CHECK_EQ(b.bit_count(), size_t{168});
    return b.bytes();
}

/* Message 5, static and voyage related data. 424 bits. */
std::vector<uint8_t> make_message_5(uint32_t mmsi,
                                    const std::string& call_sign,
                                    const std::string& name,
                                    const std::string& destination) {
    PayloadBuilder b;
    b.put(5, 6);                       /*   0 message id     */
    b.put(0, 2);                       /*   6 repeat         */
    b.put(mmsi, 30);                   /*   8 MMSI           */
    b.put(0, 2);                       /*  38 AIS version    */
    b.put(9134567, 30);                /*  40 IMO number     */
    b.put_text(call_sign, 7);          /*  70 call sign      */
    b.put_text(name, 20);              /* 112 vessel name    */
    b.put(70, 8);                      /* 232 ship type      */
    b.put(90, 9);                      /* 240 dim to bow     */
    b.put(30, 9);                      /* 249 dim to stern   */
    b.put(10, 6);                      /* 258 dim to port    */
    b.put(10, 6);                      /* 264 dim to stbd    */
    b.put(1, 4);                       /* 270 EPFD type      */
    b.put(5, 4);                       /* 274 ETA month      */
    b.put(17, 5);                      /* 278 ETA day        */
    b.put(9, 5);                       /* 283 ETA hour       */
    b.put(30, 6);                      /* 288 ETA minute     */
    b.put(64, 8);                      /* 294 draught        */
    b.put_text(destination, 20);       /* 302 destination    */
    b.put(0, 1);                       /* 422 DTE            */
    b.put(0, 1);                       /* 423 spare          */
    CHECK_EQ(b.bit_count(), size_t{424});
    return b.bytes();
}

/* The 1/10000-minute raw value for a decimal degree position. */
int32_t degrees_to_raw(double degrees) {
    return static_cast<int32_t>(degrees * 600000.0);
}

ais::Packet decode_bits(const std::vector<uint8_t>& payload) {
    return ais::Packet::from_bits(ais::build_packet_bits(payload));
}

/* Modulates an NRZI symbol stream as AIS GMSK at 38.4 kHz. */
std::vector<dsp::cfloat> modulate_air_bits(const std::vector<uint8_t>& air_bits) {
    dsp::BitStreamWriter w{dsp::BitOrder::MsbFirst};
    for (const uint8_t bit : air_bits) w.write_bit(bit != 0);
    w.align_to_byte(false);

    dsp::FskKeyer keyer;
    keyer.configure(38400.0f, 9600.0f, 2400.0f);
    keyer.set_gaussian(0.4f, 4); /* AIS GMSK, BT = 0.4 */
    keyer.set_repeat(1, 0);
    keyer.set_data(w.bytes().data(), air_bits.size());

    std::vector<dsp::cfloat> signal(air_bits.size() * 4 + 256);
    const size_t n = keyer.process(signal.data(), signal.size());
    signal.resize(n);
    return signal;
}

}  // namespace

/* =========================================================================
 * Registration
 * =========================================================================*/

TEST(ais_app_is_registered) {
    const auto* entry = app::AppRegistry::instance().by_id("ais");
    CHECK(entry != nullptr);
    if (entry != nullptr) {
        CHECK_STR_EQ(entry->display_name, "AIS Boats");
        CHECK(entry->category == app::Category::Receive);
        CHECK(entry->icon == &ui::bitmap_icon_ais);
        CHECK(!entry->hardware_limited);
        CHECK(static_cast<bool>(entry->factory));
    }
}

/* =========================================================================
 * FCS
 * =========================================================================*/

TEST(ais_fcs_check_value) {
    const std::string check = "123456789";
    const std::vector<uint8_t> bytes(check.begin(), check.end());

    /* Rocksoft catalogue check value for CRC-16/X-25 (CRC-16/IBM-SDLC). */
    CHECK_EQ(ais::fcs(bytes), uint16_t{0x906E});
    CHECK_EQ(reference_fcs16(bytes), uint16_t{0x906E});
}

TEST(ais_fcs_matches_rfc1662_reference) {
    const std::vector<std::vector<uint8_t>> cases{
        {},
        {0x00},
        {0xFF},
        {0x01, 0x02, 0x03},
        {0xDE, 0xAD, 0xBE, 0xEF},
        {0x55, 0xAA, 0x55, 0xAA, 0x00, 0xFF, 0x10, 0x20, 0x30},
    };

    for (const auto& c : cases) CHECK_EQ(ais::fcs(c), reference_fcs16(c));

    /* A longer pseudo-random run, so the comparison is not just short vectors. */
    std::vector<uint8_t> big(257);
    uint32_t lfsr = 0xACE1u;
    for (auto& byte : big) {
        lfsr = (lfsr >> 1) ^ (static_cast<uint32_t>(-static_cast<int32_t>(lfsr & 1u)) & 0xB400u);
        byte = static_cast<uint8_t>(lfsr & 0xFF);
    }
    CHECK_EQ(ais::fcs(big), reference_fcs16(big));
}

TEST(ais_fcs_residue_is_rfc1662_good_fcs) {
    /* RFC 1662: running the FCS over the data *and* its transmitted FCS leaves
     * the register at 0xF0B8 before the final complement. */
    const std::vector<uint8_t> data{0x11, 0x22, 0x33, 0x44, 0x55};
    const uint16_t f = ais::fcs(data);

    std::vector<uint8_t> with_fcs = data;
    with_fcs.push_back(static_cast<uint8_t>(f & 0xFF));
    with_fcs.push_back(static_cast<uint8_t>(f >> 8));

    /* reference_fcs16 complements at the end, so undo that to see the residue. */
    CHECK_EQ(static_cast<uint16_t>(~reference_fcs16(with_fcs)), uint16_t{0xF0B8});
    CHECK_EQ(static_cast<uint16_t>(~ais::fcs(with_fcs)), uint16_t{0xF0B8});
}

TEST(ais_packet_crc_matches_standalone_fcs) {
    const auto payload = make_message_1(244660320u, 5, 0, 74, degrees_to_raw(4.4079),
                                        degrees_to_raw(51.9), 1234, 41);
    const auto packet = decode_bits(payload);

    /* Upstream's in-packet check (stream octets through a non-reflected CRC-16)
     * and the reflected standalone FCS must agree. */
    CHECK(packet.crc_ok());
    CHECK_EQ(ais::fcs(payload), reference_fcs16(payload));
}

/* =========================================================================
 * NRZI
 * =========================================================================*/

TEST(ais_nrzi_decode_known_sequence) {
    /* A data 1 is "no transition", a data 0 is "transition"; the decoder starts
     * from a 0 line level. Symbols 0,0,1,1,0 therefore decode to 1,1,0,1,0. */
    const std::vector<uint8_t> symbols{0, 0, 1, 1, 0};
    const std::vector<uint8_t> expected{1, 1, 0, 1, 0};
    CHECK(ais::nrzi_decode(symbols) == expected);

    /* A constant line level is an unbroken run of 1s. */
    CHECK(ais::nrzi_decode(std::vector<uint8_t>(6, 1)) == std::vector<uint8_t>({0, 1, 1, 1, 1, 1}));
    CHECK(ais::nrzi_decode(std::vector<uint8_t>(6, 0)) == std::vector<uint8_t>(6, 1));
}

TEST(ais_nrzi_round_trip) {
    const std::vector<uint8_t> bits{1, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 0, 1, 0, 0};
    CHECK(ais::nrzi_decode(ais::nrzi_encode(bits)) == bits);
}

TEST(ais_nrzi_decode_is_inversion_immune) {
    /* Which FSK tone the discriminator calls positive does not matter: NRZI
     * decodes a globally inverted symbol stream identically past the first
     * symbol. */
    const std::vector<uint8_t> symbols{1, 1, 0, 1, 0, 0, 1, 0};
    std::vector<uint8_t> inverted;
    for (uint8_t s : symbols) inverted.push_back(static_cast<uint8_t>(s ^ 1));

    const auto a = ais::nrzi_decode(symbols);
    const auto b = ais::nrzi_decode(inverted);
    for (size_t i = 1; i < a.size(); i++) CHECK_EQ(a[i], b[i]);
}

/* =========================================================================
 * Bit stuffing
 * =========================================================================*/

TEST(ais_bit_stuffing_inserts_zero_after_five_ones) {
    const std::vector<uint8_t> bits{1, 1, 1, 1, 1, 1, 0};
    const std::vector<uint8_t> stuffed = ais::hdlc_stuff(bits);

    /* One 0 goes in after the fifth 1; the sixth 1 then starts a new run. */
    const std::vector<uint8_t> expected{1, 1, 1, 1, 1, 0, 1, 0};
    CHECK(stuffed == expected);
    CHECK_EQ(stuffed.size(), bits.size() + 1);
}

TEST(ais_bit_destuffing_removes_the_stuffed_zero) {
    /* The exact inverse of the case above. */
    const std::vector<uint8_t> on_wire{1, 1, 1, 1, 1, 0, 1, 0};
    const std::vector<uint8_t> expected{1, 1, 1, 1, 1, 1, 0};
    CHECK(ais::hdlc_destuff(on_wire) == expected);

    /* A run shorter than five is untouched. */
    const std::vector<uint8_t> short_run{0, 1, 1, 1, 1, 0, 1};
    CHECK(ais::hdlc_destuff(short_run) == short_run);

    /* Two stuffed zeros in a ten-bit run of ones. */
    std::vector<uint8_t> ten_ones(10, 1);
    const auto stuffed = ais::hdlc_stuff(ten_ones);
    CHECK_EQ(stuffed.size(), size_t{12});
    CHECK(ais::hdlc_destuff(stuffed) == ten_ones);
}

TEST(ais_bit_stuffing_round_trip) {
    std::vector<uint8_t> bits;
    uint32_t lfsr = 0x1234u;
    for (size_t i = 0; i < 500; i++) {
        lfsr = (lfsr >> 1) ^ (static_cast<uint32_t>(-static_cast<int32_t>(lfsr & 1u)) & 0xB400u);
        /* Bias towards ones so long runs actually occur. */
        bits.push_back(static_cast<uint8_t>(((lfsr & 3u) != 0) ? 1 : 0));
    }
    const auto stuffed = ais::hdlc_stuff(bits);
    CHECK(stuffed.size() > bits.size());
    CHECK(ais::hdlc_destuff(stuffed) == bits);

    /* A flag can never appear inside stuffed data. */
    size_t run = 0;
    for (const uint8_t b : stuffed) {
        run = b ? (run + 1) : 0;
        CHECK(run <= 5);
    }
}

/* =========================================================================
 * Six-bit ASCII
 * =========================================================================*/

TEST(ais_six_bit_ascii_table) {
    /* ITU-R M.1371 Table 47: 0..31 map to '@'..'_', 32..63 to ' '..'?'. */
    struct Case {
        uint8_t value;
        char expected;
    };
    const std::vector<Case> table{
        {0, '@'}, {1, 'A'},  {26, 'Z'}, {31, '_'},
        {32, ' '}, {48, '0'}, {57, '9'}, {63, '?'},
    };

    for (const auto& c : table) CHECK_EQ(ais::six_bit_to_ascii(c.value), c.expected);

    /* And the two contiguous runs the table is made of. */
    for (uint8_t v = 0; v < 32; v++)
        CHECK_EQ(ais::six_bit_to_ascii(v), static_cast<char>(64 + v));
    for (uint8_t v = 32; v < 64; v++)
        CHECK_EQ(ais::six_bit_to_ascii(v), static_cast<char>(v));
}

TEST(ais_text_padding_is_trimmed) {
    CHECK_STR_EQ(ais::format::text("EVER GIVEN@@@@@@@@@@"), "EVER GIVEN");
    CHECK_STR_EQ(ais::format::text("@@@@@@@@@@"), "");
    CHECK_STR_EQ(ais::format::text("ROTTERDAM"), "ROTTERDAM");
}

/* =========================================================================
 * Packet parsing
 * =========================================================================*/

TEST(ais_message_1_fields) {
    const int32_t lon = degrees_to_raw(4.4079);   /*   4.407900 E */
    const int32_t lat = degrees_to_raw(51.9);     /*  51.900000 N */

    const auto payload = make_message_1(244660320u, 5, -14, 74, lon, lat, 1234, 41);
    const auto packet = decode_bits(payload);

    CHECK(packet.length_valid());
    CHECK(packet.crc_ok());
    CHECK(packet.is_valid());

    CHECK_EQ(packet.data_length(), size_t{168});
    CHECK_EQ(packet.message_id(), uint32_t{1});
    CHECK_EQ(packet.source_id(), uint32_t{244660320});
    CHECK_EQ(packet.user_id(), uint32_t{244660320});

    CHECK_EQ(packet.read(38, 4), uint32_t{5});       /* nav status */
    CHECK_EQ(static_cast<int8_t>(packet.read(42, 8)), int8_t{-14});
    CHECK_EQ(packet.read(50, 10), uint32_t{74});     /* SOG */
    CHECK_EQ(packet.read(116, 12), uint32_t{1234});  /* COG */
    CHECK_EQ(packet.read(128, 9), uint32_t{41});     /* heading */

    CHECK_EQ(packet.latitude(89).normalized(), lat);
    CHECK_EQ(packet.longitude(61).normalized(), lon);
    CHECK(packet.latitude(89).is_valid());
    CHECK(packet.longitude(61).is_valid());
}

TEST(ais_message_1_negative_position) {
    /* Southern and western positions exercise the sign extension of the 27- and
     * 28-bit fields. */
    const int32_t lon = degrees_to_raw(-58.3816);
    const int32_t lat = degrees_to_raw(-34.6037);

    const auto payload = make_message_1(701000001u, 0, 0, 0, lon, lat, 0, 511);
    const auto packet = decode_bits(payload);

    CHECK(packet.is_valid());
    CHECK_EQ(packet.longitude(61).normalized(), lon);
    CHECK_EQ(packet.latitude(89).normalized(), lat);
    CHECK(packet.longitude(61).is_valid());
    CHECK(packet.latitude(89).is_valid());
}

TEST(ais_message_5_text_fields) {
    const auto payload = make_message_5(244660320u, "PBRV", "EVER GIVEN", "ROTTERDAM");
    const auto packet = decode_bits(payload);

    CHECK(packet.length_valid());
    CHECK(packet.crc_ok());
    CHECK_EQ(packet.data_length(), size_t{424});
    CHECK_EQ(packet.message_id(), uint32_t{5});
    CHECK_EQ(packet.source_id(), uint32_t{244660320});

    CHECK_STR_EQ(packet.text(70, 7), "PBRV@@@");
    CHECK_STR_EQ(packet.text(112, 20), "EVER GIVEN@@@@@@@@@@");
    CHECK_STR_EQ(packet.text(302, 20), "ROTTERDAM@@@@@@@@@@@");

    CHECK_STR_EQ(ais::format::text(packet.text(112, 20)), "EVER GIVEN");
    CHECK_STR_EQ(ais::format::text(packet.text(70, 7)), "PBRV");
    CHECK_STR_EQ(ais::format::text(packet.text(302, 20)), "ROTTERDAM");
}

TEST(ais_message_5_all_six_bit_values) {
    /* Every printable value the encoding can carry, through the name field. */
    const std::string name = "0123456789 ABCDEFGH";
    const auto payload = make_message_5(1u, "ABC1234", name, "");
    const auto packet = decode_bits(payload);

    CHECK(packet.is_valid());
    CHECK_STR_EQ(ais::format::text(packet.text(112, 20)), name);
    CHECK_STR_EQ(ais::format::text(packet.text(302, 20)), "");
}

TEST(ais_packet_rejects_corrupted_payload) {
    const auto payload = make_message_1(244660320u, 5, 0, 74, degrees_to_raw(4.4),
                                        degrees_to_raw(51.9), 1234, 41);
    auto bits = ais::build_packet_bits(payload);

    CHECK(ais::Packet::from_bits(bits).is_valid());

    /* Flip one payload bit: the length is still right, the FCS is not. */
    bits[42] = static_cast<uint8_t>(bits[42] ^ 1);
    const auto corrupted = ais::Packet::from_bits(bits);
    CHECK(corrupted.length_valid());
    CHECK(!corrupted.crc_ok());
    CHECK(!corrupted.is_valid());
}

TEST(ais_packet_rejects_corrupted_fcs) {
    const auto payload = make_message_1(1u, 0, 0, 0, 0, 0, 0, 0);
    auto bits = ais::build_packet_bits(payload);

    /* Flip a bit inside the FCS itself. */
    const size_t fcs_start = payload.size() * 8;
    bits[fcs_start + 3] = static_cast<uint8_t>(bits[fcs_start + 3] ^ 1);
    CHECK(!ais::Packet::from_bits(bits).crc_ok());
}

TEST(ais_packet_rejects_wrong_length_for_message_id) {
    /* Message 1 must be exactly 168 bits. Twenty-four short is a legal frame at
     * the HDLC level and must still be thrown out. */
    auto payload = make_message_1(1u, 0, 0, 0, 0, 0, 0, 0);
    payload.resize(18);  /* 144 bits */

    const auto packet = decode_bits(payload);
    CHECK(packet.crc_ok());          /* the FCS was recomputed over the short payload */
    CHECK(!packet.length_valid());
    CHECK(!packet.is_valid());
}

TEST(ais_packet_rejects_runt_frames) {
    /* Anything too short to hold an FCS plus one payload octet must be
     * rejected rather than underflowing the length arithmetic (upstream
     * computes length() - 7 - 16 on a size_t and wraps). */
    for (size_t n = 0; n < 31; n++) {
        const auto packet = ais::Packet::from_bits(std::vector<uint8_t>(n, 1));
        CHECK(!packet.length_valid());
        CHECK(!packet.is_valid());
        /* Below seven flag bits plus a sixteen-bit FCS there is no payload at
         * all, and the subtraction must clamp rather than wrap. */
        if (n < 23) CHECK_EQ(packet.data_length(), size_t{0});
        CHECK(packet.data_length() < 16);
    }
}

TEST(ais_packet_length_table) {
    CHECK(ais::length_valid_for(1, 168));
    CHECK(ais::length_valid_for(2, 168));
    CHECK(ais::length_valid_for(3, 168));
    CHECK(!ais::length_valid_for(1, 160));
    CHECK(ais::length_valid_for(5, 424));
    CHECK(!ais::length_valid_for(5, 416));
    CHECK(ais::length_valid_for(18, 168));
    CHECK(ais::length_valid_for(21, 272));
    CHECK(ais::length_valid_for(21, 360));
    CHECK(!ais::length_valid_for(21, 368));
    CHECK(ais::length_valid_for(24, 160));
    CHECK(ais::length_valid_for(24, 168));
    /* Ids upstream leaves at {0, 0}, and ids past the table. */
    CHECK(!ais::length_valid_for(7, 168));
    CHECK(!ais::length_valid_for(40, 168));
    CHECK(!ais::length_valid_for(64, 168));
}

TEST(ais_packet_hex_nibble_log_format) {
    /* Upstream's AISLogger writes the frame four bits at a time, using
     * 'W' + nibble for 10..15 — i.e. lowercase hex. The nibbles come from the
     * byte-reversed reader, so they are the payload octets' nibbles in order. */
    const std::vector<uint8_t> payload{0x01, 0x23, 0xAB, 0xCD};
    const auto packet = ais::Packet::from_bits(ais::build_packet_bits(payload));

    const std::string hex = packet.to_hex_nibbles();
    CHECK(hex.size() >= 8);
    CHECK_STR_EQ(hex.substr(0, 8), "0123abcd");
}

/* =========================================================================
 * Position field types and formatting
 * =========================================================================*/

TEST(ais_latlon_not_available_encodings) {
    const ais::Latitude lat_na{};
    const ais::Longitude lon_na{};

    CHECK(lat_na.is_not_available());
    CHECK(lon_na.is_not_available());
    CHECK(!lat_na.is_valid());
    CHECK(!lon_na.is_valid());

    /* 91 degrees and 181 degrees, in 1/10000 minute. */
    CHECK_EQ(lat_na.raw(), int32_t{0x3412140});
    CHECK_EQ(lon_na.raw(), int32_t{0x6791AC0});
    CHECK_EQ(lat_na.raw(), int32_t{91 * 600000});
    CHECK_EQ(lon_na.raw(), int32_t{181 * 600000});

    CHECK_STR_EQ(ais::format::latlon(lat_na, lon_na), "not available");
}

TEST(ais_latlon_sign_extension_and_range) {
    /* The 27-bit latitude field holds -34.6037 degrees as a two's complement
     * value; normalized() must sign-extend it. */
    const int32_t lat_raw = degrees_to_raw(-34.6037);
    const ais::Latitude lat{static_cast<int32_t>(lat_raw & 0x7FFFFFF)};
    CHECK_EQ(lat.normalized(), lat_raw);
    CHECK(lat.is_valid());

    const int32_t lon_raw = degrees_to_raw(-58.3816);
    const ais::Longitude lon{static_cast<int32_t>(lon_raw & 0xFFFFFFF)};
    CHECK_EQ(lon.normalized(), lon_raw);
    CHECK(lon.is_valid());

    /* Just outside the legal range. */
    const ais::Latitude too_far{static_cast<int32_t>((90 * 600000 + 1) & 0x7FFFFFF)};
    CHECK(!too_far.is_valid());
}

TEST(ais_latlon_formatting) {
    CHECK_STR_EQ(ais::format::latlon_abs_normalized(degrees_to_raw(51.9), "SN"), "51.900000N");
    CHECK_STR_EQ(ais::format::latlon_abs_normalized(degrees_to_raw(-51.9), "SN"), "51.900000S");
    CHECK_STR_EQ(ais::format::latlon_abs_normalized(degrees_to_raw(4.4079), "WE"), "4.407900E");
    CHECK_STR_EQ(ais::format::latlon_abs_normalized(degrees_to_raw(-4.4079), "WE"), "4.407900W");

    CHECK_NEAR(ais::format::latlon_float(degrees_to_raw(51.9)), 51.9, 1e-4);
    CHECK_NEAR(ais::format::latlon_float(degrees_to_raw(-4.4079)), -4.4079, 1e-4);
}

TEST(ais_scalar_formatting) {
    CHECK_STR_EQ(ais::format::mmsi(244660320u), "244660320");
    CHECK_STR_EQ(ais::format::mmsi(1234u), "000001234");

    CHECK_STR_EQ(ais::format::navigational_status(0), "under way w/engine");
    CHECK_STR_EQ(ais::format::navigational_status(5), "moored");
    CHECK_STR_EQ(ais::format::navigational_status(14), "SART/MOB/EPIRB");
    CHECK_STR_EQ(ais::format::navigational_status(16), "unknown");

    CHECK_STR_EQ(ais::format::rate_of_turn(-128), "not available");
    CHECK_STR_EQ(ais::format::rate_of_turn(0), "0 deg/min");
    CHECK_STR_EQ(ais::format::rate_of_turn(127), "right >5 deg/30sec");
    CHECK_STR_EQ(ais::format::rate_of_turn(-127), "left >5 deg/30sec");

    CHECK_STR_EQ(ais::format::speed_over_ground(1023), "not available");
    CHECK_STR_EQ(ais::format::speed_over_ground(1022), ">= 102.2 knots");
    CHECK_STR_EQ(ais::format::speed_over_ground(74), "7.4 knots");

    CHECK_STR_EQ(ais::format::course_over_ground(3600), "not available");
    CHECK_STR_EQ(ais::format::course_over_ground(3601), "invalid");
    CHECK_STR_EQ(ais::format::course_over_ground(1234), "123.4 deg");

    CHECK_STR_EQ(ais::format::true_heading(511), "not available");
    CHECK_STR_EQ(ais::format::true_heading(360), "invalid");
    CHECK_STR_EQ(ais::format::true_heading(41), "41 deg");
}

/* =========================================================================
 * Recent-entry accumulation
 * =========================================================================*/

TEST(ais_recent_entry_update_from_position_and_static) {
    const int32_t lon = degrees_to_raw(4.4079);
    const int32_t lat = degrees_to_raw(51.9);

    app::AISRecentEntry entry{244660320u};

    entry.update(decode_bits(make_message_1(244660320u, 5, 0, 74, lon, lat, 1234, 41)));
    CHECK_EQ(entry.received_count, size_t{1});
    CHECK_EQ(entry.navigational_status, int8_t{5});
    CHECK_EQ(entry.last_position.speed_over_ground, uint16_t{74});
    CHECK_EQ(entry.last_position.course_over_ground, uint16_t{1234});
    CHECK_EQ(entry.last_position.true_heading, uint16_t{41});
    CHECK_EQ(entry.last_position.latitude.normalized(), lat);
    CHECK_EQ(entry.last_position.longitude.normalized(), lon);
    CHECK(!entry.last_position.timestamp.empty());

    entry.update(decode_bits(make_message_5(244660320u, "PBRV", "EVER GIVEN", "ROTTERDAM")));
    CHECK_EQ(entry.received_count, size_t{2});
    /* Upstream stores the field as received — its trim() strips whitespace, not
     * the '@' padding — and strips the padding only when drawing, through
     * ais::format::text. Both steps are checked here. */
    CHECK_STR_EQ(entry.name, "EVER GIVEN@@@@@@@@@@");
    CHECK_STR_EQ(entry.call_sign, "PBRV@@@");
    CHECK_STR_EQ(entry.destination, "ROTTERDAM@@@@@@@@@@@");
    CHECK_STR_EQ(ais::format::text(entry.name), "EVER GIVEN");
    CHECK_STR_EQ(ais::format::text(entry.call_sign), "PBRV");
    CHECK_STR_EQ(ais::format::text(entry.destination), "ROTTERDAM");
    /* The static message must not disturb the position. */
    CHECK_EQ(entry.last_position.latitude.normalized(), lat);
}

TEST(ais_recent_entries_keyed_by_mmsi) {
    app::AISRecentEntries recent;

    auto& a = ui::on_packet(recent, ais::MMSI{111111111});
    a.update(decode_bits(make_message_1(111111111u, 0, 0, 10, 0, 0, 0, 0)));

    auto& b = ui::on_packet(recent, ais::MMSI{222222222});
    b.update(decode_bits(make_message_1(222222222u, 0, 0, 20, 0, 0, 0, 0)));

    CHECK_EQ(recent.size(), size_t{2});
    CHECK_EQ(recent.front().key(), ais::MMSI{222222222});

    auto& again = ui::on_packet(recent, ais::MMSI{111111111});
    again.update(decode_bits(make_message_1(111111111u, 0, 0, 30, 0, 0, 0, 0)));
    CHECK_EQ(recent.size(), size_t{2});
    CHECK_EQ(recent.front().key(), ais::MMSI{111111111});
    CHECK_EQ(recent.front().received_count, size_t{2});
    CHECK_EQ(recent.front().last_position.speed_over_ground, uint16_t{30});
}

/* =========================================================================
 * Framing: the bit stream the packet builder must see
 * =========================================================================*/

TEST(ais_air_frame_structure) {
    const auto payload = make_message_1(244660320u, 5, 0, 74, degrees_to_raw(4.4),
                                        degrees_to_raw(51.9), 1234, 41);
    const auto air = ais::build_air_bits(payload, 24);

    /* NRZI decoding the burst must show the training sequence followed by the
     * start flag: exactly upstream's sixteen-bit preamble pattern. */
    const auto decoded = ais::nrzi_decode(air);
    const std::vector<uint8_t> preamble{0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0};
    for (size_t i = 0; i < preamble.size(); i++) CHECK_EQ(decoded[16 + i], preamble[i]);

    /* Bit stuffing really is exercised by this payload. */
    const auto unstuffed_len = payload.size() * 8 + 16;
    CHECK(air.size() > 24 + 8 + unstuffed_len + 8);

    /* Destuffing the region between the flags recovers the packet bits. */
    const std::vector<uint8_t> between(decoded.begin() + 32,
                                       decoded.end() - 8);  /* drop training+flag and end flag */
    const auto destuffed = ais::hdlc_destuff(between);
    CHECK_EQ(destuffed.size(), unstuffed_len);

    std::vector<uint8_t> as_packet = destuffed;
    const std::vector<uint8_t> partial_flag{0, 1, 1, 1, 1, 1, 1};
    as_packet.insert(as_packet.end(), partial_flag.begin(), partial_flag.end());
    CHECK(ais::Packet::from_bits(as_packet).is_valid());
}

/* =========================================================================
 * End to end: GMSK burst in, parsed packet out
 * =========================================================================*/

TEST(ais_decoder_recovers_position_report_from_gmsk) {
    const int32_t lon = degrees_to_raw(4.4079);
    const int32_t lat = degrees_to_raw(51.9);
    const auto payload = make_message_1(244660320u, 5, -14, 74, lon, lat, 1234, 41);

    auto air = ais::build_air_bits(payload, 64);
    air.insert(air.end(), 24, air.back()); /* let the last flag clock out */

    const auto signal = modulate_air_bits(air);
    CHECK(signal.size() > 1000);

    std::vector<ais::Packet> received;
    ais::Decoder decoder;
    decoder.configure(38400.0f);
    decoder.set_packet_handler([&received](const ais::Packet& p) { received.push_back(p); });
    decoder.feed(signal.data(), signal.size());

    CHECK_EQ(received.size(), size_t{1});
    CHECK_EQ(decoder.crc_failures(), size_t{0});
    if (received.empty()) return;

    const auto& packet = received.front();
    CHECK_EQ(packet.message_id(), uint32_t{1});
    CHECK_EQ(packet.source_id(), uint32_t{244660320});
    CHECK_EQ(packet.read(38, 4), uint32_t{5});
    CHECK_EQ(static_cast<int8_t>(packet.read(42, 8)), int8_t{-14});
    CHECK_EQ(packet.read(50, 10), uint32_t{74});
    CHECK_EQ(packet.read(116, 12), uint32_t{1234});
    CHECK_EQ(packet.read(128, 9), uint32_t{41});
    CHECK_EQ(packet.latitude(89).normalized(), lat);
    CHECK_EQ(packet.longitude(61).normalized(), lon);
}

TEST(ais_decoder_recovers_ship_name_from_gmsk) {
    const auto payload = make_message_5(244660320u, "PBRV", "EVER GIVEN", "ROTTERDAM");

    auto air = ais::build_air_bits(payload, 64);
    air.insert(air.end(), 24, air.back());

    const auto signal = modulate_air_bits(air);

    std::vector<ais::Packet> received;
    ais::Decoder decoder;
    decoder.configure(38400.0f);
    decoder.set_packet_handler([&received](const ais::Packet& p) { received.push_back(p); });
    decoder.feed(signal.data(), signal.size());

    CHECK_EQ(received.size(), size_t{1});
    if (received.empty()) return;

    app::AISRecentEntry entry{received.front().source_id()};
    entry.update(received.front());
    CHECK_STR_EQ(ais::format::text(entry.name), "EVER GIVEN");
    CHECK_STR_EQ(ais::format::text(entry.call_sign), "PBRV");
    CHECK_STR_EQ(ais::format::text(entry.destination), "ROTTERDAM");
}

TEST(ais_decoder_feeds_in_arbitrary_chunks) {
    /* The host tap hands over blocks of whatever size it has; a burst split
     * across calls must still decode. */
    const auto payload = make_message_1(316001234u, 0, 0, 123, degrees_to_raw(-123.1),
                                        degrees_to_raw(49.3), 900, 90);
    auto air = ais::build_air_bits(payload, 64);
    air.insert(air.end(), 24, air.back());
    const auto signal = modulate_air_bits(air);

    std::vector<ais::Packet> received;
    ais::Decoder decoder;
    decoder.configure(38400.0f);
    decoder.set_packet_handler([&received](const ais::Packet& p) { received.push_back(p); });

    size_t offset = 0;
    size_t chunk = 7;
    while (offset < signal.size()) {
        const size_t n = std::min(chunk, signal.size() - offset);
        decoder.feed(signal.data() + offset, n);
        offset += n;
        chunk = (chunk * 3 + 1) % 500 + 1;
    }

    CHECK_EQ(received.size(), size_t{1});
    if (!received.empty()) {
        CHECK_EQ(received.front().source_id(), uint32_t{316001234});
        CHECK_EQ(received.front().latitude(89).normalized(), degrees_to_raw(49.3));
    }
}

TEST(ais_decoder_rejects_a_corrupted_burst) {
    const auto payload = make_message_1(244660320u, 5, 0, 74, degrees_to_raw(4.4),
                                        degrees_to_raw(51.9), 1234, 41);
    auto air = ais::build_air_bits(payload, 64);
    air.insert(air.end(), 24, air.back());

    /* Flip an NRZI symbol in the middle of the payload. NRZI turns one symbol
     * error into two bit errors, so the FCS must reject the frame. */
    air[200] = static_cast<uint8_t>(air[200] ^ 1);

    const auto signal = modulate_air_bits(air);

    size_t delivered = 0;
    ais::Decoder decoder;
    decoder.configure(38400.0f);
    decoder.set_packet_handler([&delivered](const ais::Packet&) { delivered++; });
    decoder.feed(signal.data(), signal.size());

    CHECK_EQ(delivered, size_t{0});
    CHECK_EQ(decoder.packets_valid(), size_t{0});
    /* It must have been caught, not simply missed. */
    CHECK(decoder.crc_failures() + decoder.length_failures() > 0);
}

TEST(ais_decoder_emits_nothing_for_noise) {
    ais::Decoder decoder;
    decoder.configure(38400.0f);

    size_t delivered = 0;
    decoder.set_packet_handler([&delivered](const ais::Packet&) { delivered++; });

    std::vector<dsp::cfloat> noise(40000);
    uint32_t lfsr = 0xBEEFu;
    for (auto& s : noise) {
        lfsr = (lfsr >> 1) ^ (static_cast<uint32_t>(-static_cast<int32_t>(lfsr & 1u)) & 0xB400u);
        const float i = (static_cast<float>(lfsr & 0xFF) - 128.0f) / 128.0f;
        lfsr = (lfsr >> 1) ^ (static_cast<uint32_t>(-static_cast<int32_t>(lfsr & 1u)) & 0xB400u);
        const float q = (static_cast<float>(lfsr & 0xFF) - 128.0f) / 128.0f;
        s = dsp::cfloat{i, q};
    }

    decoder.feed(noise.data(), noise.size());
    CHECK_EQ(delivered, size_t{0});
    CHECK_EQ(decoder.packets_valid(), size_t{0});
}

TEST(ais_decoder_handles_back_to_back_bursts) {
    const auto payload_a = make_message_1(111111111u, 0, 0, 10, degrees_to_raw(1.0),
                                          degrees_to_raw(2.0), 100, 10);
    const auto payload_b = make_message_1(222222222u, 1, 0, 20, degrees_to_raw(3.0),
                                          degrees_to_raw(4.0), 200, 20);

    std::vector<uint8_t> air;
    for (const auto* p : {&payload_a, &payload_b}) {
        auto burst = ais::build_air_bits(*p, 64);
        burst.insert(burst.end(), 24, burst.back());
        air.insert(air.end(), burst.begin(), burst.end());
    }

    const auto signal = modulate_air_bits(air);

    std::vector<uint32_t> mmsis;
    ais::Decoder decoder;
    decoder.configure(38400.0f);
    decoder.set_packet_handler(
        [&mmsis](const ais::Packet& p) { mmsis.push_back(p.source_id()); });
    decoder.feed(signal.data(), signal.size());

    CHECK_EQ(mmsis.size(), size_t{2});
    if (mmsis.size() == 2) {
        CHECK_EQ(mmsis[0], uint32_t{111111111});
        CHECK_EQ(mmsis[1], uint32_t{222222222});
    }
}

TEST(ais_decoder_configures_for_other_channel_rates) {
    /* The host capture rate is not fixed at upstream's 2.4576 Msps, so the
     * chain has to configure itself for whatever multiple of 19200 the channel
     * decimation lands on. */
    const auto payload = make_message_1(244660320u, 5, 0, 74, degrees_to_raw(4.4),
                                        degrees_to_raw(51.9), 1234, 41);
    auto air = ais::build_air_bits(payload, 64);
    air.insert(air.end(), 24, air.back());

    /* Modulate at 38.4 kHz, then upsample by 2 with linear interpolation to
     * get a 76.8 kHz version of the same burst. */
    const auto base = modulate_air_bits(air);
    std::vector<dsp::cfloat> fast(base.size() * 2);
    for (size_t i = 0; i < base.size(); i++) {
        fast[2 * i] = base[i];
        fast[2 * i + 1] = (i + 1 < base.size()) ? (base[i] + base[i + 1]) * 0.5f : base[i];
    }

    size_t delivered = 0;
    ais::Decoder decoder;
    decoder.configure(76800.0f);
    CHECK_NEAR(decoder.channel_rate(), 76800.0, 0.1);
    decoder.set_packet_handler([&delivered](const ais::Packet& p) {
        CHECK_EQ(p.source_id(), uint32_t{244660320});
        delivered++;
    });
    decoder.feed(fast.data(), fast.size());

    CHECK_EQ(delivered, size_t{1});
}
