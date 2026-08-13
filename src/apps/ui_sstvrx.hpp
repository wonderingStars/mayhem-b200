/*
 * mayhem-b200 — SSTV (slow-scan television) receiver.
 *
 * Ported from firmware/application/external/sstvrx/ (ui_sstvrx.*),
 * firmware/baseband/proc_sstvrx.{hpp,cpp} and firmware/common/sstv.hpp.
 * Upstream runs the decimation chain, the FM discriminator, a four-bin Goertzel
 * tone estimator, sync detection and the scanline-to-pixel mapping on the M4,
 * and ships finished RGB lines to the M0 through a 512-byte shared buffer in
 * two chunks per line. The M0 draws them and streams them into a BMP.
 *
 * There is no second core here, so the whole chain runs on the UI thread and
 * the chunking disappears: a completed line is handed straight to a callback.
 *
 * PIPELINE
 *
 *   complex baseband @ channel rate
 *     -> FM discriminator (dsp::FmDemod, 7.5 kHz deviation as upstream sets it)
 *     -> ToneEstimator: quadrature mix to 1750 Hz, lowpass, instantaneous
 *        frequency from the phase difference           -> tone frequency in Hz
 *     -> VisDecoder      : 1900/1200 header, 8 x 30 ms bits, even parity
 *                          -> VIS byte -> Mode
 *     -> LineDecoder     : 1200 Hz sync detect, porch/separator gaps, per-mode
 *                          pixel clock, 1500 Hz = black .. 2300 Hz = white
 *                          -> 320 x RGB scan line
 *     -> BMP row (core::BmpFile) and the on-screen image
 *
 * WHAT IS PORTED EXACTLY
 *   - the mode table and its VIS codes, including sstv_parity() (common/sstv.hpp)
 *   - freq_to_pixel(): clamp to 1500..2300 Hz, linear to 0..255
 *   - sync detection: 1200 Hz +/- 150 Hz, accepted at 1/3 of the nominal sync
 *     length, two syncs required before the first line
 *   - the fractional pixel clock with its phase/slant adjustments, the channel
 *     gap handling, and the colour-sequence mapping (RGB {0,1,2}, GBR {1,2,0})
 *
 * DEVIATIONS, and why:
 *
 *  1. VIS decoding. Upstream's STATE_VIS_DECODE is a stub — the comment says
 *     "VIS code detection not implemented yet" and it falls straight through to
 *     the separator wait, so the mode has to be picked by hand in the UI. This
 *     port implements the VIS header from the SSTV specification (1900 Hz
 *     leader 300 ms, 1200 Hz break 10 ms, 1900 Hz leader 300 ms, 1200 Hz start
 *     bit 30 ms, eight 30 ms data bits least significant first with 1100 Hz = 1
 *     and 1300 Hz = 0, even parity in bit 7, 1200 Hz stop bit) and looks the
 *     resulting byte up in upstream's own table. Manual selection is kept.
 *
 *  2. Tone estimation. Upstream runs four Goertzel bins (1200/1500/1900/2300)
 *     and picks the strongest with a crude neighbour interpolation, which
 *     quantises luminance onto a handful of levels — the greys between 1500 and
 *     2300 Hz have only the 1900 Hz bin between them. This port measures the
 *     instantaneous frequency instead (quadrature mix to the 1750 Hz band
 *     centre, lowpass, phase difference). It is the same measurement the sync
 *     detector and freq_to_pixel() were written against, just accurate; the
 *     protocol is untouched.
 *
 *  3. Scottie channel order. Upstream's mode table carries `sync_on_first` and
 *     `sync_index`, but proc_sstvrx.cpp ignores them and always restarts at
 *     channel 0 after a sync. Scottie sends sync between the blue and red
 *     scans, so upstream writes red data into the green buffer. This port
 *     starts at `sync_index` and wraps, which is what those fields are for.
 *     Residual: Scottie's red scan belongs to the row above the green and blue
 *     that follow it, so red is one line out. Correcting that needs the line
 *     buffer to straddle a sync; noted, not done.
 *
 *  4. Upstream's process_line() resets to line 1 when the frame wraps, leaving
 *     row 0 unwritten. This resets to 0.
 *
 *  5. All of upstream's 0xFFF0-and-up debug messages through the progress queue
 *     are dropped; the host has no queue and the same information is on screen.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek (common/sstv.hpp mode table)
 * Copyright (C) 2025 StarVore Labs (original app and baseband processor)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SSTVRX_H__
#define __MB200_UI_SSTVRX_H__

#include "../core/bmp_file.hpp"
#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace app {
namespace sstv {

/* ===========================================================================
 * Mode table (firmware/common/sstv.hpp)
 * ===========================================================================*/

/* Upstream's sstv_parity(): folds the byte to a nibble and looks the parity up
 * in a 16-bit constant, then places it in bit 7. SSTV VIS uses even parity, so
 * bit 7 is the XOR of bits 0..6. */
constexpr uint8_t vis_parity(uint8_t code) {
    uint8_t out = static_cast<uint8_t>(code ^ (code >> 4));
    out = static_cast<uint8_t>(out & 0x0F);
    return static_cast<uint8_t>((((0b0110100110010110 >> out) & 1) << 7) | code);
}

/* True if the parity bit of a received VIS byte agrees with its seven data
 * bits. */
constexpr bool vis_parity_ok(uint8_t byte) {
    return vis_parity(static_cast<uint8_t>(byte & 0x7F)) == byte;
}

enum class ColorSeq : uint8_t {
    Rgb = 0,
    Gbr = 1,
    Yuv = 2, /* upstream declares it; no mode in the table uses it */
};

struct Mode {
    const char* name;
    uint8_t vis_code; /* including the parity bit, as upstream stores it */
    bool color;
    ColorSeq color_sequence;
    uint16_t pixels;
    uint16_t lines;
    /* Upstream expresses these as sample counts at its 3.072 MHz transmit rate
     * via SSTV_MS2S(); milliseconds are the same numbers without the rate
     * baked in, which matters because the host runs at whatever the B200 gives
     * it. The values are upstream's, unchanged. */
    float pixel_time_ms;
    bool sync_on_first;
    uint8_t sync_index;
    bool gaps;
    float sync_time_ms;
    float gap_time_ms;
};

inline constexpr size_t kModeCount = 6;

inline constexpr Mode kModes[kModeCount] = {
    {"Scottie 1", vis_parity(60), true, ColorSeq::Gbr, 320, 256, 0.4320f, true, 2, true, 9.0f, 1.5f},
    {"Scottie 2", vis_parity(56), true, ColorSeq::Gbr, 320, 256, 0.2752f, true, 2, true, 9.0f, 1.5f},
    {"Scottie DX", vis_parity(76), true, ColorSeq::Gbr, 320, 256, 1.08f, true, 2, true, 9.0f, 1.5f},
    {"Martin 1", vis_parity(44), true, ColorSeq::Gbr, 320, 256, 0.4576f, false, 0, true, 4.862f, 0.572f},
    {"Martin 2", vis_parity(40), true, ColorSeq::Gbr, 320, 256, 0.2288f, false, 0, true, 4.862f, 0.572f},
    {"SC2-180", vis_parity(55), true, ColorSeq::Rgb, 320, 256, 0.7344f, false, 0, false, 5.5225f, 0.5f},
};

/* Upstream's find_mode_by_vis_code(). Null if the byte is not a mode this
 * build knows. */
inline const Mode* mode_for_vis(uint8_t vis_code) {
    for (const auto& m : kModes)
        if (m.vis_code == vis_code) return &m;
    return nullptr;
}

inline const Mode* mode_by_index(size_t index) {
    return (index < kModeCount) ? &kModes[index] : nullptr;
}

/* Upstream's color_order_for_mode(): index by channel position, value is the
 * colour plane (0 = R, 1 = G, 2 = B). */
inline void color_order_for(const Mode& mode, uint8_t out[3]) {
    if (mode.color_sequence == ColorSeq::Gbr) {
        out[0] = 1;
        out[1] = 2;
        out[2] = 0;
    } else {
        out[0] = 0;
        out[1] = 1;
        out[2] = 2;
    }
}

/* ===========================================================================
 * Tone frequencies (proc_sstvrx.hpp)
 * ===========================================================================*/

inline constexpr int32_t kFreqBlack = 1500;
inline constexpr int32_t kFreqWhite = 2300;
inline constexpr int32_t kFreqSync = 1200;
inline constexpr int32_t kFreqVisZero = 1300;
inline constexpr int32_t kFreqVisOne = 1100;
inline constexpr int32_t kFreqLeader = 1900;

/* Upstream SSTVRXProcessor::freq_to_pixel(). */
constexpr uint8_t freq_to_pixel(int32_t freq) {
    if (freq < kFreqBlack) freq = kFreqBlack;
    if (freq > kFreqWhite) freq = kFreqWhite;

    int32_t pixel = ((freq - kFreqBlack) * 255) / (kFreqWhite - kFreqBlack);
    if (pixel < 0) pixel = 0;
    if (pixel > 255) pixel = 255;
    return static_cast<uint8_t>(pixel);
}

/* ===========================================================================
 * Tone estimator (deviation 2)
 *
 * Quadrature mix of the discriminator audio down by the 1750 Hz centre of the
 * SSTV tone band, a lowpass that keeps +/-900 Hz and kills the sum image at
 * 2.85 kHz and above, then instantaneous frequency from the phase difference.
 * ===========================================================================*/

inline constexpr float kToneCentreHz = 1750.0f;

class ToneEstimator {
   public:
    void configure(float sample_rate_hz) {
        sample_rate_hz_ = (sample_rate_hz > 0.0f) ? sample_rate_hz : 48000.0f;

        const auto taps = dsp::design_lowpass(900.0, 1900.0,
                                              static_cast<double>(sample_rate_hz_), 60.0);
        lp_i_.configure(taps);
        lp_q_.configure(taps);
        group_delay_ = (taps.size() - 1) / 2;

        phase_step_ = -2.0 * 3.14159265358979323846 * static_cast<double>(kToneCentreHz) /
                      static_cast<double>(sample_rate_hz_);
        hz_per_radian_ = sample_rate_hz_ / (2.0f * 3.14159265358979323846f);
        reset();
    }

    void reset() {
        lp_i_.reset();
        lp_q_.reset();
        phase_ = 0.0;
        prev_ = dsp::cfloat{0.0f, 0.0f};
        primed_ = false;
        last_hz_ = kFreqSync;
    }

    /* One audio sample in, one tone-frequency estimate in Hz out. */
    int32_t process(float audio) {
        const float c = static_cast<float>(std::cos(phase_));
        const float s = static_cast<float>(std::sin(phase_));
        phase_ += phase_step_;
        if (phase_ < -6.283185307179586) phase_ += 6.283185307179586;
        if (phase_ > 6.283185307179586) phase_ -= 6.283185307179586;

        const dsp::cfloat z{lp_i_.process_one(audio * c), lp_q_.process_one(audio * s)};

        if (!primed_) {
            prev_ = z;
            primed_ = true;
            return last_hz_;
        }

        const dsp::cfloat d = z * std::conj(prev_);
        prev_ = z;

        /* A dead input gives atan2(0,0) == 0, i.e. the band centre; hold the
         * previous estimate instead so silence does not read as 1750 Hz. */
        if (std::fabs(d.real()) < 1e-20f && std::fabs(d.imag()) < 1e-20f) return last_hz_;

        const float offset = std::atan2(d.imag(), d.real()) * hz_per_radian_;
        last_hz_ = static_cast<int32_t>(std::lround(kToneCentreHz + offset));
        return last_hz_;
    }

    /* Samples of lag the lowpass adds. Sync detection and the pixel clock both
     * see it, so it cancels out of the horizontal alignment. */
    size_t group_delay() const { return group_delay_; }

   private:
    dsp::FirR lp_i_{};
    dsp::FirR lp_q_{};
    dsp::cfloat prev_{0.0f, 0.0f};
    double phase_{0.0};
    double phase_step_{0.0};
    float sample_rate_hz_{48000.0f};
    float hz_per_radian_{1.0f};
    size_t group_delay_{0};
    int32_t last_hz_{kFreqSync};
    bool primed_{false};
};

/* ===========================================================================
 * VIS decoder (deviation 1)
 * ===========================================================================*/

class VisDecoder {
   public:
    void configure(float sample_rate_hz) {
        sample_rate_hz_ = (sample_rate_hz > 0.0f) ? sample_rate_hz : 48000.0f;

        /* The specification says 300 ms of leader; accepting 100 ms lets the
         * decoder latch on when it is started part-way through a header, and
         * a false latch still fails the parity and stop-bit checks. */
        leader_min_samples_ = ms_to_samples(100.0f);
        break_min_samples_ = ms_to_samples(5.0f);
        start_min_samples_ = ms_to_samples(20.0f);
        bit_samples_ = ms_to_samples(30.0f);
        lost_limit_samples_ = ms_to_samples(60.0f);
        reset();
    }

    void reset() {
        state_ = State::Leader1;
        run_ = 0;
        lost_ = 0;
        bit_index_ = 0;
        bit_timer_ = 0;
        accumulator_ = 0;
        code_ = 0;
        parity_ok_ = false;
    }

    /* One tone estimate in. Returns true on the sample that completes a VIS
     * byte; code() and parity_ok() then describe it. */
    bool process(int32_t hz) {
        switch (state_) {
            case State::Leader1:
            case State::Leader2:
                if (is_leader(hz)) {
                    if (++run_ >= leader_min_samples_) {
                        state_ = (state_ == State::Leader1) ? State::Break : State::Start;
                        run_ = 0;
                        lost_ = 0;
                    }
                } else {
                    run_ = 0;
                }
                break;

            case State::Break:
                /* The 10 ms break between the two leader halves. */
                if (is_sync(hz)) {
                    if (++run_ >= break_min_samples_) {
                        state_ = State::Leader2;
                        run_ = 0;
                        lost_ = 0;
                    }
                } else if (is_leader(hz)) {
                    run_ = 0;
                    lost_ = 0;
                } else if (++lost_ > lost_limit_samples_) {
                    reset();
                }
                break;

            case State::Start:
                /* The 30 ms start bit, again at 1200 Hz. The byte's bit windows
                 * are measured from the moment it ends. */
                if (is_sync(hz)) {
                    run_++;
                    lost_ = 0;
                } else if (run_ >= start_min_samples_) {
                    state_ = State::Bits;
                    bit_index_ = 0;
                    bit_timer_ = 0;
                    accumulator_ = 0;
                    code_ = 0;
                    /* The first sample after the start bit is bit 0's first
                     * sample, so it has already been consumed below. */
                    return accumulate_bit(hz);
                } else if (is_leader(hz)) {
                    run_ = 0;
                    lost_ = 0;
                } else if (++lost_ > lost_limit_samples_) {
                    reset();
                }
                break;

            case State::Bits:
                return accumulate_bit(hz);
        }
        return false;
    }

    uint8_t code() const { return code_; }
    bool parity_ok() const { return parity_ok_; }

   private:
    enum class State : uint8_t { Leader1, Break, Leader2, Start, Bits };

    uint32_t ms_to_samples(float ms) const {
        const float n = sample_rate_hz_ * ms / 1000.0f;
        return static_cast<uint32_t>(n < 1.0f ? 1.0f : n);
    }

    static bool is_leader(int32_t hz) { return hz > 1750 && hz < 2050; }
    /* Narrower than the sync detector's +/-150 Hz: the VIS bit tones sit at
     * 1100 and 1300, so the window has to exclude them. */
    static bool is_sync(int32_t hz) { return hz > 1150 && hz < 1250; }

    /* Averages each 30 ms window and slices it at 1200 Hz: below is a 1
     * (1100 Hz), above is a 0 (1300 Hz), exactly as the specification defines
     * the VIS bit tones. */
    bool accumulate_bit(int32_t hz) {
        accumulator_ += hz;
        bit_timer_++;
        if (bit_timer_ < bit_samples_) return false;

        const int32_t avg = accumulator_ / static_cast<int32_t>(bit_timer_);
        const uint8_t bit = (avg < kFreqSync) ? 1u : 0u;
        code_ = static_cast<uint8_t>(code_ | (bit << bit_index_));

        accumulator_ = 0;
        bit_timer_ = 0;
        bit_index_++;

        if (bit_index_ >= 8) {
            parity_ok_ = vis_parity_ok(code_);
            state_ = State::Leader1;
            run_ = 0;
            lost_ = 0;
            bit_index_ = 0;
            return true;
        }
        return false;
    }

    State state_{State::Leader1};
    float sample_rate_hz_{48000.0f};

    uint32_t leader_min_samples_{4800};
    uint32_t break_min_samples_{240};
    uint32_t start_min_samples_{960};
    uint32_t bit_samples_{1440};
    uint32_t lost_limit_samples_{2880};

    uint32_t run_{0};
    uint32_t lost_{0};
    uint32_t bit_timer_{0};
    int32_t accumulator_{0};
    uint8_t bit_index_{0};
    uint8_t code_{0};
    bool parity_ok_{false};
};

/* ===========================================================================
 * Line decoder
 *
 * Port of SSTVRXProcessor's detect_sync / begin_line_after_sync / start_gap /
 * process_pixel_sample / store_pixel_value / process_line.
 * ===========================================================================*/

inline constexpr uint16_t kPixelsPerLine = 320;

class LineDecoder {
   public:
    /* Called with the line number and 320 interleaved RGB triples. */
    std::function<void(uint16_t, const uint8_t*)> on_line{};

    bool configure(const Mode& mode, float sample_rate_hz) {
        if (mode.pixels != kPixelsPerLine) return false;

        mode_ = &mode;
        sample_rate_hz_ = (sample_rate_hz > 0.0f) ? sample_rate_hz : 48000.0f;

        lines_ = (mode.lines > 0) ? mode.lines : 1;
        channel_count_ = static_cast<uint8_t>(mode.color ? 3 : 1);
        color_order_for(mode, color_order_);

        pixel_time_frac_ = sample_rate_hz_ * mode.pixel_time_ms / 1000.0f;
        if (pixel_time_frac_ < 1.0f) pixel_time_frac_ = 1.0f;

        samples_per_sync_ = to_samples(mode.sync_time_ms);
        samples_per_gap_ = to_samples(mode.gap_time_ms);
        channel_gap_samples_ = mode.gaps ? samples_per_gap_ : 0;

        rgb_.assign(static_cast<size_t>(kPixelsPerLine) * 3, 0);
        reset();
        return true;
    }

    void reset() {
        state_ = State::SyncSearch;
        sample_count_ = 0;
        separator_target_ = 0;
        pixel_index_ = 0;
        channel_index_ = 0;
        channels_done_ = 0;
        pixel_accumulator_ = 0;
        pixel_sample_count_ = 0;
        pixel_phase_ = 0.0f;
        sync_sample_count_ = 0;
        in_sync_ = false;
        sync_count_ = 0;
        waiting_for_first_line_ = true;
        line_ = 0;
        std::fill(rgb_.begin(), rgb_.end(), uint8_t{0});
    }

    /* Upstream's SSTVRXPhaseSlantMessage handler. */
    void set_phase_offset(int16_t pixels) { phase_offset_ = pixels; }
    void set_slant(int16_t tenths_of_a_percent) {
        slant_rate_ = tenths_of_a_percent;
        slant_factor_ = 1.0f + (static_cast<float>(slant_rate_) / 1000.0f);
    }
    int16_t phase_offset() const { return phase_offset_; }
    int16_t slant() const { return slant_rate_; }

    void process_frequency(int32_t hz) {
        if (mode_ == nullptr) return;

        switch (state_) {
            case State::SyncSearch:
                detect_sync(hz);
                break;

            case State::Separator:
                sample_count_++;
                if (separator_target_ == 0 || sample_count_ >= separator_target_) {
                    sample_count_ = 0;
                    state_ = State::ImageData;
                }
                break;

            case State::ImageData:
                process_pixel_sample(hz);
                break;
        }
    }

    const Mode* mode() const { return mode_; }
    uint16_t line() const { return line_; }
    uint16_t lines() const { return lines_; }
    uint32_t sync_count() const { return sync_count_; }
    float samples_per_pixel() const { return pixel_time_frac_; }
    uint32_t samples_per_sync() const { return samples_per_sync_; }
    uint32_t samples_per_gap() const { return samples_per_gap_; }
    const std::vector<uint8_t>& line_rgb() const { return rgb_; }

   private:
    enum class State : uint8_t { SyncSearch, Separator, ImageData };

    uint32_t to_samples(float ms) const {
        const float n = std::round(sample_rate_hz_ * ms / 1000.0f);
        return static_cast<uint32_t>(n < 1.0f ? 1.0f : n);
    }

    /* Upstream SSTVRXProcessor::detect_sync(), minus the debug traffic. */
    void detect_sync(int32_t hz) {
        constexpr int32_t tolerance = 150;

        if (hz > (kFreqSync - tolerance) && hz < (kFreqSync + tolerance)) {
            sync_sample_count_++;
            in_sync_ = true;
            return;
        }

        if (in_sync_ && sync_sample_count_ >= (samples_per_sync_ / 3)) {
            sync_count_++;

            bool ready = false;
            if (waiting_for_first_line_) {
                /* Upstream waits for two syncs so the VIS stop bit, which is
                 * also 1200 Hz, cannot start a line. */
                if (sync_count_ >= 2) {
                    waiting_for_first_line_ = false;
                    ready = true;
                }
            } else {
                ready = true;
            }
            if (ready) begin_line_after_sync();
        }

        in_sync_ = false;
        sync_sample_count_ = 0;
    }

    void begin_line_after_sync() {
        pixel_index_ = 0;
        /* Deviation 3: Scottie's sync sits between blue and red, so the first
         * channel after it is the one `sync_index` names. */
        channel_index_ = mode_->sync_on_first
                             ? static_cast<uint8_t>(mode_->sync_index % channel_count_)
                             : 0;
        channels_done_ = 0;
        std::fill(rgb_.begin(), rgb_.end(), uint8_t{0});
        start_gap(samples_per_gap_);
    }

    void start_gap(uint32_t duration) {
        reset_pixel_state();
        separator_target_ = duration;
        sample_count_ = 0;
        state_ = (duration == 0) ? State::ImageData : State::Separator;
    }

    void reset_pixel_state() {
        pixel_accumulator_ = 0;
        pixel_sample_count_ = 0;
        pixel_phase_ = 0.0f;
    }

    void store_pixel_value(uint8_t channel, uint16_t pixel, uint8_t value) {
        const size_t base = static_cast<size_t>(pixel) * 3;

        if (!mode_->color) {
            rgb_[base + 0] = value;
            rgb_[base + 1] = value;
            rgb_[base + 2] = value;
            return;
        }
        if (channel >= channel_count_ || channel >= 3) return;
        rgb_[base + color_order_[channel]] = value;
    }

    /* Upstream SSTVRXProcessor::process_pixel_sample(). */
    void process_pixel_sample(int32_t hz) {
        pixel_accumulator_ += hz;
        pixel_sample_count_++;
        pixel_phase_ += slant_factor_;

        while (pixel_phase_ >= pixel_time_frac_ && pixel_index_ < kPixelsPerLine) {
            const int32_t avg = (pixel_sample_count_ > 0)
                                    ? (pixel_accumulator_ / static_cast<int32_t>(pixel_sample_count_))
                                    : hz;
            const uint8_t value = freq_to_pixel(avg);

            int32_t idx = static_cast<int32_t>(pixel_index_) + phase_offset_;
            if (idx < 0)
                idx = 0;
            else if (idx >= kPixelsPerLine)
                idx = kPixelsPerLine - 1;

            store_pixel_value(channel_index_, static_cast<uint16_t>(idx), value);

            pixel_index_++;
            pixel_accumulator_ = hz;
            pixel_sample_count_ = 1;
            pixel_phase_ -= pixel_time_frac_;

            if (pixel_index_ >= kPixelsPerLine) {
                pixel_index_ = 0;
                channels_done_++;

                if (channels_done_ >= channel_count_) {
                    emit_line();
                    channels_done_ = 0;
                    state_ = State::SyncSearch;
                    sync_sample_count_ = 0;
                    in_sync_ = false;
                    reset_pixel_state();
                    break;
                }

                channel_index_ = static_cast<uint8_t>((channel_index_ + 1) % channel_count_);
                reset_pixel_state();
                if (channel_gap_samples_ > 0)
                    start_gap(channel_gap_samples_);
                else
                    state_ = State::ImageData;
                break;
            }
        }
    }

    /* Upstream process_line(), without the shared-memory chunking. Deviation 4:
     * a wrapped frame restarts at row 0, not row 1. */
    void emit_line() {
        if (line_ >= lines_) line_ = 0;
        if (on_line) on_line(line_, rgb_.data());
        line_++;
    }

    const Mode* mode_{nullptr};
    float sample_rate_hz_{48000.0f};

    uint16_t lines_{256};
    uint8_t channel_count_{3};
    uint8_t color_order_[3]{1, 2, 0};

    float pixel_time_frac_{1.0f};
    uint32_t samples_per_sync_{1};
    uint32_t samples_per_gap_{1};
    uint32_t channel_gap_samples_{0};

    State state_{State::SyncSearch};
    uint32_t sample_count_{0};
    uint32_t separator_target_{0};

    uint16_t pixel_index_{0};
    uint8_t channel_index_{0};
    uint8_t channels_done_{0};
    int32_t pixel_accumulator_{0};
    uint32_t pixel_sample_count_{0};
    float pixel_phase_{0.0f};

    uint32_t sync_sample_count_{0};
    bool in_sync_{false};
    uint32_t sync_count_{0};
    bool waiting_for_first_line_{true};

    int16_t phase_offset_{0};
    int16_t slant_rate_{0};
    float slant_factor_{1.0f};

    uint16_t line_{0};
    std::vector<uint8_t> rgb_{};
};

/* ===========================================================================
 * Whole receiver
 * ===========================================================================*/

/* Deviation upstream sets on its FM demodulator for SSTV. It only scales the
 * discriminator output; the tone estimator converts back to Hz. */
inline constexpr float kSstvDeviationHz = 7500.0f;

class SstvDecoder {
   public:
    std::function<void(uint16_t, const uint8_t*)> on_line{};
    std::function<void(const Mode&, uint8_t vis)> on_mode_detected{};

    void configure(float sample_rate_hz) {
        sample_rate_hz_ = (sample_rate_hz > 0.0f) ? sample_rate_hz : 48000.0f;
        fm_.configure(sample_rate_hz_, kSstvDeviationHz);
        tone_.configure(sample_rate_hz_);
        vis_.configure(sample_rate_hz_);
        line_.on_line = [this](uint16_t n, const uint8_t* rgb) {
            if (on_line) on_line(n, rgb);
        };
        if (mode_) line_.configure(*mode_, sample_rate_hz_);
        reset();
    }

    void reset() {
        fm_.reset();
        tone_.reset();
        vis_.reset();
        line_.reset();
        primed_ = false;
        last_vis_ = 0;
        scratch_.clear();
    }

    /* Manual mode selection, as upstream's OptionsField does it. */
    bool set_mode(const Mode* mode) {
        mode_ = mode;
        if (!mode_) return false;
        const bool ok = line_.configure(*mode_, sample_rate_hz_);
        line_.on_line = [this](uint16_t n, const uint8_t* rgb) {
            if (on_line) on_line(n, rgb);
        };
        return ok;
    }

    void set_auto_vis(bool enable) { auto_vis_ = enable; }
    bool auto_vis() const { return auto_vis_; }

    const Mode* mode() const { return mode_; }
    uint8_t last_vis() const { return last_vis_; }
    LineDecoder& line_decoder() { return line_; }
    const LineDecoder& line_decoder() const { return line_; }
    ToneEstimator& tone() { return tone_; }

    /* Discriminator audio in (real, +/-1.0 == +/-7.5 kHz). */
    void process_audio(const float* in, size_t count) {
        for (size_t i = 0; i < count; i++) process_sample(in[i]);
    }

    /* Complex baseband in; FM-demodulated first, as upstream's chain does. */
    void process(const dsp::cfloat* in, size_t count) {
        if (count == 0) return;
        scratch_.clear();
        fm_.process(in, count, scratch_);

        size_t first = 0;
        if (!primed_ && !scratch_.empty()) {
            first = 1; /* the first phase difference has no valid reference */
            primed_ = true;
        }
        for (size_t i = first; i < scratch_.size(); i++) process_sample(scratch_[i]);
    }

    /* Tone estimates straight in — the path the decoder tests drive. */
    void process_frequency(int32_t hz) {
        if (auto_vis_ && vis_.process(hz)) {
            const uint8_t code = vis_.code();
            last_vis_ = code;
            if (vis_.parity_ok()) {
                if (const Mode* m = mode_for_vis(code)) {
                    /* A VIS header always precedes a picture, so the line
                     * decoder restarts whether or not the mode changed. It has
                     * to: the header's own 1200 Hz break and start/stop bits
                     * fall inside the sync detector's window, so without the
                     * reset those count as the two syncs that start line 0 and
                     * the picture lands one sync early. */
                    if (m != mode_) set_mode(m);
                    line_.reset();
                    if (on_mode_detected) on_mode_detected(*m, code);
                }
            }
        }
        line_.process_frequency(hz);
    }

   private:
    void process_sample(float audio) { process_frequency(tone_.process(audio)); }

    dsp::FmDemod fm_{};
    ToneEstimator tone_{};
    VisDecoder vis_{};
    LineDecoder line_{};
    std::vector<float> scratch_{};

    const Mode* mode_{&kModes[3]}; /* Martin 1, until VIS says otherwise */
    float sample_rate_hz_{48000.0f};
    uint8_t last_vis_{0};
    bool auto_vis_{true};
    bool primed_{false};
};

/* ===========================================================================
 * Host front end: wideband tap -> channel rate
 *
 * Same two-stage channeliser the RTTY app carries. Duplicated rather than
 * shared because the porting contract forbids adding shared files.
 * ===========================================================================*/

class SstvFrontEnd {
   public:
    void configure(double input_rate_hz, double target_rate_hz) {
        input_rate_hz_ = (input_rate_hz > 0.0) ? input_rate_hz : 2'400'000.0;
        const double target = (target_rate_hz > 0.0) ? target_rate_hz : 48'000.0;

        const double intermediate_min = target * 4.0;
        size_t d1 = static_cast<size_t>(input_rate_hz_ / intermediate_min);
        if (d1 < 1) d1 = 1;
        if (d1 > 64) d1 = 64;

        const double mid_rate = input_rate_hz_ / static_cast<double>(d1);
        size_t d2 = static_cast<size_t>(mid_rate / target);
        if (d2 < 1) d2 = 1;

        output_rate_hz_ = mid_rate / static_cast<double>(d2);

        if (d1 > 1) {
            stage1_.configure(dsp::design_lowpass(mid_rate * 0.20, mid_rate * 0.25,
                                                  input_rate_hz_, 60.0),
                              d1);
            stage1_enabled_ = true;
        } else {
            stage1_enabled_ = false;
        }

        stage2_.configure(dsp::design_lowpass(output_rate_hz_ * 0.25, output_rate_hz_ * 0.25,
                                              mid_rate, 60.0),
                          d2);
        decimation_ = d1 * d2;
        reset();
    }

    void reset() {
        stage1_.reset();
        stage2_.reset();
        nco_.reset();
        mid_.clear();
    }

    void set_offset(double offset_hz) {
        if (offset_hz == offset_hz_) return;
        offset_hz_ = offset_hz;
        nco_.set_frequency(-offset_hz, input_rate_hz_);
    }

    double output_rate() const { return output_rate_hz_; }
    size_t decimation() const { return decimation_; }

    void process(std::vector<dsp::cfloat>& in_out, std::vector<dsp::cfloat>& out) {
        out.clear();
        if (in_out.empty()) return;

        nco_.mix(in_out.data(), in_out.data(), in_out.size());

        if (stage1_enabled_) {
            mid_.clear();
            stage1_.process(in_out.data(), in_out.size(), mid_);
            stage2_.process(mid_.data(), mid_.size(), out);
        } else {
            stage2_.process(in_out.data(), in_out.size(), out);
        }
    }

   private:
    dsp::Nco nco_{};
    dsp::FirDecimateC stage1_{};
    dsp::FirDecimateC stage2_{};
    std::vector<dsp::cfloat> mid_{};

    double input_rate_hz_{2'400'000.0};
    double output_rate_hz_{48'000.0};
    double offset_hz_{0.0};
    size_t decimation_{50};
    bool stage1_enabled_{false};
};

/* ===========================================================================
 * Test/synthesis helper: build the audio a transmitter would send.
 *
 * Upstream has a matching transmitter (proc_sstvtx.cpp) but it emits complex
 * baseband on the M4 and is not reachable from here. This produces the tone
 * sequence for a VIS header and for one scan line of a mode, which is what
 * tests/test_sstvrx.cpp needs to prove the decoder end to end.
 * ===========================================================================*/

/* Appends a constant tone of `ms` milliseconds, continuing the phase. */
inline void append_tone(std::vector<float>& audio, double& phase,
                        float sample_rate_hz, float freq_hz, float ms) {
    const size_t n = static_cast<size_t>(std::lround(
        static_cast<double>(sample_rate_hz) * static_cast<double>(ms) / 1000.0));
    const double step = 2.0 * 3.14159265358979323846 * static_cast<double>(freq_hz) /
                        static_cast<double>(sample_rate_hz);
    for (size_t i = 0; i < n; i++) {
        audio.push_back(static_cast<float>(std::sin(phase)));
        phase += step;
    }
}

/* Appends `pixels` luminance values as their SSTV tones, one pixel_time_ms
 * each. */
inline void append_scan(std::vector<float>& audio, double& phase,
                        float sample_rate_hz, const uint8_t* values, size_t pixels,
                        float pixel_time_ms) {
    const double step_per_px = static_cast<double>(sample_rate_hz) *
                               static_cast<double>(pixel_time_ms) / 1000.0;
    double emitted = 0.0;
    for (size_t p = 0; p < pixels; p++) {
        const double want = step_per_px * static_cast<double>(p + 1);
        const size_t n = static_cast<size_t>(std::lround(want - emitted));
        const float freq = static_cast<float>(kFreqBlack) +
                           (static_cast<float>(kFreqWhite - kFreqBlack) *
                            static_cast<float>(values[p]) / 255.0f);
        const double step = 2.0 * 3.14159265358979323846 * static_cast<double>(freq) /
                            static_cast<double>(sample_rate_hz);
        for (size_t i = 0; i < n; i++) {
            audio.push_back(static_cast<float>(std::sin(phase)));
            phase += step;
        }
        emitted += static_cast<double>(n);
    }
}

/* The VIS header for a mode: leader, break, leader, start bit, eight data bits
 * least significant first, stop bit. */
inline void append_vis_header(std::vector<float>& audio, double& phase,
                              float sample_rate_hz, uint8_t vis_code) {
    append_tone(audio, phase, sample_rate_hz, static_cast<float>(kFreqLeader), 300.0f);
    append_tone(audio, phase, sample_rate_hz, static_cast<float>(kFreqSync), 10.0f);
    append_tone(audio, phase, sample_rate_hz, static_cast<float>(kFreqLeader), 300.0f);
    append_tone(audio, phase, sample_rate_hz, static_cast<float>(kFreqSync), 30.0f);
    for (int i = 0; i < 8; i++) {
        const bool one = ((vis_code >> i) & 1) != 0;
        append_tone(audio, phase, sample_rate_hz,
                    static_cast<float>(one ? kFreqVisOne : kFreqVisZero), 30.0f);
    }
    append_tone(audio, phase, sample_rate_hz, static_cast<float>(kFreqSync), 30.0f);
}

}  // namespace sstv

/* ===========================================================================
 * View
 * ===========================================================================*/

/* Shows the received picture. Kept inside this app's files, per the porting
 * contract; it is a straight framebuffer blit, not a general widget. */
class SstvImage : public ui::Widget {
   public:
    explicit SstvImage(ui::Rect parent_rect);

    void clear();
    /* `rgb` is 320 interleaved RGB triples; it is scaled to the widget width. */
    void set_line(uint16_t line, const uint8_t* rgb);

    /* Read-only view of the received picture, for src/remote/provider_sstv.cpp
     * to publish to the web portal. Const by design: the portal only ever
     * reads. Same idiom as AprsTableView::entries() (ui_aprs_rx.hpp). */
    const std::vector<ui::Color>& pixels() const { return pixels_; }

    void paint(ui::Painter& painter) override;

   private:
    std::vector<ui::Color> pixels_{};
    int width_{0};
    int height_{0};
};

class SstvRxView : public ui::View {
   public:
    SstvRxView();
    ~SstvRxView() override;

    SstvRxView(const SstvRxView&) = delete;
    SstvRxView& operator=(const SstvRxView&) = delete;

    std::string title() const override { return "SSTV RX"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

    /* Read-only view of the received picture, for src/remote/provider_sstv.cpp
     * to publish to the web portal. Const by design: the portal only ever
     * reads. */
    const SstvImage& image() const { return image_; }

   private:
    void rebuild_chain();
    void on_start_stop();
    void start_receiving();
    void stop_receiving();
    void handle_line(uint16_t line, const uint8_t* rgb);
    void update_status();

    sstv::SstvDecoder decoder_{};
    sstv::SstvFrontEnd front_end_{};

    std::vector<dsp::cfloat> raw_{};
    std::vector<dsp::cfloat> channel_{};

    core::BmpFile bmp_{};
    std::string image_path_{};
    uint32_t file_line_{0};

    double configured_input_rate_{0.0};
    uint32_t frame_counter_{0};
    uint16_t last_line_{0};
    bool receiving_{false};
    bool chain_valid_{false};
    std::string detected_{};

    ui::Labels labels_{
        {{0, 2}, "Freq", ui::Color::light_grey()},
        {{0, 20}, "Mode", ui::Color::light_grey()},
        {{0, 38}, "Ph", ui::Color::light_grey()},
        {{72, 38}, "Slnt", ui::Color::light_grey()},
        {{160, 20}, "Gain", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 2}};
    ui::FrequencyStepView step_view_{{132, 2}, field_frequency_};

    ui::OptionsField options_mode_{{40, 20}, 11, {}};
    ui::NumberField field_gain_{{200, 20}, 3, {0, 76}, 1, ' '};

    ui::NumberField field_phase_{{24, 38}, 3, {-50, 50}, 1, ' '};
    ui::NumberField field_slant_{{112, 38}, 4, {-100, 100}, 1, ' '};
    ui::Checkbox check_auto_vis_{{160, 36}, 4, "VIS"};

    ui::Text text_status_{{0, 56, 240, 16}, "Stopped"};
    ui::Button button_start_{{0, 74, 96, 24}, "Start RX"};
    ui::Text text_path_{{100, 78, 140, 16}, ""};

    SstvImage image_{{0, 102, 240, 202}};
};

}  // namespace app

#endif /*__MB200_UI_SSTVRX_H__*/
