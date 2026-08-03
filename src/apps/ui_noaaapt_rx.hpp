/*
 * mayhem-b200 — NOAA APT receiver (137 MHz weather-satellite image).
 *
 * Port of the firmware's application/external/noaaapt_rx/* and its baseband
 * processor baseband/proc_noaaapt_rx.{hpp,cpp}, plus the piece of
 * baseband/audio_output.cpp (`AudioOutput::apt_write`) that actually recovers
 * the picture: everything above the pixel clock lives in audio_output, not in
 * proc_noaaapt_rx, which is easy to miss.
 *
 * Upstream signal path, and what each stage is for:
 *
 *   3.072 Msps -> decim 4 -> decim 8  = 96 kHz complex, ~38 kHz wide
 *   -> WFM discriminator (17 kHz deviation)
 *   -> CIC/2, CIC/2                   = 24 kHz real
 *   -> 64-tap bandpass, centre 2.4 kHz, BW 2 kHz, /2  = 12 kHz real
 *   -> AM envelope of the 2400 Hz APT subcarrier      (apt_write)
 *   -> pixel clock at 4160 px/s, amplitude -> 8-bit grey
 *
 * The envelope detector is the interesting bit. apt_write computes
 *
 *     mag = sqrt(x[n-1]^2 + x[n]^2 - 2*x[n-1]*x[n]*cos T) / sin T
 *
 * with cos T = 0.30901699, sin T = 0.95105652 — that is T = 72 degrees, i.e.
 * exactly 2*pi*2400/12000, the per-sample phase advance of the subcarrier. For
 * x[n] = A sin(w n + p) the identity gives mag == A exactly, so this is an
 * exact two-sample envelope detector for a tone of known frequency, no
 * quadrature channel and no rectify-and-smooth lag. Upstream then scales by
 * ki = 1/32768 to undo its int16 sample scaling; on the host the audio is
 * already float in [-1, 1], so the same formula is applied directly and the
 * output is in the same 0..1 domain the pixel mapping expects.
 *
 * Pixel mapping is upstream's verbatim: >= 1.0 -> 255, <= 0 -> 0, otherwise
 * truncate(v * 255).
 *
 * LINE SYNC IS A HOST ADDITION, and is marked as such wherever it appears.
 * Upstream's sync branch is `if (status_message.state == 0 && false)` with an
 * empty body — it never syncs, so lines drift. The port adds a normalised
 * cross-correlation against the APT sync-A burst (39 words: 4 low, then 7
 * cycles of the 1040 Hz square wave as 2 high / 2 low, then 7 low, at the
 * 4160 word/s pixel rate). That pattern comes from the APT signal format, not
 * from upstream, and has never been checked against a real satellite pass —
 * see the report. The correlator itself is tested on synthetic lines.
 *
 * Copyright (C) 2025 Brumi, HTotoo
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_NOAAAPT_RX_H__
#define __MB200_UI_NOAAAPT_RX_H__

#include "../core/bmp_file.hpp"
#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* ======================================================================== *
 *  Pure APT logic — no UI, no radio.                                       *
 * ======================================================================== */
namespace noaaapt {

/* NOAAAPT_PX_SIZE: one APT line is 2080 words — sync A, space A, image A,
 * telemetry A, then the same again for channel B. */
inline constexpr uint16_t px_per_line = 2080;

/* Two lines per second, so 4160 words per second. */
inline constexpr double lines_per_second = 2.0;
inline constexpr double pixel_rate_hz = static_cast<double>(px_per_line) * lines_per_second;

/* The APT amplitude-modulated subcarrier, and the audio rate upstream demods
 * to. 12000 / 2400 = 5, which is why upstream's cos/sin constants are those of
 * 360/5 = 72 degrees. */
inline constexpr double subcarrier_hz = 2400.0;
inline constexpr double audio_rate_hz = 12000.0;

/* Rows of the view given over to the image; upstream's NOAA_IMG_START_ROW. */
inline constexpr int image_start_row = 4;

/* Two-sample AM envelope detector for a tone of known frequency. Port of
 * AudioOutput::apt_write's inner loop.
 *
 *   mag[n] = sqrt(x[n-1]^2 + x[n]^2 - 2 x[n-1] x[n] cos T) / sin T
 *
 * Exact for a pure tone at T radians per sample; `configure` derives T from the
 * subcarrier and sample rate, and the defaults reproduce upstream's hard-coded
 * cos 72 / sin 72 to the last digit. */
class SubcarrierEnvelope {
   public:
    SubcarrierEnvelope() { configure(subcarrier_hz, audio_rate_hz); }

    void configure(double tone_hz, double sample_rate_hz) {
        const double theta = 2.0 * 3.14159265358979323846 * tone_hz / sample_rate_hz;
        cos_theta_ = static_cast<float>(std::cos(theta));
        sin_theta_ = static_cast<float>(std::sin(theta));
        reset();
    }

    void reset() {
        prev_ = 0.0f;
        prev2_ = 0.0f;
    }

    float process(float sample) {
        const float cur = sample;
        const float cur2 = cur * cur;
        const float disc = prev2_ + cur2 - (2.0f * prev_ * cur * cos_theta_);
        /* Rounding can push the discriminant a hair below zero on a silent
         * input; upstream's sqrtf would return NaN there. */
        const float mag = std::sqrt(disc > 0.0f ? disc : 0.0f) / sin_theta_;
        prev_ = cur;
        prev2_ = cur2;
        return mag;
    }

    float cos_theta() const { return cos_theta_; }
    float sin_theta() const { return sin_theta_; }

   private:
    float cos_theta_{0.30901699437494742410f};
    float sin_theta_{0.95105651629515357212f};
    float prev_{0.0f};
    float prev2_{0.0f};
};

/* Fractional sample-per-pixel clock. Port of the samples_per_pixel / pxRem /
 * pxRoll trio in proc_noaaapt_rx: the integer part gates the counter and the
 * fractional remainder is accumulated so the clock does not walk off over a
 * line. At 12 kHz and 4160 px/s the ratio is 2.884615, so the clock alternates
 * between 2- and 3-sample pixels in that proportion. */
class PixelClock {
   public:
    PixelClock() { configure(audio_rate_hz, pixel_rate_hz); }

    void configure(double sample_rate_hz, double px_rate_hz) {
        double rem = (px_rate_hz > 0.0) ? (sample_rate_hz / px_rate_hz) : 1.0;
        samples_per_pixel_ = static_cast<uint32_t>(rem);
        rem -= samples_per_pixel_;
        remainder_ = rem;
        roll_ = 0.0;
        cnt_ = 0;
    }

    void reset() {
        roll_ = 0.0;
        cnt_ = 0;
    }

    /* One input sample. True when it completes a pixel. */
    bool tick() {
        cnt_++;
        if (cnt_ >= (samples_per_pixel_ + static_cast<uint32_t>(roll_))) {
            cnt_ = 0;
            if (roll_ >= 1) roll_ -= 1.0;
            roll_ += remainder_;
            return true;
        }
        return false;
    }

    uint32_t samples_per_pixel() const { return samples_per_pixel_; }
    double remainder() const { return remainder_; }
    double roll() const { return roll_; }
    uint32_t count() const { return cnt_; }

   private:
    uint32_t samples_per_pixel_{1};
    double remainder_{0.0};
    double roll_{0.0};
    uint32_t cnt_{0};
};

/* Envelope amplitude to 8-bit grey, verbatim from proc_noaaapt_rx::execute.
 * Note the truncating cast: 0.999 maps to 254, not 255. */
inline uint8_t amplitude_to_pixel(float v) {
    if (v >= 1.0f) return 255;
    if (v <= 0.0f) return 0;
    return static_cast<uint8_t>(v * 255.0f);
}

/* Column of the 240-wide preview a line position maps to. Upstream:
 *   xpos = line_in_part / (NOAAAPT_PX_SIZE / 240); if (xpos >= 240) xpos = 239;
 * 2080 / 240 is 8 in integer arithmetic, so positions 1920..2079 all clamp onto
 * the last column — the right-hand eighth of every line is squashed into one
 * pixel of the preview. Ported unchanged; the BMP on disk is full width. */
inline uint16_t preview_column(uint16_t line_in_part) {
    uint16_t xpos = line_in_part / (px_per_line / 240);
    if (xpos >= 240) xpos = 239;
    return xpos;
}

/* --- Line sync (HOST ADDITION — upstream has none) --------------------- */

/* APT sync A as +1 (white) / -1 (black) words at the 4160 word/s pixel rate:
 * 4 black, 7 cycles of the 1040 Hz square wave (4160/1040 = 4 words per cycle,
 * so 2 white + 2 black each), then 7 black. 4 + 28 + 7 = 39 words. */
inline std::vector<int8_t> sync_a_pattern() {
    std::vector<int8_t> t;
    t.reserve(39);
    for (int i = 0; i < 4; i++) t.push_back(-1);
    for (int c = 0; c < 7; c++) {
        t.push_back(1);
        t.push_back(1);
        t.push_back(-1);
        t.push_back(-1);
    }
    for (int i = 0; i < 7; i++) t.push_back(-1);
    return t;
}

/* Sliding normalised cross-correlation (Pearson) of the incoming pixel stream
 * against a sync pattern. Being normalised, it is indifferent to the receiver's
 * gain and to the DC offset of the video, which an unnormalised correlation is
 * not.
 *
 * PEAK PICKING, not first crossing. Sync A's body is a square wave of period
 * exactly 4 words, so a window offset by +/-4 words still correlates strongly —
 * only the 4-word lead-in and 7-word tail distinguish alignment, and on a clean
 * signal a 4-word-early window measures around 0.75. Firing on the first sample
 * to cross the threshold therefore locks a whole cycle early. Instead the first
 * crossing opens a candidate, the best score inside the following
 * `peak_window` words wins, and the match is reported then; a history ring
 * keeps the winning window available for copy_window().
 *
 * A refractory period then suppresses matches until nearly a line later, so the
 * burst cannot re-trigger on its own tail.
 *
 * process() returns true on the pixel that resolves a match; the matched
 * window's first pixel is at stream index last_sync_start(). */
class SyncDetector {
   public:
    SyncDetector() { configure(sync_a_pattern(), 0.7f, px_per_line - 2 * 39, 20); }

    void configure(std::vector<int8_t> pattern,
                   float threshold,
                   uint32_t refractory,
                   uint32_t peak_window) {
        pattern_ = std::move(pattern);
        threshold_ = threshold;
        refractory_ = refractory;
        peak_window_ = peak_window;

        const size_t n = pattern_.size();
        zero_mean_.assign(n, 0.0f);
        double mean = 0.0;
        for (int8_t v : pattern_) mean += v;
        mean = n ? mean / static_cast<double>(n) : 0.0;
        double energy = 0.0;
        for (size_t i = 0; i < n; i++) {
            zero_mean_[i] = static_cast<float>(pattern_[i] - mean);
            energy += static_cast<double>(zero_mean_[i]) * zero_mean_[i];
        }
        pattern_norm_ = static_cast<float>(std::sqrt(energy));

        hist_.assign(n + peak_window_ + 1, 0);
        reset();
    }

    void reset() {
        std::fill(hist_.begin(), hist_.end(), static_cast<uint8_t>(0));
        seen_ = 0;
        hold_ = 0;
        have_candidate_ = false;
        settle_ = 0;
        best_score_ = 0.0f;
        best_start_ = 0;
        last_score_ = 0.0f;
        sync_score_ = 0.0f;
        last_sync_start_ = 0;
    }

    bool process(uint8_t px) {
        const size_t n = pattern_.size();
        if (n == 0 || hist_.empty()) return false;

        hist_[static_cast<size_t>(seen_ % hist_.size())] = px;
        seen_++;

        if (hold_ > 0) {
            hold_--;
            return false;
        }
        if (seen_ < n) return false;

        const uint64_t start = seen_ - n;
        last_score_ = correlate(start);

        if (last_score_ >= threshold_) {
            if (!have_candidate_ || last_score_ > best_score_) {
                best_score_ = last_score_;
                best_start_ = start;
            }
            if (!have_candidate_) {
                have_candidate_ = true;
                settle_ = peak_window_;
            }
        }

        if (!have_candidate_) return false;
        if (settle_ > 0) {
            settle_--;
            return false;
        }

        have_candidate_ = false;
        last_sync_start_ = best_start_;
        sync_score_ = best_score_;
        best_score_ = 0.0f;
        hold_ = refractory_;
        return true;
    }

    /* The n pixels of the window that matched, oldest first. Valid until
     * peak_window more pixels have gone by. */
    void copy_window(uint8_t* out) const {
        const size_t n = pattern_.size();
        for (size_t i = 0; i < n; i++)
            out[i] = hist_[static_cast<size_t>((last_sync_start_ + i) % hist_.size())];
    }

    /* Pixels that arrived after the matched window, oldest first — the ones
     * peak picking made the caller wait through. Returns how many were
     * written. */
    size_t copy_since_sync(uint8_t* out, size_t max) const {
        const uint64_t first = last_sync_start_ + pattern_.size();
        if (seen_ <= first) return 0;
        size_t count = static_cast<size_t>(seen_ - first);
        if (count > max) count = max;
        for (size_t i = 0; i < count; i++)
            out[i] = hist_[static_cast<size_t>((first + i) % hist_.size())];
        return count;
    }

    size_t pattern_size() const { return pattern_.size(); }
    float last_score() const { return last_score_; }
    float sync_score() const { return sync_score_; }
    uint64_t last_sync_start() const { return last_sync_start_; }
    uint64_t pixels_seen() const { return seen_; }
    float threshold() const { return threshold_; }

   private:
    /* Pearson correlation of the window starting at absolute index `start`
     * against the zero-mean pattern. */
    float correlate(uint64_t start) const {
        const size_t n = pattern_.size();
        const size_t h = hist_.size();

        double mean = 0.0;
        for (size_t i = 0; i < n; i++) mean += hist_[static_cast<size_t>((start + i) % h)];
        mean /= static_cast<double>(n);

        double dot = 0.0;
        double energy = 0.0;
        for (size_t i = 0; i < n; i++) {
            const double d =
                static_cast<double>(hist_[static_cast<size_t>((start + i) % h)]) - mean;
            dot += d * zero_mean_[i];
            energy += d * d;
        }

        if (energy <= 0.0 || pattern_norm_ <= 0.0f) return 0.0f;
        return static_cast<float>(dot / (std::sqrt(energy) * pattern_norm_));
    }

    std::vector<int8_t> pattern_{};
    std::vector<float> zero_mean_{};
    std::vector<uint8_t> hist_{};
    float pattern_norm_{0.0f};
    float threshold_{0.7f};
    uint32_t refractory_{0};
    uint32_t peak_window_{0};
    uint32_t hold_{0};
    uint32_t settle_{0};
    bool have_candidate_{false};
    float best_score_{0.0f};
    uint64_t best_start_{0};
    uint64_t seen_{0};
    uint64_t last_sync_start_{0};
    float last_score_{0.0f};
    float sync_score_{0.0f};
};

/* A 240-wide grey preview that grows a row at a time. The firmware pushes each
 * finished line straight at the LCD with render_line(); a host widget has to
 * survive being hidden and repainted, so the rows are kept. */
class ScanCanvas : public ui::Widget {
   public:
    explicit ScanCanvas(ui::Rect parent_rect);

    void set_row(int row, const ui::Color* pixels);
    void clear();
    int rows() const { return rows_; }

    void paint(ui::Painter& painter) override;

   private:
    std::vector<ui::Color> fb_{};
    int rows_{0};
};

}  // namespace noaaapt

/* ======================================================================== *
 *  View                                                                     *
 * ======================================================================== */

class NoaaAptRxView : public ui::View {
   public:
    NoaaAptRxView();
    ~NoaaAptRxView() override;

    NoaaAptRxView(const NoaaAptRxView&) = delete;
    NoaaAptRxView& operator=(const NoaaAptRxView&) = delete;

    std::string title() const override { return "NOAA APT"; }

    void focus() override;
    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void rebuild_chain();
    void pump();
    void feed_pixel(uint8_t px);
    void flush_line();
    void toggle_capture();
    void set_status(const std::string& s);

    radio::ReceiverModel& receiver_;

    /* Decode state. */
    noaaapt::SubcarrierEnvelope envelope_{};
    noaaapt::PixelClock pixel_clock_{};
    noaaapt::SyncDetector sync_{};
    bool use_sync_{true};

    std::vector<uint8_t> line_{};
    size_t line_pos_{0};
    uint16_t line_num_{0};
    uint32_t lines_done_{0};
    uint32_t frame_counter_{0};
    std::string status_{};

    core::BmpFile bmp_{};
    bool capturing_{false};
    std::string capture_path_{};

    /* Host channel chain (see the comment in pump()). */
    bool chain_valid_{false};
    double chain_audio_rate_{noaaapt::audio_rate_hz};

    dsp::Nco nco_{};
    dsp::FirDecimateC channel_filter_{};
    dsp::FmDemod fm_{};
    dsp::FirDecimateR audio_decim_{};
    dsp::FirDecimateR subcarrier_bpf_{};

    std::vector<dsp::cfloat> raw_{};
    std::vector<dsp::cfloat> mixed_{};
    std::vector<dsp::cfloat> channel_{};
    std::vector<float> demod_{};
    std::vector<float> audio24_{};
    std::vector<float> audio12_{};
    std::vector<ui::Color> row_{};

    /* --- Widgets --- */
    ui::FrequencyField field_frequency_{{0, 0}};
    ui::NumberField field_gain_{{104, 0}, 3, {0, 76}, 1, ' '};
    ui::Text text_level_{{144, 0, 96, 16}, ""};

    ui::Text text_status_{{0, 16, 168, 16}, "Waiting for signal."};
    ui::Button button_ss_{{176, 16, 64, 32}, "START"};

    ui::Checkbox check_sync_{{0, 32}, 4, "Sync", true};
    ui::Text text_lines_{{88, 36, 80, 16}, ""};

    ui::Text text_tap_{{0, 58, 240, 14}, ""};

    /* Upstream draws the image from NOAA_IMG_START_ROW (y = 64). The port
     * starts one row lower to leave space for the tap-duty banner, which is
     * information a PortaPack build does not need. */
    noaaapt::ScanCanvas canvas_{{0, 72, 240, 304 - 72}};
};

}  // namespace app

#endif /*__MB200_UI_NOAAAPT_RX_H__*/
