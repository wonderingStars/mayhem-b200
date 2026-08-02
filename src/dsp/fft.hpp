/*
 * mayhem-b200 — radix-2 FFT and analysis windows.
 *
 * Small and self-contained on purpose: the spectrum display needs a few hundred
 * transforms a second at sizes of 256-4096, which an iterative radix-2 handles
 * comfortably without pulling in FFTW or KissFFT as a dependency.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_FFT_H__
#define __MB200_FFT_H__

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsp {

using cfloat = std::complex<float>;

enum class WindowType : uint8_t {
    Rectangular = 0,
    Hamming,
    Hann,
    Blackman,
    BlackmanHarris,
};

/* Window coefficients of length n, normalised so coherent gain is 1.0 — the
 * spectrum magnitudes then read as amplitudes regardless of window choice. */
std::vector<float> make_window(WindowType type, size_t n);

/* In-place iterative radix-2 FFT. `data.size()` must be a power of two. */
void fft_inplace(std::vector<cfloat>& data, bool inverse = false);

/* Reusable planner: precomputes the bit-reversal permutation and twiddles.
 * Prefer this in a streaming path — it removes the per-call setup cost. */
class Fft {
   public:
    Fft() = default;
    explicit Fft(size_t size) { configure(size); }

    /* `size` is rounded up to the next power of two. */
    void configure(size_t size);
    size_t size() const { return size_; }

    void transform(std::vector<cfloat>& data, bool inverse = false) const;

    /* Windows `in`, transforms, and writes magnitudes in dBFS to `out_db`,
     * reordered so index 0 is the most negative frequency and index size-1 the
     * most positive — the layout the waterfall expects.
     *
     * Full-scale reference: a complex sinusoid of unit amplitude reads 0 dBFS.
     * Values are floored at `floor_db` so log(0) cannot produce -inf. */
    void spectrum_db(const cfloat* in,
                     const std::vector<float>& window,
                     std::vector<float>& out_db,
                     float floor_db = -140.0f) const;

   private:
    size_t size_{0};
    std::vector<size_t> bit_reverse_;
    std::vector<cfloat> twiddles_;  /* forward twiddles, size/2 of them */
};

}  // namespace dsp

#endif /*__MB200_FFT_H__*/
