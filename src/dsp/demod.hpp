/*
 * mayhem-b200 — demodulators and audio conditioning.
 *
 * Structured to mirror the firmware's baseband processors (proc_am_audio,
 * proc_nfm_audio, proc_wfm_audio, proc_ssb...) so the modes and their controls
 * map one-to-one onto Mayhem's, but implemented in float against whatever
 * sample rate the B200 is streaming.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_DEMOD_H__
#define __MB200_DEMOD_H__

#include "fir.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsp {

/* --- Numerically controlled oscillator / complex mixer ---------------------- */

class Nco {
   public:
    void set_frequency(double freq_hz, double sample_rate_hz);
    void reset();

    /* Multiplies `in` by exp(j*2*pi*f*t), writing to `out` (may alias `in`). */
    void mix(const cfloat* in, cfloat* out, size_t count);

    double frequency() const { return frequency_hz_; }

   private:
    double frequency_hz_{0.0};
    double phase_{0.0};
    double phase_step_{0.0};
};

/* --- DC blocker (single-pole highpass) -------------------------------------- */

class DcBlocker {
   public:
    void configure(float cutoff_hz, float sample_rate_hz);
    void reset();
    float process(float x);

   private:
    float alpha_{0.999f};
    float prev_in_{0.0f};
    float prev_out_{0.0f};
};

/* --- Fixed delay line, used to time-align the I path against a Hilbert Q ---- */

class Delay {
   public:
    void configure(size_t samples);
    void reset();
    float process(float x);

   private:
    std::vector<float> buffer_;
    size_t pos_{0};
};

/* --- AM ---------------------------------------------------------------------
 * Envelope detection plus a DC blocker to strip the carrier term. */

class AmDemod {
   public:
    void configure(float sample_rate_hz);
    void reset();
    void process(const cfloat* in, size_t count, std::vector<float>& out);

   private:
    DcBlocker dc_{};
};

/* --- FM ---------------------------------------------------------------------
 * Phase discriminator: arg(conj(prev) * curr), scaled so that a deviation of
 * `deviation_hz` produces an output of +/-1.0. */

class FmDemod {
   public:
    void configure(float sample_rate_hz, float deviation_hz);
    void reset();
    void process(const cfloat* in, size_t count, std::vector<float>& out);

    /* Mean squared value of the last block, before scaling — the squelch and
     * the signal-strength readout both use it. */
    float last_block_power() const { return last_power_; }

   private:
    cfloat prev_{1.0f, 0.0f};
    float scale_{1.0f};
    float last_power_{0.0f};
};

/* --- SSB / CW ---------------------------------------------------------------
 * Phasing method: USB = I - H{Q}, LSB = I + H{Q}, with I delayed to match the
 * Hilbert transformer's group delay. */

class SsbDemod {
   public:
    enum class Sideband : uint8_t { Upper = 0, Lower = 1 };

    void configure(float sample_rate_hz, Sideband sideband, size_t hilbert_taps = 63);
    void set_sideband(Sideband sideband) { sideband_ = sideband; }
    Sideband sideband() const { return sideband_; }
    void reset();

    void process(const cfloat* in, size_t count, std::vector<float>& out);

   private:
    FirR hilbert_{};
    Delay delay_{};
    Sideband sideband_{Sideband::Upper};
};

/* --- FM de-emphasis (single-pole, 50 us EU / 75 us US) ---------------------- */

class Deemphasis {
   public:
    void configure(float time_constant_us, float sample_rate_hz);
    void reset();
    void process(float* samples, size_t count);

   private:
    float alpha_{0.0f};
    float state_{0.0f};
    bool enabled_{false};
};

/* --- Audio AGC ---------------------------------------------------------------
 * Peak-tracking with fast attack and slow release, and a hold floor so quiet
 * passages are not pumped up into the noise. */

class AudioAgc {
   public:
    void configure(float sample_rate_hz,
                   float attack_ms = 5.0f,
                   float release_ms = 500.0f,
                   float target = 0.5f,
                   float max_gain = 64.0f);
    void reset();
    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

    void process(float* samples, size_t count);

    float gain() const { return gain_; }

   private:
    float attack_{0.01f};
    float release_{0.001f};
    float target_{0.5f};
    float max_gain_{64.0f};
    float envelope_{0.0f};
    float gain_{1.0f};
    bool enabled_{true};
};

/* --- Squelch -----------------------------------------------------------------
 * Compares a smoothed signal level against a threshold, with hysteresis so a
 * signal sitting exactly at the threshold does not chatter. Level is expressed
 * 0..99 to match Mayhem's squelch control. */

class Squelch {
   public:
    void configure(float sample_rate_hz, float hysteresis_db = 3.0f);
    void set_level(uint8_t level_0_99);
    uint8_t level() const { return level_; }
    void reset();

    /* Feeds the block's signal level (linear RMS) and returns true if audio
     * should pass. A level of 0 disables the squelch entirely. */
    bool update(float rms);

    bool open() const { return open_; }

   private:
    uint8_t level_{0};
    float threshold_db_{-100.0f};
    float hysteresis_db_{3.0f};
    float smoothed_db_{-140.0f};
    float smoothing_{0.2f};
    bool open_{true};
};

/* --- Fractional resampler ----------------------------------------------------
 * Linear interpolation. Adequate here because the signal reaching it has
 * already been band-limited well below the output Nyquist by the channel
 * filter, so the interpolation error lands under the noise floor. */

class Resampler {
   public:
    void configure(double input_rate_hz, double output_rate_hz);
    void reset();

    double ratio() const { return step_; }

    void process(const float* in, size_t count, std::vector<float>& out);

   private:
    double step_{1.0};
    double phase_{0.0};
    float last_{0.0f};
    bool primed_{false};
};

/* Root-mean-square of a complex block — the common input to squelch and the
 * RSSI readout. */
float rms(const cfloat* x, size_t count);
float rms(const float* x, size_t count);

/* Linear amplitude to dBFS, floored so silence does not produce -inf. */
float to_db(float amplitude, float floor_db = -140.0f);

}  // namespace dsp

#endif /*__MB200_DEMOD_H__*/
