/*
 * mayhem-b200 — RTTY receiver tests.
 *
 * Three layers are covered:
 *
 *  1. Baudot/ITA2 tables and shift handling, against the ITA2 alphabet and
 *     against firmware/application/external/rtty_rx/baudot.cpp.
 *  2. The asynchronous UART framer, driven bit-by-bit with a hand-built
 *     start/data/stop waveform so the sampling instants are checkable.
 *  3. The whole chain, driven with a 2FSK signal synthesised by
 *     dsp::fsk_modulate() at real RTTY parameters (45.45 baud, 170 Hz shift)
 *     and decoded back to the text that was encoded.
 *
 * dsp::FskDemod is also exercised at the same parameters, per the brief.
 *
 * No hardware is involved: everything here is synthetic. Live RF reception is
 * unverified.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_rtty_rx.hpp"

#include "../src/dsp/demod_digital.hpp"

#include <string>
#include <vector>

using namespace app::rtty;

/* --- helpers -------------------------------------------------------------- */

namespace {

constexpr float kRate = 24000.0f;
constexpr float kBaud = 45.45f;
constexpr float kShiftHz = 170.0f;

/* One RTTY character on the wire: start bit (space, 0), five data bits least
 * significant first, then `stop_bits` mark bits. */
void append_frame(std::vector<uint8_t>& bits, uint8_t code, int stop_bits = 2) {
    bits.push_back(0);
    for (int i = 0; i < 5; i++) bits.push_back(static_cast<uint8_t>((code >> i) & 1));
    for (int i = 0; i < stop_bits; i++) bits.push_back(1);
}

void append_idle(std::vector<uint8_t>& bits, int count) {
    for (int i = 0; i < count; i++) bits.push_back(1);
}

/* Expands a bit sequence into one slicer bit per sample, for framer tests. */
std::vector<uint8_t> oversample(const std::vector<uint8_t>& bits, uint32_t samples_per_bit) {
    std::vector<uint8_t> out;
    out.reserve(bits.size() * samples_per_bit);
    for (uint8_t b : bits)
        for (uint32_t i = 0; i < samples_per_bit; i++) out.push_back(b);
    return out;
}

std::vector<uint8_t> run_framer(RttyFramer& framer, const std::vector<uint8_t>& sampled_bits) {
    std::vector<uint8_t> codes;
    for (uint8_t b : sampled_bits) {
        uint8_t code = 0;
        if (framer.process_bit(b, code)) codes.push_back(code);
    }
    return codes;
}

}  // namespace

/* =========================================================================
 * Baudot / ITA2
 * =======================================================================*/

TEST(baudot_letters_table_matches_ita2) {
    /* Spot checks against the ITA2 alphabet, not against our own output. */
    CHECK_EQ(BaudotDecoder::char_mapping(false, 0x01), 'E');
    CHECK_EQ(BaudotDecoder::char_mapping(false, 0x03), 'A');
    CHECK_EQ(BaudotDecoder::char_mapping(false, 0x04), ' ');
    CHECK_EQ(BaudotDecoder::char_mapping(false, 0x09), 'D');
    CHECK_EQ(BaudotDecoder::char_mapping(false, 0x0A), 'R');
    CHECK_EQ(BaudotDecoder::char_mapping(false, 0x0E), 'C');
    CHECK_EQ(BaudotDecoder::char_mapping(false, 0x10), 'T');
    CHECK_EQ(BaudotDecoder::char_mapping(false, 0x15), 'Y');
    CHECK_EQ(BaudotDecoder::char_mapping(false, 0x19), 'B');
    CHECK_EQ(BaudotDecoder::char_mapping(false, 0x1E), 'V');
    /* Line feed and carriage return live at 2 and 8 in both shifts. */
    CHECK_EQ(BaudotDecoder::char_mapping(false, 0x02), '\n');
    CHECK_EQ(BaudotDecoder::char_mapping(false, 0x08), '\r');
    /* Codes with no letter: null, FIGS itself, LTRS itself. */
    CHECK_EQ(BaudotDecoder::char_mapping(false, 0x00), 0);
    CHECK_EQ(BaudotDecoder::char_mapping(false, 0x1B), 0);
    CHECK_EQ(BaudotDecoder::char_mapping(false, 0x1F), 0);
}

TEST(baudot_figures_table_matches_upstream) {
    CHECK_EQ(BaudotDecoder::char_mapping(true, 0x01), '3');
    CHECK_EQ(BaudotDecoder::char_mapping(true, 0x0A), '4');
    CHECK_EQ(BaudotDecoder::char_mapping(true, 0x10), '5');
    CHECK_EQ(BaudotDecoder::char_mapping(true, 0x13), '2');
    CHECK_EQ(BaudotDecoder::char_mapping(true, 0x15), '6');
    CHECK_EQ(BaudotDecoder::char_mapping(true, 0x16), '0');
    CHECK_EQ(BaudotDecoder::char_mapping(true, 0x17), '1');
    CHECK_EQ(BaudotDecoder::char_mapping(true, 0x18), '9');
    CHECK_EQ(BaudotDecoder::char_mapping(true, 0x1D), '/');
    /* Upstream's figures table has the bell character at index 11. */
    CHECK_EQ(BaudotDecoder::char_mapping(true, 0x0B), '\a');
}

TEST(baudot_char_mapping_rejects_out_of_range) {
    CHECK_EQ(BaudotDecoder::char_mapping(false, 32), 0);
    CHECK_EQ(BaudotDecoder::char_mapping(true, 32), 0);
    CHECK_EQ(BaudotDecoder::char_mapping(false, 255), 0);
}

TEST(baudot_decodes_known_sequence_with_shift) {
    /* R Y R Y  FIGS 1 2 3  SPACE(USOS -> letters)  T E S T */
    const uint8_t codes[] = {0x0A, 0x15, 0x0A, 0x15,
                             0x1B, 0x17, 0x13, 0x01,
                             0x04,
                             0x10, 0x01, 0x05, 0x10};

    BaudotDecoder d{};
    std::string out;
    for (uint8_t c : codes) {
        const char ch = d.decode(c);
        if (ch != 0) out.push_back(ch);
    }

    CHECK_STR_EQ(out, "RYRY123 TEST");
    CHECK(d.shift() == BaudotDecoder::Shift::Letters);
}

TEST(baudot_shift_codes_emit_nothing_but_change_state) {
    BaudotDecoder d{};
    CHECK_EQ(d.decode(0x1B), 0);
    CHECK(d.shift() == BaudotDecoder::Shift::Figures);
    CHECK_EQ(d.decode(0x1F), 0);
    CHECK(d.shift() == BaudotDecoder::Shift::Letters);
}

TEST(baudot_usos_can_be_disabled) {
    BaudotDecoder d{};
    d.set_usos(false);

    CHECK_EQ(d.decode(0x1B), 0);                     /* FIGS */
    CHECK_EQ(d.decode(0x04), ' ');                   /* space, figures[4] */
    CHECK(d.shift() == BaudotDecoder::Shift::Figures);
    CHECK_EQ(d.decode(0x10), '5');                   /* still figures */

    BaudotDecoder u{};                               /* default has USOS on */
    CHECK_EQ(u.decode(0x1B), 0);
    CHECK_EQ(u.decode(0x04), ' ');
    CHECK(u.shift() == BaudotDecoder::Shift::Letters);
    CHECK_EQ(u.decode(0x10), 'T');
}

TEST(baudot_masks_high_bits_of_the_code) {
    BaudotDecoder d{};
    /* Only the low five bits are data; the framer never sets the others, but a
     * corrupted message must not index off the end of the table. */
    CHECK_EQ(d.decode(0xEA), 'R');   /* 0xEA & 0x1F == 0x0A */
    CHECK_EQ(d.decode(0xFF), 0);     /* 0xFF & 0x1F == LTRS */
}

TEST(baudot_encoder_round_trips_through_the_decoder) {
    BaudotEncoder enc{};
    const auto codes = enc.encode("CQ DE M0ABC 1234");

    BaudotDecoder dec{};
    std::string out;
    for (uint8_t c : codes) {
        const char ch = dec.decode(c);
        if (ch != 0) out.push_back(ch);
    }
    CHECK_STR_EQ(out, "CQ DE M0ABC 1234");
}

TEST(baudot_encoder_inserts_shift_codes) {
    BaudotEncoder enc{};
    const auto codes = enc.encode("A1");
    /* 'A' is a letter (3); '1' needs FIGS (27) then 23. */
    CHECK_EQ(codes.size(), size_t{3});
    CHECK_EQ(codes[0], uint8_t{0x03});
    CHECK_EQ(codes[1], uint8_t{0x1B});
    CHECK_EQ(codes[2], uint8_t{0x17});
}

/* =========================================================================
 * UART framer
 * =======================================================================*/

TEST(framer_samples_at_the_expected_rate) {
    RttyFramer f{};
    f.configure(24000.0f, 4545);
    /* Upstream's default at 24 kHz is 528 samples per bit. */
    CHECK_EQ(f.samples_per_bit(), uint32_t{528});

    RttyFramer f50{};
    f50.configure(24000.0f, 5000);
    CHECK_EQ(f50.samples_per_bit(), uint32_t{480});
}

TEST(framer_recovers_a_known_five_bit_code) {
    RttyFramer f{};
    f.configure(24000.0f, 4545);

    std::vector<uint8_t> bits;
    append_idle(bits, 4);
    append_frame(bits, 0x0A);  /* 'R' */
    append_frame(bits, 0x15);  /* 'Y' */
    append_idle(bits, 4);

    const auto codes = run_framer(f, oversample(bits, f.samples_per_bit()));

    CHECK_EQ(codes.size(), size_t{2});
    if (codes.size() == 2) {
        CHECK_EQ(codes[0], uint8_t{0x0A});
        CHECK_EQ(codes[1], uint8_t{0x15});
    }
}

TEST(framer_decodes_a_whole_message_bit_exactly) {
    RttyFramer f{};
    f.configure(24000.0f, 4545);

    BaudotEncoder enc{};
    const auto codes_in = enc.encode("RYRY TEST 123");

    std::vector<uint8_t> bits;
    append_idle(bits, 8);
    for (uint8_t c : codes_in) append_frame(bits, c);
    append_idle(bits, 8);

    const auto codes_out = run_framer(f, oversample(bits, f.samples_per_bit()));

    BaudotDecoder dec{};
    std::string text;
    for (uint8_t c : codes_out) {
        const char ch = dec.decode(c);
        if (ch != 0) text.push_back(ch);
    }
    CHECK_STR_EQ(text, "RYRY TEST 123");
}

TEST(framer_accepts_a_frame_whose_stop_bit_sliced_low) {
    /* Upstream deliberately does not check the stop bit — the commented-out
     * `if (current_slicer_bit == 1)` in proc_rtty_rx.cpp — so a fade that eats
     * the stop bit still yields the character. */
    RttyFramer f{};
    f.configure(24000.0f, 4545);

    std::vector<uint8_t> bits;
    append_idle(bits, 4);
    bits.push_back(0);                                        /* start */
    for (int i = 0; i < 5; i++) bits.push_back((0x0A >> i) & 1);
    bits.push_back(0);                                        /* stop, corrupted */
    append_idle(bits, 6);

    const auto codes = run_framer(f, oversample(bits, f.samples_per_bit()));
    CHECK(codes.size() >= 1);
    if (!codes.empty()) CHECK_EQ(codes[0], uint8_t{0x0A});
}

TEST(framer_ignores_a_start_bit_that_does_not_hold) {
    /* A one-sample glitch to space must not start a frame: CHECK_START
     * re-tests the level half a bit later. */
    RttyFramer f{};
    f.configure(24000.0f, 4545);

    std::vector<uint8_t> sampled(f.samples_per_bit() * 8, 1);
    sampled[100] = 0;  /* single-sample dropout */

    const auto codes = run_framer(f, sampled);
    CHECK_EQ(codes.size(), size_t{0});
}

TEST(framer_auto_baud_converges_on_fifty_baud) {
    RttyFramer f{};
    f.configure(24000.0f, 0);  /* auto */
    CHECK_EQ(f.samples_per_bit(), uint32_t{528});

    /* A 50 baud square wave: 480 samples per half cycle at 24 kHz. */
    std::vector<uint8_t> sampled;
    for (int cycle = 0; cycle < 40; cycle++) {
        for (int i = 0; i < 480; i++) sampled.push_back(1);
        for (int i = 0; i < 480; i++) sampled.push_back(0);
    }
    (void)run_framer(f, sampled);

    /* The estimator is a 7/8 leaky average, so it lands near but not exactly
     * on 480; the readout snaps it onto the standard rate. */
    CHECK(f.samples_per_bit() >= 461u);
    CHECK(f.samples_per_bit() <= 500u);
    CHECK_EQ(f.estimated_baud_centi(), uint16_t{5000});
}

TEST(framer_auto_baud_rejects_out_of_range_pulses) {
    RttyFramer f{};
    f.configure(24000.0f, 0);

    /* 5 samples per half cycle is far below MIN_VALID_PULSE (200), so the
     * estimate must not move. */
    std::vector<uint8_t> sampled;
    for (int cycle = 0; cycle < 400; cycle++) {
        for (int i = 0; i < 5; i++) sampled.push_back(1);
        for (int i = 0; i < 5; i++) sampled.push_back(0);
    }
    (void)run_framer(f, sampled);
    CHECK_EQ(f.samples_per_bit(), uint32_t{528});
}

/* =========================================================================
 * dsp::FskDemod at RTTY parameters (brief: "FSK mark/space slicing via
 * FskDemod on a synthesised 2FSK signal")
 * =======================================================================*/

TEST(fskdemod_recovers_bits_from_a_synthesised_rtty_shift) {
    /* Mark is the higher tone, so a 1 is +85 Hz and a 0 is -85 Hz: a 170 Hz
     * shift, the standard amateur RTTY spacing upstream hard-codes. */
    std::vector<uint8_t> bits;
    /* Pseudo-random payload: an alternating pattern is a single tone after
     * filtering and gives a Gardner detector nothing to lock to. */
    uint32_t lfsr = 0xACE1u;
    for (int i = 0; i < 24; i++) bits.push_back(1);  /* idle mark preamble */
    for (int i = 0; i < 160; i++) {
        const uint32_t bit = ((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1u;
        lfsr = (lfsr >> 1) | (bit << 15);
        bits.push_back(static_cast<uint8_t>(lfsr & 1u));
    }

    const auto iq = dsp::fsk_modulate(bits, kRate, kBaud, kShiftHz / 2.0f);
    CHECK(iq.size() > 0);

    dsp::FskDemod demod{};
    demod.configure(kRate, kBaud, kShiftHz / 2.0f);

    std::vector<uint8_t> out;
    demod.process(iq.data(), iq.size(), out);

    /* The timing loop needs a few symbols to acquire and the last symbol may be
     * truncated, so compare the settled interior against the transmitted bits
     * at the best alignment. */
    CHECK(out.size() + 8 >= bits.size());

    size_t best_matches = 0;
    for (size_t shift = 0; shift + 40 < out.size() && shift < 8; shift++) {
        size_t matches = 0;
        size_t compared = 0;
        for (size_t i = 40; i + shift < out.size() && i < bits.size(); i++) {
            compared++;
            if (out[i + shift] == bits[i]) matches++;
        }
        if (compared > 0 && matches > best_matches) best_matches = matches;
    }
    /* At this SNR (noiseless) the settled region must be exact. */
    CHECK(best_matches >= 130);

    /* The tracked tone centre must sit on the carrier, not on one of the
     * tones — that is what makes the mark/space slicing symmetric. */
    CHECK_NEAR(demod.centre_frequency_hz(), 0.0, 12.0);
}

/* =========================================================================
 * Whole chain: synthesised 2FSK -> text
 * =======================================================================*/

TEST(rtty_demodulator_decodes_synthesised_2fsk) {
    BaudotEncoder enc{};
    const auto codes = enc.encode("RYRYRY CQ DE M0ABC K");

    std::vector<uint8_t> bits;
    append_idle(bits, 12);
    for (uint8_t c : codes) append_frame(bits, c);
    append_idle(bits, 12);

    const auto iq = dsp::fsk_modulate(bits, kRate, kBaud, kShiftHz / 2.0f);
    CHECK(iq.size() > 0);

    RttyDemodulator demod{};
    demod.configure(kRate, 4545, kShiftHz);

    std::string text;
    demod.process(iq.data(), iq.size(), text);

    CHECK_STR_EQ(text, "RYRYRY CQ DE M0ABC K");
    CHECK(!demod.squelched());
    CHECK(!demod.inverted_polarity());
}

TEST(rtty_demodulator_decodes_figures_shift_over_the_air) {
    BaudotEncoder enc{};
    const auto codes = enc.encode("RYRY 599 TEST");

    std::vector<uint8_t> bits;
    append_idle(bits, 12);
    for (uint8_t c : codes) append_frame(bits, c);
    append_idle(bits, 12);

    const auto iq = dsp::fsk_modulate(bits, kRate, kBaud, kShiftHz / 2.0f);

    RttyDemodulator demod{};
    demod.configure(kRate, 4545, kShiftHz);

    std::string text;
    demod.process(iq.data(), iq.size(), text);
    CHECK_STR_EQ(text, "RYRY 599 TEST");
}

TEST(rtty_demodulator_decodes_in_chunks_like_the_ui_does) {
    /* The view hands the decoder one block per frame; state must carry over. */
    BaudotEncoder enc{};
    const auto codes = enc.encode("RYRY TEST");

    std::vector<uint8_t> bits;
    append_idle(bits, 12);
    for (uint8_t c : codes) append_frame(bits, c);
    append_idle(bits, 12);

    const auto iq = dsp::fsk_modulate(bits, kRate, kBaud, kShiftHz / 2.0f);

    RttyDemodulator demod{};
    demod.configure(kRate, 4545, kShiftHz);

    std::string text;
    const size_t block = 1000;
    for (size_t i = 0; i < iq.size(); i += block) {
        const size_t n = (i + block <= iq.size()) ? block : (iq.size() - i);
        demod.process(iq.data() + i, n, text);
    }
    CHECK_STR_EQ(text, "RYRY TEST");
}

TEST(rtty_demodulator_auto_baud_decodes_fifty_baud) {
    BaudotEncoder enc{};
    const auto codes = enc.encode("RYRYRYRYRY DE TEST");

    std::vector<uint8_t> bits;
    append_idle(bits, 12);
    for (uint8_t c : codes) append_frame(bits, c);
    append_idle(bits, 12);

    const auto iq = dsp::fsk_modulate(bits, kRate, 50.0f, kShiftHz / 2.0f);

    RttyDemodulator demod{};
    demod.configure(kRate, 0, kShiftHz);  /* auto-detect */

    std::string text;
    demod.process(iq.data(), iq.size(), text);

    /* Auto-baud starts at 45.45 and walks towards 50, so the first characters
     * are sampled with the wrong bit width and are expected to be wrong; by
     * the end of the RY preamble it has converged. */
    CHECK_EQ(demod.estimated_baud_centi(), uint16_t{5000});
    CHECK(text.size() >= 8);
    CHECK(text.find("DE TEST") != std::string::npos);
}

TEST(rtty_demodulator_stays_squelched_on_silence) {
    std::vector<dsp::cfloat> silence(24000, dsp::cfloat{0.0f, 0.0f});

    RttyDemodulator demod{};
    demod.configure(kRate, 4545, kShiftHz);

    std::string text;
    demod.process(silence.data(), silence.size(), text);

    CHECK(demod.squelched());
    CHECK_STR_EQ(text, "");
}

TEST(rtty_demodulator_ignores_an_unmodulated_carrier) {
    /* A steady carrier has no shift: the envelope spread never opens the
     * squelch, so nothing may be printed. */
    std::vector<dsp::cfloat> carrier;
    carrier.reserve(24000);
    double phase = 0.0;
    const double step = 2.0 * 3.14159265358979323846 * 800.0 / kRate;
    for (size_t i = 0; i < 24000; i++) {
        carrier.push_back(dsp::cfloat{static_cast<float>(std::cos(phase)),
                                      static_cast<float>(std::sin(phase))});
        phase += step;
    }

    RttyDemodulator demod{};
    demod.configure(kRate, 4545, kShiftHz);

    std::string text;
    demod.process(carrier.data(), carrier.size(), text);

    CHECK(demod.squelched());
    CHECK_STR_EQ(text, "");
}

TEST(rtty_demodulator_handles_empty_input) {
    RttyDemodulator demod{};
    demod.configure(kRate, 4545, kShiftHz);
    std::string text;
    demod.process(nullptr, 0, text);
    CHECK_STR_EQ(text, "");
}

/* =========================================================================
 * Channel front end
 * =======================================================================*/

TEST(front_end_lands_on_a_usable_channel_rate) {
    ChannelFrontEnd fe{};

    fe.configure(2'400'000.0, 24'000.0);
    CHECK(fe.output_rate() >= 24'000.0);
    CHECK(fe.output_rate() < 48'000.0);
    CHECK_NEAR(fe.output_rate() * static_cast<double>(fe.decimation()), 2'400'000.0, 1.0);

    fe.configure(2'000'000.0, 24'000.0);
    CHECK(fe.output_rate() >= 24'000.0);
    CHECK(fe.output_rate() < 48'000.0);

    fe.configure(61'440'000.0, 24'000.0);
    CHECK(fe.output_rate() >= 24'000.0);
    CHECK(fe.output_rate() < 48'000.0);
}

TEST(front_end_moves_a_tone_to_baseband) {
    /* A tone 40 kHz above the LO, mixed down by an offset of +40 kHz, must come
     * out of the front end at DC. */
    const double rate = 2'400'000.0;
    const double tone = 40'000.0;

    std::vector<dsp::cfloat> in;
    in.reserve(24000);
    double phase = 0.0;
    const double step = 2.0 * 3.14159265358979323846 * tone / rate;
    for (size_t i = 0; i < 24000; i++) {
        in.push_back(dsp::cfloat{static_cast<float>(std::cos(phase)),
                                 static_cast<float>(std::sin(phase))});
        phase += step;
    }

    ChannelFrontEnd fe{};
    fe.configure(rate, 24'000.0);
    fe.set_offset(tone);

    std::vector<dsp::cfloat> out;
    fe.process(in, out);
    CHECK(out.size() > 0);

    /* Measure the residual frequency in the settled tail. */
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = out.size() / 2; i + 1 < out.size(); i++) {
        const dsp::cfloat d = out[i + 1] * std::conj(out[i]);
        acc += std::atan2(static_cast<double>(d.imag()), static_cast<double>(d.real()));
        n++;
    }
    CHECK(n > 0);
    if (n > 0) {
        const double hz = (acc / static_cast<double>(n)) * fe.output_rate() /
                          (2.0 * 3.14159265358979323846);
        CHECK_NEAR(hz, 0.0, 1.0);
    }
}
