/*
 * mayhem-b200 — FIR filter design and decimating/interpolating filters.
 *
 * The PortaPack firmware ships hand-tuned fixed-point tap sets (dsp_fir_taps.cpp)
 * because the M4 has no FPU headroom to spare. On the host we design taps at
 * run time in floating point, which lets the channel filter track whatever
 * sample rate the B200 is actually running at instead of a fixed 3.072 Msps.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_FIR_H__
#define __MB200_FIR_H__

#include <complex>
#include <cstddef>
#include <vector>

namespace dsp {

using cfloat = std::complex<float>;

/* --- Filter design --------------------------------------------------------- */

/* Kaiser-windowed sinc lowpass.
 *
 *   cutoff_hz      -3 dB-ish passband edge
 *   transition_hz  width of the transition band
 *   sample_rate_hz input rate
 *   stopband_db    desired stopband attenuation (e.g. 60)
 *
 * Tap count follows Kaiser's estimate and is forced odd so the filter has an
 * exact integer group delay. */
std::vector<float> design_lowpass(double cutoff_hz,
                                  double transition_hz,
                                  double sample_rate_hz,
                                  double stopband_db = 60.0,
                                  size_t max_taps = 2047);

/* Fixed-length Hamming-windowed sinc lowpass. Useful when the tap budget
 * matters more than the exact stopband. `num_taps` is forced odd. */
std::vector<float> design_lowpass_fixed(double cutoff_hz,
                                        double sample_rate_hz,
                                        size_t num_taps);

/* Hilbert transformer (90-degree phase shift), odd length, for SSB. */
std::vector<float> design_hilbert(size_t num_taps);

/* --- Decimating FIR, complex signal, real taps ------------------------------
 *
 * Only every Mth output is computed, so cost is (taps / M) multiply-accumulates
 * per input sample. The delay line is a flat vector with a duplicated write so
 * the convolution is a straight contiguous walk. */
class FirDecimateC {
   public:
    FirDecimateC() = default;
    FirDecimateC(std::vector<float> taps, size_t decimation);

    void configure(std::vector<float> taps, size_t decimation);
    void reset();

    size_t decimation() const { return decimation_; }
    size_t taps() const { return taps_.size(); }

    /* Filters `count` input samples, appending outputs to `out`.
     * Returns the number of samples appended. */
    size_t process(const cfloat* in, size_t count, std::vector<cfloat>& out);

   private:
    std::vector<float> taps_;
    std::vector<cfloat> history_;  /* 2x taps, duplicated window */
    size_t decimation_{1};
    size_t pos_{0};    /* next write index within [0, taps) */
    size_t phase_{0};  /* input samples since the last emitted output */
};

/* Decimating FIR for a real signal — the audio-side counterpart. */
class FirDecimateR {
   public:
    FirDecimateR() = default;
    FirDecimateR(std::vector<float> taps, size_t decimation);

    void configure(std::vector<float> taps, size_t decimation);
    void reset();

    size_t process(const float* in, size_t count, std::vector<float>& out);

   private:
    std::vector<float> taps_;
    std::vector<float> history_;
    size_t decimation_{1};
    size_t pos_{0};
    size_t phase_{0};
};

/* Plain (non-decimating) real FIR, used for audio shaping. */
class FirR {
   public:
    FirR() = default;
    explicit FirR(std::vector<float> taps);

    void configure(std::vector<float> taps);
    void reset();

    float process_one(float x);
    void process(const float* in, size_t count, float* out);

   private:
    std::vector<float> taps_;
    std::vector<float> history_;
    size_t pos_{0};
};

}  // namespace dsp

#endif /*__MB200_FIR_H__*/
