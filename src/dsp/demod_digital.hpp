/*
 * mayhem-b200 — digital (data) demodulators.
 *
 * Where demod.hpp carries the analogue modes that end at a loudspeaker, this
 * file carries the ones that end in a bit stream: OOK/ASK, 2FSK/GFSK, AFSK and
 * BPSK/QPSK. Each demodulator takes complex baseband at a stated sample rate
 * and appends recovered bits (one uint8_t of value 0 or 1 per bit) to a vector.
 *
 * Ported from the PortaPack firmware:
 *
 *   baseband/ook.hpp        -> OokSlicer (OOKSlicerMagSquaredInt),
 *                              OokDemod's clock (OOKClockRecovery)
 *   baseband/proc_afskrx.cpp,
 *   baseband/proc_aprsrx.cpp-> AfskDemod (delay-and-multiply correlator, the
 *                              phase-accumulator bit clock with its clean-
 *                              transition nudge, and the popcount majority
 *                              vote at the sampling instant)
 *   baseband/proc_fsk_rx.cpp-> FskDemod's phase-difference discriminator
 *
 * Departures from upstream, each justified where it appears:
 *
 *   1. AfskDemod picks its correlator delay as the D in [1, samples_per_bit]
 *      that most separates the mark and space tones, instead of upstream's
 *      hard-coded samples_per_bit/2. That constant only discriminates well for
 *      Bell 202 at upstream's 24 kHz audio rate; at Bell 103's 200 Hz tone
 *      spacing it collapses the two tones onto nearly the same value.
 *   2. AfskDemod's post-mix lowpass is a designed FIR rather than upstream's
 *      two-tap-plus-one-pole, which does not remove the sum-frequency product
 *      for arbitrary tone pairs. The host has the cycles.
 *   3. FSK and AFSK slice against a peak-following midpoint rather than
 *      upstream's fixed integer threshold, which was tied to its fixed-point
 *      scaling. A midpoint between decaying peak followers also survives
 *      unbalanced data, which a running mean does not.
 *   4. The timing loops use DeadbandErrorFilter rather than upstream's
 *      FixedErrorFilter, which votes a full step on symbols that carry no
 *      timing information and so walks the sampling instant off during any
 *      run of identical bits. See that class in protocol.hpp for the
 *      measurement.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_DEMOD_DIGITAL_H__
#define __MB200_DEMOD_DIGITAL_H__

#include "fir.hpp"
#include "protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsp {

/* ===========================================================================
 * OOK / ASK
 * ===========================================================================*/

/* Envelope slicer with an adaptive threshold: a peak-hold at 1/8 of the
 * magnitude-squared (i.e. -4.5 dB in amplitude) that leaks away over roughly
 * eight symbols, so the decision level follows a fading carrier but does not
 * collapse during a long run of zeros. Port of OOKSlicerMagSquaredInt. */
class OokSlicer {
   public:
    void configure(float samples_per_symbol);
    void reset();

    bool process(cfloat sample);
    bool process_mag2(float mag2);

    /* Current decision level, in magnitude-squared units. */
    float threshold() const { return threshold_; }
    float leak_factor() const { return leak_; }

   private:
    float leak_{1.0f};
    float threshold_{0.0f};
};

/* Slicer plus the early-late-gate symbol clock, so an app can hand it complex
 * baseband and get bits. Port of OOKSlicerMagSquaredInt + OOKClockRecovery. */
class OokDemod {
   public:
    void configure(float sample_rate_hz, float symbol_rate_hz);
    void reset();

    void process(const cfloat* in, size_t count, std::vector<uint8_t>& out);
    void process_mag2(const float* mag2, size_t count, std::vector<uint8_t>& out);

    float samples_per_symbol() const { return samples_per_symbol_; }
    const OokSlicer& slicer() const { return slicer_; }
    OokSlicer& slicer() { return slicer_; }

   private:
    void feed_slice(bool sliced, std::vector<uint8_t>& out);

    OokSlicer slicer_{};
    PhaseAccumulator accumulator_{};
    PhaseDetectorEarlyLateGate detector_{};
    uint32_t inc_nominal_{0};
    uint32_t inc_k_{0};
    uint32_t slicer_history_{0};
    float samples_per_symbol_{1.0f};
    bool configured_{false};
};

/* ===========================================================================
 * 2FSK / GFSK
 * ===========================================================================*/

/* Quadrature discriminator followed by Gardner timing recovery.
 *
 * GFSK differs from plain 2FSK only in the transmit pulse shaping, so the same
 * receiver handles both; the Gaussian filter softens the discriminator's
 * transitions but does not move the decision instants. */
class FskDemod {
   public:
    /* `deviation_hz` is used only to normalise the reported discriminator
     * output. Pass 0 to have it default to symbol_rate/2 (modulation index 1).
     * Slicing never depends on it — the decision level is tracked. */
    void configure(float sample_rate_hz, float symbol_rate_hz, float deviation_hz = 0.0f);
    void reset();

    /* Inverts the bit sense; some protocols call the high tone a 0. */
    void set_invert(bool invert) { invert_ = invert; }
    bool invert() const { return invert_; }

    void process(const cfloat* in, size_t count, std::vector<uint8_t>& out);

    /* Instantaneous frequency of the most recent sample, in Hz, before the
     * centre-frequency correction. */
    float last_frequency_hz() const { return last_freq_hz_; }
    /* Tracked centre of the two tones, in Hz — the residual carrier offset. */
    float centre_frequency_hz() const { return 0.5f * (peak_max_ + peak_min_); }
    float deviation_hz() const { return deviation_hz_; }

   private:
    void on_symbol(float symbol);

    ClockRecovery<DeadbandErrorFilter> clock_recovery_{};
    /* Upstream places a matched filter between the channel and the timing loop
     * (proc_ais); a Gardner detector fed a hard-edged square wave has no clean
     * zero crossing to lock to. This is that filter, generalised. */
    FirR post_filter_{};
    std::vector<uint8_t>* out_{nullptr};
    cfloat prev_{0.0f, 0.0f};
    float sample_rate_hz_{1.0f};
    float symbol_rate_hz_{1.0f};
    float deviation_hz_{1.0f};
    float last_freq_hz_{0.0f};
    float peak_max_{0.0f};
    float peak_min_{0.0f};
    float peak_decay_{0.0f};
    bool invert_{false};
    bool primed_{false};
    bool configured_{false};
};

/* ===========================================================================
 * AFSK
 * ===========================================================================*/

/* Audio-frequency-shift keying: a delay-and-multiply tone discriminator, a
 * lowpass, a tracked-level slicer, and the phase-accumulator bit clock from
 * proc_aprsrx.
 *
 * A mark tone always decodes to 1, whichever side of the discriminator it
 * lands on — the polarity is worked out at configure() time from the tone
 * frequencies. AX.25 then runs the result through NrziDecoder. */
class AfskDemod {
   public:
    enum class Standard : uint8_t {
        /* Bell 202: 1200 baud, mark 1200 Hz, space 2200 Hz. APRS/AX.25. */
        Bell202 = 0,
        /* Bell 103 originate: 300 baud, mark 1270 Hz, space 1070 Hz. */
        Bell103Originate = 1,
        /* Bell 103 answer: 300 baud, mark 2225 Hz, space 2025 Hz. */
        Bell103Answer = 2,
    };

    void configure(float audio_rate_hz, Standard standard);
    void configure(float audio_rate_hz, float mark_hz, float space_hz, float baud);
    void reset();

    /* Real audio input — the output of an FM discriminator, or a sound card. */
    void process_audio(const float* in, size_t count, std::vector<uint8_t>& out);
    /* Complex baseband input: FM-demodulated first, then as above. */
    void process(const cfloat* in, size_t count, std::vector<uint8_t>& out);

    float mark_hz() const { return mark_hz_; }
    float space_hz() const { return space_hz_; }
    float baud() const { return baud_; }
    float samples_per_bit() const { return samples_per_bit_; }
    /* Delay, in samples, chosen for the correlator. */
    size_t correlator_delay() const { return delay_; }

   private:
    void slice_and_clock(float filtered, std::vector<uint8_t>& out);

    std::vector<float> delay_line_{};
    size_t delay_pos_{0};
    size_t delay_{1};

    FirR lowpass_{};
    float peak_max_{0.0f};
    float peak_min_{0.0f};
    float peak_decay_{0.0f};
    float polarity_{1.0f};

    uint32_t sample_bits_{0};
    uint32_t phase_{0};
    uint32_t phase_inc_{0};

    cfloat prev_{0.0f, 0.0f};
    bool primed_{false};

    float audio_rate_hz_{1.0f};
    float mark_hz_{1200.0f};
    float space_hz_{2200.0f};
    float baud_{1200.0f};
    float samples_per_bit_{1.0f};
    bool configured_{false};
};

/* ===========================================================================
 * BPSK / QPSK
 * ===========================================================================*/

enum class PskOrder : uint8_t {
    Bpsk = 1, /* 1 bit per symbol,  constellation exp(j*pi*k),        k in 0..1 */
    Qpsk = 2, /* 2 bits per symbol, constellation exp(j*(pi/4 + k*pi/2)), k in 0..3 */
};

/* Timing recovery (Gardner on the complex signal) followed by a
 * decision-directed carrier loop.
 *
 * The carrier loop's phase error is arg(y * conj(nearest constellation
 * point)) — a true phase error in radians, so the loop gains are in known
 * units rather than scaled by the signal level. That is the decision-directed
 * form of a Costas loop; for BPSK the two are algebraically the same at small
 * errors.
 *
 * Carrier recovery on an M-PSK signal is ambiguous by a multiple of 2*pi/M.
 * Non-differential output is therefore correct only up to a constant rotation
 * of the constellation; set_differential(true) resolves it, at the cost of
 * doubling the bit error rate, exactly as in any real PSK receiver.
 *
 * ACQUISITION PREAMBLE: it has to be pseudo-random, not a repeating short
 * pattern. A Gardner detector's error is (current - previous) * midpoint, and
 * a preamble that repeats every two symbols is, after pulse shaping, a single
 * tone — for which that product averages to exactly zero at every sampling
 * phase. The loop then dithers wherever it started instead of acquiring.
 * Measured here: with an alternating preamble the receiver decodes only when
 * it happens to start within about a quarter symbol of the right instant;
 * with a pseudo-random one it acquires from any offset. This is a property of
 * the detector, not of this implementation. */
class PskDemod {
   public:
    void configure(float sample_rate_hz, float symbol_rate_hz, PskOrder order);
    void reset();

    void set_differential(bool enabled) { differential_ = enabled; }
    bool differential() const { return differential_; }

    /* Carrier loop gains: alpha multiplies the phase error into the phase,
     * beta into the frequency. Defaults are 0.10 / 0.002 per symbol. */
    void set_carrier_loop_gains(float alpha, float beta);

    /* Gardner loop step and dead zone, in DeadbandErrorFilter's units. Larger
     * weights acquire faster and jitter more once locked. Must be called after
     * configure(), which installs weight 1/8 and DeadbandErrorFilter's default
     * dead zone — the defaults here are the same, so passing only a weight
     * changes only the weight. */
    void set_timing_loop_gain(float weight, float deadband = 0.25f);

    /* Receive matched filter, real taps applied to the complex baseband.
     * configure() installs a root-raised-cosine matching psk_modulate()'s
     * transmit pulse; pass an empty vector to run unfiltered. */
    void set_receive_filter(std::vector<float> taps);

    void process(const cfloat* in, size_t count, std::vector<uint8_t>& out);

    float carrier_phase() const { return phase_; }
    /* Tracked carrier frequency error, in Hz. */
    float carrier_frequency_offset_hz() const;
    PskOrder order() const { return order_; }

   private:
    void on_symbol(cfloat symbol);

    ComplexClockRecovery<DeadbandErrorFilter> clock_recovery_{};
    FirDecimateC receive_filter_{};
    std::vector<cfloat> filtered_{};
    std::vector<uint8_t>* out_{nullptr};

    float sample_rate_hz_{1.0f};
    float symbol_rate_hz_{1.0f};
    float phase_{0.0f};
    float frequency_{0.0f};
    float alpha_{0.10f};
    float beta_{0.002f};
    float magnitude_{1.0f};
    uint8_t prev_index_{0};
    bool have_prev_{false};
    bool differential_{false};
    bool filter_enabled_{false};
    PskOrder order_{PskOrder::Bpsk};
    bool configured_{false};
};

/* ===========================================================================
 * Pulse shaping
 * ===========================================================================*/

/* Raised cosine (a Nyquist pulse: zero ISI at the symbol instants when used
 * alone) and its square root (the standard split between transmitter and
 * receiver). `sps` is samples per symbol, `span_symbols` the one-sided length
 * in symbols; the result has 2*span_symbols*sps + 1 taps and unit peak. */
std::vector<float> design_raised_cosine(double rolloff, size_t sps, size_t span_symbols);
std::vector<float> design_root_raised_cosine(double rolloff, size_t sps, size_t span_symbols);

/* ===========================================================================
 * Modulators
 *
 * The transmit side of each demodulator above. Apps need them; tests need them
 * more, because a demodulator is only verifiable against a signal whose
 * intended bits are known.
 * ===========================================================================*/

/* Amplitude `amplitude` while the bit is 1, zero while it is 0. */
std::vector<cfloat> ook_modulate(const std::vector<uint8_t>& bits,
                                 float sample_rate_hz,
                                 float symbol_rate_hz,
                                 float amplitude = 1.0f);

/* Continuous-phase 2FSK: +deviation_hz for a 1, -deviation_hz for a 0.
 * `gaussian_bt` above zero applies a Gaussian filter of that bandwidth-time
 * product to the frequency pulse, which is what makes it GFSK. */
std::vector<cfloat> fsk_modulate(const std::vector<uint8_t>& bits,
                                 float sample_rate_hz,
                                 float symbol_rate_hz,
                                 float deviation_hz,
                                 float gaussian_bt = 0.0f);

/* Continuous-phase AFSK audio: mark tone for a 1, space tone for a 0. */
std::vector<float> afsk_modulate(const std::vector<uint8_t>& bits,
                                 float sample_rate_hz,
                                 float mark_hz,
                                 float space_hz,
                                 float baud,
                                 float amplitude = 0.8f);

/* Root-raised-cosine shaped PSK. Bits are consumed most-significant first
 * within each symbol; a trailing partial symbol is zero-padded. */
std::vector<cfloat> psk_modulate(const std::vector<uint8_t>& bits,
                                 float sample_rate_hz,
                                 float symbol_rate_hz,
                                 PskOrder order,
                                 bool differential = false,
                                 double rolloff = 0.35,
                                 size_t span_symbols = 6);

}  // namespace dsp

#endif /*__MB200_DEMOD_DIGITAL_H__*/
