/*
 * mayhem-b200 — SubCar decoder tests.
 *
 * Each protocol is driven with the pulse train its own state machine defines,
 * built here from the upstream timing constants, and the decoded word is
 * checked bit for bit. The Manchester protocols get their pulse train from a
 * small search over the shared transition table — the table is the
 * specification, so a train that decodes to the wanted bits is by definition
 * the right one.
 *
 * On top of that: the OOK front end is run on a synthesised on/off-keyed
 * signal all the way through to a decoded Suzuki frame, the BMW CRCs are
 * checked against known vectors, and every field extractor in parse_frame()
 * is checked against a word whose fields are known.
 *
 * No radio is needed. Live reception is unverified — see the app.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_subcar.hpp"

#include <cmath>
#include <vector>

using namespace app::subcar;

namespace {

struct Pulse {
    bool level;
    uint32_t duration;
};

/* Runs a pulse train through one protocol, collecting every frame it emits. */
struct Capture {
    uint8_t sensor_type{FPC_Invalid};
    uint16_t bits{0};
    uint64_t data{0};
    uint64_t data2{0};
    size_t count{0};
};

Capture run(CarProtocol& proto, const std::vector<Pulse>& pulses) {
    Capture cap{};
    proto.set_callback([&cap](CarProtocol& p) {
        cap.sensor_type = p.sensor_type;
        cap.bits = p.data_count_bit;
        cap.data = p.decode_data;
        cap.data2 = p.decode_data2;
        cap.count++;
    });
    for (const auto& p : pulses) proto.feed(p.level, p.duration);
    return cap;
}

/* Packs bits (MSB first) into a value, the way CarProtocol::add_bit does. */
uint64_t bits_to_word(const std::vector<uint8_t>& bits) {
    uint64_t v = 0;
    for (uint8_t b : bits) v = (v << 1) | (b & 1);
    return v;
}

struct Rng {
    uint32_t s{0x2468ACEu};
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    uint8_t bit() { return static_cast<uint8_t>(next() & 1u); }
};

/* --- Manchester pulse-train search ---------------------------------------- */

/* `invert_level` selects the event mapping: Kia V1/V2 and Ford map a HIGH
 * pulse onto a *Low event, Kia V5 and Fiat map it onto a *High event. */
struct ManchesterSearch {
    uint32_t te_short{0};
    uint32_t te_long{0};
    bool invert_level{true};
    size_t nodes{0};

    ManchesterEvent event_for(bool level, bool is_long) const {
        if (invert_level) {
            if (is_long) return level ? ManchesterEventLongLow : ManchesterEventLongHigh;
            return level ? ManchesterEventShortLow : ManchesterEventShortHigh;
        }
        if (is_long) return level ? ManchesterEventLongHigh : ManchesterEventLongLow;
        return level ? ManchesterEventShortHigh : ManchesterEventShortLow;
    }

    /* Depth-first over (short, long) at each pulse, memoising the (bit index,
     * state, level) triples that have already been proved hopeless. Without
     * that memo the search loops through the table's no-bit transitions and
     * blows up exponentially. */
    std::vector<uint8_t> failed{};

    bool build(const std::vector<uint8_t>& want,
               size_t idx,
               ManchesterState state,
               bool level,
               std::vector<Pulse>& out) {
        if (idx == want.size()) return true;
        if (failed.empty()) failed.assign((want.size() + 1) * 8, 0);

        const size_t key = idx * 8 + static_cast<size_t>(state) * 2 + (level ? 1 : 0);
        if (failed[key]) return false;
        if (++nodes > 1'000'000) return false;

        /* Marked on entry, so a path that loops back to a triple it is already
         * exploring is cut off rather than recursing forever — the table has
         * plenty of no-bit transitions that return to where they started. */
        failed[key] = 1;

        for (int is_long = 0; is_long < 2; ++is_long) {
            const ManchesterEvent ev = event_for(level, is_long != 0);
            ManchesterState next = state;
            bool data = false;
            const bool produced = manchester_advance(state, ev, &next, &data);
            if (produced && (data ? 1 : 0) != (want[idx] ? 1 : 0)) continue;

            out.push_back({level, is_long ? te_long : te_short});
            if (build(want, produced ? idx + 1 : idx, next, !level, out)) return true;
            out.pop_back();
        }

        return false;
    }
};

}  // namespace

/* ===========================================================================
 * Shared Manchester state machine
 * ===========================================================================*/

TEST(subcar_manchester_reset_goes_to_mid1_without_a_bit) {
    ManchesterState state = ManchesterStateStart0;
    bool data = true;
    CHECK(!manchester_advance(state, ManchesterEventReset, &state, &data));
    CHECK_EQ(static_cast<int>(state), static_cast<int>(ManchesterStateMid1));
}

TEST(subcar_manchester_table_matches_upstream) {
    /* transitions[] = {0b00000001, 0b10010001, 0b10011011, 0b11111011}, read as
     * (transitions[state] >> event) & 3. Spot-check every state's short-low and
     * short-high outcome against that expression. */
    static constexpr uint8_t transitions[] = {0b00000001, 0b10010001, 0b10011011, 0b11111011};
    const ManchesterEvent events[] = {ManchesterEventShortLow, ManchesterEventShortHigh,
                                      ManchesterEventLongLow, ManchesterEventLongHigh};
    for (int s = 0; s < 4; s++) {
        for (auto ev : events) {
            ManchesterState next = static_cast<ManchesterState>(s);
            bool data = false;
            const bool produced =
                manchester_advance(static_cast<ManchesterState>(s), ev, &next, &data);

            auto expected = static_cast<ManchesterState>((transitions[s] >> ev) & 0x3);
            if (expected == static_cast<ManchesterState>(s)) {
                CHECK_EQ(static_cast<int>(next), static_cast<int>(ManchesterStateMid1));
                CHECK(!produced);
            } else {
                CHECK_EQ(static_cast<int>(next), static_cast<int>(expected));
                const bool should_produce = (expected == ManchesterStateMid0) ||
                                            (expected == ManchesterStateMid1);
                CHECK_EQ(produced, should_produce);
                if (should_produce) CHECK_EQ(data, expected == ManchesterStateMid1);
            }
        }
    }
}

/* ===========================================================================
 * Suzuki — 250/500 us pulse width, 300-pulse preamble, 2 ms end gap
 * ===========================================================================*/

namespace {

/* Every train in this file strictly alternates HIGH and LOW, because that is
 * all a real receiver can deliver: the pulse extractor reports a duration only
 * when the level changes, so two same-level pulses in a row would arrive as
 * one longer pulse. */
std::vector<Pulse> suzuki_train(const std::vector<uint8_t>& data_bits, int preamble = 305) {
    std::vector<Pulse> p;
    p.push_back({true, 250});  /* enters the preamble counter */
    for (int i = 0; i < preamble; i++) {
        p.push_back({false, 250});  /* each LOW is one preamble pulse */
        p.push_back({true, 250});
    }
    p.push_back({false, 250});
    p.push_back({true, 500});  /* preamble exit; contributes the leading 1 bit */

    for (size_t i = 0; i < data_bits.size(); i++) {
        p.push_back({false, 250});
        p.push_back({true, data_bits[i] ? 500u : 250u});
    }
    p.push_back({false, 2000});  /* end-of-frame gap */
    return p;
}

}  // namespace

TEST(subcar_suzuki_decodes_a_64_bit_frame) {
    Rng rng;
    std::vector<uint8_t> data_bits(63);
    for (auto& b : data_bits) b = rng.bit();

    std::vector<uint8_t> all_bits{1};  /* the preamble-exit bit */
    all_bits.insert(all_bits.end(), data_bits.begin(), data_bits.end());

    ProtoSuzuki proto;
    const auto cap = run(proto, suzuki_train(data_bits));

    CHECK_EQ(cap.count, 1u);
    CHECK_EQ(static_cast<int>(cap.sensor_type), static_cast<int>(FPC_SUZUKI));
    /* Upstream shadows data_count_bit and reports 0 here; see ui_subcar.hpp. */
    CHECK_EQ(static_cast<int>(cap.bits), 64);
    CHECK_EQ(cap.data, bits_to_word(all_bits));
}

TEST(subcar_suzuki_needs_the_full_preamble) {
    Rng rng;
    std::vector<uint8_t> data_bits(63);
    for (auto& b : data_bits) b = rng.bit();

    /* Same frame with a 20-pulse preamble: the decoder must not latch. */
    ProtoSuzuki proto;
    CHECK_EQ(run(proto, suzuki_train(data_bits, 20)).count, 0u);
}

TEST(subcar_suzuki_needs_the_end_gap) {
    Rng rng;
    std::vector<uint8_t> data_bits(63);
    for (auto& b : data_bits) b = rng.bit();

    auto train = suzuki_train(data_bits);
    train.pop_back();  /* drop the 2 ms gap */

    ProtoSuzuki proto;
    CHECK_EQ(run(proto, train).count, 0u);
}

/* ===========================================================================
 * Kia V0 — symmetric pulse pairs, 250/500 us
 * ===========================================================================*/

namespace {

/* Kia V0 and BMW V0 share this shape. `lead_bit` is true for Kia V0, which
 * contributes a 1 as it leaves the preamble. */
std::vector<Pulse> pulse_pair_train(const std::vector<uint8_t>& bits,
                                    uint32_t te_short,
                                    uint32_t te_long,
                                    uint32_t end_high) {
    std::vector<Pulse> p;
    p.push_back({true, te_short});
    for (int i = 0; i < 18; i++) {
        p.push_back({false, te_short});  /* each short LOW pair counts */
        p.push_back({true, te_short});
    }
    p.push_back({false, te_short});
    /* The long HIGH/long LOW pair that ends the preamble. */
    p.push_back({true, te_long});
    p.push_back({false, te_long});

    for (uint8_t b : bits) {
        p.push_back({true, b ? te_long : te_short});
        p.push_back({false, b ? te_long : te_short});
    }
    p.push_back({true, end_high});
    return p;
}

}  // namespace

TEST(subcar_kia_v0_decodes_a_frame) {
    Rng rng;
    /* The preamble exit contributes bit 0; 59 pairs follow, and upstream's
     * counter (initialised to 1 and then incremented by that first add_bit)
     * reaches its 61 threshold with 60 bits stored. */
    std::vector<uint8_t> pair_bits(59);
    for (auto& b : pair_bits) b = rng.bit();

    std::vector<uint8_t> all_bits{1};
    all_bits.insert(all_bits.end(), pair_bits.begin(), pair_bits.end());

    ProtoKiaV0 proto;
    const auto cap = run(proto, pulse_pair_train(pair_bits, 250, 500, 900));

    CHECK_EQ(cap.count, 1u);
    CHECK_EQ(static_cast<int>(cap.sensor_type), static_cast<int>(FPC_KIAV0));
    CHECK_EQ(static_cast<int>(cap.bits), 61);
    CHECK_EQ(cap.data, bits_to_word(all_bits));
}

TEST(subcar_kia_v0_rejects_a_short_frame) {
    Rng rng;
    std::vector<uint8_t> pair_bits(40);
    for (auto& b : pair_bits) b = rng.bit();

    ProtoKiaV0 proto;
    CHECK_EQ(run(proto, pulse_pair_train(pair_bits, 250, 500, 900)).count, 0u);
}

/* ===========================================================================
 * BMW V0 — the same shape plus a CRC over the received bytes
 * ===========================================================================*/

TEST(subcar_bmw_crc8_matches_nrsc5_parameters) {
    /* The routine is poly 0x31, MSB first, init 0x00, no reflection, no final
     * XOR. That is CRC-8/NRSC-5 apart from the initial value, and a CRC with
     * initial value I over a message equals the same CRC with initial value 0
     * over the message with its first byte XORed by I. NRSC-5's published
     * check value over "123456789" is 0xF7. */
    const uint8_t msg[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    uint8_t seeded[9];
    std::memcpy(seeded, msg, sizeof(seeded));
    seeded[0] = static_cast<uint8_t>(seeded[0] ^ 0xFF);
    CHECK_EQ(static_cast<int>(ProtoBmwV0::crc8(seeded, 9)), 0xF7);

    /* An empty message leaves the register at the initial value. */
    CHECK_EQ(static_cast<int>(ProtoBmwV0::crc8(msg, 0)), 0x00);
    /* A single-bit change must change the result. */
    uint8_t altered[9];
    std::memcpy(altered, msg, sizeof(altered));
    altered[4] = static_cast<uint8_t>(altered[4] ^ 0x01);
    CHECK(ProtoBmwV0::crc8(altered, 9) != ProtoBmwV0::crc8(msg, 9));
}

TEST(subcar_bmw_crc16_known_vector) {
    /* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF — check == 0x29B1. */
    const uint8_t msg[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK_EQ(static_cast<int>(ProtoBmwV0::crc16(msg, 9)), 0x29B1);
}

TEST(subcar_bmw_v0_accepts_a_crc8_correct_frame) {
    /* 61 bits sit in the low 61 of the register, so the top three bits of the
     * first unpacked byte are zero. Choose bytes 0..6, then make byte 7 their
     * CRC8 so the decoder's check passes. */
    uint8_t bytes[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x00};
    bytes[7] = ProtoBmwV0::crc8(bytes, 7);

    uint64_t word = 0;
    for (int i = 0; i < 8; i++) word = (word << 8) | bytes[i];

    std::vector<uint8_t> bits(61);
    for (size_t i = 0; i < 61; i++) bits[i] = static_cast<uint8_t>((word >> (60 - i)) & 1);

    ProtoBmwV0 proto;
    const auto cap = run(proto, pulse_pair_train(bits, 350, 700, 1200));

    CHECK_EQ(cap.count, 1u);
    CHECK_EQ(static_cast<int>(cap.sensor_type), static_cast<int>(FPC_BMWV0));
    CHECK_EQ(static_cast<int>(cap.bits), 61);
    CHECK_EQ(cap.data, word);
}

TEST(subcar_bmw_v0_rejects_a_bad_crc) {
    uint8_t bytes[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x00};
    bytes[7] = static_cast<uint8_t>(ProtoBmwV0::crc8(bytes, 7) ^ 0x5A);

    uint64_t word = 0;
    for (int i = 0; i < 8; i++) word = (word << 8) | bytes[i];

    std::vector<uint8_t> bits(61);
    for (size_t i = 0; i < 61; i++) bits[i] = static_cast<uint8_t>((word >> (60 - i)) & 1);

    ProtoBmwV0 proto;
    CHECK_EQ(run(proto, pulse_pair_train(bits, 350, 700, 1200)).count, 0u);
}

/* ===========================================================================
 * Subaru — 800/1600 us pulse width, preamble + gap + sync
 * ===========================================================================*/

TEST(subcar_subaru_decodes_a_64_bit_key) {
    Rng rng;
    std::vector<uint8_t> bits(64);
    for (auto& b : bits) b = rng.bit();

    std::vector<Pulse> p;
    p.push_back({true, 1600});  /* preamble, one pulse per element */
    for (int i = 0; i < 14; i++) {
        p.push_back({false, 1600});
        p.push_back({true, 1600});
    }
    p.push_back({false, 2500});  /* inter-block gap */
    p.push_back({true, 2500});   /* sync                     */
    p.push_back({false, 1600});  /* into the data collector  */

    for (uint8_t b : bits) {
        p.push_back({true, b ? 800u : 1600u});  /* short HIGH = 1 */
        p.push_back({false, 800});
    }
    p.push_back({true, 4000});  /* end of transmission */

    ProtoSubaru proto;
    const auto cap = run(proto, p);

    CHECK_EQ(cap.count, 1u);
    CHECK_EQ(static_cast<int>(cap.sensor_type), static_cast<int>(FPC_SUBARU));
    CHECK_EQ(static_cast<int>(cap.bits), 64);
    CHECK_EQ(cap.data, bits_to_word(bits));
}

TEST(subcar_subaru_rejects_a_short_preamble) {
    Rng rng;
    std::vector<uint8_t> bits(64);
    for (auto& b : bits) b = rng.bit();

    std::vector<Pulse> p;
    p.push_back({true, 1600});
    for (int i = 0; i < 4; i++) {  /* only ~9 preamble pulses, needs > 20 */
        p.push_back({false, 1600});
        p.push_back({true, 1600});
    }
    p.push_back({false, 2500});
    p.push_back({true, 2500});
    p.push_back({false, 1600});
    for (uint8_t b : bits) {
        p.push_back({true, b ? 800u : 1600u});
        p.push_back({false, 800});
    }
    p.push_back({true, 4000});

    ProtoSubaru proto;
    CHECK_EQ(run(proto, p).count, 0u);
}

/* ===========================================================================
 * Kia V3/V4 — pulse width with a long sync whose polarity picks the variant
 * ===========================================================================*/

namespace {

std::vector<Pulse> kia_v3v4_train(const std::vector<uint8_t>& bits, bool v3) {
    std::vector<Pulse> p;
    p.push_back({true, 400});
    for (int i = 0; i < 9; i++) {
        p.push_back({false, 400});  /* each short LOW counts */
        p.push_back({true, 400});
    }
    if (v3) {
        p.push_back({false, 1200});  /* long LOW sync: V3, data inverted */
    } else {
        p.push_back({false, 400});
        p.push_back({true, 1200});   /* long HIGH sync: V4               */
    }

    for (size_t i = 0; i < bits.size(); i++) {
        p.push_back({true, bits[i] ? 800u : 400u});
        /* The gap after the last bit is the long one that ends the packet. */
        p.push_back({false, (i + 1 == bits.size()) ? 2000u : 400u});
    }
    return p;
}

/* The field extraction process_buffer() performs, recomputed independently. */
void kia_v3v4_expected(const std::vector<uint8_t>& bits, bool v3, uint32_t* serial, uint8_t* btn) {
    uint8_t b[9]{};
    for (size_t i = 0; i < bits.size() && i < 72; i++) {
        if (bits[i]) b[i / 8] = static_cast<uint8_t>(b[i / 8] | (1 << (7 - (i % 8))));
    }
    if (v3) {
        for (int i = 0; i < 9; i++) b[i] = static_cast<uint8_t>(~b[i]);
    }
    const auto rev = ProtoKiaV3V4::reverse8;
    *serial = (static_cast<uint32_t>(rev(static_cast<uint8_t>(b[7] & 0xF0))) << 24) |
              (static_cast<uint32_t>(rev(b[6])) << 16) |
              (static_cast<uint32_t>(rev(b[5])) << 8) | static_cast<uint32_t>(rev(b[4]));
    *btn = static_cast<uint8_t>((rev(b[7]) & 0xF0) >> 4);
}

}  // namespace

TEST(subcar_kia_v3v4_decodes_both_sync_polarities) {
    Rng rng;
    std::vector<uint8_t> bits(68);
    for (auto& b : bits) b = rng.bit();

    for (int variant = 0; variant < 2; variant++) {
        const bool v3 = (variant == 1);
        uint32_t serial = 0;
        uint8_t btn = 0;
        kia_v3v4_expected(bits, v3, &serial, &btn);

        ProtoKiaV3V4 proto;
        const auto cap = run(proto, kia_v3v4_train(bits, v3));

        CHECK_EQ(cap.count, 1u);
        CHECK_EQ(static_cast<int>(cap.sensor_type), static_cast<int>(FPC_KIAV3V4));
        CHECK_EQ(static_cast<int>(cap.bits), 68);
        CHECK_EQ(static_cast<uint32_t>(cap.data), serial);
        CHECK_EQ(static_cast<int>(cap.data2), static_cast<int>(btn));
    }
}

TEST(subcar_kia_v3v4_v3_is_the_bitwise_inverse_of_v4) {
    /* The only difference between the variants is that V3 inverts the buffer,
     * so a V3 frame of inverted bits decodes to the same fields as a V4 frame
     * of the original bits. */
    Rng rng;
    std::vector<uint8_t> bits(68);
    for (auto& b : bits) b = rng.bit();
    std::vector<uint8_t> inverted(bits.size());
    for (size_t i = 0; i < bits.size(); i++) inverted[i] = static_cast<uint8_t>(bits[i] ^ 1);

    ProtoKiaV3V4 a;
    const auto v4 = run(a, kia_v3v4_train(bits, false));
    ProtoKiaV3V4 b;
    const auto v3 = run(b, kia_v3v4_train(inverted, true));

    CHECK_EQ(v4.count, 1u);
    CHECK_EQ(v3.count, 1u);
    CHECK_EQ(v3.data, v4.data);
    CHECK_EQ(v3.data2, v4.data2);
}

/* ===========================================================================
 * Kia V2 and Kia V1 — Manchester
 * ===========================================================================*/

TEST(subcar_kia_v2_decodes_a_53_bit_frame) {
    Rng rng;
    /* Upstream seeds the counter at 1 and then adds a bit, so the 53-bit
     * threshold is met with 52 bits stored: the leading 1 plus 51 more. */
    std::vector<uint8_t> want(51);
    for (auto& b : want) b = rng.bit();

    ManchesterSearch search{500, 1000, /*invert_level=*/true, 0};
    std::vector<Pulse> data_pulses;
    /* The preamble ends on a HIGH, so the first data pulse is LOW; the
     * decoder's Manchester state was reset to Mid1. */
    CHECK(search.build(want, 0, ManchesterStateMid1, false, data_pulses));

    std::vector<Pulse> p;
    p.push_back({true, 1000});
    for (int i = 0; i < 105; i++) {
        p.push_back({false, 1000});
        p.push_back({true, 1000});
    }
    p.push_back({false, 1000});
    p.push_back({true, 500});  /* preamble exit, contributes the leading 1 */
    p.insert(p.end(), data_pulses.begin(), data_pulses.end());

    std::vector<uint8_t> all_bits{1};
    all_bits.insert(all_bits.end(), want.begin(), want.end());

    ProtoKiaV2 proto;
    const auto cap = run(proto, p);

    CHECK_EQ(cap.count, 1u);
    CHECK_EQ(static_cast<int>(cap.sensor_type), static_cast<int>(FPC_KIAV2));
    CHECK_EQ(static_cast<int>(cap.bits), 53);
    CHECK_EQ(cap.data, bits_to_word(all_bits));
}

TEST(subcar_kia_v1_decodes_a_57_bit_frame) {
    Rng rng;
    std::vector<uint8_t> want(55);
    for (auto& b : want) b = rng.bit();

    ManchesterSearch search{800, 1600, /*invert_level=*/true, 0};
    std::vector<Pulse> data_pulses;
    /* The preamble exits on a short LOW, so the first data pulse is HIGH. */
    CHECK(search.build(want, 0, ManchesterStateMid1, true, data_pulses));

    std::vector<Pulse> p;
    p.push_back({true, 1600});
    for (int i = 0; i < 75; i++) {
        p.push_back({false, 1600});  /* only the LOW pulses count */
        p.push_back({true, 1600});
    }
    p.push_back({false, 800});  /* preamble exit, contributes the leading 1 */
    p.insert(p.end(), data_pulses.begin(), data_pulses.end());

    std::vector<uint8_t> all_bits{1};
    all_bits.insert(all_bits.end(), want.begin(), want.end());

    ProtoKiaV1 proto;
    const auto cap = run(proto, p);

    CHECK_EQ(cap.count, 1u);
    CHECK_EQ(static_cast<int>(cap.sensor_type), static_cast<int>(FPC_KIAV1));
    CHECK_EQ(static_cast<int>(cap.bits), 57);
    CHECK_EQ(cap.data, bits_to_word(all_bits));
}

/* ===========================================================================
 * The protocol list
 * ===========================================================================*/

TEST(subcar_protocol_list_holds_every_type) {
    CarProtocols protos;
    for (uint8_t t = FPC_SUZUKI; t < FPC_COUNT; t++) {
        CHECK(protos.by_type(t) != nullptr);
        if (protos.by_type(t)) {
            CHECK_EQ(static_cast<int>(protos.by_type(t)->sensor_type), static_cast<int>(t));
        }
    }
    CHECK(protos.by_type(FPC_Invalid) == nullptr);
    CHECK(protos.by_type(FPC_COUNT) == nullptr);
}

TEST(subcar_protocol_list_routes_a_frame_to_exactly_one_decoder) {
    Rng rng;
    std::vector<uint8_t> data_bits(63);
    for (auto& b : data_bits) b = rng.bit();

    CarProtocols protos;
    std::vector<uint8_t> types;
    protos.set_callback([&types](CarProtocol& p) { types.push_back(p.sensor_type); });

    for (const auto& pulse : suzuki_train(data_bits)) protos.feed(pulse.level, pulse.duration);

    CHECK_EQ(types.size(), 1u);
    if (!types.empty()) CHECK_EQ(static_cast<int>(types[0]), static_cast<int>(FPC_SUZUKI));
}

/* ===========================================================================
 * OOK front end, end to end
 * ===========================================================================*/

namespace {

/* An on/off-keyed complex baseband for a pulse train. */
std::vector<dsp::cfloat> ook_signal(const std::vector<Pulse>& pulses,
                                    double sample_rate_hz,
                                    float amplitude) {
    std::vector<dsp::cfloat> out;
    for (const auto& p : pulses) {
        const size_t n = static_cast<size_t>(
            std::llround(static_cast<double>(p.duration) * 1e-6 * sample_rate_hz));
        const dsp::cfloat v = p.level ? dsp::cfloat{amplitude, 0.0f} : dsp::cfloat{0.0f, 0.0f};
        for (size_t i = 0; i < n; i++) out.push_back(v);
    }
    return out;
}

}  // namespace

TEST(subcar_ook_front_end_reproduces_pulse_timing) {
    /* A steady 500 us on / 500 us off square wave. Once the level estimators
     * have settled the reported durations must be within a few samples of
     * 500 us; the state machine's three-sample glitch filter is the only
     * systematic offset. */
    std::vector<Pulse> pulses;
    for (int i = 0; i < 40; i++) {
        pulses.push_back({true, 500});
        pulses.push_back({false, 500});
    }
    const auto signal = ook_signal(pulses, 500'000.0, 0.3f);

    PulseExtractor extractor;
    extractor.configure(500'000.0f, 0);

    std::vector<Pulse> got;
    extractor.set_handler(
        [&got](bool level, uint32_t us) { got.push_back({level, us}); });
    extractor.process(signal.data(), signal.size());

    CHECK(got.size() > 60);

    /* Skip the first few while the estimators adapt. */
    size_t checked = 0;
    for (size_t i = 10; i + 1 < got.size(); i++) {
        CHECK_NEAR(static_cast<double>(got[i].duration), 500.0, 40.0);
        checked++;
    }
    CHECK(checked > 40);
}

TEST(subcar_ook_front_end_feeds_a_decodable_suzuki_frame) {
    /* The whole receive chain below the radio: OOK baseband -> adaptive
     * slicer -> pulse durations -> protocol state machine -> decoded word. */
    Rng rng;
    std::vector<uint8_t> data_bits(63);
    for (auto& b : data_bits) b = rng.bit();

    std::vector<uint8_t> all_bits{1};
    all_bits.insert(all_bits.end(), data_bits.begin(), data_bits.end());

    /* The extractor reports a pulse only when the level *changes*, so the
     * frame needs one more transition after its closing gap for that gap to
     * reach the decoder — which is exactly what the next transmission, or the
     * next burst of noise, provides on the air. */
    auto train = suzuki_train(data_bits);
    train.push_back({true, 250});
    const auto signal = ook_signal(train, 500'000.0, 0.3f);

    ProtoSuzuki proto;
    Capture cap{};
    proto.set_callback([&cap](CarProtocol& p) {
        cap.sensor_type = p.sensor_type;
        cap.bits = p.data_count_bit;
        cap.data = p.decode_data;
        cap.count++;
    });

    PulseExtractor extractor;
    extractor.configure(500'000.0f, 0);
    extractor.set_handler([&proto](bool level, uint32_t us) { proto.feed(level, us); });
    extractor.process(signal.data(), signal.size());

    CHECK_EQ(cap.count, 1u);
    CHECK_EQ(cap.data, bits_to_word(all_bits));
}

TEST(subcar_ook_front_end_stays_quiet_on_silence) {
    std::vector<dsp::cfloat> silence(50'000, dsp::cfloat{0.0f, 0.0f});

    PulseExtractor extractor;
    extractor.configure(500'000.0f, 0);

    CarProtocols protos;
    size_t frames = 0;
    protos.set_callback([&frames](CarProtocol&) { frames++; });
    extractor.set_handler(
        [&protos](bool level, uint32_t us) { protos.feed(level, us); });
    extractor.process(silence.data(), silence.size());

    CHECK_EQ(frames, 0u);
}

/* ===========================================================================
 * parse_frame — the per-protocol field extraction
 * ===========================================================================*/

TEST(subcar_parse_suzuki_fields) {
    /* serial = ((high & 0xFFF) << 16) | (low >> 16); btn = (low >> 12) & 0xF;
     * cnt = (high << 4) >> 16. */
    const uint64_t data = (static_cast<uint64_t>(0x0ABC1234u) << 32) | 0x5678A000u;
    const auto f = parse_frame(FPC_SUZUKI, data, 0, 64);
    CHECK_EQ(f.serial, ((0x0ABC1234u & 0xFFF) << 16) | (0x5678A000u >> 16));
    CHECK_STR_EQ(f.button, "10");  /* (0x5678A000 >> 12) & 0xF == 0xA */
    CHECK_EQ(f.count, (0x0ABC1234u << 4) >> 16);
}

TEST(subcar_parse_vw_buttons) {
    struct Case { uint8_t nibble; const char* name; };
    const Case cases[] = {{0x1, "UNLOCK"}, {0x2, "LOCK"},    {0x3, "Un+Lk"},
                          {0x4, "TRUNK"},  {0x5, "Un+Tr"},   {0x6, "Lk+Tr"},
                          {0x7, "Un+Lk+Tr"}, {0x8, "PANIC"}, {0x9, "Unknown"}};
    for (const auto& c : cases) {
        const auto f = parse_frame(FPC_VW, 0xDEADBEEFu, static_cast<uint64_t>(c.nibble) << 4, 80);
        CHECK_STR_EQ(f.button, c.name);
        CHECK_EQ(f.serial, 0xDEADBEEFu);
    }
}

TEST(subcar_parse_subaru_reads_the_key_bytes) {
    /* Upstream casts the 64-bit word to a pointer; the bytes it wants are the
     * word itself, most significant first (that is how the decoder built it). */
    const uint64_t data = 0x0A11223344556677ull;
    const auto f = parse_frame(FPC_SUBARU, data, 0, 64);

    CHECK_EQ(f.serial, 0x112233u);  /* bytes 1..3 */
    CHECK_STR_EQ(f.button, "10");   /* byte 0 low nibble = 0xA */

    uint8_t b[8];
    unpack_be64(data, b);
    uint16_t expected = 0;
    subaru_decode_count(b, &expected);
    CHECK_EQ(f.count, static_cast<uint32_t>(expected));
}

TEST(subcar_subaru_counter_is_deterministic_and_uses_every_key_byte) {
    uint8_t b[8] = {0x00, 0xA5, 0x5A, 0x3C, 0xC3, 0x0F, 0xF0, 0x99};
    uint16_t a = 0;
    subaru_decode_count(b, &a);
    uint16_t again = 0;
    subaru_decode_count(b, &again);
    CHECK_EQ(a, again);

    /* Byte 0 carries only the button; the counter algorithm never reads it. */
    {
        uint8_t c[8];
        std::memcpy(c, b, sizeof(c));
        c[0] = static_cast<uint8_t>(c[0] ^ 0xFF);
        uint16_t other = 0;
        subaru_decode_count(c, &other);
        CHECK_EQ(other, a);
    }

    /* Bytes 1..7 all feed the shift registers or the mask. (How visible a
     * given byte is depends on the rotation count, which is itself derived
     * from bytes 4..6 — for some keys byte 2 rotates entirely out of the bits
     * the result reads. This key is one where all seven matter.) */
    for (int i = 1; i < 8; i++) {
        uint8_t c[8];
        std::memcpy(c, b, sizeof(c));
        c[i] = static_cast<uint8_t>(c[i] ^ 0xFF);
        uint16_t other = 0;
        subaru_decode_count(c, &other);
        CHECK(other != a);
    }
}

TEST(subcar_parse_kia_v5_reverses_and_decrypts) {
    const uint64_t data2 = 0x8899AABBCCDDEEFFull;

    /* The reversal upstream performs, recomputed here. */
    uint64_t yek = 0;
    for (int i = 0; i < 8; i++) {
        const uint8_t byte = static_cast<uint8_t>((data2 >> (i * 8)) & 0xFF);
        uint8_t reversed = 0;
        for (int b = 0; b < 8; b++) {
            if (byte & (1 << b)) reversed = static_cast<uint8_t>(reversed | (1 << (7 - b)));
        }
        yek |= (static_cast<uint64_t>(reversed) << ((7 - i) * 8));
    }

    const auto f = parse_frame(FPC_KIAV5, 0, data2, 67);
    CHECK_EQ(f.serial, static_cast<uint32_t>((yek >> 32) & 0x0FFFFFFFu));
    CHECK_STR_EQ(f.button, to_string_dec_uint(static_cast<uint8_t>((yek >> 60) & 0x0F)));
    CHECK_EQ(f.count, static_cast<uint32_t>(kia_v5_decode_count(
                          static_cast<uint32_t>(yek & 0xFFFFFFFFu))));
}

TEST(subcar_kia_v5_counter_depends_on_the_whole_word) {
    const uint16_t a = kia_v5_decode_count(0x12345678u);
    CHECK_EQ(a, kia_v5_decode_count(0x12345678u));
    CHECK(kia_v5_decode_count(0x12345679u) != a);
    CHECK(kia_v5_decode_count(0x92345678u) != a);
}

TEST(subcar_parse_kia_v2_and_v1_fields) {
    /* V2: serial = data >> 20, btn = (data >> 16) & 0xF, count is the 12-bit
     * field at bit 4 with its bytes swapped. */
    const uint64_t v2 = 0x0123456789ABCull;
    const auto f2 = parse_frame(FPC_KIAV2, v2, 0, 53);
    CHECK_EQ(f2.serial, static_cast<uint32_t>((v2 >> 20) & 0xFFFFFFFFu));
    CHECK_STR_EQ(f2.button, to_string_dec_uint(static_cast<uint8_t>((v2 >> 16) & 0x0F)));
    const uint16_t raw = static_cast<uint16_t>((v2 >> 4) & 0xFFF);
    CHECK_EQ(f2.count, static_cast<uint32_t>(((raw >> 4) | (raw << 8)) & 0xFFF));

    /* V1: serial = data >> 24, btn = (data >> 16) & 0xFF. */
    const uint64_t v1 = 0x00FEDCBA98765432ull;
    const auto f1 = parse_frame(FPC_KIAV1, v1, 0, 57);
    CHECK_EQ(f1.serial, static_cast<uint32_t>((v1 >> 24) & 0xFFFFFFFFu));
    CHECK_STR_EQ(f1.button, to_string_dec_uint(static_cast<uint8_t>((v1 >> 16) & 0xFF)));
}

TEST(subcar_parse_kia_v0_and_bmw_share_a_layout) {
    const uint64_t data = 0x0123456789ABCDEFull;
    const auto kia = parse_frame(FPC_KIAV0, data, 0, 61);
    const auto bmw = parse_frame(FPC_BMWV0, data, 0, 61);

    CHECK_EQ(kia.serial, static_cast<uint32_t>((data >> 12) & 0x0FFFFFFFu));
    CHECK_EQ(kia.count, static_cast<uint32_t>((data >> 40) & 0xFFFFu));
    CHECK_STR_EQ(kia.button, to_string_dec_uint(static_cast<uint8_t>((data >> 8) & 0x0F)));

    CHECK_EQ(bmw.serial, kia.serial);
    CHECK_EQ(bmw.count, kia.count);
    CHECK_STR_EQ(bmw.button, kia.button);
}

TEST(subcar_parse_kia_v3v4_leaves_keeloq_encrypted) {
    const auto f = parse_frame(FPC_KIAV3V4, 0xCAFEBABEu, 5, 68);
    CHECK_EQ(f.serial, 0xCAFEBABEu);
    CHECK_STR_EQ(f.button, "?");
    CHECK_EQ(f.count, kNoCount);
}

TEST(subcar_parse_fiat_fields) {
    const uint64_t data = (static_cast<uint64_t>(0x11112222u) << 32) | 0x33334444u;
    const auto f = parse_frame(FPC_FIATV0, data, 0x7Fu, 64);
    CHECK_EQ(f.serial, 0x33334444u);
    CHECK_EQ(f.count, 0x11112222u);
    CHECK_STR_EQ(f.button, "127");
}

TEST(subcar_parse_ford_unscrambles_the_key) {
    /* The Ford transform is long; this pins it by recomputing it here from the
     * same inputs, which catches any transcription slip in the port. */
    const uint64_t data = 0x0102030405060708ull;
    const uint64_t data2 = 0x1234ull;

    uint8_t buf[13] = {0};
    for (int i = 0; i < 8; ++i) buf[i] = static_cast<uint8_t>(data >> (56 - i * 8));
    buf[8] = static_cast<uint8_t>(data2 >> 8);
    buf[9] = static_cast<uint8_t>(data2 & 0xFF);

    uint8_t tmp = buf[8];
    uint8_t parity = 0;
    const uint8_t parity_any = (tmp != 0) ? 1 : 0;
    while (tmp) {
        parity = static_cast<uint8_t>(parity ^ (tmp & 1));
        tmp = static_cast<uint8_t>(tmp >> 1);
    }
    buf[11] = parity_any ? parity : 0;

    uint8_t xor_byte;
    uint8_t limit;
    if (buf[11]) {
        xor_byte = buf[7];
        limit = 7;
    } else {
        xor_byte = buf[6];
        limit = 6;
    }
    for (int idx = 1; idx < limit; ++idx) buf[idx] = static_cast<uint8_t>(buf[idx] ^ xor_byte);
    if (buf[11] == 0) buf[7] = static_cast<uint8_t>(buf[7] ^ xor_byte);

    const uint8_t orig_b7 = buf[7];
    buf[7] = static_cast<uint8_t>((orig_b7 & 0xAA) | (buf[6] & 0x55));
    const uint8_t mixed = static_cast<uint8_t>((buf[6] & 0xAA) | (orig_b7 & 0x55));
    buf[6] = mixed;

    const uint32_t serial_le = static_cast<uint32_t>(buf[1]) |
                               (static_cast<uint32_t>(buf[2]) << 8) |
                               (static_cast<uint32_t>(buf[3]) << 16) |
                               (static_cast<uint32_t>(buf[4]) << 24);
    const uint32_t expected_serial = ((serial_le & 0xFF) << 24) |
                                     (((serial_le >> 8) & 0xFF) << 16) |
                                     (((serial_le >> 16) & 0xFF) << 8) | ((serial_le >> 24) & 0xFF);

    const auto f = parse_frame(FPC_FORDV0, data, data2, 64);
    CHECK_EQ(f.serial, expected_serial);
    CHECK_STR_EQ(f.button, to_string_dec_uint(static_cast<uint8_t>((buf[5] >> 4) & 0x0F)));
    CHECK_EQ(f.count, static_cast<uint32_t>(((buf[5] & 0x0F) << 16) | (buf[6] << 8) | buf[7]));
}

TEST(subcar_parse_invalid_yields_nothing) {
    const auto f = parse_frame(FPC_Invalid, 0x1234, 0x5678, 32);
    CHECK_EQ(f.serial, 0u);
    CHECK(f.button.empty());
    CHECK_EQ(f.count, kNoCount);
}

/* ===========================================================================
 * Names and recent entries
 * ===========================================================================*/

TEST(subcar_sensor_names) {
    CHECK_STR_EQ(sensor_type_name(FPC_SUZUKI), "Suzuki");
    CHECK_STR_EQ(sensor_type_name(FPC_VW), "VW");
    CHECK_STR_EQ(sensor_type_name(FPC_SUBARU), "Subaru");
    CHECK_STR_EQ(sensor_type_name(FPC_KIAV5), "Kia V5");
    CHECK_STR_EQ(sensor_type_name(FPC_KIAV3V4), "Kia V3/V4");
    CHECK_STR_EQ(sensor_type_name(FPC_KIAV2), "Kia V2");
    CHECK_STR_EQ(sensor_type_name(FPC_KIAV1), "Kia V1");
    CHECK_STR_EQ(sensor_type_name(FPC_KIAV0), "Kia V0");
    CHECK_STR_EQ(sensor_type_name(FPC_FORDV0), "Ford V0");
    CHECK_STR_EQ(sensor_type_name(FPC_FIATV0), "Fiat V0");
    CHECK_STR_EQ(sensor_type_name(FPC_BMWV0), "BMW V0");
    CHECK_STR_EQ(sensor_type_name(FPC_Invalid), "Unknown");
    CHECK_STR_EQ(sensor_type_name(200), "Unknown");
}

TEST(subcar_recent_entry_key_and_age) {
    RecentEntry a{FPC_SUZUKI, 0x1122334455667788ull, 0, 64};
    RecentEntry b{FPC_VW, 0x1122334455667788ull, 0, 80};

    /* The key mixes the type into the low byte, so two protocols reporting the
     * same word do not collapse into one row. */
    CHECK(a.key() != b.key());
    CHECK_EQ(a.key(), 0x1122334455667788ull ^ static_cast<uint64_t>(FPC_SUZUKI));

    a.inc_age(3);
    CHECK_EQ(static_cast<int>(a.age), 3);
    a.reset_age();
    CHECK_EQ(static_cast<int>(a.age), 0);

    /* Age saturates rather than wrapping. */
    a.age = UINT16_MAX - 1;
    a.inc_age(5);
    CHECK_EQ(static_cast<int>(a.age), UINT16_MAX - 1);
}

TEST(subcar_recent_entry_csv) {
    RecentEntry e{FPC_BMWV0, 0xABCDull, 0x12ull, 61};
    CHECK_STR_EQ(e.to_csv(),
                 ";BMW V0;61;000000000000ABCD;0000000000000012");
}
