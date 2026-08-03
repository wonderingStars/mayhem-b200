/*
 * mayhem-b200 — VOR receiver.
 *
 * Ported from firmware/application/external/vor_rx/ (ui_vor_rx.*) and its
 * baseband processor firmware/baseband/proc_vor_rx.* .
 *
 * What a VOR transmits (this is the specification the decode follows):
 *
 *   A VHF Omnidirectional Range beacon on 108.00-117.95 MHz radiates an AM
 *   carrier carrying two 30 Hz tones.
 *
 *     - The VARIABLE tone is 30 Hz amplitude modulation of the carrier itself,
 *       produced by a rotating (or electronically rotated) directional pattern.
 *       Its phase at the receiver depends on where the receiver is: it is what
 *       carries the bearing.
 *     - The REFERENCE tone is 30 Hz frequency modulation, +/-480 Hz, of a
 *       9960 Hz subcarrier which is itself amplitude-modulated onto the same
 *       carrier. Being FM on a subcarrier it arrives with the same phase
 *       everywhere.
 *
 *   The magnetic bearing FROM the station (the "radial") is the phase by which
 *   the variable tone lags the reference tone. Both tones sit at 30 Hz and both
 *   are nominally 30% modulation; a 1020 Hz Morse identification tone and (on
 *   some stations) voice share the same channel and are ignored here.
 *
 * Decode pipeline, the same one proc_vor_rx.cpp runs:
 *
 *   IQ -> mix to the tuned carrier -> channel filter/decimate -> AM envelope
 *      -> +-- correlate the envelope against a local 30 Hz tone .... VARIABLE
 *         |
 *         +-- quadrature down-convert 9960 Hz -> one-pole low-pass
 *             -> FM discriminate (arg of z[n] * conj(z[n-1]))
 *             -> correlate that against the same local 30 Hz tone ... REFERENCE
 *
 *   radial = arg(REFERENCE) - arg(VARIABLE) + receive-chain phase lag
 *
 * Deliberate departures from the firmware, all noted where they occur:
 *
 *  - The envelope is NOT low-passed to 4.5 kHz. Upstream omits the AM decim_2
 *    stage for exactly this reason: the 9960 Hz subcarrier has to survive. That
 *    also rules out ReceiverModel's audio tap on the host, whose AM path runs a
 *    4.5 kHz audio filter, an AGC and a DC blocker — all three would destroy the
 *    decode. See ui_vor_rx.cpp for what tap this app uses instead.
 *  - The local oscillators are double-precision phase accumulators rather than
 *    the firmware's recursive rotator. The rotator exists because an M4 cannot
 *    afford a sine per sample; it is not part of the protocol.
 *  - The window accumulates per contiguous SEGMENT and combines segments as
 *    REFERENCE * conj(VARIABLE). For one contiguous segment per window — the
 *    firmware's case — that is algebraically identical to upstream's
 *    arg(REFERENCE) - arg(VARIABLE). It matters on the host because the only
 *    sample tap available delivers bursts with gaps between them (see below).
 *  - `ref_level` is reported as recovered subcarrier deviation in Hz rather than
 *    upstream's radians-per-sample x 10000, so the lock thresholds do not have
 *    to be re-derived for every sample rate.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2026 PortaPack Mayhem (original app and baseband)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_VOR_RX_H__
#define __MB200_UI_VOR_RX_H__

#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace app {

/* --- Protocol constants (ICAO Annex 10 / upstream proc_vor_rx.hpp) ---------- */

/* Both tones. The reference is FM on the subcarrier, the variable is AM on the
 * carrier; they are the same frequency, which is why the bearing is a phase. */
constexpr float kVorToneHz = 30.0f;
constexpr float kVorSubcarrierHz = 9960.0f;
/* Peak FM deviation of the subcarrier by the reference tone. */
constexpr float kVorSubcarrierDeviationHz = 480.0f;

/* Corner of the one-pole low-pass that isolates the down-converted subcarrier.
 * Upstream hard-codes alpha = 0.9245 at 48 kHz; exp(-2*pi*600/48000) = 0.92446,
 * so stating the corner instead reproduces upstream's filter exactly at 48 kHz
 * and keeps it meaningful at any other rate. */
constexpr float kVorSubcarrierLpCornerHz = 600.0f;

/* Analysis window: upstream uses 4800 samples at 48 kHz, i.e. 100 ms, which is
 * exactly three cycles of 30 Hz. Whole cycles matter — that is what rejects the
 * carrier DC from the variable correlator. */
constexpr int kVorWindowCycles = 3;

/* Lock thresholds, upstream's converted to rate-independent units.
 *
 *  - Upstream requires the FM-demodulated 30 Hz amplitude > 0.03 rad/sample.
 *    At its fixed 48 kHz that is 0.03 * 48000 / (2*pi) = 229.2 Hz of recovered
 *    subcarrier deviation (nominal is 480 Hz).
 *  - Upstream's quality reference of 0.065 rad/sample is 496.6 Hz.
 *  - The AM depth threshold (0.15) is already scale-free: it is the 30 Hz
 *    envelope amplitude over the carrier level, ~0.30 for a real VOR. */
constexpr float kVorMinReferenceDeviationHz = 229.2f;
constexpr float kVorQualityReferenceDeviationHz = 496.6f;
constexpr float kVorMinVariableDepth = 0.15f;

/* Band edges, for the on-screen readout and an out-of-band warning. */
constexpr uint64_t kVorBandLowHz = 108'000'000ull;
constexpr uint64_t kVorBandHighHz = 117'950'000ull;
/* VOR channel spacing (50 kHz); the ILS/VOR grid also allows 100 kHz. */
constexpr uint64_t kVorChannelStepHz = 50'000ull;

/* --- Pure helpers (all exercised directly by tests/test_vor_rx.cpp) --------- */

/* Upstream VorRx::normalize_degrees(): wrap into [0, 360) and round. Note that
 * a value that rounds to 360 must come back as 0. */
inline uint16_t vor_normalize_degrees(float degrees) {
    while (degrees < 0.0f) degrees += 360.0f;
    while (degrees >= 360.0f) degrees -= 360.0f;
    auto d = static_cast<int32_t>(degrees + 0.5f);
    if (d >= 360) d -= 360;
    if (d < 0) d += 360;
    return static_cast<uint16_t>(d);
}

/* Upstream VorCdiIndicator::normalize_signed_degrees(): wrap to (-180, 180]. */
inline int32_t vor_wrap_signed_degrees(int32_t degrees) {
    while (degrees <= -180) degrees += 360;
    while (degrees > 180) degrees -= 360;
    return degrees;
}

/* Upstream VorLogger::log_status()'s deviation term: how far the received
 * radial is from the selected OBS course, wrapped to [-180, 180). */
inline int32_t vor_course_deviation(int32_t radial_deg, int32_t course_deg) {
    return ((radial_deg - course_deg + 540) % 360) - 180;
}

/* Upstream VorRxView::calibrated_radial(): apply the user's calibration offset,
 * wrapped back into [0, 360). */
inline uint16_t vor_calibrated_radial(uint16_t radial_deg, int32_t calibration_deg) {
    int32_t value = (static_cast<int32_t>(radial_deg) + calibration_deg) % 360;
    if (value < 0) value += 360;
    return static_cast<uint16_t>(value);
}

/* Upstream VorCdiIndicator::paint()'s needle placement: the deviation clamped
 * to +/-10 degrees full scale, mapped onto +/-2 tick spacings. */
inline int32_t vor_cdi_needle_offset(int32_t radial_deg, int32_t course_deg,
                                     int32_t tick_spacing) {
    const int32_t deviation = vor_wrap_signed_degrees(radial_deg - course_deg);
    const int32_t scaled = (deviation < -10) ? -10 : ((deviation > 10) ? 10 : deviation);
    return (scaled * (2 * tick_spacing)) / 10;
}

/* Upstream VorRxView::to_from_label()'s state machine, lifted out so it can be
 * tested. TO/FROM is not a property of the radial: it says whether flying the
 * selected OBS course takes you toward the station or away from it, so it is
 * decided by the radial-to-course difference, switching at the 90 degree abeam
 * point. The +/-5 degree dead zone holds the previous answer so noise near
 * abeam does not flicker the flag. */
enum class VorFlag : uint8_t { Unknown = 0, From = 1, To = 2 };

inline const char* vor_flag_label(VorFlag flag) {
    switch (flag) {
        case VorFlag::From: return "FROM";
        case VorFlag::To: return "TO";
        case VorFlag::Unknown: break;
    }
    return "--";
}

class VorFlagState {
   public:
    VorFlag update(int32_t radial_deg, int32_t course_deg, bool valid) {
        if (!valid) {
            flag_ = VorFlag::Unknown;
            return flag_;
        }
        const int32_t diff = vor_course_deviation(radial_deg, course_deg);
        const int32_t adiff = (diff < 0) ? -diff : diff;
        if (adiff < 85)
            flag_ = VorFlag::From;
        else if (adiff > 95)
            flag_ = VorFlag::To;
        return flag_;
    }

    VorFlag flag() const { return flag_; }
    void reset() { flag_ = VorFlag::Unknown; }

   private:
    VorFlag flag_{VorFlag::Unknown};
};

/* Upstream VorRxView::smooth_radial(): a wrapped exponential moving average in
 * 1/64 degree fixed point, stepping 20% of the shortest arc toward each new
 * reading. Kept in fixed point (rather than rewritten with trig) so it behaves
 * exactly as upstream's does, including at the 0/360 wrap. */
class VorRadialSmoother {
   public:
    static constexpr int32_t scale = 64;
    static constexpr int32_t full_circle = 360 * scale;

    uint16_t update(uint16_t radial_deg) {
        const int32_t target = static_cast<int32_t>(radial_deg) * scale;
        if (!valid_) {
            value_ = target;
            valid_ = true;
        } else {
            int32_t diff = (target - value_) % full_circle;
            if (diff < -full_circle / 2)
                diff += full_circle;
            else if (diff > full_circle / 2)
                diff -= full_circle;
            value_ += diff / 5;
            value_ %= full_circle;
            if (value_ < 0) value_ += full_circle;
        }
        return static_cast<uint16_t>((value_ + scale / 2) / scale) % 360;
    }

    /* Dropped whenever lock is lost, so a fresh lock is not dragged by stale
     * history (upstream does this in on_vor_status()). */
    void reset() {
        valid_ = false;
        value_ = 0;
    }
    bool primed() const { return valid_; }

   private:
    bool valid_{false};
    int32_t value_{0};
};

/* --- Decoder status, the host's VorRxStatusDataMessage ---------------------- */

struct VorStatus {
    /* (reference - variable) phase, degrees, wrapped to [0, 360). */
    uint16_t phase_deg{0};
    /* The radial. Same number as phase_deg — upstream reports both. */
    uint16_t radial_deg{0};
    /* Recovered subcarrier FM deviation of the 30 Hz reference, in Hz.
     * Nominal 480; upstream sends radians-per-sample x 10000 instead. */
    uint16_t ref_level{0};
    /* 30 Hz AM depth over the carrier level, x1000. Nominal 300. */
    uint16_t var_level{0};
    uint8_t quality{0};  /* 0..100 */
    bool valid{false};
};

/* --- The decoder ------------------------------------------------------------
 *
 * Feed it the AM envelope (magnitude of the channel-filtered IQ, carrier DC
 * left in — the DC is the reference the modulation depth is measured against).
 * Every window_samples() samples it produces one VorStatus.
 *
 * Contiguity: the sample stream must be contiguous *within* a segment. Call
 * mark_discontinuity() before the first sample of a block that did not
 * immediately follow the previous one. Segments are combined as
 * REFERENCE * conj(VARIABLE), which cancels the unknown timing offset a gap
 * introduces (it rotates both tones by the same amount, because both are 30 Hz
 * in real time). Nothing else in the decode depends on where a segment starts:
 * the bearing is a phase DIFFERENCE, so the local oscillator's own start phase
 * cancels too. */
class VorDecoder {
   public:
    void configure(float sample_rate_hz) {
        sample_rate_ = (sample_rate_hz > 0.0f) ? sample_rate_hz : 48000.0f;

        const auto cycle = std::lround(static_cast<double>(sample_rate_) /
                                       static_cast<double>(kVorToneHz));
        window_samples_ = static_cast<uint32_t>(
            (cycle < 1 ? 1 : cycle) * kVorWindowCycles);

        tone_step_ = 2.0 * M_PI * static_cast<double>(kVorToneHz) /
                     static_cast<double>(sample_rate_);
        sub_step_ = 2.0 * M_PI * static_cast<double>(kVorSubcarrierHz) /
                    static_cast<double>(sample_rate_);

        /* One-pole low-pass, y += (1-a)(x - y), a = exp(-2*pi*fc/fs). */
        lp_a_ = std::exp(-2.0f * static_cast<float>(M_PI) *
                         kVorSubcarrierLpCornerHz / sample_rate_);

        /* Carrier-level estimator, used to keep the carrier out of the
         * subcarrier down-conversion and to project it out of the variable
         * correlation of a sub-cycle segment. It runs as a true cumulative mean
         * until one second of samples has been seen and as a 1 s exponential
         * average afterwards. Starting cumulative matters: a plain exponential
         * average begins at whatever the first sample happened to be and is
         * still tens of percent out a whole window later. */
        dc_ema_samples_ = (sample_rate_ > 1.0f) ? static_cast<uint32_t>(sample_rate_) : 1u;

        reference_phase_lag_deg_ = compute_reference_phase_lag_deg(sample_rate_, lp_a_);

        reset();
    }

    void reset() {
        window_count_ = 0;
        segment_count_ = 0;
        metric_samples_ = 0;
        seg_var_i_ = seg_var_q_ = 0.0f;
        seg_ref_i_ = seg_ref_q_ = 0.0f;
        seg_tone_i_ = seg_tone_q_ = 0.0f;
        prod_i_ = prod_q_ = 0.0f;
        carrier_i_ = carrier_q_ = 0.0f;
        var_mag_acc_ = ref_mag_acc_ = 0.0f;
        dc_sum_ = 0.0;
        tone_phase_ = 0.0;
        sub_phase_ = 0.0;
        dc_est_ = 0.0f;
        dc_count_ = 0;
        have_status_ = false;
        mark_discontinuity();
    }

    /* "The next sample starts a new contiguous run." Closes the segment in
     * progress so its phase is banked before the unknown gap rotates the tones,
     * primes the subcarrier low-pass from the next sample instead of carrying a
     * stale state across the gap, and contributes zero to the FM discriminator
     * for that one sample (a phase difference needs a predecessor). The
     * variable correlator keeps every sample. */
    void mark_discontinuity() {
        close_segment();
        priming_ = true;
    }

    void process(const float* envelope, size_t count) {
        if (window_samples_ == 0) configure(48000.0f);

        for (size_t i = 0; i < count; ++i) {
            const float sample = envelope[i];

            const auto tone_cos = static_cast<float>(std::cos(tone_phase_));
            const auto tone_sin = static_cast<float>(std::sin(tone_phase_));
            const auto sub_cos = static_cast<float>(std::cos(sub_phase_));
            const auto sub_sin = static_cast<float>(std::sin(sub_phase_));

            if (dc_count_ < dc_ema_samples_) ++dc_count_;
            dc_est_ += (sample - dc_est_) / static_cast<float>(dc_count_);

            /* VARIABLE: 30 Hz amplitude modulation of the carrier envelope.
             * Correlated raw, with the local tone's own sum kept alongside so
             * the carrier can be projected out afterwards (see finish_window). */
            seg_var_i_ += sample * tone_cos;
            seg_var_q_ -= sample * tone_sin;
            seg_tone_i_ += tone_cos;
            seg_tone_q_ -= tone_sin;
            dc_sum_ += static_cast<double>(sample);

            /* REFERENCE: quadrature down-convert the 9960 Hz subcarrier, keep
             * its +/-480 Hz baseband, then FM-discriminate.
             *
             * The carrier is subtracted before the mix. Upstream mixes the raw
             * envelope, which lands the whole carrier at 9960 Hz in the
             * down-converted signal; the one-pole below only holds that ~24 dB
             * down, so what is left rotates the subcarrier vector and biases the
             * recovered reference phase by an amount that depends on the ratio
             * of carrier to subcarrier depth. Measured on an ideal composite
             * that bias moved 3.4 degrees between 20% and 30% subcarrier depth.
             * Mixing the carrier-free envelope removes the term at source: the
             * residual is then flat to better than 0.02 degrees over 20-40%
             * subcarrier depth, 20-30% variable depth and a 4:1 carrier change,
             * which is what makes a single correction constant meaningful. */
            const float ac = sample - dc_est_;
            const float sc_i = ac * sub_cos;
            const float sc_q = -ac * sub_sin;

            float fm;
            if (priming_) {
                lp_i_ = sc_i;
                lp_q_ = sc_q;
                fm = 0.0f;
                priming_ = false;
            } else {
                lp_i_ += (1.0f - lp_a_) * (sc_i - lp_i_);
                lp_q_ += (1.0f - lp_a_) * (sc_q - lp_q_);
                const float num = (lp_q_ * prev_i_) - (lp_i_ * prev_q_);
                const float den = (lp_i_ * prev_i_) + (lp_q_ * prev_q_);
                fm = std::atan2(num, den);
            }
            prev_i_ = lp_i_;
            prev_q_ = lp_q_;

            seg_ref_i_ += fm * tone_cos;
            seg_ref_q_ -= fm * tone_sin;

            tone_phase_ += tone_step_;
            if (tone_phase_ >= 2.0 * M_PI) tone_phase_ -= 2.0 * M_PI;
            sub_phase_ += sub_step_;
            if (sub_phase_ >= 2.0 * M_PI) sub_phase_ -= 2.0 * M_PI;

            ++segment_count_;
            ++window_count_;
            if (window_count_ >= window_samples_) finish_window();
        }
    }

    /* Returns the newest status once; false until the next window completes. */
    bool take_status(VorStatus& out) {
        if (!have_status_) return false;
        out = status_;
        have_status_ = false;
        return true;
    }

    /* Peek without consuming — the UI repaints more often than windows land. */
    const VorStatus& status() const { return status_; }
    bool has_status() const { return have_status_; }

    float sample_rate() const { return sample_rate_; }
    uint32_t window_samples() const { return window_samples_; }
    float window_seconds() const {
        return (sample_rate_ > 0.0f) ? static_cast<float>(window_samples_) / sample_rate_ : 0.0f;
    }
    float reference_phase_lag_deg() const { return reference_phase_lag_deg_; }
    float subcarrier_lp_alpha() const { return lp_a_; }

    /* The receive chain delays the recovered reference tone but not the
     * variable tone, so the raw (reference - variable) reads low and the lag is
     * added back. Two contributions, both at 30 Hz:
     *
     *   - the one-pole subcarrier low-pass, arg of 1/(1 - a e^-jw);
     *   - the FM discriminator, which reports the frequency between samples
     *     n-1 and n, i.e. half a sample of delay.
     *
     * At 48 kHz with a = 0.9245 this evaluates to 2.75 + 0.11 = 2.86 degrees.
     *
     * A third term is measured rather than derived. The one-pole is only 600 Hz
     * wide while the subcarrier's FM occupies +/-510 Hz (Carson, deviation 480,
     * modulating 30), so the recovered tone does not arrive at exactly the phase
     * the small-signal response predicts. Decoding an ideal composite and
     * bisecting for the input bearing at which the reported degree flips puts
     * the residual at -0.34 degrees, unchanged across 41.7/48/50 kHz, 20-40%
     * subcarrier depth, 20-30% variable depth, a 4:1 carrier level change and
     * 460 vs 480 Hz deviation — so it is a property of the demodulator, not of
     * the signal, and is corrected here.
     *
     * Total: 2.52 degrees at 48 kHz. Upstream carries a hard-coded 3.2 degrees
     * from a TX->RX loopback on real firmware, resolved only to whole degrees. */
    static constexpr float kSubcarrierDemodBiasDeg = -0.34f;

    static float compute_reference_phase_lag_deg(float sample_rate_hz, float lp_alpha) {
        if (sample_rate_hz <= 0.0f) return 0.0f;
        const double w = 2.0 * M_PI * static_cast<double>(kVorToneHz) /
                         static_cast<double>(sample_rate_hz);
        const double a = static_cast<double>(lp_alpha);
        const double lpf = std::atan2(a * std::sin(w), 1.0 - a * std::cos(w));
        const double diff = 0.5 * w;
        return static_cast<float>((lpf + diff) * 180.0 / M_PI) + kSubcarrierDemodBiasDeg;
    }

   private:
    /* Fold the segment that just ended into the window totals.
     *
     * Two complex sums are banked rather than one. Writing V for the segment's
     * raw variable correlation, T for the same correlation of a constant 1 (the
     * local tone summed over the segment) and R for the reference correlation,
     * the carrier-free variable vector is V - carrier * T. The carrier is not
     * known accurately until the window's own mean is available, so instead of
     * committing to an estimate here both
     *
     *     A = sum of R * conj(V)      and      B = sum of R * conj(T)
     *
     * are accumulated, and the window closes with A - carrier * B. That is
     * exact for any carrier value, so the bearing does not depend on how well a
     * running estimate happened to have converged.
     *
     * Over a whole number of 30 Hz cycles T is zero and the correction does
     * nothing, which is upstream's case. */
    void close_segment() {
        if (segment_count_ == 0) return;

        /* REFERENCE * conj(VARIABLE): its argument is the phase difference,
         * with any common rotation (an unknown gap before this segment, or the
         * local oscillator's arbitrary start phase) cancelled. */
        prod_i_ += (seg_ref_i_ * seg_var_i_) + (seg_ref_q_ * seg_var_q_);
        prod_q_ += (seg_ref_q_ * seg_var_i_) - (seg_ref_i_ * seg_var_q_);

        /* REFERENCE * conj(TONE SUM): the carrier's contribution to the above. */
        carrier_i_ += (seg_ref_i_ * seg_tone_i_) + (seg_ref_q_ * seg_tone_q_);
        carrier_q_ += (seg_ref_q_ * seg_tone_i_) - (seg_ref_i_ * seg_tone_q_);

        /* Sum of |.|, so dividing by the sample count gives the tone amplitude
         * exactly as upstream's 2*sqrt(i^2+q^2)/N does for a single segment.
         * These are lock indicators rather than the bearing, so the running
         * carrier estimate is good enough to de-bias them. */
        const float var_i = seg_var_i_ - dc_est_ * seg_tone_i_;
        const float var_q = seg_var_q_ - dc_est_ * seg_tone_q_;
        var_mag_acc_ += 2.0f * std::sqrt((var_i * var_i) + (var_q * var_q));
        ref_mag_acc_ += 2.0f * std::sqrt((seg_ref_i_ * seg_ref_i_) + (seg_ref_q_ * seg_ref_q_));
        metric_samples_ += segment_count_;

        seg_var_i_ = seg_var_q_ = 0.0f;
        seg_ref_i_ = seg_ref_q_ = 0.0f;
        seg_tone_i_ = seg_tone_q_ = 0.0f;
        segment_count_ = 0;
    }

    void finish_window() {
        close_segment();

        VorStatus s{};
        if (metric_samples_ > 0) {
            const auto inv_n = 1.0f / static_cast<float>(metric_samples_);
            const auto dc_level = static_cast<float>(dc_sum_ / static_cast<double>(metric_samples_));
            const float var_amp = var_mag_acc_ * inv_n;
            const float ref_amp = ref_mag_acc_ * inv_n;
            const float var_ratio = (dc_level > 0.0f) ? (var_amp / dc_level) : 0.0f;

            /* rad/sample -> Hz of subcarrier deviation. */
            const float ref_dev_hz = ref_amp * sample_rate_ / (2.0f * static_cast<float>(M_PI));

            /* Project the carrier out. On a contiguous window the correction
             * term is identically zero (whole 30 Hz cycles), so the estimate
             * used cannot matter; it only bites when the tap delivered
             * sub-cycle bursts, and there the running mean is the better
             * estimate — it averages the modulation over a second rather than
             * over whichever fragments of a cycle happened to be sampled. For
             * the very first window the running mean IS the window mean, since
             * it starts life as a cumulative average. */
            const float phase_rad = std::atan2(prod_q_ - dc_est_ * carrier_q_,
                                               prod_i_ - dc_est_ * carrier_i_);
            const float phase_deg = (phase_rad * 180.0f / static_cast<float>(M_PI)) +
                                    reference_phase_lag_deg_;

            s.phase_deg = vor_normalize_degrees(phase_deg);
            s.radial_deg = s.phase_deg;
            s.ref_level = static_cast<uint16_t>((ref_dev_hz > 65535.0f) ? 65535.0f
                                                                       : ((ref_dev_hz > 0.0f) ? ref_dev_hz : 0.0f));
            const float var_scaled = var_ratio * 1000.0f;
            s.var_level = static_cast<uint16_t>((var_scaled > 65535.0f) ? 65535.0f
                                                                       : ((var_scaled > 0.0f) ? var_scaled : 0.0f));
            s.valid = (ref_dev_hz > kVorMinReferenceDeviationHz) &&
                      (var_ratio > kVorMinVariableDepth);
            if (s.valid) {
                float q = (ref_dev_hz / kVorQualityReferenceDeviationHz) * 100.0f;
                if (q < 0.0f) q = 0.0f;
                if (q > 100.0f) q = 100.0f;
                s.quality = static_cast<uint8_t>(q);
            }
        }

        status_ = s;
        have_status_ = true;

        window_count_ = 0;
        metric_samples_ = 0;
        prod_i_ = prod_q_ = 0.0f;
        carrier_i_ = carrier_q_ = 0.0f;
        var_mag_acc_ = ref_mag_acc_ = 0.0f;
        dc_sum_ = 0.0;
    }

    float sample_rate_{48000.0f};
    uint32_t window_samples_{4800};

    double tone_step_{0.0};
    double sub_step_{0.0};
    double tone_phase_{0.0};
    double sub_phase_{0.0};

    float lp_a_{0.9245f};
    float lp_i_{0.0f};
    float lp_q_{0.0f};
    float prev_i_{0.0f};
    float prev_q_{0.0f};
    bool priming_{true};

    uint32_t dc_ema_samples_{48000};
    uint32_t dc_count_{0};
    float dc_est_{0.0f};

    float seg_var_i_{0.0f};
    float seg_var_q_{0.0f};
    float seg_ref_i_{0.0f};
    float seg_ref_q_{0.0f};
    float seg_tone_i_{0.0f};
    float seg_tone_q_{0.0f};
    uint32_t segment_count_{0};

    float prod_i_{0.0f};
    float prod_q_{0.0f};
    float carrier_i_{0.0f};
    float carrier_q_{0.0f};
    float var_mag_acc_{0.0f};
    float ref_mag_acc_{0.0f};
    double dc_sum_{0.0};
    uint32_t metric_samples_{0};
    uint32_t window_count_{0};

    float reference_phase_lag_deg_{0.0f};

    VorStatus status_{};
    bool have_status_{false};
};

/* --- Front end: raw IQ -> AM envelope --------------------------------------
 *
 * The host counterpart of proc_vor_rx's decim_0/decim_1/channel_filter/demod_am
 * chain. Kept separate from VorDecoder so the decoder can be tested on
 * synthetic audio with no filter design involved. */
class VorChannelizer {
   public:
    /* The VOR channel is 2 x (9960 + 480) wide plus room for the filter skirt.
     * 40 kHz keeps the decode close to upstream's fixed 48 kHz. */
    static constexpr double kMinChannelRateHz = 40000.0;
    /* Half-bandwidth of the channel filter: past the subcarrier's upper
     * sideband, short of anything on the adjacent 50 kHz channel. */
    static constexpr double kChannelCutoffHz = 13000.0;

    /* Picks a channel rate of at least kMinChannelRateHz so the 9960 Hz
     * subcarrier and its +/-480 Hz sidebands stay well inside Nyquist, then
     * designs the matching low-pass. Returns the channel rate. */
    float configure(double input_rate_hz, double carrier_offset_hz) {
        input_rate_ = input_rate_hz;
        offset_hz_ = carrier_offset_hz;

        if (input_rate_ <= 0.0) {
            channel_rate_ = 0.0f;
            decimation_ = 1;
            return 0.0f;
        }

        decimation_ = 1;
        if (input_rate_ > kMinChannelRateHz)
            decimation_ = static_cast<size_t>(input_rate_ / kMinChannelRateHz);
        if (decimation_ < 1) decimation_ = 1;

        const double rate = input_rate_ / static_cast<double>(decimation_);
        channel_rate_ = static_cast<float>(rate);

        /* Keep the whole subcarrier and stop before the neighbouring 50 kHz
         * channel. If the radio is running so slowly that even that does not
         * fit, fall back to just under Nyquist rather than designing nonsense. */
        const double cutoff = (kChannelCutoffHz < rate * 0.45) ? kChannelCutoffHz : rate * 0.45;
        const double transition = (cutoff * 0.3 > 500.0) ? cutoff * 0.3 : 500.0;
        filter_.configure(dsp::design_lowpass(cutoff, transition, input_rate_, 60.0, 1023),
                          decimation_);

        nco_.set_frequency(-offset_hz_, input_rate_);
        nco_.reset();
        return channel_rate_;
    }

    void set_carrier_offset(double offset_hz) {
        offset_hz_ = offset_hz;
        nco_.set_frequency(-offset_hz_, input_rate_);
    }

    double carrier_offset() const { return offset_hz_; }
    float channel_rate() const { return channel_rate_; }
    size_t decimation() const { return decimation_; }

    void reset() {
        filter_.reset();
        nco_.reset();
    }

    /* Mixes, filters, decimates and envelope-detects. Appends to `envelope`.
     * Returns the number of envelope samples appended. */
    size_t process(const dsp::cfloat* in, size_t count, std::vector<float>& envelope) {
        if (in == nullptr || count == 0 || channel_rate_ <= 0.0f) return 0;

        mixed_.resize(count);
        nco_.mix(in, mixed_.data(), count);

        channel_.clear();
        filter_.process(mixed_.data(), count, channel_);

        /* AM envelope, carrier DC left in — the decoder measures modulation
         * depth against it. Same magnitude detector as the firmware's
         * dsp::demodulate::AM; what is deliberately absent is any DC blocker or
         * audio low-pass, either of which would take the subcarrier with it. */
        const size_t before = envelope.size();
        envelope.reserve(before + channel_.size());
        for (const auto& z : channel_) envelope.push_back(std::abs(z));
        return envelope.size() - before;
    }

   private:
    dsp::Nco nco_{};
    dsp::FirDecimateC filter_{};
    std::vector<dsp::cfloat> mixed_{};
    std::vector<dsp::cfloat> channel_{};
    double input_rate_{0.0};
    double offset_hz_{0.0};
    float channel_rate_{0.0f};
    size_t decimation_{1};
};

/* --- Widgets ---------------------------------------------------------------- */

/* Upstream's course deviation indicator: a horizontal scale with a vertical
 * needle whose offset is the radial-to-course error, +/-10 degrees full scale. */
class VorCdiIndicator : public ui::Widget {
   public:
    explicit VorCdiIndicator(ui::Rect parent_rect);

    void set_course(uint16_t course_deg);
    void set_radial(uint16_t radial_deg);
    void set_valid(bool valid);

    void paint(ui::Painter& painter) override;

   private:
    static constexpr int32_t tick_spacing = 40;

    uint16_t course_deg_{0};
    uint16_t radial_deg_{0};
    bool valid_{false};
};

/* A compass rose with the received radial drawn as a needle and the selected
 * OBS course as a second marker. Upstream has no such widget — a PortaPack
 * cannot spare the trig — but the radial is a bearing, and a bearing is far
 * easier to read off a card than off a number. */
class VorCompass : public ui::Widget {
   public:
    explicit VorCompass(ui::Rect parent_rect);

    void set_radial(uint16_t radial_deg);
    void set_course(uint16_t course_deg);
    void set_valid(bool valid);

    void paint(ui::Painter& painter) override;

   private:
    /* Screen position of a point at `radius` px along `bearing_deg`, measured
     * clockwise from up — the convention a compass card is drawn in. */
    static ui::Point polar(ui::Point center, int radius, int32_t bearing_deg);

    uint16_t radial_deg_{0};
    uint16_t course_deg_{0};
    bool valid_{false};
};

/* --- The view --------------------------------------------------------------- */

class VorRxView : public ui::View {
   public:
    VorRxView();
    ~VorRxView() override;

    VorRxView(const VorRxView&) = delete;
    VorRxView& operator=(const VorRxView&) = delete;

    std::string title() const override { return "VOR RX"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void start_receiver();
    void stop_receiver();
    void update_status_text();
    void update_tap_note();
    void pump_samples();
    void on_vor_status(const VorStatus& status);
    void refresh_radial();
    void update_logging();
    void log_status(const VorStatus& status, uint16_t radial_deg);

    radio::ReceiverModel& receiver_;

    VorChannelizer channelizer_{};
    VorDecoder decoder_{};
    VorRadialSmoother smoother_{};
    VorFlagState flag_{};

    std::vector<dsp::cfloat> raw_{};
    std::vector<float> envelope_{};

    bool running_{false};
    bool logging_{false};
    bool have_status_{false};
    bool last_valid_{false};
    uint16_t last_radial_deg_{0};
    uint32_t windows_decoded_{0};
    uint32_t frame_counter_{0};
    double configured_input_rate_{0.0};
    std::unique_ptr<std::ofstream> log_file_{};
    std::string log_path_{};

    ui::Labels labels_{
        {{0, 2}, "Freq", ui::Color::light_grey()},
        {{0, 20}, "Gain", ui::Color::light_grey()},
        {{120, 20}, "Vol", ui::Color::light_grey()},
        {{0, 38}, "State", ui::Color::light_grey()},
        {{0, 56}, "Radial", ui::Color::light_grey()},
        {{136, 56}, "Flag", ui::Color::light_grey()},
        {{0, 90}, "OBS", ui::Color::light_grey()},
        {{120, 90}, "Cal", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 2}};
    ui::FrequencyStepView step_view_{{132, 2}, field_frequency_};

    ui::NumberField field_gain_{{40, 20}, 3, {0, 76}, 1, ' '};
    ui::NumberField field_volume_{{160, 20}, 2, {0, 99}, 1, ' '};

    ui::Text text_state_{{48, 38, 104, 16}, "Idle"};
    ui::Button button_start_stop_{{156, 32, 80, 22}, "Start"};

    ui::Text text_radial_{{56, 56, 72, 16}, "--"};
    ui::Text text_flag_{{176, 56, 64, 16}, "--"};
    ui::Text text_metrics_{{0, 72, 240, 16}, "Dev --- Hz  Depth ---  Q --"};

    ui::NumberField field_course_{{40, 90}, 3, {0, 359}, 1, '0', true};
    ui::NumberField field_calibration_{{160, 90}, 4, {-359, 359}, 1, ' ', true};

    VorCdiIndicator cdi_{{0, 108, 240, 28}};
    VorCompass compass_{{50, 140, 140, 140}};

    ui::Checkbox check_log_{{0, 280}, 8, "Log CSV", true};
    ui::Text text_note_{{96, 284, 144, 16}, ""};
};

}  // namespace app

#endif /*__MB200_UI_VOR_RX_H__*/
