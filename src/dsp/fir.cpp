/*
 * mayhem-b200 — FIR filter design and decimating filters.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "fir.hpp"

#include <algorithm>
#include <cmath>

namespace dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

/* Zeroth-order modified Bessel function of the first kind, for the Kaiser
 * window. The series converges quickly for the beta values we use (< 12). */
double bessel_i0(double x) {
    double sum = 1.0;
    double term = 1.0;
    const double half_x = x / 2.0;

    for (int k = 1; k < 64; k++) {
        term *= (half_x / k) * (half_x / k);
        sum += term;
        if (term < sum * 1e-16) break;
    }
    return sum;
}

double sinc(double x) {
    if (std::fabs(x) < 1e-12) return 1.0;
    return std::sin(kPi * x) / (kPi * x);
}

}  // namespace

std::vector<float> design_lowpass(double cutoff_hz,
                                  double transition_hz,
                                  double sample_rate_hz,
                                  double stopband_db,
                                  size_t max_taps) {
    if (sample_rate_hz <= 0.0) return {1.0f};

    /* Keep the cutoff strictly inside Nyquist; a cutoff at or above it produces
     * an all-pass that silently defeats the anti-alias guarantee. */
    const double nyquist = sample_rate_hz / 2.0;
    cutoff_hz = std::clamp(cutoff_hz, 1.0, nyquist * 0.995);
    transition_hz = std::max(transition_hz, sample_rate_hz * 1e-4);

    /* Kaiser's design formulas (Oppenheim & Schafer). */
    double beta;
    if (stopband_db > 50.0)
        beta = 0.1102 * (stopband_db - 8.7);
    else if (stopband_db >= 21.0)
        beta = 0.5842 * std::pow(stopband_db - 21.0, 0.4) + 0.07886 * (stopband_db - 21.0);
    else
        beta = 0.0;

    const double delta_w = 2.0 * kPi * transition_hz / sample_rate_hz;
    size_t n = static_cast<size_t>(std::ceil((stopband_db - 8.0) / (2.285 * delta_w))) + 1;

    if (n < 5) n = 5;
    if (n > max_taps) n = max_taps;
    if ((n % 2) == 0) n += 1;  /* odd length -> integer group delay */

    std::vector<float> taps(n);
    const double m = static_cast<double>(n - 1);
    const double fc = cutoff_hz / sample_rate_hz;  /* normalised, 0..0.5 */
    const double i0_beta = bessel_i0(beta);

    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double k = static_cast<double>(i) - m / 2.0;
        const double ideal = 2.0 * fc * sinc(2.0 * fc * k);

        const double ratio = (2.0 * static_cast<double>(i) / m) - 1.0;
        const double window = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio))) / i0_beta;

        const double v = ideal * window;
        taps[i] = static_cast<float>(v);
        sum += v;
    }

    /* Normalise to unity DC gain so cascaded stages do not drift in level. */
    if (std::fabs(sum) > 1e-12) {
        for (auto& t : taps) t = static_cast<float>(t / sum);
    }

    return taps;
}

std::vector<float> design_lowpass_fixed(double cutoff_hz,
                                        double sample_rate_hz,
                                        size_t num_taps) {
    if (sample_rate_hz <= 0.0 || num_taps == 0) return {1.0f};
    if (num_taps < 3) num_taps = 3;
    if ((num_taps % 2) == 0) num_taps += 1;

    const double nyquist = sample_rate_hz / 2.0;
    cutoff_hz = std::clamp(cutoff_hz, 1.0, nyquist * 0.995);

    std::vector<float> taps(num_taps);
    const double m = static_cast<double>(num_taps - 1);
    const double fc = cutoff_hz / sample_rate_hz;

    double sum = 0.0;
    for (size_t i = 0; i < num_taps; i++) {
        const double k = static_cast<double>(i) - m / 2.0;
        const double ideal = 2.0 * fc * sinc(2.0 * fc * k);
        const double window = 0.54 - 0.46 * std::cos(2.0 * kPi * static_cast<double>(i) / m);
        const double v = ideal * window;
        taps[i] = static_cast<float>(v);
        sum += v;
    }

    if (std::fabs(sum) > 1e-12) {
        for (auto& t : taps) t = static_cast<float>(t / sum);
    }

    return taps;
}

std::vector<float> design_hilbert(size_t num_taps) {
    if (num_taps < 3) num_taps = 3;
    if ((num_taps % 2) == 0) num_taps += 1;

    std::vector<float> taps(num_taps, 0.0f);
    const double m = static_cast<double>(num_taps - 1);
    const int centre = static_cast<int>(num_taps / 2);

    for (size_t i = 0; i < num_taps; i++) {
        const int k = static_cast<int>(i) - centre;
        if (k == 0 || (k % 2) == 0) {
            taps[i] = 0.0f;  /* even taps of a Hilbert transformer are zero */
            continue;
        }
        const double ideal = 2.0 / (kPi * static_cast<double>(k));
        const double window = 0.54 - 0.46 * std::cos(2.0 * kPi * static_cast<double>(i) / m);
        taps[i] = static_cast<float>(ideal * window);
    }

    return taps;
}

/* --- FirDecimateC ---------------------------------------------------------- */

FirDecimateC::FirDecimateC(std::vector<float> taps, size_t decimation) {
    configure(std::move(taps), decimation);
}

void FirDecimateC::configure(std::vector<float> taps, size_t decimation) {
    if (taps.empty()) taps = {1.0f};
    taps_ = std::move(taps);
    decimation_ = (decimation == 0) ? 1 : decimation;
    history_.assign(taps_.size() * 2, cfloat{0.0f, 0.0f});
    pos_ = 0;
    phase_ = 0;
}

void FirDecimateC::reset() {
    std::fill(history_.begin(), history_.end(), cfloat{0.0f, 0.0f});
    pos_ = 0;
    phase_ = 0;
}

size_t FirDecimateC::process(const cfloat* in, size_t count, std::vector<cfloat>& out) {
    if (taps_.empty()) return 0;

    const size_t n = taps_.size();
    size_t produced = 0;

    for (size_t i = 0; i < count; i++) {
        /* Store each sample twice so the newest `n` samples are always
         * contiguous starting at pos_+1. */
        history_[pos_] = in[i];
        history_[pos_ + n] = in[i];
        pos_ = (pos_ + 1) % n;

        if (++phase_ < decimation_) continue;
        phase_ = 0;

        /* history_[pos_ .. pos_+n) is the window oldest-to-newest, so the taps
         * are applied time-reversed — which for a symmetric lowpass is the same
         * thing, but doing it correctly matters for asymmetric designs. */
        const cfloat* w = &history_[pos_];
        float acc_r = 0.0f;
        float acc_i = 0.0f;
        for (size_t k = 0; k < n; k++) {
            const float t = taps_[n - 1 - k];
            acc_r += w[k].real() * t;
            acc_i += w[k].imag() * t;
        }

        out.emplace_back(acc_r, acc_i);
        produced++;
    }

    return produced;
}

/* --- FirDecimateR ---------------------------------------------------------- */

FirDecimateR::FirDecimateR(std::vector<float> taps, size_t decimation) {
    configure(std::move(taps), decimation);
}

void FirDecimateR::configure(std::vector<float> taps, size_t decimation) {
    if (taps.empty()) taps = {1.0f};
    taps_ = std::move(taps);
    decimation_ = (decimation == 0) ? 1 : decimation;
    history_.assign(taps_.size() * 2, 0.0f);
    pos_ = 0;
    phase_ = 0;
}

void FirDecimateR::reset() {
    std::fill(history_.begin(), history_.end(), 0.0f);
    pos_ = 0;
    phase_ = 0;
}

size_t FirDecimateR::process(const float* in, size_t count, std::vector<float>& out) {
    if (taps_.empty()) return 0;

    const size_t n = taps_.size();
    size_t produced = 0;

    for (size_t i = 0; i < count; i++) {
        history_[pos_] = in[i];
        history_[pos_ + n] = in[i];
        pos_ = (pos_ + 1) % n;

        if (++phase_ < decimation_) continue;
        phase_ = 0;

        const float* w = &history_[pos_];
        float acc = 0.0f;
        for (size_t k = 0; k < n; k++) acc += w[k] * taps_[n - 1 - k];

        out.push_back(acc);
        produced++;
    }

    return produced;
}

/* --- FirR ------------------------------------------------------------------ */

FirR::FirR(std::vector<float> taps) {
    configure(std::move(taps));
}

void FirR::configure(std::vector<float> taps) {
    if (taps.empty()) taps = {1.0f};
    taps_ = std::move(taps);
    history_.assign(taps_.size() * 2, 0.0f);
    pos_ = 0;
}

void FirR::reset() {
    std::fill(history_.begin(), history_.end(), 0.0f);
    pos_ = 0;
}

float FirR::process_one(float x) {
    const size_t n = taps_.size();

    history_[pos_] = x;
    history_[pos_ + n] = x;
    pos_ = (pos_ + 1) % n;

    const float* w = &history_[pos_];
    float acc = 0.0f;
    for (size_t k = 0; k < n; k++) acc += w[k] * taps_[n - 1 - k];
    return acc;
}

void FirR::process(const float* in, size_t count, float* out) {
    for (size_t i = 0; i < count; i++) out[i] = process_one(in[i]);
}

}  // namespace dsp
