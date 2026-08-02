/*
 * mayhem-b200 — radix-2 FFT and analysis windows.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "fft.hpp"

#include <algorithm>
#include <cmath>

namespace dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

}  // namespace

std::vector<float> make_window(WindowType type, size_t n) {
    std::vector<float> w(n, 1.0f);
    if (n == 0) return w;
    if (n == 1) return w;

    const double m = static_cast<double>(n - 1);

    for (size_t i = 0; i < n; i++) {
        const double x = static_cast<double>(i) / m;
        double v = 1.0;
        switch (type) {
            case WindowType::Rectangular:
                v = 1.0;
                break;
            case WindowType::Hamming:
                v = 0.54 - 0.46 * std::cos(2.0 * kPi * x);
                break;
            case WindowType::Hann:
                v = 0.5 - 0.5 * std::cos(2.0 * kPi * x);
                break;
            case WindowType::Blackman:
                v = 0.42 - 0.5 * std::cos(2.0 * kPi * x) + 0.08 * std::cos(4.0 * kPi * x);
                break;
            case WindowType::BlackmanHarris:
                v = 0.35875 - 0.48829 * std::cos(2.0 * kPi * x) +
                    0.14128 * std::cos(4.0 * kPi * x) - 0.01168 * std::cos(6.0 * kPi * x);
                break;
        }
        w[i] = static_cast<float>(v);
    }

    /* Normalise coherent gain: sum(w)/n == 1. A tone then reads its true
     * amplitude no matter which window is selected. */
    double sum = 0.0;
    for (float v : w) sum += v;
    if (sum > 1e-12) {
        const float scale = static_cast<float>(static_cast<double>(n) / sum);
        for (auto& v : w) v *= scale;
    }

    return w;
}

void Fft::configure(size_t size) {
    size_ = next_pow2(std::max<size_t>(size, 2));

    /* Bit-reversal permutation table. */
    bit_reverse_.assign(size_, 0);
    size_t log2n = 0;
    while ((size_t{1} << log2n) < size_) log2n++;

    for (size_t i = 0; i < size_; i++) {
        size_t r = 0;
        for (size_t b = 0; b < log2n; b++) {
            if (i & (size_t{1} << b)) r |= size_t{1} << (log2n - 1 - b);
        }
        bit_reverse_[i] = r;
    }

    /* Forward twiddles: exp(-j*2*pi*k/N) for k in [0, N/2). */
    twiddles_.resize(size_ / 2);
    for (size_t k = 0; k < size_ / 2; k++) {
        const double angle = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(size_);
        twiddles_[k] = cfloat{static_cast<float>(std::cos(angle)),
                              static_cast<float>(std::sin(angle))};
    }
}

void Fft::transform(std::vector<cfloat>& data, bool inverse) const {
    if (size_ == 0 || data.size() != size_) return;

    for (size_t i = 0; i < size_; i++) {
        const size_t j = bit_reverse_[i];
        if (i < j) std::swap(data[i], data[j]);
    }

    for (size_t len = 2; len <= size_; len <<= 1) {
        const size_t half = len / 2;
        const size_t step = size_ / len;

        for (size_t i = 0; i < size_; i += len) {
            for (size_t k = 0; k < half; k++) {
                cfloat w = twiddles_[k * step];
                if (inverse) w = std::conj(w);

                const cfloat u = data[i + k];
                const cfloat t = w * data[i + k + half];
                data[i + k] = u + t;
                data[i + k + half] = u - t;
            }
        }
    }

    if (inverse) {
        const float scale = 1.0f / static_cast<float>(size_);
        for (auto& v : data) v *= scale;
    }
}

void Fft::spectrum_db(const cfloat* in,
                      const std::vector<float>& window,
                      std::vector<float>& out_db,
                      float floor_db) const {
    if (size_ == 0 || in == nullptr) return;

    std::vector<cfloat> buf(size_);
    const bool have_window = (window.size() == size_);

    for (size_t i = 0; i < size_; i++) {
        const float w = have_window ? window[i] : 1.0f;
        buf[i] = in[i] * w;
    }

    transform(buf, false);

    out_db.resize(size_);
    const float norm = 1.0f / static_cast<float>(size_);
    const float floor_mag2 = std::pow(10.0f, floor_db / 10.0f);

    /* Reorder to -Fs/2 .. +Fs/2: the upper half of the FFT output holds the
     * negative frequencies and belongs on the left. */
    const size_t half = size_ / 2;
    for (size_t i = 0; i < size_; i++) {
        const size_t src = (i < half) ? (i + half) : (i - half);
        const cfloat v = buf[src] * norm;
        float mag2 = v.real() * v.real() + v.imag() * v.imag();
        if (mag2 < floor_mag2) mag2 = floor_mag2;
        out_db[i] = 10.0f * std::log10(mag2);
    }
}

void fft_inplace(std::vector<cfloat>& data, bool inverse) {
    Fft f{data.size()};
    if (f.size() != data.size()) return;  /* not a power of two */
    f.transform(data, inverse);
}

}  // namespace dsp
