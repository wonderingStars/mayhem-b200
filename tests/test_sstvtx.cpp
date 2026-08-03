/*
 * mayhem-b200 — SSTV transmitter tests.
 *
 * The encoder is the deliverable, so it is checked against the specification and
 * against the upstream sources, never against whatever this port happens to
 * produce:
 *
 *  1. VIS codes, via the exact port of common/sstv.hpp's sstv_parity(), against
 *     the published SSTV VIS numbers (Martin 1 = 44, Scottie 1 = 60, ...), and
 *     the LSB-first data-bit tone sequence proc_sstvtx.cpp emits.
 *  2. The pixel -> tone mapping 1500 + luma*800/256 (proc_sstvtx.cpp), covering
 *     the 1500..2300 Hz scanline band and its monotonicity.
 *  3. Scanline timing (samples per line) for Martin 1 at 48 kHz, plus the sync /
 *     gap / pixel dwell samples.
 *  4. The per-component tone plan: Martin's start-of-line sync, Scottie's
 *     between-line sync (sync_on_first + sync_index), and the GBR/RGB channel
 *     order.
 *  5. The rendered audio itself: a zero-crossing frequency estimate of the
 *     calibration leader (1900 Hz) and of a uniform scanline (1500 and 2296 Hz),
 *     which closes the loop from tone plan to synthesised waveform.
 *
 * No hardware is involved. Live RF radiation is unverified.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_sstvtx.hpp"

#include <cstdint>
#include <vector>

using namespace mb200test;
using namespace app::sstvtx;

namespace {

/* Rising-edge (negative -> non-negative) zero-crossing frequency estimate over
 * a window. For a clean sine this equals cycles, so freq = risings*SR/N. */
double estimate_frequency(const std::vector<float>& audio, size_t start,
                          size_t len, uint32_t sample_rate) {
    if (start + len > audio.size() || len < 2) return 0.0;
    size_t risings = 0;
    for (size_t i = start + 1; i < start + len; i++) {
        if (audio[i - 1] < 0.0f && audio[i] >= 0.0f) risings++;
    }
    return static_cast<double>(risings) * static_cast<double>(sample_rate) /
           static_cast<double>(len);
}

/* A solid image with every channel set to `value`. */
std::vector<uint8_t> uniform_image(uint8_t value) {
    return std::vector<uint8_t>(kImageBytes, value);
}

}  // namespace

/* --- 1. VIS codes and parity --------------------------------------------- */

TEST(sstvtx_parity_matches_upstream) {
    /* Even parity in bit 7. Values worked out by hand from sstv_parity(). */
    CHECK_EQ(static_cast<int>(sstv_parity(60)), 60);    /* Scottie 1 */
    CHECK_EQ(static_cast<int>(sstv_parity(56)), 0xB8);  /* Scottie 2 */
    CHECK_EQ(static_cast<int>(sstv_parity(76)), 0xCC);  /* Scottie DX */
    CHECK_EQ(static_cast<int>(sstv_parity(44)), 0xAC);  /* Martin 1 */
    CHECK_EQ(static_cast<int>(sstv_parity(40)), 40);    /* Martin 2 */
    CHECK_EQ(static_cast<int>(sstv_parity(55)), 0xB7);  /* SC2-180 */
}

TEST(sstvtx_mode_table_vis_codes) {
    /* The table stores the parity-augmented byte; the low 7 bits are the
     * published VIS number. */
    CHECK_EQ(kModeCount, size_t{6});
    CHECK_STR_EQ(kModes[3].name, "Martin 1");
    CHECK_EQ(static_cast<int>(kModes[0].vis_code & 0x7F), 60);  /* Scottie 1 */
    CHECK_EQ(static_cast<int>(kModes[3].vis_code & 0x7F), 44);  /* Martin 1 */
    CHECK_EQ(static_cast<int>(kModes[5].vis_code & 0x7F), 55);  /* SC2-180 */
    CHECK_EQ(static_cast<int>(kModes[3].vis_code), 0xAC);       /* incl. parity */
}

TEST(sstvtx_vis_bit_tone_sequence_lsb_first) {
    /* Martin 1 VIS byte 0xAC = 0b10101100. Bits LSB-first are 0,0,1,1,0,1,0,1,
     * so the data tones are 1300,1300,1100,1100,1300,1100,1300,1100 with 1200
     * start/stop bits framing them (proc_sstvtx.cpp). */
    Encoder enc;
    enc.configure(kModes[3], 48000);  /* Martin 1 */
    const auto segs = enc.build_segments();

    /* seg[0..2] calibration, seg[3] VIS start, seg[4..11] data, seg[12] stop. */
    CHECK_NEAR(segs[3].frequency_hz, kVisStartHz, 1e-9);
    const double expect[8] = {kVisZeroHz, kVisZeroHz, kVisOneHz, kVisOneHz,
                              kVisZeroHz, kVisOneHz, kVisZeroHz, kVisOneHz};
    for (int i = 0; i < 8; i++)
        CHECK_NEAR(segs[4 + i].frequency_hz, expect[i], 1e-9);
    CHECK_NEAR(segs[12].frequency_hz, kVisStartHz, 1e-9);

    /* Each VIS bit is 30 ms = 1440 samples at 48 kHz. */
    CHECK_EQ(enc.vis_bit_samples(), uint32_t{1440});
    for (int i = 3; i <= 12; i++) CHECK_EQ(segs[i].samples, uint32_t{1440});
}

/* --- 2. Pixel -> tone mapping -------------------------------------------- */

TEST(sstvtx_pixel_frequency_mapping) {
    /* 1500 + luma*800/256 (integer division), the 1500..2300 Hz band. */
    CHECK_NEAR(pixel_frequency(0), 1500.0, 1e-9);
    CHECK_NEAR(pixel_frequency(128), 1900.0, 1e-9);   /* 1500 + 400 */
    CHECK_NEAR(pixel_frequency(255), 2296.0, 1e-9);   /* 1500 + 796 */

    /* Monotonic non-decreasing and always inside the band. */
    double prev = -1.0;
    for (int l = 0; l <= 255; l++) {
        const double f = pixel_frequency(static_cast<uint8_t>(l));
        CHECK(f >= 1500.0 && f <= 2300.0);
        CHECK(f >= prev);
        prev = f;
    }
}

/* --- 3. Scanline timing (Martin 1) --------------------------------------- */

TEST(sstvtx_martin1_timing) {
    Encoder enc;
    enc.configure(kModes[3], 48000);  /* Martin 1 */

    /* Dwell samples at 48 kHz. */
    CHECK_EQ(enc.pixel_samples(), uint32_t{22});  /* round(0.4576 * 48) */
    CHECK_EQ(enc.sync_samples(), uint32_t{233});  /* round(4.862 * 48)  */
    CHECK_EQ(enc.gap_samples(), uint32_t{27});    /* round(0.572 * 48)  */
    CHECK_EQ(enc.vis_bit_samples(), uint32_t{1440});

    /* One line = sync + 3 gaps + 3 * 320 pixels
     *          = 233 + 3*27 + 3*320*22 = 21434 samples. */
    CHECK_EQ(enc.samples_per_line(), uint32_t{21434});

    /* Whole transmission = calibration + VIS + 256 lines. */
    const uint64_t cal = 2ull * 14400 + 480;  /* 1900/300ms x2 + 1200/10ms */
    const uint64_t vis = 10ull * 1440;
    const uint64_t total = cal + vis + 256ull * 21434;
    CHECK_EQ(total, uint64_t{5530784});
    CHECK_EQ(enc.total_samples(), total);
}

TEST(sstvtx_total_matches_segment_sum) {
    /* total_samples() (analytic) must equal the sum of the built plan for every
     * mode, image or no image. */
    for (size_t mi = 0; mi < kModeCount; mi++) {
        Encoder enc;
        enc.configure(kModes[mi], 48000);
        const auto segs = enc.build_segments();
        uint64_t sum = 0;
        for (const auto& s : segs) sum += s.samples;
        CHECK_EQ(sum, enc.total_samples());
    }
}

/* --- 4. Tone plan structure ---------------------------------------------- */

TEST(sstvtx_martin1_line_starts_with_sync) {
    Encoder enc;
    enc.configure(kModes[3], 48000);  /* Martin 1: sync_index 0, gaps */
    enc.set_image(uniform_image(0).data(), kImageBytes);
    const auto segs = enc.build_segments();

    /* Calibration (3) + VIS (10) = 13 header segments; the first scanline
     * component then leads with a 1200 Hz sync and a 1500 Hz gap. */
    CHECK_NEAR(segs[13].frequency_hz, kSyncHz, 1e-9);
    CHECK_EQ(segs[13].samples, enc.sync_samples());
    CHECK_NEAR(segs[14].frequency_hz, kGapHz, 1e-9);
    CHECK_EQ(segs[14].samples, enc.gap_samples());
    /* Then 320 pixel tones. */
    CHECK_EQ(segs[15].samples, enc.pixel_samples());
}

TEST(sstvtx_scottie_sync_placement) {
    /* Scottie 1: sync_on_first true, sync_index 2. So component 0 of line 0
     * carries a sync (sync_on_first), component 1 does not, component 2 does. */
    Encoder enc;
    enc.configure(kModes[0], 48000);  /* Scottie 1 */
    enc.set_image(uniform_image(0).data(), kImageBytes);
    const auto segs = enc.build_segments();

    const uint32_t sync_n = enc.sync_samples();  /* 9 ms  -> 432 */
    const uint32_t gap_n = enc.gap_samples();    /* 1.5ms -> 72  */
    CHECK_EQ(sync_n, uint32_t{432});
    CHECK_EQ(gap_n, uint32_t{72});

    /* Header 13. sc0 = sync(13) + gap(14) + 320 px (15..334). */
    CHECK_NEAR(segs[13].frequency_hz, kSyncHz, 1e-9);
    CHECK_EQ(segs[13].samples, sync_n);
    CHECK_NEAR(segs[14].frequency_hz, kGapHz, 1e-9);

    /* sc1 begins at 335 with a gap, NOT a sync. */
    CHECK_NEAR(segs[335].frequency_hz, kGapHz, 1e-9);
    CHECK_EQ(segs[335].samples, gap_n);

    /* sc2 begins at 656 with the between-line sync. */
    CHECK_NEAR(segs[656].frequency_hz, kSyncHz, 1e-9);
    CHECK_EQ(segs[656].samples, sync_n);
    CHECK_NEAR(segs[657].frequency_hz, kGapHz, 1e-9);
}

TEST(sstvtx_gbr_channel_order) {
    /* Martin (GBR): component 0 -> G, 1 -> B, 2 -> R. Set line 0 pixel 0 to a
     * distinct R/G/B and confirm each component's first pixel tone reads the
     * right channel. */
    std::vector<uint8_t> img(kImageBytes, 0);
    img[0] = 10;   /* R */
    img[1] = 200;  /* G */
    img[2] = 50;   /* B */

    Encoder enc;
    enc.configure(kModes[3], 48000);  /* Martin 1 */
    enc.set_image(img.data(), img.size());
    const auto segs = enc.build_segments();

    /* comp0 (G=200): sc0 = sync(13)+gap(14)+pixels(15..334), first pixel seg15. */
    CHECK_NEAR(segs[15].frequency_hz, pixel_frequency(200), 1e-9);
    /* comp1 (B=50): sc1 = gap(335)+pixels(336..655), first pixel seg336. */
    CHECK_NEAR(segs[336].frequency_hz, pixel_frequency(50), 1e-9);
    /* comp2 (R=10): sc2 = gap(656)+pixels(657..976), first pixel seg657. */
    CHECK_NEAR(segs[657].frequency_hz, pixel_frequency(10), 1e-9);
}

TEST(sstvtx_rgb_channel_order) {
    /* SC2-180 (RGB, no gaps): component 0 -> R, 1 -> G, 2 -> B, and non-sync
     * components carry no gap tone. */
    std::vector<uint8_t> img(kImageBytes, 0);
    img[0] = 10;   /* R */
    img[1] = 200;  /* G */
    img[2] = 50;   /* B */

    Encoder enc;
    enc.configure(kModes[5], 48000);  /* SC2-180 */
    enc.set_image(img.data(), img.size());
    const auto segs = enc.build_segments();

    /* Header 13. sc0 (sync_index 0) = sync(13)+gap(14)+320 px R (15..334). */
    CHECK_NEAR(segs[13].frequency_hz, kSyncHz, 1e-9);
    CHECK_NEAR(segs[15].frequency_hz, pixel_frequency(10), 1e-9);  /* R */

    /* sc1: no gaps, so pixels start immediately at 335 with G. */
    CHECK_NEAR(segs[335].frequency_hz, pixel_frequency(200), 1e-9);  /* G */
    /* sc2: pixels at 335+320=655 with B. */
    CHECK_NEAR(segs[655].frequency_hz, pixel_frequency(50), 1e-9);  /* B */
}

/* --- 5. Boundary: no image ----------------------------------------------- */

TEST(sstvtx_no_image_is_all_black) {
    /* With no image set, every pixel is luma 0 -> 1500 Hz, and the plan is still
     * well-formed. */
    Encoder enc;
    enc.configure(kModes[3], 48000);  /* Martin 1 */
    const auto segs = enc.build_segments();

    CHECK_NEAR(segs[15].frequency_hz, 1500.0, 1e-9);  /* first pixel */

    uint64_t sum = 0;
    for (const auto& s : segs) sum += s.samples;
    CHECK_EQ(sum, enc.total_samples());
}

/* --- 6. Rendered audio --------------------------------------------------- */

TEST(sstvtx_rendered_calibration_and_scanline_frequencies) {
    /* Render the header and the first scanline of a uniform image, then measure
     * the tone frequencies in the synthesised audio. */
    const uint32_t sr = 48000;

    Encoder enc;
    enc.configure(kModes[3], sr);  /* Martin 1 */
    enc.set_image(uniform_image(0).data(), kImageBytes);  /* black -> 1500 Hz */
    enc.begin();

    std::vector<float> audio(51000);
    const size_t n = enc.fill(audio.data(), audio.size());
    CHECK_EQ(n, audio.size());

    /* Samples stay in range. */
    for (float s : audio) CHECK(s >= -1.0001f && s <= 1.0001f);

    /* Calibration leader is 1900 Hz for the first 14400 samples. */
    const double cal = estimate_frequency(audio, 1000, 12000, sr);
    CHECK_NEAR(cal, 1900.0, 30.0);

    /* First G scan begins at 29280(cal)+14400(vis)+233(sync)+27(gap) = 43940,
     * black pixels -> 1500 Hz, running 320*22 = 7040 samples. */
    const double scan = estimate_frequency(audio, 44500, 6000, sr);
    CHECK_NEAR(scan, 1500.0, 30.0);
}

TEST(sstvtx_rendered_white_scanline_is_top_of_band) {
    const uint32_t sr = 48000;

    Encoder enc;
    enc.configure(kModes[3], sr);  /* Martin 1 */
    enc.set_image(uniform_image(255).data(), kImageBytes);  /* white -> 2296 Hz */
    enc.begin();

    std::vector<float> audio(51000);
    enc.fill(audio.data(), audio.size());

    const double scan = estimate_frequency(audio, 44500, 6000, sr);
    CHECK_NEAR(scan, 2296.0, 30.0);
}

TEST(sstvtx_full_render_emits_exact_total) {
    /* Streaming the whole image emits exactly total_samples() and then stops. */
    Encoder enc;
    enc.configure(kModes[3], 48000);  /* Martin 1 */
    enc.set_image(uniform_image(128).data(), kImageBytes);
    enc.begin();

    const uint64_t total = enc.total_samples();
    std::vector<float> buf(4096);
    uint64_t emitted = 0;
    for (;;) {
        const size_t got = enc.fill(buf.data(), buf.size());
        if (got == 0) break;
        for (size_t i = 0; i < got; i++)
            CHECK(buf[i] >= -1.0001f && buf[i] <= 1.0001f);
        emitted += got;
    }
    CHECK_EQ(emitted, total);
    CHECK_EQ(enc.emitted(), total);
    CHECK(enc.done());
}
