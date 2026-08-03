/*
 * mayhem-b200 — SSTV receiver tests.
 *
 * Layers covered:
 *
 *  1. The mode table and its VIS codes, checked against the published SSTV VIS
 *     numbers and against firmware/common/sstv.hpp's sstv_parity().
 *  2. freq_to_pixel(), the 1500 Hz = black / 2300 Hz = white scanline mapping,
 *     against the arithmetic in proc_sstvrx.cpp.
 *  3. The VIS header decoder, driven with the tone sequence the specification
 *     defines, both as bare frequency estimates and as synthesised audio.
 *  4. Martin 1 line sync and the scanline-to-pixel conversion, driven with a
 *     synthesised line whose luminance ramps are known, and checked plane by
 *     plane so a colour-order mistake cannot pass.
 *  5. The whole receiver from complex baseband: an FM carrier modulated with
 *     the SSTV audio, demodulated and decoded back to the picture line.
 *  6. Writing a decoded line into a BMP with core::BmpFile and reading it back.
 *
 * No hardware is involved. Live RF reception is unverified.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_sstvrx.hpp"

#include "../src/core/bmp_file.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace app::sstv;

namespace {

constexpr float kRate = 48000.0f;
constexpr double kPi = 3.14159265358979323846;

/* --- synthetic frequency streams ---------------------------------------- */

void push_tone(std::vector<int32_t>& out, float rate, int32_t hz, float ms) {
    const size_t n = static_cast<size_t>(std::lround(
        static_cast<double>(rate) * static_cast<double>(ms) / 1000.0));
    for (size_t i = 0; i < n; i++) out.push_back(hz);
}

int32_t luma_to_hz(uint8_t v) {
    return kFreqBlack + static_cast<int32_t>(std::llround(
                            static_cast<double>(kFreqWhite - kFreqBlack) *
                            static_cast<double>(v) / 255.0));
}

/* One colour channel's scan, with the same fractional pixel timing the decoder
 * uses so the two clocks do not drift apart. */
void push_scan(std::vector<int32_t>& out, float rate,
               const std::vector<uint8_t>& values, float pixel_ms) {
    const double per_px = static_cast<double>(rate) * static_cast<double>(pixel_ms) / 1000.0;
    double emitted = 0.0;
    for (size_t p = 0; p < values.size(); p++) {
        const double want = per_px * static_cast<double>(p + 1);
        const size_t n = static_cast<size_t>(std::lround(want - emitted));
        const int32_t hz = luma_to_hz(values[p]);
        for (size_t i = 0; i < n; i++) out.push_back(hz);
        emitted += static_cast<double>(n);
    }
}

void push_vis_header(std::vector<int32_t>& out, float rate, uint8_t vis) {
    push_tone(out, rate, kFreqLeader, 300.0f);
    push_tone(out, rate, kFreqSync, 10.0f);
    push_tone(out, rate, kFreqLeader, 300.0f);
    push_tone(out, rate, kFreqSync, 30.0f);
    for (int i = 0; i < 8; i++)
        push_tone(out, rate, ((vis >> i) & 1) ? kFreqVisOne : kFreqVisZero, 30.0f);
    push_tone(out, rate, kFreqSync, 30.0f);
}

/* --- test picture -------------------------------------------------------- */

struct TestLine {
    std::vector<uint8_t> green;
    std::vector<uint8_t> blue;
    std::vector<uint8_t> red;
};

TestLine make_test_line() {
    TestLine t;
    t.green.resize(kPixelsPerLine);
    t.blue.resize(kPixelsPerLine);
    t.red.resize(kPixelsPerLine);
    for (size_t p = 0; p < kPixelsPerLine; p++) {
        t.green[p] = static_cast<uint8_t>((p * 255) / (kPixelsPerLine - 1));
        t.blue[p] = static_cast<uint8_t>(255 - (p * 255) / (kPixelsPerLine - 1));
        t.red[p] = 200;
    }
    return t;
}

/* Martin 1's line: sync, porch, green, separator, blue, separator, red. */
void push_martin_line(std::vector<int32_t>& out, float rate, const Mode& mode,
                      const TestLine& t) {
    push_tone(out, rate, kFreqSync, mode.sync_time_ms);
    push_tone(out, rate, kFreqBlack, mode.gap_time_ms);
    push_scan(out, rate, t.green, mode.pixel_time_ms);
    push_tone(out, rate, kFreqBlack, mode.gap_time_ms);
    push_scan(out, rate, t.blue, mode.pixel_time_ms);
    push_tone(out, rate, kFreqBlack, mode.gap_time_ms);
    push_scan(out, rate, t.red, mode.pixel_time_ms);
}

/* --- synthetic audio ----------------------------------------------------- */

std::vector<float> tones_to_audio(const std::vector<int32_t>& hz, float rate) {
    std::vector<float> audio;
    audio.reserve(hz.size());
    double phase = 0.0;
    for (int32_t f : hz) {
        audio.push_back(static_cast<float>(std::sin(phase)));
        phase += 2.0 * kPi * static_cast<double>(f) / static_cast<double>(rate);
        if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
    }
    return audio;
}

/* FM-modulates the SSTV audio onto a complex carrier at the deviation the app
 * demodulates with, i.e. the signal the channel filter would hand it. */
std::vector<dsp::cfloat> audio_to_baseband(const std::vector<float>& audio, float rate) {
    std::vector<dsp::cfloat> iq;
    iq.reserve(audio.size());
    double phase = 0.0;
    const double k = 2.0 * kPi * static_cast<double>(kSstvDeviationHz) /
                     static_cast<double>(rate);
    for (float a : audio) {
        iq.push_back(dsp::cfloat{static_cast<float>(std::cos(phase)),
                                 static_cast<float>(std::sin(phase))});
        phase += k * static_cast<double>(a);
    }
    return iq;
}

/* Largest absolute error between a decoded plane and what was transmitted,
 * ignoring `skip` pixels at each end (the tone estimator's group delay smears
 * the first and last pixels of every channel). */
int plane_max_error(const std::vector<uint8_t>& rgb, size_t plane,
                    const std::vector<uint8_t>& expected, size_t skip) {
    int worst = 0;
    for (size_t p = skip; p + skip < expected.size(); p++) {
        const int got = rgb[p * 3 + plane];
        const int want = expected[p];
        const int err = (got > want) ? (got - want) : (want - got);
        if (err > worst) worst = err;
    }
    return worst;
}

}  // namespace

/* =========================================================================
 * Mode table and VIS codes
 * =======================================================================*/

TEST(vis_parity_matches_published_codes) {
    /* Even parity in bit 7 over the seven data bits. These are the VIS numbers
     * every SSTV reference lists. The pairs live in an array so the checks are
     * runtime comparisons rather than folded constants. */
    struct Case {
        uint8_t code;
        uint8_t vis;
    };
    const Case cases[] = {
        {60, 60},   /* Scottie 1,  even parity */
        {56, 184},  /* Scottie 2,  odd  */
        {76, 204},  /* Scottie DX, odd  */
        {44, 172},  /* Martin 1,   odd  */
        {40, 40},   /* Martin 2,   even */
        {55, 183},  /* SC2-180,    odd  */
    };
    for (const auto& c : cases) CHECK_EQ(vis_parity(c.code), c.vis);
}

TEST(mode_table_carries_those_vis_codes) {
    const uint8_t expected[kModeCount] = {60, 184, 204, 172, 40, 183};
    for (size_t i = 0; i < kModeCount; i++) CHECK_EQ(kModes[i].vis_code, expected[i]);
}

TEST(vis_parity_is_even_parity_over_seven_bits) {
    for (int code = 0; code < 128; code++) {
        int ones = 0;
        for (int b = 0; b < 7; b++) ones += (code >> b) & 1;
        const uint8_t expect = static_cast<uint8_t>(code | ((ones & 1) << 7));
        CHECK_EQ(vis_parity(static_cast<uint8_t>(code)), expect);
    }
}

TEST(vis_parity_ok_rejects_a_corrupt_byte) {
    const uint8_t good[] = {172, 60, 184, 40};
    for (uint8_t b : good) CHECK(vis_parity_ok(b));

    /* Same codes with the parity bit flipped. */
    const uint8_t bad[] = {44, 188, 56, 168};
    for (uint8_t b : bad) CHECK(!vis_parity_ok(b));
}

TEST(mode_lookup_by_vis_code) {
    const Mode* found = mode_for_vis(172);
    CHECK(found != nullptr);
    if (found) CHECK_STR_EQ(found->name, "Martin 1");

    found = mode_for_vis(184);
    CHECK(found != nullptr);
    if (found) CHECK_STR_EQ(found->name, "Scottie 2");

    found = mode_for_vis(183);
    CHECK(found != nullptr);
    if (found) CHECK_STR_EQ(found->name, "SC2-180");

    /* 113 is Pasokon P3, which upstream's table has commented out. */
    CHECK(mode_for_vis(113) == nullptr);
    CHECK(mode_for_vis(0) == nullptr);
}

TEST(mode_table_timings_match_upstream) {
    const Mode& martin1 = kModes[3];
    CHECK_STR_EQ(martin1.name, "Martin 1");
    CHECK_NEAR(martin1.pixel_time_ms, 0.4576, 1e-6);
    CHECK_NEAR(martin1.sync_time_ms, 4.862, 1e-6);
    CHECK_NEAR(martin1.gap_time_ms, 0.572, 1e-6);
    /* 320 pixels x 0.4576 ms is Martin 1's 146.432 ms scan. */
    CHECK_NEAR(martin1.pixel_time_ms * 320.0, 146.432, 1e-3);

    const Mode& scottie1 = kModes[0];
    CHECK_STR_EQ(scottie1.name, "Scottie 1");
    CHECK_NEAR(scottie1.pixel_time_ms * 320.0, 138.24, 1e-3);
    CHECK_NEAR(scottie1.sync_time_ms, 9.0, 1e-6);
    CHECK(scottie1.sync_on_first);
    CHECK_EQ(scottie1.sync_index, uint8_t{2});

    const Mode& sc2 = kModes[5];
    CHECK_STR_EQ(sc2.name, "SC2-180");
    CHECK_NEAR(sc2.pixel_time_ms * 320.0, 235.008, 1e-3);
    CHECK(!sc2.gaps);
    CHECK(sc2.color_sequence == ColorSeq::Rgb);
}

TEST(colour_sequence_maps_channels_to_planes) {
    uint8_t order[3]{};
    color_order_for(kModes[3], order); /* Martin 1, GBR */
    CHECK_EQ(order[0], uint8_t{1});    /* first channel is green */
    CHECK_EQ(order[1], uint8_t{2});    /* then blue */
    CHECK_EQ(order[2], uint8_t{0});    /* then red */

    color_order_for(kModes[5], order); /* SC2-180, RGB */
    CHECK_EQ(order[0], uint8_t{0});
    CHECK_EQ(order[1], uint8_t{1});
    CHECK_EQ(order[2], uint8_t{2});
}

/* =========================================================================
 * Scanline frequency -> pixel
 * =======================================================================*/

TEST(freq_to_pixel_maps_the_sstv_luminance_range) {
    struct Case {
        int32_t hz;
        uint8_t value;
    };
    const Case cases[] = {
        {1500, 0},
        {2300, 255},
        {1900, 127}, /* (1900-1500)*255/800 == 127.5, truncated */
        {1700, 63},
        {2100, 191},
    };
    for (const auto& c : cases) CHECK_EQ(freq_to_pixel(c.hz), c.value);
}

TEST(freq_to_pixel_clamps_out_of_band_tones) {
    const int32_t low[] = {1200, 0, -500}; /* 1200 is the sync tone */
    for (int32_t hz : low) CHECK_EQ(freq_to_pixel(hz), uint8_t{0});

    const int32_t high[] = {2301, 3000, 100000};
    for (int32_t hz : high) CHECK_EQ(freq_to_pixel(hz), uint8_t{255});
}

TEST(luma_round_trips_through_the_tone_mapping) {
    for (int v = 0; v <= 255; v++) {
        const int32_t hz = luma_to_hz(static_cast<uint8_t>(v));
        const int got = freq_to_pixel(hz);
        CHECK(got >= v - 1 && got <= v + 1);
    }
}

/* =========================================================================
 * VIS decoding
 * =======================================================================*/

TEST(vis_decoder_reads_martin_one_from_the_tone_sequence) {
    std::vector<int32_t> f;
    push_vis_header(f, kRate, 172);

    VisDecoder v{};
    v.configure(kRate);

    bool done = false;
    for (int32_t hz : f) {
        if (v.process(hz)) {
            done = true;
            break;
        }
    }
    CHECK(done);
    CHECK_EQ(v.code(), uint8_t{172});
    CHECK(v.parity_ok());
    const Mode* found = mode_for_vis(v.code());
    CHECK(found != nullptr);
    if (found) CHECK_STR_EQ(found->name, "Martin 1");
}

TEST(vis_decoder_reads_every_mode_in_the_table) {
    for (const auto& entry : kModes) {
        std::vector<int32_t> f;
        push_vis_header(f, kRate, entry.vis_code);

        VisDecoder v{};
        v.configure(kRate);
        bool done = false;
        for (int32_t hz : f) {
            if (v.process(hz)) {
                done = true;
                break;
            }
        }
        CHECK(done);
        CHECK_EQ(v.code(), entry.vis_code);
        CHECK(v.parity_ok());
    }
}

TEST(vis_decoder_flags_a_bad_parity_bit) {
    /* Martin 1 with the parity bit cleared: the byte still decodes, but it is
     * reported as failing parity and must not name a mode. */
    std::vector<int32_t> f;
    push_vis_header(f, kRate, 44);

    VisDecoder v{};
    v.configure(kRate);
    bool done = false;
    for (int32_t hz : f) {
        if (v.process(hz)) {
            done = true;
            break;
        }
    }
    CHECK(done);
    CHECK_EQ(v.code(), uint8_t{44});
    CHECK(!v.parity_ok());
}

TEST(vis_decoder_ignores_a_leader_with_no_header_after_it) {
    std::vector<int32_t> f;
    push_tone(f, kRate, kFreqLeader, 500.0f);
    push_tone(f, kRate, 1800, 500.0f);

    VisDecoder v{};
    v.configure(kRate);
    for (int32_t hz : f) CHECK(!v.process(hz));
}

TEST(vis_decoder_ignores_plain_image_tones) {
    /* A scan sweeping the whole luminance range must not synthesise a VIS
     * byte out of nothing. */
    std::vector<int32_t> f;
    for (int rep = 0; rep < 20; rep++)
        for (int32_t hz = kFreqBlack; hz <= kFreqWhite; hz += 4) push_tone(f, kRate, hz, 0.5f);

    VisDecoder v{};
    v.configure(kRate);
    for (int32_t hz : f) CHECK(!v.process(hz));
}

TEST(vis_decoder_reads_the_header_from_synthesised_audio) {
    std::vector<int32_t> f;
    push_vis_header(f, kRate, 172);
    const auto audio = tones_to_audio(f, kRate);

    ToneEstimator tone{};
    tone.configure(kRate);
    VisDecoder v{};
    v.configure(kRate);

    bool done = false;
    for (float a : audio) {
        if (v.process(tone.process(a))) {
            done = true;
            break;
        }
    }
    CHECK(done);
    CHECK_EQ(v.code(), uint8_t{172});
    CHECK(v.parity_ok());
}

/* =========================================================================
 * Tone estimator
 * =======================================================================*/

TEST(tone_estimator_measures_the_sstv_tones) {
    ToneEstimator tone{};
    tone.configure(kRate);

    const int32_t wanted[] = {1100, 1200, 1300, 1500, 1900, 2300};
    for (int32_t want : wanted) {
        tone.reset();
        std::vector<int32_t> f;
        push_tone(f, kRate, want, 50.0f);
        const auto audio = tones_to_audio(f, kRate);

        int32_t last = 0;
        for (float a : audio) last = tone.process(a);
        CHECK_NEAR(last, want, 3.0);
    }
}

TEST(tone_estimator_holds_its_last_reading_on_silence) {
    ToneEstimator tone{};
    tone.configure(kRate);

    std::vector<int32_t> f;
    push_tone(f, kRate, 1900, 50.0f);
    const auto audio = tones_to_audio(f, kRate);
    for (float a : audio) tone.process(a);

    /* Digital silence has no frequency; reporting the band centre instead
     * would look like a mid-grey pixel. */
    int32_t last = 0;
    for (int i = 0; i < 4800; i++) last = tone.process(0.0f);
    CHECK(last != 0);
}

/* =========================================================================
 * Line decode, driven with exact tone estimates
 * =======================================================================*/

TEST(line_decoder_configures_martin_one_timings) {
    LineDecoder ld{};
    CHECK(ld.configure(kModes[3], kRate));

    /* 0.4576 ms at 48 kHz is 21.9648 samples per pixel. */
    CHECK_NEAR(ld.samples_per_pixel(), 21.9648, 1e-3);
    CHECK_EQ(ld.samples_per_sync(), uint32_t{233});  /* round(4.862 * 48) */
    CHECK_EQ(ld.samples_per_gap(), uint32_t{27});    /* round(0.572 * 48) */
    CHECK_EQ(ld.lines(), uint16_t{256});
}

TEST(line_decoder_rejects_a_mode_it_cannot_hold) {
    Mode wide = kModes[3];
    wide.pixels = 640;
    LineDecoder ld{};
    CHECK(!ld.configure(wide, kRate));
}

TEST(line_decoder_decodes_a_martin_one_scanline) {
    const Mode& mode = kModes[3];
    const TestLine t = make_test_line();

    LineDecoder ld{};
    CHECK(ld.configure(mode, kRate));

    std::vector<uint8_t> got;
    int lines = 0;
    uint16_t got_line = 0xFFFF;
    ld.on_line = [&](uint16_t n, const uint8_t* rgb) {
        got.assign(rgb, rgb + static_cast<size_t>(kPixelsPerLine) * 3);
        got_line = n;
        lines++;
    };

    std::vector<int32_t> f;
    /* Upstream needs two syncs before it starts the first line. */
    push_tone(f, kRate, kFreqSync, mode.sync_time_ms);
    push_tone(f, kRate, kFreqBlack, 5.0f);
    push_martin_line(f, kRate, mode, t);
    push_tone(f, kRate, kFreqSync, mode.sync_time_ms);
    push_tone(f, kRate, kFreqBlack, 1.0f);

    for (int32_t hz : f) ld.process_frequency(hz);

    CHECK_EQ(lines, 1);
    CHECK_EQ(got_line, uint16_t{0});
    CHECK_EQ(got.size(), static_cast<size_t>(kPixelsPerLine) * 3);
    if (got.size() != static_cast<size_t>(kPixelsPerLine) * 3) return;

    /* Plane by plane: green ramps up, blue ramps down, red is flat. A colour
     * order mistake moves a whole plane and cannot pass this. */
    CHECK(plane_max_error(got, 1, t.green, 1) <= 2);
    CHECK(plane_max_error(got, 2, t.blue, 1) <= 2);
    CHECK(plane_max_error(got, 0, t.red, 1) <= 2);
}

TEST(line_decoder_needs_two_syncs_before_the_first_line) {
    const Mode& mode = kModes[3];
    const TestLine t = make_test_line();

    LineDecoder ld{};
    CHECK(ld.configure(mode, kRate));
    int lines = 0;
    ld.on_line = [&](uint16_t, const uint8_t*) { lines++; };

    /* Only one sync: nothing may be emitted. */
    std::vector<int32_t> f;
    push_martin_line(f, kRate, mode, t);
    push_tone(f, kRate, kFreqBlack, 1.0f);
    for (int32_t hz : f) ld.process_frequency(hz);
    CHECK_EQ(lines, 0);
}

TEST(line_decoder_numbers_consecutive_lines) {
    const Mode& mode = kModes[3];
    const TestLine t = make_test_line();

    LineDecoder ld{};
    CHECK(ld.configure(mode, kRate));

    std::vector<uint16_t> numbers;
    ld.on_line = [&](uint16_t n, const uint8_t*) { numbers.push_back(n); };

    std::vector<int32_t> f;
    push_tone(f, kRate, kFreqSync, mode.sync_time_ms);
    push_tone(f, kRate, kFreqBlack, 5.0f);
    for (int i = 0; i < 3; i++) push_martin_line(f, kRate, mode, t);
    push_tone(f, kRate, kFreqSync, mode.sync_time_ms);
    push_tone(f, kRate, kFreqBlack, 1.0f);

    for (int32_t hz : f) ld.process_frequency(hz);

    CHECK_EQ(numbers.size(), size_t{3});
    for (size_t i = 0; i < numbers.size(); i++) CHECK_EQ(numbers[i], static_cast<uint16_t>(i));
}

TEST(line_decoder_phase_offset_shifts_the_line) {
    const Mode& mode = kModes[3];
    const TestLine t = make_test_line();

    LineDecoder ld{};
    CHECK(ld.configure(mode, kRate));
    ld.set_phase_offset(10);
    CHECK_EQ(ld.phase_offset(), int16_t{10});

    std::vector<uint8_t> got;
    ld.on_line = [&](uint16_t, const uint8_t* rgb) {
        got.assign(rgb, rgb + static_cast<size_t>(kPixelsPerLine) * 3);
    };

    std::vector<int32_t> f;
    push_tone(f, kRate, kFreqSync, mode.sync_time_ms);
    push_tone(f, kRate, kFreqBlack, 5.0f);
    push_martin_line(f, kRate, mode, t);
    push_tone(f, kRate, kFreqSync, mode.sync_time_ms);
    push_tone(f, kRate, kFreqBlack, 1.0f);
    for (int32_t hz : f) ld.process_frequency(hz);

    CHECK_EQ(got.size(), static_cast<size_t>(kPixelsPerLine) * 3);
    if (got.size() != static_cast<size_t>(kPixelsPerLine) * 3) return;

    /* Green at column x now carries what was sent for column x-10. */
    for (size_t x = 12; x < 300; x++) {
        const int want = t.green[x - 10];
        const int have = got[x * 3 + 1];
        CHECK(have >= want - 2 && have <= want + 2);
    }
}

TEST(line_decoder_scottie_starts_at_the_sync_index_channel) {
    /* Scottie sends sync between blue and red, so the channel that follows a
     * sync is red (colour plane 0), not green. Upstream's proc ignores
     * sync_index and gets this wrong; see deviation 3 in the header. */
    const Mode& mode = kModes[1]; /* Scottie 2 */
    const TestLine t = make_test_line();

    LineDecoder ld{};
    CHECK(ld.configure(mode, kRate));

    std::vector<uint8_t> got;
    ld.on_line = [&](uint16_t, const uint8_t* rgb) {
        got.assign(rgb, rgb + static_cast<size_t>(kPixelsPerLine) * 3);
    };

    /* After the sync, Scottie sends: porch, red, separator, green,
     * separator, blue. */
    std::vector<int32_t> f;
    push_tone(f, kRate, kFreqSync, mode.sync_time_ms);
    push_tone(f, kRate, kFreqBlack, 5.0f);
    push_tone(f, kRate, kFreqSync, mode.sync_time_ms);
    push_tone(f, kRate, kFreqBlack, mode.gap_time_ms);
    push_scan(f, kRate, t.red, mode.pixel_time_ms);
    push_tone(f, kRate, kFreqBlack, mode.gap_time_ms);
    push_scan(f, kRate, t.green, mode.pixel_time_ms);
    push_tone(f, kRate, kFreqBlack, mode.gap_time_ms);
    push_scan(f, kRate, t.blue, mode.pixel_time_ms);
    push_tone(f, kRate, kFreqSync, mode.sync_time_ms);
    push_tone(f, kRate, kFreqBlack, 1.0f);

    for (int32_t hz : f) ld.process_frequency(hz);

    CHECK_EQ(got.size(), static_cast<size_t>(kPixelsPerLine) * 3);
    if (got.size() != static_cast<size_t>(kPixelsPerLine) * 3) return;

    CHECK(plane_max_error(got, 0, t.red, 1) <= 2);
    CHECK(plane_max_error(got, 1, t.green, 1) <= 2);
    CHECK(plane_max_error(got, 2, t.blue, 1) <= 2);
}

TEST(line_decoder_sc2_180_has_no_channel_gaps) {
    const Mode& mode = kModes[5]; /* SC2-180, RGB, gaps == false */
    const TestLine t = make_test_line();

    LineDecoder ld{};
    CHECK(ld.configure(mode, kRate));
    CHECK_EQ(ld.samples_per_gap(), uint32_t{24}); /* the porch is still 0.5 ms */

    std::vector<uint8_t> got;
    ld.on_line = [&](uint16_t, const uint8_t* rgb) {
        got.assign(rgb, rgb + static_cast<size_t>(kPixelsPerLine) * 3);
    };

    std::vector<int32_t> f;
    push_tone(f, kRate, kFreqSync, mode.sync_time_ms);
    push_tone(f, kRate, kFreqBlack, 5.0f);
    push_tone(f, kRate, kFreqSync, mode.sync_time_ms);
    push_tone(f, kRate, kFreqBlack, mode.gap_time_ms);
    push_scan(f, kRate, t.red, mode.pixel_time_ms);
    push_scan(f, kRate, t.green, mode.pixel_time_ms);
    push_scan(f, kRate, t.blue, mode.pixel_time_ms);
    push_tone(f, kRate, kFreqSync, mode.sync_time_ms);
    push_tone(f, kRate, kFreqBlack, 1.0f);

    for (int32_t hz : f) ld.process_frequency(hz);

    CHECK_EQ(got.size(), static_cast<size_t>(kPixelsPerLine) * 3);
    if (got.size() != static_cast<size_t>(kPixelsPerLine) * 3) return;

    CHECK(plane_max_error(got, 0, t.red, 1) <= 2);
    CHECK(plane_max_error(got, 1, t.green, 1) <= 2);
    CHECK(plane_max_error(got, 2, t.blue, 1) <= 2);
}

/* =========================================================================
 * Whole receiver
 * =======================================================================*/

TEST(sstv_decoder_detects_the_mode_and_decodes_a_line_from_audio) {
    const Mode& mode = kModes[3]; /* Martin 1 */
    const TestLine t = make_test_line();

    std::vector<int32_t> f;
    push_vis_header(f, kRate, mode.vis_code);
    /* A short black gap so the VIS stop bit and the first sync are separate
     * events for the line decoder. */
    push_tone(f, kRate, kFreqBlack, 5.0f);
    push_martin_line(f, kRate, mode, t);
    push_tone(f, kRate, kFreqSync, mode.sync_time_ms);
    push_tone(f, kRate, kFreqBlack, 2.0f);

    const auto audio = tones_to_audio(f, kRate);

    SstvDecoder dec{};
    dec.configure(kRate);
    dec.set_mode(mode_by_index(1)); /* deliberately wrong; VIS must correct it */
    dec.set_auto_vis(true);

    std::string detected;
    uint8_t detected_vis = 0;
    dec.on_mode_detected = [&](const Mode& found, uint8_t vis) {
        detected = found.name;
        detected_vis = vis;
    };

    std::vector<uint8_t> got;
    int lines = 0;
    dec.on_line = [&](uint16_t, const uint8_t* rgb) {
        got.assign(rgb, rgb + static_cast<size_t>(kPixelsPerLine) * 3);
        lines++;
    };

    dec.process_audio(audio.data(), audio.size());

    CHECK_STR_EQ(detected, "Martin 1");
    CHECK_EQ(detected_vis, uint8_t{172});
    CHECK_EQ(lines, 1);
    if (got.size() != static_cast<size_t>(kPixelsPerLine) * 3) return;

    /* The tone estimator's lowpass smears about two pixels at each channel
     * boundary, so the first and last few pixels of a scan are excluded.
     * Everything between them has to be right to within the +/-1 the integer
     * luminance-to-tone mapping itself costs. */
    CHECK(plane_max_error(got, 1, t.green, 6) <= 2);
    CHECK(plane_max_error(got, 2, t.blue, 6) <= 2);
    CHECK(plane_max_error(got, 0, t.red, 6) <= 2);
}

TEST(sstv_decoder_decodes_a_line_from_complex_baseband) {
    const Mode& mode = kModes[3];
    const TestLine t = make_test_line();

    std::vector<int32_t> f;
    push_vis_header(f, kRate, mode.vis_code);
    push_tone(f, kRate, kFreqBlack, 5.0f);
    push_martin_line(f, kRate, mode, t);
    push_tone(f, kRate, kFreqSync, mode.sync_time_ms);
    push_tone(f, kRate, kFreqBlack, 2.0f);

    const auto audio = tones_to_audio(f, kRate);
    const auto iq = audio_to_baseband(audio, kRate);

    SstvDecoder dec{};
    dec.configure(kRate);
    dec.set_auto_vis(true);

    std::vector<uint8_t> got;
    int lines = 0;
    dec.on_line = [&](uint16_t, const uint8_t* rgb) {
        got.assign(rgb, rgb + static_cast<size_t>(kPixelsPerLine) * 3);
        lines++;
    };
    std::string detected;
    dec.on_mode_detected = [&](const Mode& found, uint8_t) { detected = found.name; };

    /* In blocks, as the view feeds it one snapshot per frame. */
    const size_t block = 4096;
    for (size_t i = 0; i < iq.size(); i += block) {
        const size_t n = (i + block <= iq.size()) ? block : (iq.size() - i);
        dec.process(iq.data() + i, n);
    }

    CHECK_STR_EQ(detected, "Martin 1");
    CHECK_EQ(lines, 1);
    if (got.size() != static_cast<size_t>(kPixelsPerLine) * 3) return;

    CHECK(plane_max_error(got, 1, t.green, 6) <= 2);
    CHECK(plane_max_error(got, 2, t.blue, 6) <= 2);
    CHECK(plane_max_error(got, 0, t.red, 6) <= 2);
}

TEST(vis_header_does_not_start_a_line_from_its_own_tones) {
    /* Regression: the header's 1200 Hz break and its start/stop bits all fall
     * inside the sync detector's 1200 +/- 150 Hz window, so they look like two
     * syncs. Unless a valid VIS restarts the line decoder, line 0 begins inside
     * the header and the whole picture lands a sync early. This only shows up
     * when the VIS names the mode that was already selected. */
    const Mode& mode = kModes[3]; /* Martin 1 */
    const TestLine t = make_test_line();

    std::vector<int32_t> f;
    push_vis_header(f, kRate, mode.vis_code);
    push_tone(f, kRate, kFreqBlack, 5.0f);
    push_martin_line(f, kRate, mode, t);
    push_tone(f, kRate, kFreqSync, mode.sync_time_ms);
    push_tone(f, kRate, kFreqBlack, 1.0f);

    SstvDecoder dec{};
    dec.configure(kRate);
    dec.set_mode(&mode); /* already the right mode */
    dec.set_auto_vis(true);

    std::vector<uint8_t> got;
    int lines = 0;
    dec.on_line = [&](uint16_t, const uint8_t* rgb) {
        got.assign(rgb, rgb + static_cast<size_t>(kPixelsPerLine) * 3);
        lines++;
    };

    for (int32_t hz : f) dec.process_frequency(hz);

    CHECK_EQ(lines, 1);
    if (got.size() != static_cast<size_t>(kPixelsPerLine) * 3) return;
    CHECK(plane_max_error(got, 1, t.green, 1) <= 2);
    CHECK(plane_max_error(got, 2, t.blue, 1) <= 2);
    CHECK(plane_max_error(got, 0, t.red, 1) <= 2);
}

TEST(sstv_decoder_keeps_the_manual_mode_when_vis_is_off) {
    std::vector<int32_t> f;
    push_vis_header(f, kRate, 172); /* Martin 1 in the air */

    SstvDecoder dec{};
    dec.configure(kRate);
    dec.set_mode(mode_by_index(1)); /* Scottie 2 chosen by hand */
    dec.set_auto_vis(false);

    bool detected = false;
    dec.on_mode_detected = [&](const Mode&, uint8_t) { detected = true; };
    for (int32_t hz : f) dec.process_frequency(hz);

    CHECK(!detected);
    CHECK(dec.mode() != nullptr);
    if (dec.mode()) CHECK_STR_EQ(dec.mode()->name, "Scottie 2");
}

TEST(sstv_decoder_ignores_a_vis_code_it_does_not_know) {
    /* 113 is Pasokon P3: a well-formed header for a mode not in the table. */
    const uint8_t unknown = vis_parity(113);
    std::vector<int32_t> f;
    push_vis_header(f, kRate, unknown);

    SstvDecoder dec{};
    dec.configure(kRate);
    dec.set_mode(mode_by_index(3));
    dec.set_auto_vis(true);

    bool detected = false;
    dec.on_mode_detected = [&](const Mode&, uint8_t) { detected = true; };
    for (int32_t hz : f) dec.process_frequency(hz);

    CHECK(!detected);
    if (dec.mode()) CHECK_STR_EQ(dec.mode()->name, "Martin 1");
    CHECK_EQ(dec.last_vis(), unknown);
}

TEST(sstv_decoder_handles_empty_input) {
    SstvDecoder dec{};
    dec.configure(kRate);
    dec.process(nullptr, 0);
    dec.process_audio(nullptr, 0);
    CHECK(dec.mode() != nullptr);
}

/* =========================================================================
 * BMP output
 * =======================================================================*/

TEST(decoded_line_round_trips_through_a_bmp) {
    const TestLine t = make_test_line();

    /* Build the interleaved RGB line the decoder hands the view. */
    std::vector<uint8_t> rgb(static_cast<size_t>(kPixelsPerLine) * 3);
    for (size_t p = 0; p < kPixelsPerLine; p++) {
        rgb[p * 3 + 0] = t.red[p];
        rgb[p * 3 + 1] = t.green[p];
        rgb[p * 3 + 2] = t.blue[p];
    }

    const char* tmp = std::tmpnam(nullptr);
    CHECK(tmp != nullptr);
    if (tmp == nullptr) return;
    const std::string path = std::string{tmp} + "_sstv.bmp";

    {
        core::BmpFile bmp{};
        CHECK(bmp.create(path, kPixelsPerLine, 1));

        /* Two rows, written the way SstvRxView::handle_line() writes them. */
        for (uint32_t row = 0; row < 2; row++) {
            if (bmp.get_real_height() <= row) CHECK(bmp.expand_y(row + 1));
            CHECK(bmp.seek(0, row));
            for (uint16_t x = 0; x < kPixelsPerLine; x++) {
                const uint8_t* px = rgb.data() + static_cast<size_t>(x) * 3;
                CHECK(bmp.write_next_px(ui::Color(px[0], px[1], px[2])));
            }
        }
        bmp.close();
    }

    std::vector<ui::Color> pixels;
    uint32_t w = 0, h = 0;
    CHECK(core::load_bmp_rgb565(path, pixels, w, h));
    CHECK_EQ(w, uint32_t{kPixelsPerLine});
    CHECK_EQ(h, uint32_t{2});

    if (pixels.size() >= kPixelsPerLine) {
        /* RGB565 keeps 5 bits of red and blue and 6 of green, so a value can
         * come back up to 7 (red/blue) or 3 (green) low. */
        for (size_t x = 0; x < kPixelsPerLine; x++) {
            ui::Color c = pixels[x];
            CHECK(static_cast<int>(t.red[x]) - static_cast<int>(c.r()) <= 7);
            CHECK(static_cast<int>(c.r()) <= static_cast<int>(t.red[x]));
            CHECK(static_cast<int>(t.green[x]) - static_cast<int>(c.g()) <= 3);
            CHECK(static_cast<int>(t.blue[x]) - static_cast<int>(c.b()) <= 7);
        }
    }

    std::remove(path.c_str());
}
