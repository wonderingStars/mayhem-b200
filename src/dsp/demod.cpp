/*
 * mayhem-b200 — demodulators and audio conditioning.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "demod.hpp"

#include <algorithm>
#include <cmath>

namespace dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
}  // namespace

/* --- Nco ------------------------------------------------------------------- */

void Nco::set_frequency(double freq_hz, double sample_rate_hz) {
    frequency_hz_ = freq_hz;
    phase_step_ = (sample_rate_hz > 0.0) ? (kTwoPi * freq_hz / sample_rate_hz) : 0.0;
}

void Nco::reset() { phase_ = 0.0; }

void Nco::mix(const cfloat* in, cfloat* out, size_t count) {
    if (phase_step_ == 0.0) {
        if (in != out) std::copy(in, in + count, out);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        const cfloat rot{static_cast<float>(std::cos(phase_)),
                         static_cast<float>(std::sin(phase_))};
        out[i] = in[i] * rot;

        phase_ += phase_step_;
        /* Wrap to keep the double's precision from degrading over long runs. */
        if (phase_ >= kTwoPi)
            phase_ -= kTwoPi;
        else if (phase_ < 0.0)
            phase_ += kTwoPi;
    }
}

/* --- DcBlocker ------------------------------------------------------------- */

void DcBlocker::configure(float cutoff_hz, float sample_rate_hz) {
    if (sample_rate_hz <= 0.0f) {
        alpha_ = 0.999f;
        return;
    }
    /* y[n] = a*(y[n-1] + x[n] - x[n-1]); a set from the -3 dB corner. */
    const float rc = 1.0f / (2.0f * static_cast<float>(kPi) * std::max(0.1f, cutoff_hz));
    const float dt = 1.0f / sample_rate_hz;
    alpha_ = rc / (rc + dt);
    reset();
}

void DcBlocker::reset() {
    prev_in_ = 0.0f;
    prev_out_ = 0.0f;
}

float DcBlocker::process(float x) {
    const float y = alpha_ * (prev_out_ + x - prev_in_);
    prev_in_ = x;
    prev_out_ = y;
    return y;
}

/* --- Delay ----------------------------------------------------------------- */

void Delay::configure(size_t samples) {
    buffer_.assign(std::max<size_t>(samples, 1), 0.0f);
    pos_ = 0;
}

void Delay::reset() {
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    pos_ = 0;
}

float Delay::process(float x) {
    if (buffer_.empty()) return x;
    const float y = buffer_[pos_];
    buffer_[pos_] = x;
    pos_ = (pos_ + 1) % buffer_.size();
    return y;
}

/* --- AmDemod --------------------------------------------------------------- */

void AmDemod::configure(float sample_rate_hz) {
    /* 20 Hz corner: below any speech content, above the carrier drift we want
     * to reject. */
    dc_.configure(20.0f, sample_rate_hz);
}

void AmDemod::reset() { dc_.reset(); }

void AmDemod::process(const cfloat* in, size_t count, std::vector<float>& out) {
    out.reserve(out.size() + count);
    for (size_t i = 0; i < count; i++) {
        const float envelope = std::abs(in[i]);
        out.push_back(dc_.process(envelope));
    }
}

/* --- FmDemod --------------------------------------------------------------- */

void FmDemod::configure(float sample_rate_hz, float deviation_hz) {
    /* Instantaneous frequency in Hz is dphase * fs / 2pi. Dividing by the
     * deviation puts full deviation at +/-1.0. */
    if (deviation_hz <= 0.0f || sample_rate_hz <= 0.0f) {
        scale_ = 1.0f;
        return;
    }
    scale_ = static_cast<float>(sample_rate_hz / (kTwoPi * deviation_hz));
    reset();
}

void FmDemod::reset() {
    prev_ = cfloat{1.0f, 0.0f};
    last_power_ = 0.0f;
}

void FmDemod::process(const cfloat* in, size_t count, std::vector<float>& out) {
    out.reserve(out.size() + count);

    double power_acc = 0.0;

    for (size_t i = 0; i < count; i++) {
        const cfloat x = in[i];
        const cfloat d = x * std::conj(prev_);
        prev_ = x;

        power_acc += static_cast<double>(x.real()) * x.real() +
                     static_cast<double>(x.imag()) * x.imag();

        /* atan2(0,0) is 0 by convention, which is the right answer for a dead
         * carrier — no special case needed. */
        const float phase = std::atan2(d.imag(), d.real());
        out.push_back(phase * scale_);
    }

    last_power_ = (count > 0) ? static_cast<float>(power_acc / static_cast<double>(count)) : 0.0f;
}

/* --- SsbDemod -------------------------------------------------------------- */

void SsbDemod::configure(float /*sample_rate_hz*/, Sideband sideband, size_t hilbert_taps) {
    if (hilbert_taps < 15) hilbert_taps = 15;
    if ((hilbert_taps % 2) == 0) hilbert_taps += 1;

    auto taps = design_hilbert(hilbert_taps);
    hilbert_.configure(taps);

    /* A linear-phase FIR of odd length N delays by (N-1)/2 samples; the I path
     * must be delayed by the same amount or the sideband cancellation fails. */
    delay_.configure((hilbert_taps - 1) / 2);

    sideband_ = sideband;
}

void SsbDemod::reset() {
    hilbert_.reset();
    delay_.reset();
}

void SsbDemod::process(const cfloat* in, size_t count, std::vector<float>& out) {
    out.reserve(out.size() + count);

    const float sign = (sideband_ == Sideband::Upper) ? -1.0f : +1.0f;

    for (size_t i = 0; i < count; i++) {
        const float i_delayed = delay_.process(in[i].real());
        const float q_shifted = hilbert_.process_one(in[i].imag());
        out.push_back(i_delayed + sign * q_shifted);
    }
}

/* --- Deemphasis ------------------------------------------------------------ */

void Deemphasis::configure(float time_constant_us, float sample_rate_hz) {
    if (time_constant_us <= 0.0f || sample_rate_hz <= 0.0f) {
        enabled_ = false;
        alpha_ = 0.0f;
        return;
    }

    const float tau = time_constant_us * 1e-6f;
    const float dt = 1.0f / sample_rate_hz;
    alpha_ = dt / (tau + dt);
    enabled_ = true;
    reset();
}

void Deemphasis::reset() { state_ = 0.0f; }

void Deemphasis::process(float* samples, size_t count) {
    if (!enabled_) return;
    for (size_t i = 0; i < count; i++) {
        state_ += alpha_ * (samples[i] - state_);
        samples[i] = state_;
    }
}

/* --- AudioAgc -------------------------------------------------------------- */

void AudioAgc::configure(float sample_rate_hz,
                         float attack_ms,
                         float release_ms,
                         float target,
                         float max_gain) {
    if (sample_rate_hz <= 0.0f) sample_rate_hz = 48000.0f;

    /* One-pole coefficients from the requested time constants. */
    attack_ = 1.0f - std::exp(-1.0f / (std::max(0.1f, attack_ms) * 1e-3f * sample_rate_hz));
    release_ = 1.0f - std::exp(-1.0f / (std::max(0.1f, release_ms) * 1e-3f * sample_rate_hz));
    target_ = target;
    max_gain_ = max_gain;
    reset();
}

void AudioAgc::reset() {
    envelope_ = 0.0f;
    gain_ = 1.0f;
}

void AudioAgc::process(float* samples, size_t count) {
    if (!enabled_) return;

    for (size_t i = 0; i < count; i++) {
        const float magnitude = std::fabs(samples[i]);

        /* Fast attack on rising level, slow release on falling. */
        const float coeff = (magnitude > envelope_) ? attack_ : release_;
        envelope_ += coeff * (magnitude - envelope_);

        /* Floor the envelope so silence does not drive the gain to max_gain and
         * then blast the first syllable of the next transmission. */
        const float safe_env = std::max(envelope_, 1e-4f);
        const float desired = std::min(target_ / safe_env, max_gain_);

        gain_ += release_ * (desired - gain_);
        samples[i] *= gain_;
    }
}

/* --- Squelch --------------------------------------------------------------- */

void Squelch::configure(float sample_rate_hz, float hysteresis_db) {
    hysteresis_db_ = hysteresis_db;
    /* Smoothing over roughly 50 ms of blocks, independent of block size. */
    smoothing_ = (sample_rate_hz > 0.0f) ? 0.2f : 0.2f;
    reset();
}

void Squelch::set_level(uint8_t level_0_99) {
    level_ = std::min<uint8_t>(level_0_99, 99);
    /* Map 1..99 onto -90..-10 dBFS. 0 means "always open". */
    threshold_db_ = -90.0f + (static_cast<float>(level_) / 99.0f) * 80.0f;
}

void Squelch::reset() {
    smoothed_db_ = -140.0f;
    open_ = (level_ == 0);
}

bool Squelch::update(float rms_value) {
    if (level_ == 0) {
        open_ = true;
        return true;
    }

    const float db = to_db(rms_value);
    smoothed_db_ += smoothing_ * (db - smoothed_db_);

    /* Hysteresis: harder to open than to stay open. */
    if (open_) {
        if (smoothed_db_ < threshold_db_ - hysteresis_db_) open_ = false;
    } else {
        if (smoothed_db_ > threshold_db_) open_ = true;
    }

    return open_;
}

/* --- Resampler ------------------------------------------------------------- */

void Resampler::configure(double input_rate_hz, double output_rate_hz) {
    step_ = (output_rate_hz > 0.0) ? (input_rate_hz / output_rate_hz) : 1.0;
    reset();
}

void Resampler::reset() {
    phase_ = 0.0;
    last_ = 0.0f;
    primed_ = false;
}

void Resampler::process(const float* in, size_t count, std::vector<float>& out) {
    if (count == 0) return;

    if (!primed_) {
        last_ = in[0];
        primed_ = true;
    }

    for (size_t i = 0; i < count; i++) {
        const float current = in[i];

        /* Emit every output sample whose position falls inside [last, current). */
        while (phase_ < 1.0) {
            const float frac = static_cast<float>(phase_);
            out.push_back(last_ + (current - last_) * frac);
            phase_ += step_;
        }

        phase_ -= 1.0;
        last_ = current;
    }
}

/* --- helpers --------------------------------------------------------------- */

float rms(const cfloat* x, size_t count) {
    if (count == 0) return 0.0f;
    double acc = 0.0;
    for (size_t i = 0; i < count; i++)
        acc += static_cast<double>(x[i].real()) * x[i].real() +
               static_cast<double>(x[i].imag()) * x[i].imag();
    return static_cast<float>(std::sqrt(acc / static_cast<double>(count)));
}

float rms(const float* x, size_t count) {
    if (count == 0) return 0.0f;
    double acc = 0.0;
    for (size_t i = 0; i < count; i++) acc += static_cast<double>(x[i]) * x[i];
    return static_cast<float>(std::sqrt(acc / static_cast<double>(count)));
}

float to_db(float amplitude, float floor_db) {
    const float floor_amp = std::pow(10.0f, floor_db / 20.0f);
    return 20.0f * std::log10(std::max(amplitude, floor_amp));
}

}  // namespace dsp
