/*
 * mayhem-b200 — GPS L1 C/A simulator tests.
 *
 * Everything here is checked against something outside the code under test:
 *
 *  - the C/A Gold codes against the published IS-GPS-200 Table 3-Ia "first 10
 *    chips (octal)" values (PRN 1 = 1440 and PRN 2 = 1620 were confirmed against
 *    two independent public sources; the rest are the standard tabulated values,
 *    reproduced by the same verified generator) and against the Gold-code
 *    balance property (exactly 512 ones per 1023-chip period);
 *  - the nav-message parity against an INDEPENDENT encoder written straight from
 *    the IS-GPS-200 Table 20-XIV parity equations (source-bit form), which is a
 *    different algorithm from gps::compute_checksum's bit-mask form, plus a set
 *    of exact hand-computed word values;
 *  - the LNAV framing (TLM preamble 0x8B, HOW TOW-count and subframe IDs, the
 *    transmission week in subframe 1 word 3, per-word parity chaining) against
 *    the IS-GPS-200 layout;
 *  - the per-satellite BPSK modulation by generating a burst and correlating it
 *    back against a local C/A replica: the right PRN peaks, a wrong PRN does not,
 *    and the correlation sign recovers the nav-data bit.
 *
 * NOT covered: radiation. No radio is attached, so nothing here proves an actual
 * over-the-air GPS signal, and this port does not model orbit geometry, so it is
 * not a receiver position fix (see ui_gps_sim.hpp).
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_gps_sim.hpp"

#include "app_registry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace {

/* Pack the first 10 chips MSB-first into an integer, the IS-GPS-200 octal
 * convention (first chip is the leading octal digit's low bit). */
uint32_t first10(int prn) {
    const auto ca = gps::ca_code(prn);
    uint32_t v = 0;
    for (int i = 0; i < 10; i++) v = (v << 1) | static_cast<uint32_t>(ca[i]);
    return v;
}

int ones_in_code(int prn) {
    const auto ca = gps::ca_code(prn);
    int n = 0;
    for (int i = 0; i < gps::CA_SEQ_LEN; i++) n += ca[i];
    return n;
}

/* An INDEPENDENT parity encoder, written straight from the IS-GPS-200
 * Table 20-XIV equations over the source data bits d1..d24 (not the bit-mask
 * form gps::compute_checksum uses). `dsrc[1..24]` are the source bits, D29s/D30s
 * the previous word's last two bits. Returns the 30-bit transmitted word. */
uint32_t ref_encode(const int dsrc[25], uint32_t D29s, uint32_t D30s) {
    static const int eq[6][15] = {
        {1, 2, 3, 5, 6, 10, 11, 12, 13, 14, 17, 18, 20, 23, 0},   /* D25 */
        {2, 3, 4, 6, 7, 11, 12, 13, 14, 15, 18, 19, 21, 24, 0},   /* D26 */
        {1, 3, 4, 5, 7, 8, 12, 13, 14, 15, 16, 19, 20, 22, 0},    /* D27 */
        {2, 4, 5, 6, 8, 9, 13, 14, 15, 16, 17, 20, 21, 23, 0},    /* D28 */
        {1, 3, 5, 6, 7, 9, 10, 14, 15, 16, 17, 18, 21, 22, 24},   /* D29 */
        {3, 5, 6, 8, 9, 10, 11, 13, 15, 19, 22, 23, 24, 0, 0}};   /* D30 */
    static const uint32_t seed_is_D29[6] = {1, 0, 1, 0, 0, 1};

    /* Transmitted data bits D_n = d_n XOR D30* (n = 1..24). */
    uint32_t Dt[31] = {0};
    for (int n = 1; n <= 24; n++) Dt[n] = static_cast<uint32_t>(dsrc[n]) ^ D30s;

    for (int k = 0; k < 6; k++) {
        uint32_t p = seed_is_D29[k] ? D29s : D30s;
        for (int t = 0; t < 15; t++) {
            const int n = eq[k][t];
            if (n == 0) break;
            p ^= static_cast<uint32_t>(dsrc[n]);
        }
        Dt[25 + k] = p;
    }

    uint32_t w = 0;
    for (int n = 1; n <= 30; n++) w = (w << 1) | Dt[n];
    return w;
}

/* Build the gps::compute_checksum `source` argument from source data bits and
 * the previous word's last two bits. */
uint32_t make_source(const int dsrc[25], uint32_t D29s, uint32_t D30s) {
    uint32_t src = 0;
    for (int n = 1; n <= 24; n++)
        src |= static_cast<uint32_t>(dsrc[n]) << (30 - n);  /* d1->bit29 .. d24->bit6 */
    src |= D29s << 31;
    src |= D30s << 30;
    return src;
}

/* Extract the 24 data bits (bits 29..6) of a 30-bit word as an integer. */
uint32_t word_data24(uint32_t word) { return (word >> 6) & 0xFFFFFFu; }

}  // namespace

/* =========================================================================
 * Registration
 * =========================================================================*/

TEST(gpssim_app_is_registered) {
    const auto* entry = app::AppRegistry::instance().by_id("gpssim");
    CHECK(entry != nullptr);
    if (entry != nullptr) {
        CHECK_STR_EQ(entry->display_name, "GPS Sim");
        CHECK(entry->category == app::Category::Transmit);
        CHECK(!entry->hardware_limited);
        CHECK(static_cast<bool>(entry->factory));
    }
}

/* =========================================================================
 * C/A Gold code
 * =========================================================================*/

TEST(gpssim_ca_first10_chips_octal) {
    /* IS-GPS-200 Table 3-Ia "First 10 Chips (Octal)". PRN 1/2 confirmed against
     * two independent public sources; the rest are the standard tabulated
     * values. Octal literals for readability. */
    CHECK_EQ(first10(1), 01440u);
    CHECK_EQ(first10(2), 01620u);
    CHECK_EQ(first10(3), 01710u);
    CHECK_EQ(first10(4), 01744u);
    CHECK_EQ(first10(5), 01133u);
    CHECK_EQ(first10(6), 01455u);
    CHECK_EQ(first10(7), 01131u);
    CHECK_EQ(first10(8), 01454u);
    CHECK_EQ(first10(9), 01626u);
    CHECK_EQ(first10(10), 01504u);
    CHECK_EQ(first10(19), 01633u);
    CHECK_EQ(first10(25), 01743u);
    CHECK_EQ(first10(32), 01712u);
}

TEST(gpssim_ca_balance_property) {
    /* A valid 1023-chip Gold code is balanced: exactly 512 ones, 511 zeros. */
    for (int prn = 1; prn <= gps::MAX_PRN; prn++)
        CHECK_EQ(ones_in_code(prn), 512);
}

TEST(gpssim_ca_codes_are_distinct_and_period_correct) {
    const auto a = gps::ca_code(1);
    const auto b = gps::ca_code(2);
    int diff = 0;
    for (int i = 0; i < gps::CA_SEQ_LEN; i++)
        if (a[i] != b[i]) diff++;
    /* Different PRNs must differ substantially (Gold-code cross-correlation is
     * bounded; codes are far from identical). */
    CHECK(diff > 400);

    /* Chips are strictly 0/1. */
    for (int i = 0; i < gps::CA_SEQ_LEN; i++) {
        CHECK(a[i] == 0 || a[i] == 1);
    }
}

TEST(gpssim_ca_autocorrelation_peaks_at_zero_lag) {
    const auto ca = gps::ca_code(5);
    auto pm = [&](int i) { return ca[((i % gps::CA_SEQ_LEN) + gps::CA_SEQ_LEN) %
                                     gps::CA_SEQ_LEN]
                                      ? 1
                                      : -1; };
    /* Zero-lag autocorrelation is the full length (1023). */
    int zero_lag = 0;
    for (int i = 0; i < gps::CA_SEQ_LEN; i++) zero_lag += pm(i) * pm(i);
    CHECK_EQ(zero_lag, gps::CA_SEQ_LEN);

    /* Off-peak autocorrelation is one of the bounded Gold values
     * {-1, -65, 63} (magnitude well under the peak). */
    int worst = 0;
    for (int lag = 1; lag < gps::CA_SEQ_LEN; lag++) {
        int acc = 0;
        for (int i = 0; i < gps::CA_SEQ_LEN; i++) acc += pm(i) * pm(i + lag);
        if (std::abs(acc) > worst) worst = std::abs(acc);
    }
    CHECK(worst <= 65);
}

/* =========================================================================
 * Nav-message parity
 * =========================================================================*/

TEST(gpssim_parity_exact_hand_vectors) {
    /* Exact values computed from the reference algorithm (see the session
     * transcript): they pin the implementation to specific bit patterns. */
    CHECK_EQ(gps::compute_checksum(0u, false), 0x00000000u);

    /* TLM word: preamble 0x8B in the top 8 data bits, prev = 0, nib = false. */
    const uint32_t w_tlm = gps::compute_checksum(0x8B0000u << 6, false);
    CHECK_EQ(w_tlm, 0x22C00012u);
    CHECK_EQ((w_tlm >> 22) & 0xFFu, 0x8Bu);  /* preamble recovered */

    /* HOW word: TOW-count 100, subframe id 1, prev = w_tlm, nib = true. */
    const uint32_t data_how = ((100u & 0x1FFFFu) << 7) | ((1u & 0x7u) << 2);
    const uint32_t src_how = (data_how << 6) | ((w_tlm << 30) & 0xC0000000u);
    const uint32_t w_how = gps::compute_checksum(src_how, true);
    CHECK_EQ(w_how, 0x000C8134u);
}

TEST(gpssim_parity_matches_isgps200_table_20_xiv) {
    /* gps::compute_checksum (bit-mask form) must equal the independent
     * Table 20-XIV encoder (source-bit form) for every D29*,D30* combination
     * over a large pseudo-random set of data words. */
    uint32_t lfsr = 0xC0FFEEu;
    auto next_bit = [&]() -> int {
        lfsr = (lfsr >> 1) ^
               (static_cast<uint32_t>(-static_cast<int32_t>(lfsr & 1u)) & 0xB4000000u);
        return static_cast<int>(lfsr & 1u);
    };

    int mismatches = 0;
    for (int iter = 0; iter < 20000; iter++) {
        int dsrc[25];
        for (int n = 1; n <= 24; n++) dsrc[n] = next_bit();
        const uint32_t D29s = static_cast<uint32_t>(next_bit());
        const uint32_t D30s = static_cast<uint32_t>(next_bit());

        const uint32_t a = gps::compute_checksum(make_source(dsrc, D29s, D30s), false);
        const uint32_t b = ref_encode(dsrc, D29s, D30s);
        if (a != b) mismatches++;
    }
    CHECK_EQ(mismatches, 0);
}

TEST(gpssim_parity_output_passes_receiver_check) {
    /* Every compute_checksum output must satisfy gps::parity_ok (the receiver
     * side), across all previous-word bit combinations. */
    uint32_t lfsr = 0x12345u;
    auto next_bit = [&]() -> int {
        lfsr = (lfsr >> 1) ^
               (static_cast<uint32_t>(-static_cast<int32_t>(lfsr & 1u)) & 0xB4000000u);
        return static_cast<int>(lfsr & 1u);
    };

    int bad = 0;
    for (int iter = 0; iter < 20000; iter++) {
        int dsrc[25];
        for (int n = 1; n <= 24; n++) dsrc[n] = next_bit();
        const uint32_t D29s = static_cast<uint32_t>(next_bit());
        const uint32_t D30s = static_cast<uint32_t>(next_bit());

        const uint32_t w = gps::compute_checksum(make_source(dsrc, D29s, D30s), false);
        if (!gps::parity_ok(w, D29s, D30s)) bad++;
    }
    CHECK_EQ(bad, 0);
}

TEST(gpssim_parity_check_rejects_corruption) {
    int dsrc[25];
    for (int n = 1; n <= 24; n++) dsrc[n] = (n * 7) & 1;
    const uint32_t D29s = 1, D30s = 0;
    const uint32_t w = gps::compute_checksum(make_source(dsrc, D29s, D30s), false);
    CHECK(gps::parity_ok(w, D29s, D30s));

    /* Flip any single data bit: parity must fail. */
    for (int b = 6; b < 30; b++) {
        const uint32_t corrupt = w ^ (1u << b);
        CHECK(!gps::parity_ok(corrupt, D29s, D30s));
    }
}

/* =========================================================================
 * LNAV framing
 * =========================================================================*/

TEST(gpssim_frame_tlm_how_and_week) {
    uint32_t dwrd[gps::N_WORDS_FRAME];
    const uint16_t week = 150;
    const uint32_t tow6 = 100;
    gps::build_frame(dwrd, week, tow6, 0);

    for (int isbf = 0; isbf < gps::N_SBF; isbf++) {
        const uint32_t tlm = dwrd[isbf * gps::N_DWRD_SBF + 0];
        const uint32_t how = dwrd[isbf * gps::N_DWRD_SBF + 1];

        /* TLM preamble is the top 8 of the 24 data bits. */
        CHECK_EQ((word_data24(tlm) >> 16) & 0xFFu, 0x8Bu);

        /* HOW subframe ID (d20..d22 -> bits 4..2 of the data field). */
        const uint32_t sfid = (word_data24(how) >> 2) & 0x7u;
        CHECK_EQ(sfid, static_cast<uint32_t>(isbf + 1));

        /* HOW TOW-count increments by 1 each subframe. */
        const uint32_t tow_field = (word_data24(how) >> 7) & 0x1FFFFu;
        CHECK_EQ(tow_field, (tow6 + static_cast<uint32_t>(isbf) + 1) & 0x1FFFFu);
    }

    /* Transmission week appears in subframe 1 word 3 (top 10 data bits). */
    const uint32_t w3 = dwrd[0 * gps::N_DWRD_SBF + 2];
    CHECK_EQ((word_data24(w3) >> 14) & 0x3FFu, static_cast<uint32_t>(week));
}

TEST(gpssim_frame_all_words_have_valid_parity) {
    uint32_t dwrd[gps::N_WORDS_FRAME];
    const uint32_t prev0 = 0x2AAAAAAAu;  /* arbitrary chaining seed */
    gps::build_frame(dwrd, 280, 4096, prev0);

    uint32_t prev = prev0;
    for (int i = 0; i < gps::N_WORDS_FRAME; i++) {
        const uint32_t D29s = (prev >> 1) & 0x1u;
        const uint32_t D30s = prev & 0x1u;
        CHECK(gps::parity_ok(dwrd[i], D29s, D30s));
        prev = dwrd[i];
    }
}

TEST(gpssim_frame_chaining_across_frames) {
    /* The value build_frame returns must equal the last word it wrote, and feed
     * the next frame's parity chain cleanly. */
    uint32_t f1[gps::N_WORDS_FRAME];
    const uint32_t last1 = gps::build_frame(f1, 280, 0, 0);
    CHECK_EQ(last1, f1[gps::N_WORDS_FRAME - 1]);

    uint32_t f2[gps::N_WORDS_FRAME];
    const uint32_t last2 = gps::build_frame(f2, 280, gps::N_SBF, last1);

    /* First word of frame 2 chains off frame 1's last word. */
    const uint32_t D29s = (last1 >> 1) & 0x1u;
    const uint32_t D30s = last1 & 0x1u;
    CHECK(gps::parity_ok(f2[0], D29s, D30s));
    CHECK_EQ(last2, f2[gps::N_WORDS_FRAME - 1]);
}

TEST(gpssim_frame_tow_and_week_boundaries) {
    /* Boundary TOW/week values must still frame and pass parity. */
    struct Case { uint16_t week; uint32_t tow6; };
    const Case cases[] = {{0, 0}, {1023, 100794}, {512, 0x1FFFF}};
    for (const auto& c : cases) {
        uint32_t dwrd[gps::N_WORDS_FRAME];
        gps::build_frame(dwrd, c.week, c.tow6, 0);
        uint32_t prev = 0;
        for (int i = 0; i < gps::N_WORDS_FRAME; i++) {
            CHECK(gps::parity_ok(dwrd[i], (prev >> 1) & 1u, prev & 1u));
            prev = dwrd[i];
        }
    }
}

/* =========================================================================
 * Per-satellite BPSK modulation (round trip)
 * =========================================================================*/

TEST(gpssim_modulation_correlates_to_correct_prn) {
    /* Generate one PRN's baseband at 2 samples/chip (clean alignment), then
     * correlate one 1 ms code period against local replicas. The transmitted
     * PRN peaks; a different PRN does not; and the correlation sign matches the
     * first nav-data bit. */
    const int prn = 5;
    const double fs = 2.0 * gps::CA_CHIP_RATE;  /* 2.046 MHz, 2 samples/chip */

    std::vector<gps::SatConfig> sats;
    gps::SatConfig c;
    c.prn = prn;
    c.power = 1.0f;
    c.doppler_hz = 0.0;
    c.code_phase_chips = 0.0;
    sats.push_back(c);

    gps::SignalGenerator gen;
    gen.configure(fs, 280, 0, sats);

    const size_t spc = 2;                          /* samples per chip */
    const size_t period = gps::CA_SEQ_LEN * spc;   /* 2046 samples = 1 ms */
    std::vector<std::complex<float>> sig(period);
    gen.generate(sig.data(), sig.size());

    /* First nav-data bit sign: build_frame word 0 bit 0 (first transmitted bit
     * of the TLM word) maps to +/-1 via bit*2-1. */
    uint32_t dwrd[gps::N_WORDS_FRAME];
    gps::build_frame(dwrd, 280, 0, 0);
    const int first_bit = static_cast<int>((dwrd[0] >> 29) & 1u);
    const float expected_sign = first_bit ? 1.0f : -1.0f;

    auto correlate = [&](int test_prn) -> float {
        const auto ca = gps::ca_code(test_prn);
        float acc = 0.0f;
        for (size_t s = 0; s < period; s++) {
            const int chip = ca[(s / spc) % gps::CA_SEQ_LEN];
            const float code_pm = chip ? 1.0f : -1.0f;
            acc += sig[s].real() * code_pm;
        }
        return acc / static_cast<float>(period);
    };

    const float corr_right = correlate(prn);
    const float corr_wrong = correlate((prn == 5) ? 10 : 5);

    /* The matching replica correlates near +/-1 (full despread), the wrong PRN
     * near zero (bounded Gold cross-correlation). */
    CHECK(std::fabs(corr_right) > 0.8f);
    CHECK(std::fabs(corr_wrong) < 0.2f);
    /* Sign recovers the nav-data bit. */
    CHECK(std::fabs(corr_right - expected_sign) < 0.2f);
}

TEST(gpssim_modulation_multi_sat_output_is_bounded) {
    /* Summed multi-satellite output stays within unit scale (normalised). */
    std::vector<gps::SatConfig> sats;
    for (int prn = 1; prn <= 12; prn++) {
        gps::SatConfig c;
        c.prn = prn;
        sats.push_back(c);
    }
    gps::SignalGenerator gen;
    gen.configure(2600000.0, 280, 0, sats);
    CHECK_EQ(gen.satellite_count(), size_t{12});

    std::vector<std::complex<float>> sig(20000);
    gen.generate(sig.data(), sig.size());

    float peak = 0.0f;
    for (const auto& s : sig) peak = std::max(peak, std::abs(s));
    CHECK(peak <= 1.001f);
    CHECK(peak > 0.0f);  /* it is actually producing signal */
}

TEST(gpssim_generator_ignores_invalid_prn) {
    std::vector<gps::SatConfig> sats;
    gps::SatConfig a;
    a.prn = 0;  /* invalid */
    gps::SatConfig b;
    b.prn = 33;  /* invalid */
    gps::SatConfig c;
    c.prn = 7;  /* valid */
    sats.push_back(a);
    sats.push_back(b);
    sats.push_back(c);

    gps::SignalGenerator gen;
    gen.configure(2600000.0, 280, 0, sats);
    CHECK_EQ(gen.satellite_count(), size_t{1});
}
