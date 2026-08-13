/*
 * mayhem-b200 — WeFax receiver (HF radiofacsimile weather charts).
 *
 * Port of the firmware's application/external/wefax_rx/* and its baseband
 * processor baseband/proc_wefaxrx.{hpp,cpp}, together with the two pieces of
 * common/baseband DSP that sit inside its per-sample path:
 * dsp::Real_to_Complex (common/dsp_hilbert.cpp, reached through
 * dsp::demodulate::SSB_FM) and FeedForwardCompressor (baseband/
 * audio_compressor.cpp).
 *
 * Upstream signal path:
 *
 *   3.072 Msps -> /8 -> /8 -> /4          = 12 kHz complex
 *   -> 2.6 kHz USB channel filter          (taps_2k6_usb_wefax_channel)
 *   -> SSB_FM: Real_to_Complex on Re{}     = 0..1 video
 *   -> FeedForwardCompressor (in place, so it IS in the picture path)
 *   -> pixel clock at (lpm/60) * 840 px/s
 *   -> amplitude -> 8-bit grey
 *
 * Real_to_Complex is not a conventional FM discriminator. It shifts the audio
 * down by fs/4 through the classic 4-phase +1/0/-1/0 sequence, lowpasses I and
 * Q, multiplies them together, and lowpasses that product at a quarter band
 * (1.5 kHz at 12 kHz). For a tone at f the I*Q product carries a component at
 * 2*(f - fs/4); WeFax black is 1500 Hz and white 2300 Hz, so after the shift
 * those land at 3000 Hz and 1400 Hz — one of them outside the 1.5 kHz lowpass
 * and one inside it. Frequency therefore reaches the pixel stage as amplitude,
 * which is why the thresholds below are amplitude thresholds. Exactly how much
 * of that discrimination comes from the filter slope and how much from the
 * rectifying behaviour of the product (deviation 1 below) is not something this
 * port can settle without a signal to try it on; the polarity and contrast of
 * the recovered video are unverified. The 1.5 kHz lowpass placement, which is
 * the part that can be checked, is tested in tests/test_wefax_rx.cpp.
 *
 * Three deliberate deviations, the first two because upstream depends on
 * Cortex-M4 behaviour that does not exist here:
 *
 *  1. Upstream computes the I*Q product as `__SMUAD(out_i, out_q)` on two
 *     floats. CMSIS declares __SMUAD as uint32_t(uint32_t, uint32_t), so each
 *     float is converted to uint32_t first — which C++ leaves undefined for a
 *     negative value — and the instruction then returns the sum of the two
 *     packed 16-bit half-products. Over the value range this code works in
 *     (|out| well under 32768) the high halves are zero, so the result reduces
 *     to trunc(out_i) * trunc(out_q) with any negative operand contributing
 *     nothing. The port models exactly that: truncate, clamp negatives to zero,
 *     multiply. Two things argue this model rather than a plain float product:
 *     a plain product is zero-mean for a tone, so the quarter-band lowpass that
 *     follows would have nothing to pass; and upstream's "S" curve
 *     x*(1.5 - x^2/2) only clips the positive side, diverging for x < -sqrt(3),
 *     which is only safe if the product cannot go negative. The exact
 *     conversion behaviour of the M4's vcvt.u32.f32 on negative inputs has NOT
 *     been verified against the hardware — see the report.
 *  2. GainComputer uses fast_log2 / fast_pow2, polynomial approximations chosen
 *     for the M4. The port uses std::log2 / std::exp2, i.e. the same functions
 *     computed accurately.
 *  3. Upstream's channel filter has asymmetric (single-sideband) taps and
 *     SSB_FM then takes Re{} of the complex result. dsp:: has no complex-tap
 *     filter, so the view uses a symmetric channel filter of the same width
 *     followed by dsp::SsbDemod's phasing method, which produces the same real
 *     upper-sideband audio. See ui_wefax_rx.cpp.
 *
 * LEVEL DEPENDENCE: the discriminator's /32768 assumes int16-scaled input, so
 * the view multiplies its float audio by 32768 to land in upstream's numeric
 * domain. Upstream's own level at that point depends on the front-end gain —
 * this app has no AGC (its source carries an "AGC?!?" TODO), so how bright the
 * picture comes out still depends on how hard the receiver is driven. Nothing
 * here can calibrate that without a signal.
 *
 * Everything else — the SOS coefficients, the fs/4 rotation sequence, the "S"
 * compressor curve x*(1.5 - x^2/2), the 1/32768 normalisation, the compressor's
 * ratio/threshold/attack/release, the pixel clock's fractional accumulator and
 * the 0.45/0.68/1108 pixel thresholds — is upstream's, unchanged.
 *
 * The start-tone detector is upstream's too, but upstream disables it
 * (`if (status_message.state == 0 && false)`) as "so sensitive to noise". It is
 * ported as a class that can be switched on, with upstream's constants intact.
 *
 * B200 NOTE: WeFax is an HF mode (typically 3–20 MHz). A B200 tunes from about
 * 70 MHz, so without a transverter or upconverter there is nothing to receive.
 * The view reads the real limit from the device and says so on screen rather
 * than pretending.
 *
 * Copyright (C) 2020 Belousov Oleg (Real_to_Complex)
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc. (compressor)
 * Copyright (C) 2025 Brumi, HTotoo
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_WEFAX_RX_H__
#define __MB200_UI_WEFAX_RX_H__

#include "../core/bmp_file.hpp"
#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* ======================================================================== *
 *  Pure WeFax logic — no UI, no radio.                                     *
 * ======================================================================== */
namespace wefax {

/* WEFAX_PX_SIZE. Upstream samples every line onto this fixed grid whatever the
 * IOC is; see ioc_line_pixels() for what the IOC actually implies. */
inline constexpr uint16_t px_per_line = 840;

/* Upstream's WEFAX_FREQ_OFFSET. 1900 Hz is the nominal FM centre, but the app
 * tunes 2200 Hz low: with the 300 Hz margin it adds plus the truncated filter's
 * skirt, that placement measured better S/N (upstream's comment). */
inline constexpr int32_t frequency_offset_hz = -2200;

/* Upstream's ioc_mode values. The UI labels them "576" and "228"; 228 is a typo
 * for 288 — proc_wefaxrx.hpp's own comment says "0 - ioc576, 1 - ioc 288", and
 * 675 Hz is the IOC-288 start tone. The port shows 288. */
enum class Ioc : uint8_t {
    Ioc576 = 0,
    Ioc288 = 1,
};

inline uint32_t ioc_value(Ioc ioc) { return (ioc == Ioc::Ioc288) ? 288u : 576u; }

/* update_params(): 300 Hz start tone for IOC 576, 675 Hz for IOC 288. */
inline uint32_t start_tone_hz(Ioc ioc) { return (ioc == Ioc::Ioc288) ? 675u : 300u; }

/* Upstream's remaining (unused) phasing constants, kept so the numbers live
 * with the code that will eventually need them. */
inline constexpr uint32_t stop_tone_hz = 450;
inline constexpr uint32_t start_tone_ms = 3000;  /* 3–5 s of start tone */

/* --- Line geometry ------------------------------------------------------ */

/* Seconds per line at `lpm` lines per minute. */
inline double line_duration_s(uint32_t lpm) { return lpm ? 60.0 / lpm : 0.0; }

/* Input samples spanned by one line. */
inline double samples_per_line(double sample_rate_hz, uint32_t lpm) {
    return sample_rate_hz * line_duration_s(lpm);
}

/* Input samples per picture element — update_params()'s
 *   pxRem = channel_filter_input_fs / ((lpm / 60.0) * WEFAX_PX_SIZE)
 * generalised over the pixel count. */
inline double samples_per_pixel(double sample_rate_hz, uint32_t lpm, uint32_t pixels_per_line) {
    const double px_rate = (lpm / 60.0) * static_cast<double>(pixels_per_line);
    return px_rate > 0.0 ? sample_rate_hz / px_rate : 0.0;
}

/* HOST ADDITION, informational only. The Index Of Cooperation is defined so
 * that one scan line carries pi * IOC picture elements: IOC 576 is 1810 px per
 * line, IOC 288 is 905. Upstream samples 840 px per line regardless, which is
 * about half the elements an IOC-576 chart carries and slightly under an
 * IOC-288 one — i.e. the recorded image is horizontally undersampled, not
 * mis-timed, since the pixel clock still spans exactly one line period. This
 * function is what the view displays so the operator can see the difference; it
 * does not change the decode. */
inline double ioc_line_pixels(uint32_t ioc) {
    return 3.14159265358979323846 * static_cast<double>(ioc);
}

/* Column of the 240-wide preview a line position maps to. Upstream:
 *   xpos = line_in_part / (WEFAX_PX_SIZE / 240); if (xpos >= 240) xpos = 239;
 * 840 / 240 is 3 in integer arithmetic, so positions from 720 on clamp onto the
 * last column and the right sixth of each line collapses into one pixel of the
 * preview. Ported unchanged; the BMP on disk is full width. */
inline uint16_t preview_column(uint16_t line_in_part) {
    uint16_t xpos = line_in_part / (px_per_line / 240);
    if (xpos >= 240) xpos = 239;
    return xpos;
}

/* Fractional sample-per-pixel clock, the same construction proc_wefaxrx uses:
 * the integer part gates a counter and the fractional remainder accumulates in
 * pxRoll so the clock does not walk off across a line. */
class PixelClock {
   public:
    void configure(double sample_rate_hz, uint32_t lpm, uint32_t pixels_per_line) {
        /* Qualified: the accessor below would otherwise hide the free function. */
        double rem = ::app::wefax::samples_per_pixel(sample_rate_hz, lpm, pixels_per_line);
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

   private:
    uint32_t samples_per_pixel_{1};
    double remainder_{0.0};
    double roll_{0.0};
    uint32_t cnt_{0};
};

/* Video amplitude to 8-bit grey, verbatim from proc_wefaxrx::execute. Anything
 * at or above 0.68 is white, anything below 0.45 is black, and the band between
 * is stretched across the full range (0.23 * 1108 = 254.8). */
inline uint8_t amplitude_to_pixel(float v) {
    if (v >= 0.68f) return 255;
    if (v >= 0.45f) return static_cast<uint8_t>((v - 0.45f) * 1108.0f);
    return 0;
}

/* Upstream's start-signal detector, constants and all. It counts consecutive
 * samples sitting in the low band and declares sync after 110 of them, with any
 * 20 samples outside the band resetting the count.
 *
 * Upstream compiles this behind `&& false` — see the file header. Ported so it
 * exists and is testable; the view exposes it behind a checkbox that is off by
 * default, matching upstream's effective behaviour. */
class StartToneDetector {
   public:
    /* STARTSIGNAL_TH / STARTSIGNAL_NEEDCNT / STARTSIGNAL_MAXBAD. */
    static constexpr float threshold = 0.33f;
    static constexpr uint16_t need_count = 110;
    static constexpr uint16_t max_bad = 20;

    void reset() {
        sync_cnt_ = 0;
        syncnot_cnt_ = 0;
    }

    bool process(float v) {
        if (v <= threshold && v >= 0.0001f) {
            sync_cnt_++;
            if (sync_cnt_ >= need_count) {
                sync_cnt_ = 0;
                syncnot_cnt_ = 0;
                return true;
            }
        } else {
            syncnot_cnt_++;
            if (syncnot_cnt_ >= max_bad) {
                sync_cnt_ = 0;
                syncnot_cnt_ = 0;
            }
        }
        return false;
    }

    uint16_t sync_count() const { return sync_cnt_; }
    uint16_t bad_count() const { return syncnot_cnt_; }

   private:
    uint16_t sync_cnt_{0};
    uint16_t syncnot_cnt_{0};
};

/* --- Second-order sections (port of IIRBiquadDF2Filter / SOSFilter) ------ */

/* scipy's sos row: b0 b1 b2 a0 a1 a2. */
using SosSection = std::array<float, 6>;

class Biquad {
   public:
    void configure(const SosSection& c) {
        b0_ = c[0] / c[3];
        b1_ = c[1] / c[3];
        b2_ = c[2] / c[3];
        a1_ = c[4] / c[3];
        a2_ = c[5] / c[3];
        reset();
    }

    void reset() {
        z0_ = 0.0f;
        z1_ = 0.0f;
    }

    /* Direct form II transposed, identical to scipy.signal.sosfilt. */
    float execute(float x) {
        const float y = b0_ * x + z0_;
        z0_ = b1_ * x - a1_ * y + z1_;
        z1_ = b2_ * x - a2_ * y;
        return y;
    }

   private:
    float b0_{0.0f}, b1_{0.0f}, b2_{0.0f}, a1_{0.0f}, a2_{0.0f};
    float z0_{0.0f}, z1_{0.0f};
};

template <size_t N>
class SosFilter {
   public:
    void configure(const SosSection (&config)[N]) {
        for (size_t i = 0; i < N; i++) sections_[i].configure(config[i]);
    }
    void reset() {
        for (auto& s : sections_) s.reset();
    }
    float execute(float v) {
        for (auto& s : sections_) v = s.execute(v);
        return v;
    }

   private:
    Biquad sections_[N]{};
};

/* common/dsp_sos_config.hpp, verbatim.
 * scipy.signal.iirfilter(ftype="ellip", N=10, rp=0.5, rs=60, Wn=0.99, "lowpass")
 * — 6 kHz cutoff at 12 kHz. */
inline constexpr SosSection full_band_lpf_config[5] = {
    {0.88095275f, 1.76184993f, 0.88095275f, 1.0f, 1.89055677f, 0.89616378f},
    {1.0f, 1.99958798f, 1.0f, 1.0f, 1.9781807f, 0.98002549f},
    {1.0f, 1.99928911f, 1.0f, 1.0f, 1.99328036f, 0.99447816f},
    {1.0f, 1.99914562f, 1.0f, 1.0f, 1.997254f, 0.99828526f},
    {1.0f, 1.99909558f, 1.0f, 1.0f, 1.9986187f, 0.99960319f}};

/* Wn=0.25 — 1.5 kHz cutoff at 12 kHz, the subcarrier-product lowpass. */
inline constexpr SosSection quarter_band_lpf_config[5] = {
    {0.00349312f, 0.00319397f, 0.00349312f, 1.0f, -1.53025211f, 0.6203438f},
    {1.0f, -0.83483341f, 1.0f, 1.0f, -1.47619047f, 0.77120659f},
    {1.0f, -1.23050154f, 1.0f, 1.0f, -1.43058949f, 0.9000896f},
    {1.0f, -1.33837384f, 1.0f, 1.0f, -1.41007744f, 0.96349953f},
    {1.0f, -1.36921549f, 1.0f, 1.0f, -1.40680439f, 0.9910884f}};

/* Port of dsp::Real_to_Complex (reached through dsp::demodulate::SSB_FM).
 * Input is one real audio sample in upstream's int16 domain (i.e. host float
 * audio multiplied by 32768); output is the 0..1 video level the pixel
 * thresholds expect. See the file header for the two deviations. */
class Discriminator {
   public:
    Discriminator() {
        sos_input_.configure(full_band_lpf_config);
        sos_i_.configure(full_band_lpf_config);
        sos_q_.configure(full_band_lpf_config);
        sos_mag_sq_.configure(quarter_band_lpf_config);
    }

    void reset() {
        n_ = 0;
        sos_input_.reset();
        sos_i_.reset();
        sos_q_.reset();
        sos_mag_sq_.reset();
    }

    float process(float in) {
        const float in_filtered = sos_input_.execute(in);

        float a = 0.0f, b = 0.0f;
        switch (n_) {
            case 0: a = in_filtered; b = 0.0f; break;
            case 1: a = 0.0f; b = -in_filtered; break;
            case 2: a = -in_filtered; b = 0.0f; break;
            default: a = 0.0f; b = in_filtered; break;
        }

        const float i = sos_i_.execute(a);
        const float q = sos_q_.execute(b);

        float out_i = 0.0f, out_q = 0.0f;
        switch (n_) {  /* shift down by fs/4 */
            case 0: out_i = i; out_q = q; break;
            case 1: out_i = -q; out_q = i; break;
            case 2: out_i = -i; out_q = -q; break;
            default: out_i = q; out_q = -i; break;
        }
        n_ = static_cast<uint8_t>((n_ + 1) % 4);

        /* DEVIATION 1: model of upstream's __SMUAD(out_i, out_q) on floats —
         * truncate towards zero, drop negatives, multiply. See the file header
         * for why this and not a plain product. */
        const float cross = smuad_model(out_i, out_q);

        float out = sos_mag_sq_.execute(cross) * 2.0f;
        out /= 32768.0f;  /* undo the int16 sample scaling */
        return soft_clip(out);
    }

    /* Upstream's output shaping, verbatim: hard limit above +1.0, and the
     * "S" curve x*(1.5 - x^2/2) below it.
     *
     * The curve is ONE-SIDED. It is well behaved on [0, 1] — monotonic, unity
     * at both ends — but below -sqrt(3) it turns and grows without bound, so a
     * large negative excursion (elliptic-filter ringing on an overdriven input,
     * say) comes out as a large positive number rather than being limited.
     * That is upstream's behaviour and is kept; with no AGC in this app the
     * only defence is not overdriving the front end. The pixel mapping absorbs
     * it either way: anything at or above 0.68 is white and anything below
     * 0.45 is black, so a wild sample is one wrong pixel, not a crash. */
    static float soft_clip(float x) {
        if (x > 1.0f) return 1.0f;
        return x * (1.5f - ((x * x) / 2.0f));
    }

    /* What `__SMUAD(float, float)` computes on the target, for operands whose
     * magnitude stays below 32768: each float converts to uint32_t (negatives
     * clamping to zero on the M4's saturating vcvt.u32.f32), the low 16 bits of
     * each are read as int16 and multiplied, and the high halves — zero here —
     * contribute nothing. Exposed so tests can pin the behaviour. */
    static float smuad_model(float a, float b) {
        const float ia = (a > 0.0f) ? std::trunc(a) : 0.0f;
        const float ib = (b > 0.0f) ? std::trunc(b) : 0.0f;
        return ia * ib;
    }

   private:
    uint8_t n_{0};
    SosFilter<5> sos_input_{};
    SosFilter<5> sos_i_{};
    SosFilter<5> sos_q_{};
    SosFilter<5> sos_mag_sq_{};
};

/* Port of FeedForwardCompressor + GainComputer + PeakDetectorBranchingSmooth.
 * 10:1 above -30 dBFS, 10 ms attack / 300 ms release at 12 kHz, hard knee, with
 * the make-up gain that puts the threshold back where it started.
 *
 * This sits in the picture path, not just the audio path: upstream applies it
 * in place to the same buffer the pixel loop then reads. */
class Compressor {
   public:
    static constexpr float fs = 12000.0f;
    static constexpr float ratio = 10.0f;
    static constexpr float threshold_db = -30.0f;

    Compressor() { reset(); }

    void reset() { state_ = 0.0f; }

    void execute_in_place(float* samples, size_t count) {
        const float makeup =
            std::pow(10.0f, (threshold_db - (threshold_db / ratio)) / -20.0f);
        for (size_t i = 0; i < count; i++) samples[i] = execute_once(samples[i]) * makeup;
    }

    float execute_once(float x) {
        const float gain_db = gain_computer(x);
        const float peak_db = -peak_detector(-gain_db);
        /* 3.321928... = 20 / (20 * log10(2)) — dB to log2 units. */
        const float gain = std::exp2(peak_db * (3.321928094887362f / 20.0f));
        return x * gain;
    }

   private:
    static constexpr float slope() { return 1.0f / ratio - 1.0f; }
    static constexpr float db_floor = -120.0f;

    /* DEVIATION 2: std::log2 in place of the M4's fast_log2. */
    static float gain_computer(float x) {
        const float lin_floor = std::pow(10.0f, db_floor / 20.0f);
        const float log2_db_k = 20.0f * std::log10(2.0f);
        const float abs_x = std::abs(x);
        const float db = (abs_x < lin_floor) ? db_floor : log2_db_k * std::log2(abs_x);
        const float overshoot_db = db - threshold_db;
        const float rectified = std::max(overshoot_db, 0.0f);
        return rectified * slope();
    }

    float peak_detector(float db) {
        const float att_a = tau_alpha(0.010f, fs);
        const float rel_a = tau_alpha(0.300f, fs);
        const float a = (db > state_) ? att_a : rel_a;
        state_ = db + a * (state_ - db);
        return state_;
    }

    static float tau_alpha(float tau, float rate) { return std::exp(-1.0f / (tau * rate)); }

    float state_{0.0f};
};

/* A 240-wide grey preview that grows a row at a time (see ui_noaaapt_rx for the
 * same problem; the two apps are separate translation units by design). */
class ScanCanvas : public ui::Widget {
   public:
    explicit ScanCanvas(ui::Rect parent_rect);

    void set_row(int row, const ui::Color* pixels);
    void clear();
    int rows() const { return rows_; }

    /* Read-only view of the preview framebuffer, for
     * src/remote/provider_wefax.cpp to publish to the web portal. Const by
     * design: the portal only ever reads. Same idiom as
     * AprsTableView::entries() (ui_aprs_rx.hpp). */
    const std::vector<ui::Color>& pixels() const { return fb_; }

    void paint(ui::Painter& painter) override;

   private:
    std::vector<ui::Color> fb_{};
    int rows_{0};
};

}  // namespace wefax

/* ======================================================================== *
 *  View                                                                     *
 * ======================================================================== */

class WeFaxRxView : public ui::View {
   public:
    WeFaxRxView();
    ~WeFaxRxView() override;

    WeFaxRxView(const WeFaxRxView&) = delete;
    WeFaxRxView& operator=(const WeFaxRxView&) = delete;

    std::string title() const override { return "WeFax"; }

    void focus() override;
    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

    /* Read-only view of the decoded preview, for src/remote/provider_wefax.cpp
     * to publish to the web portal. Const by design: the portal only ever
     * reads. */
    const wefax::ScanCanvas& canvas() const { return canvas_; }

   private:
    void on_settings_changed();
    void rebuild_chain();
    void pump();
    void feed_pixel(uint8_t px);
    void flush_line();
    void toggle_capture();
    void set_status(const std::string& s);
    void update_range_warning();

    radio::ReceiverModel& receiver_;

    uint32_t lpm_{120};
    wefax::Ioc ioc_{wefax::Ioc::Ioc576};
    bool use_start_tone_{false};

    wefax::Discriminator discriminator_{};
    wefax::Compressor compressor_{};
    wefax::PixelClock pixel_clock_{};
    wefax::StartToneDetector start_tone_{};

    std::vector<uint8_t> line_{};
    size_t line_pos_{0};
    uint16_t line_num_{0};
    uint32_t lines_done_{0};
    uint32_t frame_counter_{0};
    std::string status_{};

    core::BmpFile bmp_{};
    bool capturing_{false};
    std::string capture_path_{};

    bool chain_valid_{false};
    double chain_audio_rate_{12000.0};

    dsp::Nco nco_{};
    dsp::FirDecimateC channel_filter_{};
    dsp::SsbDemod ssb_{};

    std::vector<dsp::cfloat> raw_{};
    std::vector<dsp::cfloat> mixed_{};
    std::vector<dsp::cfloat> channel_{};
    std::vector<float> audio_{};
    std::vector<ui::Color> row_{};

    /* --- Widgets --- */
    ui::Labels labels_{
        {{0, 16}, "LPM", ui::Color::light_grey()},
        {{80, 16}, "IOC", ui::Color::light_grey()},
        {{196, 16}, "Tone", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{0, 0}};
    ui::NumberField field_gain_{{104, 0}, 3, {0, 76}, 1, ' '};
    ui::Text text_level_{{144, 0, 96, 16}, ""};

    ui::OptionsField options_lpm_{{32, 16},
                                  4,
                                  {{"60  ", 60},
                                   {"90  ", 90},
                                   {"100 ", 100},
                                   {"120 ", 120},
                                   {"180 ", 180},
                                   {"240 ", 240}}};
    ui::OptionsField options_ioc_{{112, 16},
                                  4,
                                  {{"576 ", static_cast<int32_t>(wefax::Ioc::Ioc576)},
                                   {"288 ", static_cast<int32_t>(wefax::Ioc::Ioc288)}}};

    /* Upstream's start-tone detector is compiled out; this switches the ported
     * one on and defaults off, so the app behaves as upstream does. */
    ui::Checkbox check_tone_{{168, 14}, 0, "", true};

    ui::Text text_status_{{0, 40, 168, 16}, "Waiting for signal."};
    ui::Button button_ss_{{176, 40, 64, 30}, "START"};

    ui::Text text_geometry_{{0, 56, 240, 16}, ""};
    ui::Text text_note_{{0, 74, 240, 14}, ""};

    /* Upstream draws from y = 4*16; the port starts lower to leave room for the
     * geometry line and the two honesty banners. */
    wefax::ScanCanvas canvas_{{0, 90, 240, 304 - 90}};
};

}  // namespace app

#endif /*__MB200_UI_WEFAX_RX_H__*/
