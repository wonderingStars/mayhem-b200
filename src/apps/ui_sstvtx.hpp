/*
 * mayhem-b200 — SSTV transmitter.
 *
 * Host port of firmware/application/external/sstvtx/* together with the M4
 * baseband processor firmware/baseband/proc_sstvtx.cpp and the shared mode
 * table firmware/common/sstv.hpp.
 *
 * Upstream splits the job across two cores:
 *
 *   M0 (ui_sstvtx.cpp)      reads a 320x256 24-bit BMP from /sstv, and for each
 *                           colour component of each line fills an sstv_scanline
 *                           (optional 1200 Hz sync tone, a 1500 Hz gap tone, and
 *                           320 luma bytes) which it hands to the baseband over a
 *                           FIFO.
 *   M4 (proc_sstvtx.cpp)    walks a small FSM — calibration leader, VIS header,
 *                           then per scanline sync / gap / pixel tones — turning
 *                           each into a tone frequency, and FM-modulates a
 *                           carrier with that tone at 3.072 Msps.
 *
 * On the host there is no second core. The whole tone plan is produced by
 * `sstvtx::Encoder`, a pure class with no UI or radio dependency, and rendered
 * to a mono audio waveform in [-1, 1] at audio::sample_rate. That waveform is
 * fed to radio::TransmitterModel as an AudioSource; the transmit chain's FM
 * modulator plays the role of proc_sstvtx's FM stage, so the audio the encoder
 * produces *is* the SSTV signal (exactly what an SSTV operator would feed into
 * an SSB/FM rig). Because the encoder is self-contained it is tested against the
 * spec directly (tests/test_sstvtx.cpp): the VIS codes, the pixel -> tone
 * mapping and the scanline timing.
 *
 * Faithfulness notes, all verified against the upstream sources:
 *
 *  - The mode table, sync/gap/pixel timings, VIS codes (via the exact port of
 *    sstv_parity()) and colour sequences are common/sstv.hpp byte-for-byte, with
 *    the fixed-3.072-MHz sample counts re-expressed as the millisecond durations
 *    they encode so they can be rendered at any host sample rate.
 *  - The tone frequencies are proc_sstvtx.cpp's: calibration 1900/1200/1900,
 *    VIS start/stop 1200 with data bits 1100 (=1) / 1300 (=0) LSB-first, sync
 *    1200, gap 1500, and pixels 1500 + luma*800/256 (integer division, so
 *    luma 255 -> 2296 Hz), which is the 1500..2300 Hz scanline band.
 *  - The per-component sequencing (sync_on_first / sync_index / gaps) reproduces
 *    prepare_scanline() + the execute() FSM, so Scottie's between-line sync and
 *    Martin's start-of-line sync land in the right place.
 *
 * Host deviations, documented at the point of use:
 *
 *  - Upstream indexes a 256-entry int8 sine table with a 32-bit phase
 *    accumulator; here the sine is computed in double, which removes the
 *    quantisation without changing the tone frequencies.
 *  - The BMP is loaded through core::BmpFile, whose pixels are RGB565, so the
 *    per-channel luma the *view* feeds the encoder is quantised to 5/6 bits.
 *    The encoder itself is full 8-bit and is tested at 8-bit precision.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SSTVTX_H__
#define __MB200_UI_SSTVTX_H__

#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace app {
namespace sstvtx {

/* ======================================================================== *
 *  Pure SSTV encoder — no UI, no radio. Ported from common/sstv.hpp,        *
 *  application/external/sstvtx/ui_sstvtx.cpp and baseband/proc_sstvtx.cpp.  *
 * ======================================================================== */

/* Even parity in bit 7, exactly common/sstv.hpp's sstv_parity(). The magic
 * constant is the 16-entry parity lookup for a nibble. */
constexpr uint8_t sstv_parity(uint8_t code) {
    uint8_t out = code;
    out ^= code >> 4;
    out &= 0x0F;
    return static_cast<uint8_t>((((0b0110100110010110 >> out) & 1) << 7) | code);
}

enum class ColorSeq : uint8_t {
    RGB,  /* transmit order R, G, B */
    GBR,  /* transmit order G, B, R */
};

/* One SSTV mode. Timings are in milliseconds (the arguments common/sstv.hpp
 * passes to its SSTV_MS2S macro); vis_code already includes the parity bit. */
struct Mode {
    const char* name;
    uint8_t vis_code;
    ColorSeq color_sequence;
    uint16_t pixels;       /* 320 */
    uint16_t lines;        /* 256 */
    double pixel_ms;       /* per-pixel dwell */
    bool sync_on_first;    /* extra sync before the very first component */
    uint8_t sync_index;    /* component (0..2) that carries the line sync */
    bool gaps;             /* non-sync components get a gap tone too */
    double sync_ms;
    double gap_ms;
};

/* common/sstv.hpp's sstv_modes[], byte-for-byte. Scottie 1 first so a default
 * index of Scottie/Martin is available; the view preselects Martin 1. */
inline constexpr Mode kModes[] = {
    {"Scottie 1", sstv_parity(60), ColorSeq::GBR, 320, 256, 0.4320, true, 2, true, 9.0, 1.5},
    {"Scottie 2", sstv_parity(56), ColorSeq::GBR, 320, 256, 0.2752, true, 2, true, 9.0, 1.5},
    {"Scottie DX", sstv_parity(76), ColorSeq::GBR, 320, 256, 1.08, true, 2, true, 9.0, 1.5},
    {"Martin 1", sstv_parity(44), ColorSeq::GBR, 320, 256, 0.4576, false, 0, true, 4.862, 0.572},
    {"Martin 2", sstv_parity(40), ColorSeq::GBR, 320, 256, 0.2288, false, 0, true, 4.862, 0.572},
    {"SC2-180", sstv_parity(55), ColorSeq::RGB, 320, 256, 0.7344, false, 0, false, 5.5225, 0.5},
};
inline constexpr size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

inline constexpr uint16_t kPixelsPerLine = 320;
inline constexpr uint16_t kLinesPerImage = 256;
inline constexpr size_t kImageBytes =
    static_cast<size_t>(kPixelsPerLine) * kLinesPerImage * 3;  /* RGB */

/* Tone frequencies (Hz), from proc_sstvtx.cpp. */
inline constexpr double kVisStartHz = 1200.0;  /* VIS start/stop bit */
inline constexpr double kVisZeroHz = 1300.0;   /* VIS data bit == 0 */
inline constexpr double kVisOneHz = 1100.0;    /* VIS data bit == 1 */
inline constexpr double kSyncHz = 1200.0;
inline constexpr double kGapHz = 1500.0;
inline constexpr double kPixelBaseHz = 1500.0;  /* luma 0 */
inline constexpr double kPixelSpanHz = 800.0;   /* luma 255 -> +796 -> 2296 */
inline constexpr double kVisBitMs = 30.0;

/* Calibration leader: 1900 Hz 300 ms, 1200 Hz 10 ms, 1900 Hz 300 ms. */
inline constexpr double kCalLeaderHz = 1900.0;
inline constexpr double kCalBreakHz = 1200.0;
inline constexpr double kCalLeaderMs = 300.0;
inline constexpr double kCalBreakMs = 10.0;

inline constexpr double kPi = 3.14159265358979323846;

/* proc_sstvtx.cpp: SSTV_F2D(1500 + ((luma * 800) / 256)) — integer division. */
constexpr double pixel_frequency(uint8_t luma) {
    return kPixelBaseHz +
           static_cast<double>((static_cast<uint32_t>(luma) * 800u) / 256u);
}

/* Which RGB channel (0=R, 1=G, 2=B) component `component` (0..2) transmits, for
 * a given colour sequence. Mirrors ui_sstvtx.cpp's component_map after undoing
 * its BMP-is-BGR indexing:
 *   RGB: comp0->R, comp1->G, comp2->B
 *   GBR: comp0->G, comp1->B, comp2->R */
constexpr uint8_t channel_for_component(ColorSeq seq, uint8_t component) {
    if (seq == ColorSeq::RGB)
        return component;  /* {0,1,2} */
    constexpr uint8_t gbr[3] = {1, 2, 0};
    return gbr[component % 3];
}

/* One constant-frequency tone segment. */
struct Segment {
    double frequency_hz;
    uint32_t samples;
};

class Encoder {
   public:
    Encoder() = default;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    /* Selects the mode and audio sample rate. Resets any streaming state. */
    void configure(const Mode& mode, uint32_t sample_rate = 48000) {
        mode_ = mode;
        sample_rate_ = sample_rate ? sample_rate : 48000;
        built_ = false;
        segments_.clear();
        seg_index_ = 0;
        seg_remaining_ = 0;
        phase_ = 0.0;
        emitted_.store(0);
    }

    /* Sets the source image as `kImageBytes` RGB triplets, top-to-bottom, 320
     * wide. A shorter buffer is padded with black (luma 0 -> 1500 Hz), so an
     * absent image still produces a valid, all-black transmission. */
    void set_image(const uint8_t* rgb, size_t len) {
        image_.assign(rgb, rgb + std::min(len, kImageBytes));
        built_ = false;
    }
    void clear_image() {
        image_.clear();
        built_ = false;
    }

    const Mode& mode() const { return mode_; }
    uint32_t sample_rate() const { return sample_rate_; }

    /* --- Deterministic timing helpers (tested) --- */

    uint32_t samples_for_ms(double ms) const {
        return static_cast<uint32_t>(
            std::llround(ms * static_cast<double>(sample_rate_) / 1000.0));
    }
    uint32_t pixel_samples() const { return samples_for_ms(mode_.pixel_ms); }
    uint32_t sync_samples() const { return samples_for_ms(mode_.sync_ms); }
    uint32_t gap_samples() const { return samples_for_ms(mode_.gap_ms); }
    uint32_t vis_bit_samples() const { return samples_for_ms(kVisBitMs); }

    /* Samples in one steady-state line (three colour components): the sync
     * component contributes sync+gap, each other component a gap when the mode
     * has gaps, plus three full pixel scans. */
    uint32_t samples_per_line() const {
        uint32_t s = sync_samples() + gap_samples();
        if (mode_.gaps) s += 2u * gap_samples();
        s += 3u * static_cast<uint32_t>(kPixelsPerLine) * pixel_samples();
        return s;
    }

    /* Total samples of a whole transmission (calibration + VIS + all lines).
     * Independent of the image, so the view can size a progress bar before an
     * image is chosen. */
    uint64_t total_samples() const {
        uint64_t t = 2ull * samples_for_ms(kCalLeaderMs) + samples_for_ms(kCalBreakMs);
        t += 10ull * vis_bit_samples();  /* start + 8 data + stop */
        const uint32_t comps = static_cast<uint32_t>(mode_.lines) * 3u;
        for (uint32_t sc = 0; sc < comps; sc++) {
            const uint8_t comp = static_cast<uint8_t>(sc % 3);
            const bool is_sync =
                (sc == 0 && mode_.sync_on_first) || (comp == mode_.sync_index);
            if (is_sync)
                t += sync_samples() + gap_samples();
            else if (mode_.gaps)
                t += gap_samples();
            t += static_cast<uint64_t>(kPixelsPerLine) * pixel_samples();
        }
        return t;
    }

    /* Builds the full tone plan in transmit order. This is the encoder's
     * output: calibration leader, VIS header, then per component an optional
     * sync tone, an optional gap tone and 320 pixel tones. */
    std::vector<Segment> build_segments() const {
        std::vector<Segment> segs;

        /* Calibration leader. */
        segs.push_back({kCalLeaderHz, samples_for_ms(kCalLeaderMs)});
        segs.push_back({kCalBreakHz, samples_for_ms(kCalBreakMs)});
        segs.push_back({kCalLeaderHz, samples_for_ms(kCalLeaderMs)});

        /* VIS: 1200 start bit, 8 data bits LSB-first (1->1100, 0->1300), 1200
         * stop bit. vis_code already carries the parity bit in bit 7. */
        const uint32_t vbit = vis_bit_samples();
        segs.push_back({kVisStartHz, vbit});
        for (int b = 0; b < 8; b++) {
            const bool one = (mode_.vis_code >> b) & 1u;
            segs.push_back({one ? kVisOneHz : kVisZeroHz, vbit});
        }
        segs.push_back({kVisStartHz, vbit});

        /* Scanlines. */
        const uint32_t comps = static_cast<uint32_t>(mode_.lines) * 3u;
        const uint32_t sync_n = sync_samples();
        const uint32_t gap_n = gap_samples();
        const uint32_t pix_n = pixel_samples();
        for (uint32_t sc = 0; sc < comps; sc++) {
            const uint32_t line = sc / 3;
            const uint8_t comp = static_cast<uint8_t>(sc % 3);
            const bool is_sync =
                (sc == 0 && mode_.sync_on_first) || (comp == mode_.sync_index);

            if (is_sync) {
                segs.push_back({kSyncHz, sync_n});
                segs.push_back({kGapHz, gap_n});
            } else if (mode_.gaps) {
                segs.push_back({kGapHz, gap_n});
            }

            const uint8_t ch = channel_for_component(mode_.color_sequence, comp);
            for (uint32_t x = 0; x < kPixelsPerLine; x++) {
                segs.push_back({pixel_frequency(luma_at(line, x, ch)), pix_n});
            }
        }
        return segs;
    }

    /* --- Streaming render --- */

    /* Rebuilds the plan and rewinds. Call before the first fill(); fill() will
     * also do it lazily if you do not. */
    void begin() {
        segments_ = build_segments();
        seg_index_ = 0;
        seg_remaining_ = segments_.empty() ? 0 : segments_[0].samples;
        phase_ = 0.0;
        emitted_.store(0);
        total_ = total_samples();
        built_ = true;
    }

    /* Writes up to `count` mono samples in [-1, 1] and returns how many were
     * written. Returns 0 once the whole image has been sent. Safe to call from
     * the DSP thread once begin() has run on another thread. */
    size_t fill(float* out, size_t count) {
        if (!built_) begin();
        size_t written = 0;
        while (written < count) {
            while (seg_index_ < segments_.size() && seg_remaining_ == 0) {
                seg_index_++;
                if (seg_index_ < segments_.size())
                    seg_remaining_ = segments_[seg_index_].samples;
            }
            if (seg_index_ >= segments_.size()) break;

            const double step = segments_[seg_index_].frequency_hz /
                                static_cast<double>(sample_rate_);
            uint64_t done_here = 0;
            while (written < count && seg_remaining_ > 0) {
                out[written++] = static_cast<float>(std::sin(2.0 * kPi * phase_));
                phase_ += step;
                if (phase_ >= 1.0) phase_ -= 1.0;
                seg_remaining_--;
                done_here++;
            }
            emitted_.fetch_add(done_here);
        }
        return written;
    }

    bool built() const { return built_; }
    bool done() const { return built_ && emitted_.load() >= total_; }
    uint64_t emitted() const { return emitted_.load(); }
    double progress() const {
        if (!built_ || total_ == 0) return 0.0;
        const double p = static_cast<double>(emitted_.load()) /
                         static_cast<double>(total_);
        return p > 1.0 ? 1.0 : p;
    }

   private:
    /* 8-bit luma of channel `ch` at (line, x). Black when the image is short. */
    uint8_t luma_at(uint32_t line, uint32_t x, uint8_t ch) const {
        const size_t idx =
            (static_cast<size_t>(line) * kPixelsPerLine + x) * 3 + ch;
        return idx < image_.size() ? image_[idx] : uint8_t{0};
    }

    Mode mode_{kModes[3]};  /* Martin 1 */
    uint32_t sample_rate_{48000};
    std::vector<uint8_t> image_{};

    std::vector<Segment> segments_{};
    size_t seg_index_{0};
    uint32_t seg_remaining_{0};
    double phase_{0.0};  /* turns, [0, 1) */
    std::atomic<uint64_t> emitted_{0};
    uint64_t total_{0};
    bool built_{false};
};

}  // namespace sstvtx

/* ======================================================================== *
 *  View                                                                     *
 * ======================================================================== */

/* Small on-screen preview of the loaded BMP, downscaled to the widget size.
 * Mirrors the SstvImage widget the receiver uses. */
class SstvTxImage : public ui::Widget {
   public:
    explicit SstvTxImage(ui::Rect parent_rect);

    void clear();
    /* `rgb` is kImageBytes bytes, 320x256 top-to-bottom RGB. */
    void set_image(const uint8_t* rgb);

    void paint(ui::Painter& painter) override;

   private:
    int width_{1};
    int height_{1};
    std::vector<ui::Color> pixels_{};
};

class SstvTxView : public ui::View {
   public:
    SstvTxView();
    ~SstvTxView() override;

    SstvTxView(const SstvTxView&) = delete;
    SstvTxView& operator=(const SstvTxView&) = delete;

    std::string title() const override { return "SSTV TX"; }

    void focus() override;
    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void populate_bitmaps();
    bool load_bitmap(size_t index);
    void set_mode(size_t index);
    void start_tx();
    void stop_tx();
    void set_status(const std::string& s);

    sstvtx::Encoder encoder_{};
    std::vector<uint8_t> image_rgb_{};  /* kImageBytes, filled from the BMP */
    std::vector<std::string> bitmap_names_{};
    size_t mode_index_{3};  /* Martin 1 */
    bool transmitting_{false};
    bool image_loaded_{false};

    std::string sstv_dir_{};

    ui::Labels labels_{
        {{1 * 8, 1 * 8}, "File:", ui::Color::light_grey()},
        {{1 * 8, 3 * 8}, "Mode:", ui::Color::light_grey()},
        {{0, 5 * 8}, "Freq", ui::Color::light_grey()},
        {{0, 7 * 8}, "TX gain", ui::Color::light_grey()},
    };

    ui::OptionsField options_bitmaps_{{6 * 8, 1 * 8}, 20, {}};
    ui::OptionsField options_modes_{{6 * 8, 3 * 8}, 12, {}};

    ui::FrequencyField field_frequency_{{6 * 8, 5 * 8}};
    ui::NumberField field_gain_{{9 * 8, 7 * 8}, 3, {0, 89}, 1, ' '};

    ui::Button button_start_{{0, 9 * 8, 10 * 8, 3 * 8}, "Start"};
    ui::ProgressBar progressbar_{{16, 13 * 8, 208, 16}};
    ui::Text text_status_{{0, 16 * 8, 240, 16}, ""};

    SstvTxImage preview_{{40, 18 * 8, 160, 128}};
};

}  // namespace app

#endif /*__MB200_UI_SSTVTX_H__*/
