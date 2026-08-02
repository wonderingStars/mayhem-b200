/*
 * mayhem-b200 — protocol primitive and digital demodulator tests.
 *
 * Expectations here come from the protocol definitions or from upstream's
 * implementation, never from what this code happens to emit:
 *
 *   - CRC check values are the Rocksoft "123456789" vectors.
 *   - Matched filter taps are compared against upstream's hand-computed
 *     baseband::ais::square_taps_38k4_1t_p.
 *   - PacketBuilder's unstuffing result is traced by hand through upstream's
 *     packet_builder.hpp state machine.
 *   - The early-late gate's outputs are worked out from phase_detector.hpp's
 *     mask arithmetic.
 *   - Demodulators are checked against signals whose intended bits are known,
 *     located with a sync word exactly as a real decoder would.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "demod_digital.hpp"
#include "protocol.hpp"

#include <cmath>
#include <complex>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char kCheckVector[] = "123456789";
constexpr size_t kCheckLength = 9;

/* Deterministic pseudo-random bits — a test that depends on rand() is not a
 * test. Numerical Recipes' LCG constants. */
class Lcg {
   public:
    explicit Lcg(uint32_t seed) : state_{seed} {}
    uint32_t next() {
        state_ = state_ * 1664525u + 1013904223u;
        return state_;
    }
    uint8_t bit() { return static_cast<uint8_t>((next() >> 24) & 1u); }

   private:
    uint32_t state_;
};

std::vector<uint8_t> random_bits(size_t count, uint32_t seed) {
    Lcg lcg{seed};
    std::vector<uint8_t> bits(count);
    for (size_t i = 0; i < count; i++) bits[i] = lcg.bit();
    return bits;
}

/* MSB-first expansion of a sync word into individual bits. */
std::vector<uint8_t> sync_bits(uint64_t code, size_t length) {
    std::vector<uint8_t> bits(length);
    for (size_t i = 0; i < length; i++) {
        bits[i] = static_cast<uint8_t>((code >> (length - 1 - i)) & 1ULL);
    }
    return bits;
}

/* [preamble][sync word][payload][flush] — the shape of nearly every real
 * burst, and what lets a demodulator test assert on exact payload bits without
 * caring how many junk bits the receiver emitted while it locked.
 *
 * The trailing flush matters: a demodulator has filter group delay and a
 * symbol clock to run out, so the last payload bit only appears if the
 * transmission continues past it. Without it the final bit is simply never
 * decided, which is a property of streaming demodulation, not a defect.
 *
 * `pseudo_random_preamble` picks the acquisition sequence. Alternating bits
 * are the classic training pattern and work for OOK, FSK and AFSK, whose
 * timing recovery keys off slicer transitions. They do NOT work for PSK: see
 * PskDemod's header comment and gardner_gets_no_timing_from_a_pure_tone
 * below. */
std::vector<uint8_t> build_frame(size_t preamble_bits,
                                 uint64_t sync,
                                 size_t sync_length,
                                 const std::vector<uint8_t>& payload,
                                 bool pseudo_random_preamble = false,
                                 size_t flush_bits = 24) {
    std::vector<uint8_t> bits;
    bits.reserve(preamble_bits + sync_length + payload.size() + flush_bits);

    if (pseudo_random_preamble) {
        for (uint8_t b : random_bits(preamble_bits, 0xACE1u)) bits.push_back(b);
    } else {
        for (size_t i = 0; i < preamble_bits; i++) bits.push_back(static_cast<uint8_t>(i & 1));
    }
    for (uint8_t b : sync_bits(sync, sync_length)) bits.push_back(b);
    for (uint8_t b : payload) bits.push_back(b);
    for (size_t i = 0; i < flush_bits; i++) bits.push_back(static_cast<uint8_t>(i & 1));
    return bits;
}

/* Finds `sync` in `recovered` and counts how many of the payload bits that
 * follow it are wrong. Returns -1 if the sync was never found. */
int payload_errors(const std::vector<uint8_t>& recovered,
                   uint64_t sync,
                   size_t sync_length,
                   const std::vector<uint8_t>& payload) {
    dsp::BitCorrelator correlator{sync, sync_length};
    for (size_t i = 0; i < recovered.size(); i++) {
        if (!correlator.feed(recovered[i])) continue;

        const size_t start = i + 1;
        if (start + payload.size() > recovered.size()) return -1;

        int errors = 0;
        for (size_t j = 0; j < payload.size(); j++) {
            if ((recovered[start + j] & 1) != (payload[j] & 1)) errors++;
        }
        return errors;
    }
    return -1;
}

constexpr double kPi = 3.14159265358979323846;

}  // namespace

/* ===========================================================================
 * CRC
 * ===========================================================================*/

TEST(crc16_ccitt_check_vector) {
    /* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, xorout 0. */
    CHECK_EQ(dsp::crc16_ccitt(kCheckVector, kCheckLength), uint16_t{0x29B1});

    auto crc = dsp::make_crc16_ccitt();
    for (size_t i = 0; i < kCheckLength; i++) {
        crc.process_byte(static_cast<uint8_t>(kCheckVector[i]));
    }
    CHECK_EQ(crc.checksum(), uint32_t{0x29B1});
}

TEST(crc32_check_vector) {
    /* CRC-32/ISO-HDLC: poly 0x04C11DB7, init and xorout 0xFFFFFFFF, reflected. */
    CHECK_EQ(dsp::crc32(kCheckVector, kCheckLength), uint32_t{0xCBF43926});
}

TEST(crc8_check_vector) {
    /* CRC-8/SMBUS: poly 0x07, init 0x00, no reflection, xorout 0. */
    CHECK_EQ(dsp::crc8(kCheckVector, kCheckLength), uint8_t{0xF4});
}

TEST(crc16_ibm_check_vector) {
    /* CRC-16/ARC (IBM): poly 0x8005, init 0, reflected in and out, xorout 0. */
    CHECK_EQ(dsp::crc16_ibm(kCheckVector, kCheckLength), uint16_t{0xBB3D});
}

TEST(crc_x25_check_vector) {
    /* The AX.25 / X.25 FCS as upstream configures it in
     * application/protocols/ax25.hpp: CRC<16, true, true>{0x1021, 0xFFFF,
     * 0xFFFF}. Catalogue check value for CRC-16/IBM-SDLC is 0x906E. */
    dsp::Crc<16, true, true> crc{0x1021, 0xFFFF, 0xFFFF};
    crc.process_bytes(kCheckVector, kCheckLength);
    CHECK_EQ(crc.checksum(), uint32_t{0x906E});
}

TEST(crc_empty_message) {
    CHECK_EQ(dsp::crc8(nullptr, 0), uint8_t{0x00});
    CHECK_EQ(dsp::crc16_ccitt(nullptr, 0), uint16_t{0xFFFF});
    CHECK_EQ(dsp::crc16_ibm(nullptr, 0), uint16_t{0x0000});
    /* init 0xFFFFFFFF, reflected to itself, XORed with 0xFFFFFFFF. */
    CHECK_EQ(dsp::crc32(nullptr, 0), uint32_t{0x00000000});
}

TEST(crc_reset_restores_initial_state) {
    auto crc = dsp::make_crc16_ccitt();
    crc.process_bytes(kCheckVector, kCheckLength);
    CHECK_EQ(crc.checksum(), uint32_t{0x29B1});

    crc.reset();
    CHECK_EQ(crc.checksum(), uint32_t{0xFFFF});
    crc.process_bytes(kCheckVector, kCheckLength);
    CHECK_EQ(crc.checksum(), uint32_t{0x29B1});

    crc.reset(0x0000);
    CHECK_EQ(crc.checksum(), uint32_t{0x0000});
}

TEST(crc_bitwise_matches_bytewise) {
    /* process_bits() MSB-first over 8 bits must equal process_byte(). */
    auto by_byte = dsp::make_crc16_ccitt();
    auto by_bit = dsp::make_crc16_ccitt();
    for (size_t i = 0; i < kCheckLength; i++) {
        const uint8_t byte = static_cast<uint8_t>(kCheckVector[i]);
        by_byte.process_byte(byte);
        for (int b = 7; b >= 0; b--) by_bit.process_bit(((byte >> b) & 1) != 0);
    }
    CHECK_EQ(by_bit.checksum(), by_byte.checksum());
}

TEST(crc_process_bits_arbitrary_width) {
    /* Two 4-bit chunks MSB-first must equal the byte they concatenate to. */
    auto whole = dsp::make_crc16_ccitt();
    auto split = dsp::make_crc16_ccitt();
    whole.process_byte(0xA7);
    split.process_bits(0xA, 4);
    split.process_bits(0x7, 4);
    CHECK_EQ(split.checksum(), whole.checksum());
}

TEST(crc_reflected_input_consumes_lsb_first) {
    /* With RevIn, process_bits must walk the chunk from its LSB. */
    dsp::Crc16Ibm reflected{0x8005, 0x0000, 0x0000};
    dsp::Crc16Ibm manual{0x8005, 0x0000, 0x0000};
    reflected.process_byte(0x8D);
    for (int b = 0; b < 8; b++) manual.process_bit(((0x8D >> b) & 1) != 0);
    CHECK_EQ(reflected.checksum(), manual.checksum());
}

TEST(crc_appended_checksum_yields_known_residue) {
    /* Feeding data then its own CRC-16/CCITT leaves the register at zero —
     * the residue check every packet decoder relies on. */
    auto crc = dsp::make_crc16_ccitt();
    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    crc.process_bytes(data, sizeof(data));
    const uint16_t fcs = static_cast<uint16_t>(crc.checksum());

    auto verify = dsp::make_crc16_ccitt();
    verify.process_bytes(data, sizeof(data));
    verify.process_byte(static_cast<uint8_t>(fcs >> 8));
    verify.process_byte(static_cast<uint8_t>(fcs & 0xFF));
    CHECK_EQ(verify.checksum(), uint32_t{0x0000});
}

/* ===========================================================================
 * Manchester
 * ===========================================================================*/

TEST(manchester_roundtrip_arbitrary_bytes) {
    const std::vector<uint8_t> bits = random_bits(256, 0x5EED1234u);

    for (size_t sense = 0; sense <= 1; sense++) {
        const auto chips = dsp::manchester_encode(bits, sense);
        CHECK_EQ(chips.size(), bits.size() * 2);

        std::vector<uint8_t> errors;
        const auto decoded = dsp::manchester_decode(chips, sense, &errors);
        CHECK_EQ(decoded.size(), bits.size());

        int mismatches = 0;
        int flagged = 0;
        for (size_t i = 0; i < bits.size(); i++) {
            if (decoded[i] != bits[i]) mismatches++;
            if (errors[i]) flagged++;
        }
        CHECK_EQ(mismatches, 0);
        CHECK_EQ(flagged, 0);
    }
}

TEST(manchester_chip_pattern_matches_upstream_sense) {
    /* Upstream tpms_tx_app.cpp: "RX ManchesterDecoder with sense=0:
     * '1' = "10", '0' = "01"". */
    const std::vector<uint8_t> bits{1, 0};
    const auto chips = dsp::manchester_encode(bits, 0);
    CHECK_EQ(chips.size(), size_t{4});
    CHECK_EQ(chips[0], uint8_t{1});
    CHECK_EQ(chips[1], uint8_t{0});
    CHECK_EQ(chips[2], uint8_t{0});
    CHECK_EQ(chips[3], uint8_t{1});

    const auto sense1 = dsp::manchester_encode(bits, 1);
    CHECK_EQ(sense1[0], uint8_t{0});
    CHECK_EQ(sense1[1], uint8_t{1});
}

TEST(manchester_wrong_sense_inverts) {
    const std::vector<uint8_t> bits{1, 1, 0, 1, 0, 0};
    const auto chips = dsp::manchester_encode(bits, 0);
    const auto decoded = dsp::manchester_decode(chips, 1);
    for (size_t i = 0; i < bits.size(); i++) {
        CHECK_EQ(decoded[i], static_cast<uint8_t>(bits[i] ^ 1));
    }
}

TEST(manchester_flags_equal_chips_as_error) {
    /* A pair with no transition is not a valid Manchester symbol. */
    std::vector<uint8_t> chips{1, 0, 1, 1, 0, 1};
    std::vector<uint8_t> errors;
    const auto decoded = dsp::manchester_decode(chips, 0, &errors);
    CHECK_EQ(decoded.size(), size_t{3});
    CHECK_EQ(errors[0], uint8_t{0});
    CHECK_EQ(errors[1], uint8_t{1});
    CHECK_EQ(errors[2], uint8_t{0});
}

TEST(manchester_symbol_past_end_reports_error) {
    const std::vector<uint8_t> chips{1, 0};
    const auto s = dsp::manchester_symbol(chips.data(), chips.size(), 5, 0);
    CHECK_EQ(static_cast<int>(s.value), 0);
    CHECK_EQ(static_cast<int>(s.error), 1);
}

TEST(manchester_encode_bytes_matches_bit_encoder) {
    /* Upstream's byte form writes 0xFF/0x00 per chip from an MSB-first
     * packed source. */
    const uint8_t src[1] = {0xB2};  /* 1011 0010 */
    uint8_t dest[16] = {};
    dsp::manchester_encode_bytes(dest, src, 8, 0);

    const std::vector<uint8_t> bits{1, 0, 1, 1, 0, 0, 1, 0};
    const auto chips = dsp::manchester_encode(bits, 0);
    for (size_t i = 0; i < 16; i++) {
        CHECK_EQ(dest[i], static_cast<uint8_t>(chips[i] ? 0xFF : 0x00));
    }
}

TEST(biphase_m_known_chip_pattern) {
    /* Bi-phase mark: a transition every symbol boundary, plus a mid-symbol
     * transition for a 1. Starting level 0, bits 1,0,1 -> 10 11 01. */
    const std::vector<uint8_t> bits{1, 0, 1};
    const auto chips = dsp::biphase_m_encode(bits, 0);
    const std::vector<uint8_t> expected{1, 0, 1, 1, 0, 1};
    CHECK_EQ(chips.size(), expected.size());
    for (size_t i = 0; i < expected.size(); i++) CHECK_EQ(chips[i], expected[i]);
}

TEST(biphase_m_roundtrip) {
    const std::vector<uint8_t> bits = random_bits(200, 0xC0FFEEu);
    for (uint8_t level = 0; level <= 1; level++) {
        const auto chips = dsp::biphase_m_encode(bits, level);
        std::vector<uint8_t> errors;
        const auto decoded = dsp::biphase_m_decode(chips, &errors);
        CHECK_EQ(decoded.size(), bits.size());
        int mismatches = 0;
        int flagged = 0;
        for (size_t i = 0; i < bits.size(); i++) {
            if (decoded[i] != bits[i]) mismatches++;
            if (errors[i]) flagged++;
        }
        CHECK_EQ(mismatches, 0);
        CHECK_EQ(flagged, 0);
    }
}

TEST(biphase_m_flags_missing_boundary_transition) {
    /* Chips 1,0,0,1: the second symbol starts at the same level the first
     * ended on, so the boundary transition is missing. */
    const std::vector<uint8_t> chips{1, 0, 0, 1};
    std::vector<uint8_t> errors;
    const auto decoded = dsp::biphase_m_decode(chips, &errors);
    CHECK_EQ(decoded.size(), size_t{2});
    CHECK_EQ(errors[0], uint8_t{0});
    CHECK_EQ(errors[1], uint8_t{1});
}

TEST(nrzi_roundtrip_and_known_sequence) {
    dsp::NrziEncoder encoder;
    dsp::NrziDecoder decoder;

    const std::vector<uint8_t> bits = random_bits(128, 0x1234ABCDu);
    for (uint8_t b : bits) {
        const uint_fast8_t symbol = encoder(b);
        CHECK_EQ(static_cast<int>(decoder(symbol)), static_cast<int>(b));
    }

    /* Upstream NRZIDecoder: out = ~(symbol ^ last) & 1, last starts at 0. */
    dsp::NrziDecoder fresh;
    CHECK_EQ(static_cast<int>(fresh(0)), 1);  /* 0 vs 0 -> no change -> 1 */
    CHECK_EQ(static_cast<int>(fresh(1)), 0);  /* 0 -> 1 -> change  -> 0 */
    CHECK_EQ(static_cast<int>(fresh(1)), 1);
    CHECK_EQ(static_cast<int>(fresh(0)), 0);
}

/* ===========================================================================
 * Bit pattern correlation
 * ===========================================================================*/

TEST(correlator_finds_sync_at_known_offset) {
    constexpr uint64_t kSync = 0xB5CAu;  /* 1011 0101 1100 1010 */
    constexpr size_t kSyncLength = 16;
    constexpr size_t kLeadIn = 20;

    std::vector<uint8_t> stream(kLeadIn, 0);
    for (uint8_t b : sync_bits(kSync, kSyncLength)) stream.push_back(b);
    for (uint8_t b : random_bits(30, 0xAA55u)) stream.push_back(b);

    dsp::BitCorrelator correlator{kSync, kSyncLength};
    int matches = 0;
    size_t start = 0;
    size_t end = 0;
    for (size_t i = 0; i < stream.size(); i++) {
        if (correlator.feed(stream[i])) {
            matches++;
            start = correlator.match_start_bit();
            end = correlator.match_end_bit();
        }
    }

    CHECK_EQ(matches, 1);
    CHECK_EQ(start, kLeadIn);
    CHECK_EQ(end, kLeadIn + kSyncLength - 1);
}

TEST(correlator_silent_on_stream_without_sync) {
    constexpr uint64_t kSync = 0xB5CAu;
    dsp::BitCorrelator correlator{kSync, 16};

    /* A repeating 1100 pattern; every 16-bit window is a rotation of
     * 1100110011001100, none of which is the sync. */
    const uint8_t cycle[4] = {1, 1, 0, 0};
    int matches = 0;
    for (size_t i = 0; i < 400; i++) {
        if (correlator.feed(cycle[i % 4])) matches++;
    }
    CHECK_EQ(matches, 0);

    /* All zeros must not match either. */
    correlator.reset();
    for (size_t i = 0; i < 200; i++) {
        if (correlator.feed(0)) matches++;
    }
    CHECK_EQ(matches, 0);
}

TEST(correlator_will_not_fire_before_the_register_fills) {
    /* Sync word 0x0000: upstream's zero-initialised BitHistory would declare
     * a match on the very first bit. */
    dsp::BitCorrelator correlator{0x0000u, 16};
    for (size_t i = 0; i < 15; i++) {
        CHECK(!correlator.feed(0));
    }
    CHECK(correlator.feed(0));
    CHECK_EQ(correlator.match_start_bit(), size_t{0});
    CHECK_EQ(correlator.match_end_bit(), size_t{15});
}

TEST(correlator_hamming_tolerance) {
    constexpr uint64_t kSync = 0xB5CAu;
    constexpr size_t kSyncLength = 16;

    auto corrupted = sync_bits(kSync, kSyncLength);
    corrupted[3] ^= 1;

    dsp::BitCorrelator strict{kSync, kSyncLength, 0};
    dsp::BitCorrelator tolerant{kSync, kSyncLength, 1};

    int strict_matches = 0;
    int tolerant_matches = 0;
    for (size_t i = 0; i < 8; i++) {
        strict.feed(0);
        tolerant.feed(0);
    }
    for (uint8_t b : corrupted) {
        if (strict.feed(b)) strict_matches++;
        if (tolerant.feed(b)) tolerant_matches++;
    }
    CHECK_EQ(strict_matches, 0);
    CHECK_EQ(tolerant_matches, 1);

    /* Two bit errors must defeat a tolerance of one. */
    auto worse = sync_bits(kSync, kSyncLength);
    worse[3] ^= 1;
    worse[11] ^= 1;
    dsp::BitCorrelator tolerant2{kSync, kSyncLength, 1};
    int matches = 0;
    for (size_t i = 0; i < 8; i++) tolerant2.feed(0);
    for (uint8_t b : worse) {
        if (tolerant2.feed(b)) matches++;
    }
    CHECK_EQ(matches, 0);
}

TEST(bit_pattern_distance_is_hamming) {
    const dsp::BitPattern pattern{0b1111u, 4, 0};
    CHECK_EQ(pattern.distance(0b1111u), size_t{0});
    CHECK_EQ(pattern.distance(0b1011u), size_t{1});
    CHECK_EQ(pattern.distance(0b0000u), size_t{4});
    /* Bits above the pattern length are masked off. */
    CHECK_EQ(pattern.distance(0xDEADBEEFFFFFFFF0ULL | 0xFULL), size_t{0});
}

TEST(fixed_error_filter_is_upstream_verbatim) {
    /* clock_recovery.hpp: (lateness < 0) ? weight : -weight, with no dead
     * zone, so an exactly-zero error votes -weight. */
    const dsp::FixedErrorFilter filter{0.25f};
    CHECK_NEAR(filter(-1.0f), 0.25f, 1e-9);
    CHECK_NEAR(filter(1.0f), -0.25f, 1e-9);
    CHECK_NEAR(filter(0.0f), -0.25f, 1e-9);
    CHECK_NEAR(dsp::FixedErrorFilter{}.weight(), 1.0f / 16.0f, 1e-9);
}

TEST(deadband_error_filter_abstains_on_weak_evidence) {
    dsp::DeadbandErrorFilter filter{0.25f, 0.25f, 1.0f / 8.0f};

    /* Freshly reset the tracked mean is zero, so nothing is gated and the
     * loop acquires at full speed. */
    CHECK_NEAR(filter(1.0f), -0.25f, 1e-9);

    /* Build the mean up with strong errors, then a weak one must abstain
     * while a strong one still votes. Sign convention matches upstream:
     * negative lateness means early, so push the sampling instant later. */
    for (int i = 0; i < 60; i++) filter(1.0f);
    CHECK(filter.mean_magnitude() > 0.9f);
    CHECK_NEAR(filter(0.05f), 0.0f, 1e-9);
    CHECK_NEAR(filter(-0.05f), 0.0f, 1e-9);
    CHECK_NEAR(filter(1.0f), -0.25f, 1e-9);
    CHECK_NEAR(filter(-1.0f), 0.25f, 1e-9);

    /* An exact zero abstains whatever the mean. */
    CHECK_NEAR(filter(0.0f), 0.0f, 1e-9);

    filter.reset();
    CHECK_NEAR(filter.mean_magnitude(), 0.0f, 1e-9);
}

TEST(deadband_error_filter_does_not_drift_on_runs) {
    /* The defect this filter exists to fix, reproduced from measured numbers:
     * on a 2FSK burst, symbols at a real transition report a mean |lateness|
     * of about 0.26 and symbols inside a run report a consistently-signed
     * 0.014. Feed a nine-symbol run of that residue after some real
     * transitions and total up what each filter tells the resampler to do. */
    const float run_residue = -0.014f;

    dsp::FixedErrorFilter upstream{1.0f / 16.0f};
    dsp::DeadbandErrorFilter guarded{1.0f / 16.0f};

    /* Alternating real errors first: neither filter should have a net bias. */
    float upstream_sum = 0.0f;
    float guarded_sum = 0.0f;
    for (int i = 0; i < 40; i++) {
        const float lateness = (i & 1) ? 0.26f : -0.26f;
        upstream_sum += upstream(lateness);
        guarded_sum += guarded(lateness);
    }
    CHECK_NEAR(upstream_sum, 0.0f, 1e-5);
    CHECK_NEAR(guarded_sum, 0.0f, 1e-5);

    /* Now the run. Upstream votes a full step nine times in the same
     * direction; the guarded filter abstains. */
    float upstream_run = 0.0f;
    float guarded_run = 0.0f;
    for (int i = 0; i < 9; i++) {
        upstream_run += upstream(run_residue);
        guarded_run += guarded(run_residue);
    }
    CHECK_NEAR(upstream_run, 9.0f / 16.0f, 1e-5);
    CHECK_NEAR(guarded_run, 0.0f, 1e-9);
}

/* ===========================================================================
 * PacketBuilder
 * ===========================================================================*/

TEST(packet_builder_emits_configured_length_after_sync) {
    constexpr uint64_t kSync = 0xB5u;  /* 1011 0101 */
    constexpr size_t kSyncLength = 8;
    constexpr size_t kPayloadBits = 16;

    const std::vector<uint8_t> payload{1, 0, 0, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1};

    std::vector<dsp::Packet> received;
    dsp::FixedLengthPacketBuilder builder{
        dsp::BitPattern{kSync, kSyncLength},
        dsp::NeverMatch{},
        dsp::FixedLength{kPayloadBits},
        [&received](const dsp::Packet& p) { received.push_back(p); }};

    for (size_t i = 0; i < 24; i++) builder.execute(0);
    for (uint8_t b : sync_bits(kSync, kSyncLength)) builder.execute(b);
    for (uint8_t b : payload) builder.execute(b);

    CHECK_EQ(received.size(), size_t{1});
    if (received.size() == 1) {
        CHECK_EQ(received[0].size(), kPayloadBits);
        int mismatches = 0;
        for (size_t i = 0; i < kPayloadBits; i++) {
            if (received[0][i] != payload[i]) mismatches++;
        }
        CHECK_EQ(mismatches, 0);
    }

    /* Trailing bits after the packet must not extend it. */
    for (size_t i = 0; i < 40; i++) builder.execute(0);
    CHECK_EQ(received.size(), size_t{1});
    CHECK_EQ(builder.packets_emitted(), size_t{1});
}

TEST(packet_builder_emits_nothing_without_sync) {
    std::vector<dsp::Packet> received;
    dsp::FixedLengthPacketBuilder builder{
        dsp::BitPattern{0xB5u, 8},
        dsp::NeverMatch{},
        dsp::FixedLength{16},
        [&received](const dsp::Packet& p) { received.push_back(p); }};

    const auto bits = random_bits(300, 0x99u);
    for (uint8_t b : bits) {
        /* Force a stream that never contains 10110101 by emitting pairs. */
        builder.execute(static_cast<uint_fast8_t>(b & 1));
        builder.execute(static_cast<uint_fast8_t>(b & 1));
        builder.execute(static_cast<uint_fast8_t>(b & 1));
    }
    CHECK_EQ(received.size(), size_t{0});
}

TEST(packet_builder_handles_back_to_back_packets) {
    constexpr uint64_t kSync = 0xB5u;
    const std::vector<uint8_t> payload{1, 1, 0, 0, 1, 0, 1, 0};

    std::vector<dsp::Packet> received;
    dsp::FixedLengthPacketBuilder builder{
        dsp::BitPattern{kSync, 8},
        dsp::NeverMatch{},
        dsp::FixedLength{payload.size()},
        [&received](const dsp::Packet& p) { received.push_back(p); }};

    for (int repeat = 0; repeat < 3; repeat++) {
        for (size_t i = 0; i < 16; i++) builder.execute(0);
        for (uint8_t b : sync_bits(kSync, 8)) builder.execute(b);
        for (uint8_t b : payload) builder.execute(b);
    }

    CHECK_EQ(received.size(), size_t{3});
    for (const auto& packet : received) {
        CHECK_EQ(packet.size(), payload.size());
    }
}

TEST(packet_builder_truncates_at_capacity) {
    std::vector<dsp::Packet> received;
    dsp::FixedLengthPacketBuilder builder{
        dsp::BitPattern{0xB5u, 8},
        dsp::NeverMatch{},
        dsp::FixedLength{10000},  /* never fires */
        [&received](const dsp::Packet& p) { received.push_back(p); }};
    builder.set_capacity(32);

    for (size_t i = 0; i < 16; i++) builder.execute(0);
    for (uint8_t b : sync_bits(0xB5u, 8)) builder.execute(b);
    for (size_t i = 0; i < 200; i++) builder.execute(static_cast<uint_fast8_t>(i & 1));

    CHECK_EQ(received.size(), size_t{0});
    CHECK(builder.packets_truncated() > 0);
}

TEST(packet_builder_unstuffs_and_ends_on_flag) {
    /* AIS wiring from upstream proc_ais.hpp:
     *   preamble {0b0101010101111110, 16, 1}
     *   unstuff  {0b111110, 6}
     *   end      {0b01111110, 8}
     *
     * Bits fed after the preamble: 111110101 01111110. Tracing upstream's
     * state machine by hand, the unstuff matcher fires on the sixth bit (the
     * stuffed zero) and again on the last bit of the closing flag, so the
     * packet holds 15 bits: 111111 01011111 1 with those two dropped. */
    std::vector<dsp::Packet> received;
    dsp::FlaggedPacketBuilder builder{
        dsp::BitPattern{0b0101010101111110u, 16, 1},
        dsp::BitPattern{0b111110u, 6},
        dsp::BitPattern{0b01111110u, 8},
        [&received](const dsp::Packet& p) { received.push_back(p); }};

    for (size_t i = 0; i < 20; i++) builder.execute(0);
    for (uint8_t b : sync_bits(0b0101010101111110u, 16)) builder.execute(b);

    const std::vector<uint8_t> tail{1, 1, 1, 1, 1, 0, 1, 0, 1,
                                    0, 1, 1, 1, 1, 1, 1, 0};
    for (uint8_t b : tail) builder.execute(b);

    CHECK_EQ(received.size(), size_t{1});
    if (received.size() == 1) {
        const std::vector<uint8_t> expected{1, 1, 1, 1, 1, 1, 0, 1,
                                            0, 1, 1, 1, 1, 1, 1};
        CHECK_EQ(received[0].size(), expected.size());
        int mismatches = 0;
        for (size_t i = 0; i < expected.size() && i < received[0].size(); i++) {
            if (received[0][i] != expected[i]) mismatches++;
        }
        CHECK_EQ(mismatches, 0);
    }
}

TEST(packet_field_reader) {
    dsp::Packet packet;
    for (uint8_t b : std::vector<uint8_t>{1, 0, 1, 1, 0, 0, 1, 0}) packet.add(b != 0);

    CHECK_EQ(packet.size(), size_t{8});
    CHECK_EQ(packet.read(0, 8), uint32_t{0xB2});
    CHECK_EQ(packet.read(2, 3), uint32_t{0b110});
    CHECK_EQ(packet.read(7, 1), uint32_t{0});

    /* BitRemapByteReverse: index ^ 7, so the byte comes out bit-reversed. */
    CHECK_EQ(packet.read_byte_reversed(0, 8), uint32_t{0x4D});

    const auto bytes = packet.to_bytes();
    CHECK_EQ(bytes.size(), size_t{1});
    CHECK_EQ(bytes[0], uint8_t{0xB2});

    /* Reading past the end yields zeros, matching upstream's operator[]. */
    CHECK_EQ(packet.read(6, 8), uint32_t{0b10000000});
}

/* ===========================================================================
 * BitStream
 * ===========================================================================*/

TEST(bitstream_roundtrip_several_widths) {
    const size_t widths[] = {1, 2, 3, 5, 7, 8, 11, 12, 16, 17, 24, 31, 32, 33, 48, 64};
    const dsp::BitOrder orders[] = {dsp::BitOrder::MsbFirst, dsp::BitOrder::LsbFirst};

    for (dsp::BitOrder order : orders) {
        for (size_t width : widths) {
            const uint64_t mask =
                (width >= 64) ? ~uint64_t{0} : ((uint64_t{1} << width) - 1ULL);

            Lcg lcg{static_cast<uint32_t>(width * 7919u + 13u)};
            std::vector<uint64_t> values;
            dsp::BitStreamWriter writer{order};
            for (int i = 0; i < 20; i++) {
                const uint64_t v =
                    ((static_cast<uint64_t>(lcg.next()) << 32) | lcg.next()) & mask;
                values.push_back(v);
                writer.write(v, width);
            }
            CHECK_EQ(writer.bit_count(), width * values.size());

            dsp::BitStreamReader reader{writer.bytes(), order};
            for (size_t i = 0; i < values.size(); i++) {
                CHECK_EQ(reader.read(width), values[i]);
            }
            CHECK(!reader.overrun());
        }
    }
}

TEST(bitstream_packing_order_is_visible_in_the_bytes) {
    /* Three bits, value 0b101. MSB-first fills from 0x80 down; LSB-first
     * fills from 0x01 up. */
    dsp::BitStreamWriter msb{dsp::BitOrder::MsbFirst};
    msb.write(0b101u, 3);
    CHECK_EQ(msb.bytes().size(), size_t{1});
    CHECK_EQ(msb.bytes()[0], uint8_t{0xA0});

    dsp::BitStreamWriter lsb{dsp::BitOrder::LsbFirst};
    lsb.write(0b101u, 3);
    CHECK_EQ(lsb.bytes().size(), size_t{1});
    CHECK_EQ(lsb.bytes()[0], uint8_t{0x05});
}

TEST(bitstream_write_bytes_is_byte_transparent_msb_first) {
    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    dsp::BitStreamWriter writer{dsp::BitOrder::MsbFirst};
    writer.write_bytes(data, sizeof(data));
    CHECK_EQ(writer.bytes().size(), sizeof(data));
    for (size_t i = 0; i < sizeof(data); i++) CHECK_EQ(writer.bytes()[i], data[i]);
}

TEST(bitstream_mixed_field_widths_roundtrip) {
    dsp::BitStreamWriter writer{dsp::BitOrder::MsbFirst};
    writer.write(0x2A, 6);
    writer.write(1, 1);
    writer.write(0x1234, 16);
    writer.write(0x7, 3);
    writer.align_to_byte(false);

    CHECK_EQ(writer.bit_count() % 8, size_t{0});

    dsp::BitStreamReader reader{writer.bytes(), dsp::BitOrder::MsbFirst};
    CHECK_EQ(reader.read(6), uint64_t{0x2A});
    CHECK_EQ(reader.read(1), uint64_t{1});
    CHECK_EQ(reader.read(16), uint64_t{0x1234});
    CHECK_EQ(reader.read(3), uint64_t{0x7});
}

TEST(bitstream_reader_flags_overrun_and_pads_with_zero) {
    const uint8_t data[] = {0xFF};
    dsp::BitStreamReader reader{data, sizeof(data), dsp::BitOrder::MsbFirst};
    CHECK_EQ(reader.bits_remaining(), size_t{8});

    const uint64_t v = reader.read(16);
    CHECK(reader.overrun());
    /* Eight ones then eight missing bits read as zero. */
    CHECK_EQ(v, uint64_t{0xFF00});
    CHECK_EQ(reader.bits_remaining(), size_t{0});
}

TEST(bitstream_seek_and_peek) {
    dsp::BitStreamWriter writer{dsp::BitOrder::MsbFirst};
    writer.write(0xABCD, 16);

    dsp::BitStreamReader reader{writer.bytes(), dsp::BitOrder::MsbFirst};
    CHECK_EQ(reader.peek(8), uint64_t{0xAB});
    CHECK_EQ(reader.bit_position(), size_t{0});
    CHECK_EQ(reader.read(8), uint64_t{0xAB});
    reader.seek_bits(4);
    CHECK_EQ(reader.read(8), uint64_t{0xBC});
}

/* ===========================================================================
 * Timing recovery primitives
 * ===========================================================================*/

TEST(linear_resampler_produces_the_configured_rate) {
    dsp::LinearResampler resampler;
    resampler.configure(48000.0f, 12000.0f);

    size_t emitted = 0;
    for (size_t i = 0; i < 4800; i++) {
        resampler(static_cast<float>(i), [&emitted](float) { emitted++; });
    }
    /* 4800 input samples at a 4:1 ratio. */
    CHECK_EQ(emitted, size_t{1200});
}

TEST(gardner_error_sign_follows_timing) {
    /* Three retimed samples a, b, c (b is the midpoint): the reported
     * lateness is (c - a) * b, per clock_recovery.hpp. */
    auto lateness_for = [](float a, float b, float c) {
        dsp::GardnerTimingErrorDetector detector;
        float last = 0.0f;
        auto capture = [&last](float, float l) { last = l; };
        detector(a, capture);
        detector(b, capture);
        detector(c, capture);
        return last;
    };

    CHECK_NEAR(lateness_for(-1.0f, 0.0f, 1.0f), 0.0f, 1e-6f);   /* on time  */
    CHECK_NEAR(lateness_for(-1.0f, 1.0f, 1.0f), 2.0f, 1e-6f);   /* late     */
    CHECK_NEAR(lateness_for(-1.0f, -1.0f, 1.0f), -2.0f, 1e-6f); /* early    */
}

TEST(early_late_gate_matches_upstream_mask_arithmetic) {
    /* samples_per_symbol 8 -> threshold 4, late_mask 0x0F, early_mask 0xF0. */
    const dsp::PhaseDetectorEarlyLateGate gate{8};

    auto r = gate(0xFFu);
    CHECK(r.symbol);
    CHECK_EQ(r.error, 0);

    r = gate(0x0Fu);
    CHECK(r.symbol);
    CHECK_EQ(r.error, -4);

    r = gate(0xF0u);
    CHECK(r.symbol);
    CHECK_EQ(r.error, 4);

    r = gate(0x00u);
    CHECK(!r.symbol);
    CHECK_EQ(r.error, 0);

    r = gate(0x03u);
    CHECK(!r.symbol);
    CHECK_EQ(r.error, 2);
}

TEST(gardner_gets_no_timing_from_a_pure_tone) {
    /* The detector's error is (current - previous) * midpoint. For a signal
     * rotating by a fixed angle each sample — which is what a PSK preamble
     * that repeats every two symbols becomes after pulse shaping — that
     * product is identically zero, whatever the sampling phase. This is the
     * reason PskDemod requires a pseudo-random acquisition preamble, so it is
     * worth pinning down rather than leaving as folklore. */
    dsp::ComplexGardnerTimingErrorDetector rotating;
    float worst_rotating = 0.0f;
    for (int i = 0; i < 64; i++) {
        /* Two samples per symbol, symbols advancing 90 degrees. */
        const float angle = static_cast<float>(i) * static_cast<float>(kPi) / 4.0f;
        rotating(std::polar(1.0f, angle), [&worst_rotating](dsp::cfloat, float lateness) {
            worst_rotating = std::max(worst_rotating, std::fabs(lateness));
        });
    }
    CHECK_NEAR(worst_rotating, 0.0f, 1e-4);

    /* A signal with real transitions does report an error. Sampled a quarter
     * symbol late on a -1 -> +1 edge, the midpoint has already crossed zero,
     * so the reported lateness is positive. */
    dsp::ComplexGardnerTimingErrorDetector edge;
    float last = 0.0f;
    auto capture = [&last](dsp::cfloat, float lateness) { last = lateness; };
    edge(dsp::cfloat{-1.0f, 0.0f}, capture);
    edge(dsp::cfloat{0.5f, 0.0f}, capture);
    edge(dsp::cfloat{1.0f, 0.0f}, capture);
    CHECK(last > 0.5f);
}

TEST(phase_accumulator_wraps_once_per_period) {
    dsp::PhaseAccumulator accumulator{static_cast<uint32_t>(4294967296.0 / 8.0)};
    int wraps = 0;
    for (int i = 0; i < 80; i++) {
        if (accumulator()) wraps++;
    }
    CHECK_EQ(wraps, 10);
}

TEST(clock_recovery_symbol_count_tracks_the_symbol_rate) {
    /* A sine at half the symbol rate is the NRZ waveform of alternating data;
     * the loop should deliver one symbol per symbol period. */
    constexpr float fs = 96000.0f;
    constexpr float rs = 9600.0f;

    size_t symbols = 0;
    dsp::ClockRecovery<dsp::FixedErrorFilter> recovery{
        fs, rs, dsp::FixedErrorFilter{1.0f / 16.0f}, [&symbols](float) { symbols++; }};

    const size_t sample_count = 4000;
    for (size_t i = 0; i < sample_count; i++) {
        const double t = static_cast<double>(i) / static_cast<double>(fs);
        recovery(static_cast<float>(std::sin(2.0 * kPi * (rs / 2.0) * t)));
    }

    /* 4000 samples at 10 samples per symbol. The loop nudges the phase, so
     * allow a couple of symbols of slack either way. */
    CHECK(symbols >= 396);
    CHECK(symbols <= 404);
}

/* ===========================================================================
 * Matched filter
 * ===========================================================================*/

TEST(matched_filter_taps_reproduce_upstream_ais_set) {
    /* baseband::ais::square_taps_38k4_1t_p, common/ais_baseband.hpp. */
    const float expected[4][2] = {
        {0.25000000f, 0.00000000f},
        {0.23096988f, 0.09567086f},
        {0.17677670f, 0.17677670f},
        {0.09567086f, 0.23096988f},
    };

    const auto taps = dsp::design_matched_filter_taps(2400.0, 38400.0, 4);
    CHECK_EQ(taps.size(), size_t{4});
    for (size_t i = 0; i < 4; i++) {
        CHECK_NEAR(taps[i].real(), expected[i][0], 1e-6);
        CHECK_NEAR(taps[i].imag(), expected[i][1], 1e-6);
    }
}

TEST(matched_filter_discriminates_the_two_ais_tones) {
    /* The output is |correlation with taps| - |correlation with conjugated
     * taps|, so a tone at +2400 Hz reads positive and -2400 Hz negative. */
    const auto taps = dsp::design_matched_filter_taps(2400.0, 38400.0, 4);

    auto run_tone = [&taps](double tone_hz) {
        dsp::MatchedFilter filter{taps.data(), taps.size(), 2};
        float last = 0.0f;
        for (size_t i = 0; i < 400; i++) {
            const double p = 2.0 * kPi * tone_hz * static_cast<double>(i) / 38400.0;
            if (filter.execute_once(dsp::cfloat{static_cast<float>(std::cos(p)),
                                                static_cast<float>(std::sin(p))})) {
                last = filter.get_output();
            }
        }
        return last;
    };

    const float positive = run_tone(+2400.0);
    const float negative = run_tone(-2400.0);

    CHECK(positive > 0.2f);
    CHECK(negative < -0.2f);
    CHECK_NEAR(positive, -negative, 1e-3);
}

TEST(matched_filter_emits_one_output_per_decimation_cycle) {
    const auto taps = dsp::design_matched_filter_taps(2400.0, 38400.0, 8);
    dsp::MatchedFilter filter{taps.data(), taps.size(), 4};

    int outputs = 0;
    for (size_t i = 0; i < 100; i++) {
        if (filter.execute_once(dsp::cfloat{1.0f, 0.0f})) outputs++;
    }
    CHECK_EQ(outputs, 25);
}

/* ===========================================================================
 * Pulse shaping
 * ===========================================================================*/

TEST(raised_cosine_is_nyquist) {
    constexpr size_t sps = 8;
    constexpr size_t span = 6;
    const auto taps = dsp::design_raised_cosine(0.35, sps, span);
    CHECK_EQ(taps.size(), 2 * span * sps + 1);

    const size_t centre = span * sps;
    CHECK_NEAR(taps[centre], 1.0f, 1e-6);
    for (size_t k = 1; k <= span; k++) {
        CHECK_NEAR(taps[centre + k * sps], 0.0f, 1e-5);
        CHECK_NEAR(taps[centre - k * sps], 0.0f, 1e-5);
    }
}

TEST(root_raised_cosine_is_symmetric_and_peaks_at_the_centre) {
    constexpr size_t sps = 8;
    constexpr size_t span = 6;
    const auto taps = dsp::design_root_raised_cosine(0.35, sps, span);
    CHECK_EQ(taps.size(), 2 * span * sps + 1);

    const size_t centre = span * sps;
    CHECK_NEAR(taps[centre], 1.0f, 1e-6);
    for (size_t i = 0; i < taps.size(); i++) {
        CHECK_NEAR(taps[i], taps[taps.size() - 1 - i], 1e-6);
        CHECK(std::fabs(taps[i]) <= 1.0f + 1e-6f);
    }
}

/* ===========================================================================
 * OOK
 * ===========================================================================*/

TEST(ook_slicer_recovers_a_known_on_off_pattern) {
    constexpr float sps = 8.0f;
    dsp::OokSlicer slicer;
    slicer.configure(sps);

    /* Eight samples on, eight off, four times. */
    std::vector<uint8_t> sliced;
    for (int burst = 0; burst < 4; burst++) {
        for (int i = 0; i < 8; i++) sliced.push_back(slicer.process_mag2(1.0f) ? 1 : 0);
        for (int i = 0; i < 8; i++) sliced.push_back(slicer.process_mag2(0.0f) ? 1 : 0);
    }

    for (size_t i = 0; i < sliced.size(); i++) {
        const uint8_t expected = ((i / 8) % 2 == 0) ? 1 : 0;
        CHECK_EQ(sliced[i], expected);
    }
}

TEST(ook_slicer_threshold_tracks_carrier_level) {
    dsp::OokSlicer slicer;
    slicer.configure(8.0f);

    /* Threshold settles at one eighth of the magnitude-squared. */
    for (int i = 0; i < 16; i++) slicer.process_mag2(4.0f);
    CHECK_NEAR(slicer.threshold(), 0.5f, 1e-4);

    /* A weak signal below the tracked level slices to zero. */
    CHECK(!slicer.process_mag2(0.2f));
    /* One at the tracked level does too — strictly greater than is required. */
    CHECK(slicer.process_mag2(4.0f));
}

TEST(ook_demod_recovers_bits_from_a_modulated_burst) {
    constexpr float fs = 76800.0f;
    constexpr float rs = 9600.0f;  /* 8 samples per symbol */
    constexpr uint64_t kSync = 0xB5CAu;
    constexpr size_t kSyncLength = 16;

    const auto payload = random_bits(64, 0x1BADB002u);
    const auto bits = build_frame(48, kSync, kSyncLength, payload);

    const auto signal = dsp::ook_modulate(bits, fs, rs, 1.0f);
    CHECK_EQ(signal.size(), bits.size() * 8);

    dsp::OokDemod demod;
    demod.configure(fs, rs);

    std::vector<uint8_t> recovered;
    demod.process(signal.data(), signal.size(), recovered);

    /* One symbol decision per symbol period. */
    CHECK(recovered.size() >= bits.size() - 2);

    const int errors = payload_errors(recovered, kSync, kSyncLength, payload);
    CHECK_EQ(errors, 0);
}

/* ===========================================================================
 * FSK / GFSK
 * ===========================================================================*/

TEST(fsk_demod_recovers_a_known_bit_sequence) {
    constexpr float fs = 96000.0f;
    constexpr float rs = 9600.0f;  /* 10 samples per symbol */
    constexpr float deviation = 4800.0f;
    constexpr uint64_t kSync = 0xB5CAu;
    constexpr size_t kSyncLength = 16;

    /* Several payloads, and several starting offsets within a symbol. Runs of
     * identical bits are what break a timing loop that votes on symbols
     * carrying no timing information, and whether a given payload contains a
     * long enough run is pure luck of the seed — so a single-seed test would
     * not guard the DeadbandErrorFilter fix. Seed 0x1BADB002 is the one whose
     * nine-bit run first exposed it. */
    const uint32_t seeds[] = {0x0BADF00Du, 0x1BADB002u, 0xDEADBEEFu, 0x00000005u};

    for (uint32_t seed : seeds) {
        for (size_t offset : {size_t{0}, size_t{3}, size_t{5}, size_t{7}}) {
            const auto payload = random_bits(96, seed);
            const auto bits = build_frame(64, kSync, kSyncLength, payload);

            auto signal = dsp::fsk_modulate(bits, fs, rs, deviation, 0.0f);
            signal.erase(signal.begin(), signal.begin() + static_cast<std::ptrdiff_t>(offset));

            dsp::FskDemod demod;
            demod.configure(fs, rs, deviation);

            std::vector<uint8_t> recovered;
            demod.process(signal.data(), signal.size(), recovered);

            CHECK(recovered.size() >= bits.size() - 4);
            const int errors = payload_errors(recovered, kSync, kSyncLength, payload);
            CHECK_EQ(errors, 0);
        }
    }
}

TEST(fsk_demod_handles_gaussian_shaping) {
    constexpr float fs = 96000.0f;
    constexpr float rs = 9600.0f;
    constexpr float deviation = 4800.0f;
    constexpr uint64_t kSync = 0xB5CAu;
    constexpr size_t kSyncLength = 16;

    const auto payload = random_bits(96, 0xFEEDBEEFu);
    const auto bits = build_frame(64, kSync, kSyncLength, payload);

    auto signal = dsp::fsk_modulate(bits, fs, rs, deviation, 0.5f);  /* GFSK, BT=0.5 */
    signal.erase(signal.begin(), signal.begin() + 3);

    dsp::FskDemod demod;
    demod.configure(fs, rs, deviation);

    std::vector<uint8_t> recovered;
    demod.process(signal.data(), signal.size(), recovered);

    const int errors = payload_errors(recovered, kSync, kSyncLength, payload);
    CHECK_EQ(errors, 0);
}

TEST(fsk_demod_invert_flips_the_bit_sense) {
    constexpr float fs = 96000.0f;
    constexpr float rs = 9600.0f;
    constexpr float deviation = 4800.0f;

    const std::vector<uint8_t> bits = build_frame(64, 0xB5CAu, 16, random_bits(32, 7u));
    auto signal = dsp::fsk_modulate(bits, fs, rs, deviation, 0.0f);
    signal.erase(signal.begin(), signal.begin() + 3);

    std::vector<uint8_t> normal;
    std::vector<uint8_t> inverted;

    dsp::FskDemod a;
    a.configure(fs, rs, deviation);
    a.process(signal.data(), signal.size(), normal);

    dsp::FskDemod b;
    b.configure(fs, rs, deviation);
    b.set_invert(true);
    b.process(signal.data(), signal.size(), inverted);

    CHECK_EQ(normal.size(), inverted.size());
    int differing = 0;
    for (size_t i = 0; i < normal.size() && i < inverted.size(); i++) {
        if (normal[i] != (inverted[i] ^ 1)) differing++;
    }
    CHECK_EQ(differing, 0);
}

/* ===========================================================================
 * AFSK
 * ===========================================================================*/

TEST(afsk_bell202_recovers_a_known_bit_sequence) {
    constexpr float fs = 24000.0f;
    constexpr float mark = 1200.0f;
    constexpr float space = 2200.0f;
    constexpr float baud = 1200.0f;
    constexpr uint64_t kSync = 0xB5CAu;
    constexpr size_t kSyncLength = 16;

    const auto payload = random_bits(64, 0xA11CE5u);
    const auto bits = build_frame(64, kSync, kSyncLength, payload);

    const auto audio = dsp::afsk_modulate(bits, fs, mark, space, baud, 0.8f);
    CHECK_EQ(audio.size(), bits.size() * 20);

    dsp::AfskDemod demod;
    demod.configure(fs, dsp::AfskDemod::Standard::Bell202);
    CHECK_NEAR(demod.samples_per_bit(), 20.0f, 1e-6);

    std::vector<uint8_t> recovered;
    demod.process_audio(audio.data(), audio.size(), recovered);

    CHECK(recovered.size() >= bits.size() - 4);
    const int errors = payload_errors(recovered, kSync, kSyncLength, payload);
    CHECK_EQ(errors, 0);
}

TEST(afsk_bell103_recovers_a_known_bit_sequence) {
    constexpr float fs = 24000.0f;
    constexpr uint64_t kSync = 0xB5CAu;
    constexpr size_t kSyncLength = 16;

    const auto payload = random_bits(48, 0x1701D5u);
    const auto bits = build_frame(48, kSync, kSyncLength, payload);

    /* Bell 103 originate: mark 1270 Hz, space 1070 Hz, 300 baud. */
    const auto audio = dsp::afsk_modulate(bits, fs, 1270.0f, 1070.0f, 300.0f, 0.8f);

    dsp::AfskDemod demod;
    demod.configure(fs, dsp::AfskDemod::Standard::Bell103Originate);
    CHECK_NEAR(demod.mark_hz(), 1270.0f, 1e-6);
    CHECK_NEAR(demod.space_hz(), 1070.0f, 1e-6);

    std::vector<uint8_t> recovered;
    demod.process_audio(audio.data(), audio.size(), recovered);

    const int errors = payload_errors(recovered, kSync, kSyncLength, payload);
    CHECK_EQ(errors, 0);
}

TEST(afsk_correlator_delay_separates_the_tones) {
    /* The chosen delay must actually discriminate: cos(2*pi*f*D/fs) has to
     * differ substantially between mark and space. Upstream's fixed
     * samples_per_bit/2 fails this for Bell 103. */
    auto separation = [](float fs, float mark, float space, float baud) {
        dsp::AfskDemod demod;
        demod.configure(fs, mark, space, baud);
        const double d = static_cast<double>(demod.correlator_delay());
        const double cm = std::cos(2.0 * kPi * mark * d / fs);
        const double cs = std::cos(2.0 * kPi * space * d / fs);
        return std::fabs(cm - cs);
    };

    CHECK(separation(24000.0f, 1200.0f, 2200.0f, 1200.0f) > 1.5);
    CHECK(separation(24000.0f, 1270.0f, 1070.0f, 300.0f) > 1.5);
    CHECK(separation(24000.0f, 2225.0f, 2025.0f, 300.0f) > 1.5);
}

TEST(afsk_over_fm_baseband) {
    /* An AFSK tone pair impressed on an FM carrier, the way it arrives from
     * the air, must come back out through the complex-input path. */
    constexpr float fs = 24000.0f;
    constexpr uint64_t kSync = 0xB5CAu;
    constexpr size_t kSyncLength = 16;

    const auto payload = random_bits(48, 0x5A5A5A5Au);
    const auto bits = build_frame(64, kSync, kSyncLength, payload);
    const auto audio = dsp::afsk_modulate(bits, fs, 1200.0f, 2200.0f, 1200.0f, 1.0f);

    /* FM-modulate the audio with 3 kHz deviation. */
    std::vector<dsp::cfloat> baseband(audio.size());
    double phase = 0.0;
    for (size_t i = 0; i < audio.size(); i++) {
        baseband[i] = dsp::cfloat{static_cast<float>(std::cos(phase)),
                                  static_cast<float>(std::sin(phase))};
        phase += 2.0 * kPi * 3000.0 * static_cast<double>(audio[i]) / static_cast<double>(fs);
    }

    dsp::AfskDemod demod;
    demod.configure(fs, dsp::AfskDemod::Standard::Bell202);

    std::vector<uint8_t> recovered;
    demod.process(baseband.data(), baseband.size(), recovered);

    const int errors = payload_errors(recovered, kSync, kSyncLength, payload);
    CHECK_EQ(errors, 0);
}

/* ===========================================================================
 * PSK
 * ===========================================================================*/

TEST(psk_demod_recovers_differential_bpsk) {
    constexpr float fs = 48000.0f;
    constexpr float rs = 4800.0f;  /* 10 samples per symbol */
    constexpr uint64_t kSync = 0xB5CAu;
    constexpr size_t kSyncLength = 16;

    const auto payload = random_bits(64, 0xBEEF01u);
    const auto bits = build_frame(128, kSync, kSyncLength, payload, /*pseudo_random_preamble=*/true);

    auto signal = dsp::psk_modulate(bits, fs, rs, dsp::PskOrder::Bpsk, true);

    /* A fixed carrier phase error the loop has to pull out, plus the same
     * three-sample timing offset the FSK test uses. */
    const dsp::cfloat rotation = std::polar(1.0f, 0.7f);
    for (auto& s : signal) s *= rotation;
    signal.erase(signal.begin(), signal.begin() + 3);

    dsp::PskDemod demod;
    demod.configure(fs, rs, dsp::PskOrder::Bpsk);
    demod.set_differential(true);

    std::vector<uint8_t> recovered;
    demod.process(signal.data(), signal.size(), recovered);

    CHECK(recovered.size() >= bits.size() - 8);
    const int errors = payload_errors(recovered, kSync, kSyncLength, payload);
    CHECK_EQ(errors, 0);
}

TEST(psk_demod_tracks_a_carrier_frequency_offset) {
    constexpr float fs = 48000.0f;
    constexpr float rs = 4800.0f;
    constexpr float offset_hz = 40.0f;
    constexpr uint64_t kSync = 0xB5CAu;
    constexpr size_t kSyncLength = 16;

    const auto payload = random_bits(64, 0x77777u);
    const auto bits = build_frame(192, kSync, kSyncLength, payload, /*pseudo_random_preamble=*/true);

    auto signal = dsp::psk_modulate(bits, fs, rs, dsp::PskOrder::Bpsk, true);
    for (size_t i = 0; i < signal.size(); i++) {
        const double p = 2.0 * kPi * offset_hz * static_cast<double>(i) / fs;
        signal[i] *= dsp::cfloat{static_cast<float>(std::cos(p)),
                                 static_cast<float>(std::sin(p))};
    }
    signal.erase(signal.begin(), signal.begin() + 3);

    dsp::PskDemod demod;
    demod.configure(fs, rs, dsp::PskOrder::Bpsk);
    demod.set_differential(true);

    std::vector<uint8_t> recovered;
    demod.process(signal.data(), signal.size(), recovered);

    const int errors = payload_errors(recovered, kSync, kSyncLength, payload);
    CHECK_EQ(errors, 0);

    /* The loop's integrator should have settled near the true offset. */
    CHECK_NEAR(demod.carrier_frequency_offset_hz(), offset_hz, 8.0);
}

TEST(psk_demod_recovers_differential_qpsk) {
    constexpr float fs = 48000.0f;
    constexpr float rs = 4800.0f;
    constexpr uint64_t kSync = 0xB5CAu;
    constexpr size_t kSyncLength = 16;

    const auto payload = random_bits(64, 0x2468ACEu);
    const auto bits = build_frame(128, kSync, kSyncLength, payload, /*pseudo_random_preamble=*/true);

    auto signal = dsp::psk_modulate(bits, fs, rs, dsp::PskOrder::Qpsk, true);
    const dsp::cfloat rotation = std::polar(1.0f, 0.4f);
    for (auto& s : signal) s *= rotation;
    signal.erase(signal.begin(), signal.begin() + 3);

    dsp::PskDemod demod;
    demod.configure(fs, rs, dsp::PskOrder::Qpsk);
    demod.set_differential(true);

    std::vector<uint8_t> recovered;
    demod.process(signal.data(), signal.size(), recovered);

    const int errors = payload_errors(recovered, kSync, kSyncLength, payload);
    CHECK_EQ(errors, 0);
}

TEST(psk_bpsk_non_differential_is_correct_up_to_polarity) {
    /* Without differential coding a PSK receiver cannot resolve the 180
     * degree ambiguity, so the bits are right or uniformly inverted. */
    constexpr float fs = 48000.0f;
    constexpr float rs = 4800.0f;
    constexpr uint64_t kSync = 0xB5CAu;
    constexpr size_t kSyncLength = 16;

    const auto payload = random_bits(64, 0x13579u);
    const auto bits = build_frame(128, kSync, kSyncLength, payload, /*pseudo_random_preamble=*/true);

    auto signal = dsp::psk_modulate(bits, fs, rs, dsp::PskOrder::Bpsk, false);
    signal.erase(signal.begin(), signal.begin() + 3);

    dsp::PskDemod demod;
    demod.configure(fs, rs, dsp::PskOrder::Bpsk);

    std::vector<uint8_t> recovered;
    demod.process(signal.data(), signal.size(), recovered);

    std::vector<uint8_t> flipped(recovered.size());
    for (size_t i = 0; i < recovered.size(); i++) {
        flipped[i] = static_cast<uint8_t>(recovered[i] ^ 1);
    }

    const int direct = payload_errors(recovered, kSync, kSyncLength, payload);
    const int inverse = payload_errors(flipped, kSync, kSyncLength, payload);
    CHECK((direct == 0) || (inverse == 0));
}
