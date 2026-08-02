/*
 * mayhem-b200 — modulators, keyers and tone tables.
 *
 * The transmit-side counterpart of dsp/demod.hpp, structured to mirror the
 * firmware's baseband modulators (baseband/dsp_modulate.*, proc_mictx,
 * proc_siggen, proc_ook, proc_fsk, proc_ble_tx) so that a Mayhem transmit app
 * ported onto this host asks for the same things by the same names.
 *
 * Two deliberate departures from the firmware:
 *
 *  - Everything is float at a configurable sample rate. The M4 works in int8
 *    baseband at a fixed 1.536 MHz or 2.28 MHz with a 32-bit phase accumulator
 *    indexing a 256-entry sine table; that quantisation is a cost of the
 *    hardware, not part of the specification, and the B200 streams fc32
 *    anyway. Where a firmware constant encodes the fixed rate (`fm_delta`,
 *    `samples_per_bit`, `fs_div_factor`) the equivalent here is derived from
 *    the sample rate at configure() time.
 *
 *  - Levels are normalised so a modulator's peak output magnitude is 1.0. The
 *    firmware scales into int8 with hand-tuned headroom constants (+80 DC for
 *    the AM carrier, x256 for SSB); on the host the output scaling belongs to
 *    the transmit chain, not to each modulator.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_MODULATE_H__
#define __MB200_MODULATE_H__

#include "demod.hpp"  /* Delay, cfloat */
#include "fir.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsp {

/* --- Pulse shaping ---------------------------------------------------------- */

/* Gaussian frequency pulse for GFSK/GMSK: one rectangular symbol convolved with
 * a Gaussian of bandwidth-time product `bt`, sampled `samples_per_symbol` times
 * per symbol across `span_symbols` symbols, and normalised to unit sum so a run
 * of identical symbols reaches exactly the full deviation.
 *
 *   g(t) = 1/2 [ Q(a (t/T - 1/2)) - Q(a (t/T + 1/2)) ],  a = 2 pi BT / sqrt(ln2)
 *
 * This is the same pulse the firmware's BLE transmitter carries as a literal
 * table (baseband/proc_ble_tx.hpp, BT = 0.5, 4 samples/symbol, 4-symbol span);
 * the two agree to eight significant figures once both are normalised, which is
 * what tests/test_modulate.cpp checks. */
std::vector<float> design_gaussian_pulse(double bt,
                                         double samples_per_symbol,
                                         size_t span_symbols = 4);

/* --- Tone generator ---------------------------------------------------------
 *
 * Mayhem's ToneGen (baseband/tone_gen.cpp) is a 32-bit phase accumulator whose
 * top byte indexes a 256-entry int8 sine table, the low 24 bits giving
 * sub-table frequency resolution. Here the accumulator is a double and the sine
 * is computed, which removes the quantisation without changing the interface.
 *
 * The waveform shapes are the ones the signal generator offers
 * (baseband/proc_siggen.cpp), including its 16-bit Fibonacci LFSR noise source
 * (taps 16, 15, 13, 4; seed 0xACE1). */
class ToneGen {
   public:
    enum class Shape : uint8_t {
        Sine = 0,
        Triangle = 1,
        SawUp = 2,
        SawDown = 3,
        Square = 4,
        Noise = 5,
    };

    void configure(float frequency_hz,
                   float sample_rate_hz,
                   Shape shape = Shape::Sine,
                   float amplitude = 1.0f);

    /* Retunes without disturbing the phase, so a tone can be swept smoothly. */
    void set_frequency(float frequency_hz);
    void set_amplitude(float amplitude) { amplitude_ = amplitude; }
    void set_shape(Shape shape) { shape_ = shape; }
    void reset();

    float frequency() const { return frequency_hz_; }
    float sample_rate() const { return sample_rate_hz_; }
    float amplitude() const { return amplitude_; }
    Shape shape() const { return shape_; }

    float process_one();
    void process(float* out, size_t count);

    /* Mixes the tone into an existing audio block the way upstream's
     * ToneGen::process(sample_in) does:
     *
     *   out = in * (1 - weight) + tone * weight
     *
     * A zero frequency leaves the block untouched, matching upstream's
     * `if (!delta_) return sample_in;`. This is how a CTCSS sub-tone or a
     * selective-calling tone is added to transmit audio. */
    void mix(float* samples, size_t count, float weight);

   private:
    float frequency_hz_{0.0f};
    float sample_rate_hz_{48000.0f};
    float amplitude_{1.0f};
    Shape shape_{Shape::Sine};
    double phase_{0.0};       /* in turns, kept in [0, 1) */
    double phase_step_{0.0};
    uint16_t lfsr_{0xACE1u};
};

/* --- Frequency modulator -----------------------------------------------------
 *
 * Integrates the audio into phase, scaled so an input of +/-1.0 produces
 * +/-deviation_hz of instantaneous frequency. That is exactly the inverse of
 * dsp::FmDemod's scaling, so mod -> demod at the same deviation is an identity
 * on the audio (tests/test_modulate.cpp closes that loop).
 *
 * NFM and WFM are the same object with different deviations — upstream does the
 * same thing, passing deviation_hz through AudioTXConfigMessage. The output is
 * unit magnitude: FM is a constant-envelope modulation. */
class FmModulator {
   public:
    void configure(float sample_rate_hz, float deviation_hz);
    void reset();

    float deviation() const { return deviation_hz_; }
    float sample_rate() const { return sample_rate_hz_; }

    void process(const float* audio, size_t count, cfloat* out);
    void process(const float* audio, size_t count, std::vector<cfloat>& out);

   private:
    float deviation_hz_{5000.0f};
    float sample_rate_hz_{48000.0f};
    double phase_{0.0};           /* radians */
    double radians_per_unit_{0.0};  /* phase step per sample at audio == 1.0 */
};

/* --- Amplitude modulator -----------------------------------------------------
 *
 * AM: the envelope is carrier * (1 + depth * audio). `depth` is the classical
 * modulation index m, so for a sinusoidal audio of amplitude 1 the envelope
 * swings between (1-m) and (1+m) times the carrier and
 *
 *   (Emax - Emin) / (Emax + Emin) == m
 *
 * regardless of how the carrier is scaled. The carrier is set to 1/(1+depth) so
 * the peak envelope is exactly 1.0 and a fully modulated signal does not clip.
 *
 * DSB is the suppressed-carrier case (Mayhem's Mode::DSB): the baseband is the
 * audio itself, so the envelope is |audio| and the phase flips 180 degrees at
 * each zero crossing. `depth` does not apply.
 *
 * Upstream puts the same value on I and Q (`re = q + 80; im = q + 80`), which
 * rotates the result 45 degrees and costs 3 dB; that is an artefact of its int8
 * packing, not part of AM, so the signal is placed on I alone here. */
class AmModulator {
   public:
    enum class Variant : uint8_t { AM = 0, DSB = 1 };

    void configure(float sample_rate_hz, float depth = 1.0f, Variant variant = Variant::AM);
    void set_depth(float depth);
    void set_variant(Variant variant) { variant_ = variant; }
    void reset();

    float depth() const { return depth_; }
    Variant variant() const { return variant_; }
    /* Unmodulated carrier level, i.e. the output magnitude for silent audio. */
    float carrier_level() const { return carrier_; }

    void process(const float* audio, size_t count, cfloat* out);
    void process(const float* audio, size_t count, std::vector<cfloat>& out);

   private:
    float depth_{1.0f};
    float carrier_{0.5f};
    Variant variant_{Variant::AM};
};

/* --- Single-sideband modulator -----------------------------------------------
 *
 * Phasing method, the transmit counterpart of dsp::SsbDemod:
 *
 *   USB:  I = audio delayed by the Hilbert group delay,  Q = +H{audio}
 *   LSB:  I = the same delayed audio,                    Q = -H{audio}
 *
 * which is the analytic signal of the audio, or its conjugate, so the spectrum
 * sits entirely above or below the carrier. Feeding a USB modulator into a USB
 * demodulator returns the audio (at twice the amplitude, delayed by the two
 * Hilbert group delays) — that round trip is a test.
 *
 * Upstream's SSB modulator swaps I and Q between sidebands rather than negating
 * one, which is the same thing up to a 90 degree rotation of the whole signal;
 * the negation form is used here because it matches dsp::SsbDemod's convention
 * exactly. */
class SsbModulator {
   public:
    enum class Sideband : uint8_t { Upper = 0, Lower = 1 };

    void configure(float sample_rate_hz, Sideband sideband, size_t hilbert_taps = 63);
    void set_sideband(Sideband sideband) { sideband_ = sideband; }
    Sideband sideband() const { return sideband_; }
    /* Samples of delay the modulator adds, (taps - 1) / 2. */
    size_t group_delay() const { return group_delay_; }
    void reset();

    void process(const float* audio, size_t count, cfloat* out);
    void process(const float* audio, size_t count, std::vector<cfloat>& out);

   private:
    FirR hilbert_{};
    Delay delay_{};
    Sideband sideband_{Sideband::Upper};
    size_t group_delay_{31};
};

/* --- Bit access --------------------------------------------------------------
 * The keyers read bits MSB-first out of a byte array, the order the firmware
 * uses: bit n is `(data[n >> 3] << (n & 7)) & 0x80`. */
bool bit_at(const uint8_t* data, size_t bit_index);

/* --- OOK / ASK keyer ---------------------------------------------------------
 *
 * Ported from baseband/proc_ook.cpp: a '1' transmits the carrier at the high
 * level and a '0' at the low level (zero for plain OOK), each held for one
 * symbol period. Mayhem's repeat count and inter-repeat pause are kept because
 * every remote-control app depends on them.
 *
 * proc_ook counts whole samples per bit against a fixed baseband rate. Here the
 * symbol period is a double and boundaries are taken against the exact ideal
 * position, so an arbitrary symbol rate against an arbitrary B200 sample rate
 * neither drifts nor accumulates rounding error. When the rate divides evenly
 * the result is identical: exactly samples_per_symbol samples per symbol. */
class OokKeyer {
   public:
    void configure(float sample_rate_hz, float symbol_rate_hz);

    /* Carrier amplitude for a 0 bit and a 1 bit. Defaults are 0 and 1 (OOK);
     * any other pair is ASK. */
    void set_levels(float low, float high);

    /* `repeat` is the number of times the whole bit stream is sent (minimum 1);
     * `pause_symbols` is the number of low-level symbol periods inserted
     * between repeats. Upstream stores repeat-1 and pause+1 internally. */
    void set_repeat(uint32_t repeat, uint32_t pause_symbols);

    /* Copies `bit_count` bits out of `data`. Passing a null pointer or a zero
     * count leaves the keyer with nothing to send, i.e. immediately done. */
    void set_data(const uint8_t* data, size_t bit_count);

    void reset();

    double samples_per_symbol() const { return samples_per_symbol_; }
    size_t bit_count() const { return bit_count_; }
    /* Total samples one complete run (all repeats and pauses) will produce. */
    size_t total_samples() const;

    /* Writes up to `count` samples and returns how many were written. Returns 0
     * once the last repeat has been sent; the caller then stops the burst or
     * pads with zeros. */
    size_t process(cfloat* out, size_t count);

    bool done() const { return done_; }
    /* Completed repeats so far, for a progress bar. */
    uint32_t repeats_sent() const { return repeats_sent_; }

   private:
    void advance_symbol();

    std::vector<uint8_t> data_{};
    size_t bit_count_{0};
    double samples_per_symbol_{1.0};
    float low_{0.0f};
    float high_{1.0f};
    uint32_t repeat_{1};
    uint32_t pause_symbols_{0};

    float level_{0.0f};
    uint64_t sample_index_{0};
    double next_boundary_{0.0};
    size_t symbol_index_{0};   /* within the current repeat, incl. pause */
    uint32_t repeats_sent_{0};
    bool done_{true};
};

/* --- FSK / GFSK keyer --------------------------------------------------------
 *
 * Ported from baseband/proc_fsk.cpp: a '1' shifts the carrier up by the
 * deviation and a '0' down by the same amount, so the two tones are
 * 2 * deviation apart. That is the convention the firmware's `shift` parameter
 * carries (`shift_zero = -shift_one`).
 *
 * With a non-zero BT the +/-1 symbol stream is shaped by design_gaussian_pulse()
 * before the integrator, which is GFSK — the firmware does this only inside its
 * BLE transmitter, with a hard-coded BT of 0.5 at 4 samples per symbol.
 *
 * The pause between repeats transmits an unmodulated carrier. Upstream's FSK
 * processor has no repeat at all (it stops after one pass and outputs zeros),
 * so this is a choice of this port, made because a keyed-off carrier is what
 * every FSK protocol expects between frames. */
class FskKeyer {
   public:
    void configure(float sample_rate_hz, float symbol_rate_hz, float deviation_hz);

    /* `bt` <= 0 turns the shaping off, leaving plain hard-keyed FSK. */
    void set_gaussian(float bt, size_t span_symbols = 4);

    void set_data(const uint8_t* data, size_t bit_count);
    void set_repeat(uint32_t repeat, uint32_t pause_symbols);
    void reset();

    double samples_per_symbol() const { return samples_per_symbol_; }
    size_t bit_count() const { return bit_count_; }
    float deviation() const { return deviation_hz_; }
    float bt() const { return bt_; }
    /* Samples of delay the Gaussian shaping adds; 0 with shaping off. */
    size_t group_delay() const { return group_delay_; }

    size_t process(cfloat* out, size_t count);

    bool done() const { return done_ && flush_remaining_ == 0; }
    uint32_t repeats_sent() const { return repeats_sent_; }

   private:
    void advance_symbol();

    std::vector<uint8_t> data_{};
    size_t bit_count_{0};
    double samples_per_symbol_{1.0};
    float deviation_hz_{5000.0f};
    float sample_rate_hz_{48000.0f};
    float bt_{0.0f};
    size_t span_symbols_{4};
    size_t group_delay_{0};
    uint32_t repeat_{1};
    uint32_t pause_symbols_{0};

    FirR shaper_{};
    bool shaping_{false};

    float symbol_{0.0f};
    uint64_t sample_index_{0};
    double next_boundary_{0.0};
    size_t symbol_index_{0};
    uint32_t repeats_sent_{0};
    bool done_{true};
    /* Samples still to be pushed through the shaping filter after the last
     * symbol, so the final pulse is not truncated. */
    size_t flush_remaining_{0};

    double phase_{0.0};
    double radians_per_unit_{0.0};
};

/* --- Tone tables ------------------------------------------------------------- */

namespace tones {

struct CtcssTone {
    const char* name;      /* Mayhem's tone-key label, e.g. "12 1Z" */
    float frequency_hz;
};

/* The CTCSS "tone key" table from application/tone_key.cpp, in ascending
 * frequency order. Upstream stores the frequencies as hundredths of a hertz in
 * the same list as five ultrasonic wireless-microphone pilot tones (19 kHz to
 * 32.768 kHz) and a "None" entry; those are not CTCSS and are not here. */
extern const std::array<CtcssTone, 50> ctcss;

/* Index of the CTCSS entry nearest `hz`, or -1 if none is within
 * `tolerance_hz`. Upstream's tolerance is 4 Hz (TONE_FREQ_TOLERANCE_CENTIHZ). */
int ctcss_index(float hz, float tolerance_hz = 4.0f);

/* Selective-calling tone sets and the DTMF grid, from common/tonesets.hpp.
 * Upstream stores each entry as a phase increment for its 1.536 MHz baseband
 * (TONES_F2D); the frequency is the part that ports. */
extern const std::array<float, 16> ccir;
extern const std::array<float, 16> eia;
extern const std::array<float, 16> zvei;

/* DTMF, indexed by the character order "0123456789ABCD#*", each entry
 * {column_hz, row_hz}. */
extern const std::array<std::array<float, 2>, 16> dtmf;

/* The six-tone roger beep played at the end of a microphone transmission. */
extern const std::array<float, 6> roger_beep;

/* --- DCS ---------------------------------------------------------------------
 *
 * A DCS (CDCSS) word is 23 bits: 9 code bits, three fixed bits 100, and 11
 * Golay check bits, laid out as upstream lays them out:
 *
 *   (parity << 12) | (0b100 << 9) | code
 *
 * application/protocols/dcs.cpp carries the check bits as a 512-entry literal
 * table. They are the parity of the (23,12) Golay code with generator 0xC75
 * over the 12-bit word (0x800 | code), which reproduces all 512 upstream
 * entries exactly, so they are computed here rather than stored. */
uint32_t dcs_word(uint16_t code);

}  // namespace tones

/* --- DCS sub-audible encoder -------------------------------------------------
 *
 * Emits the 23-bit DCS word as a repeating NRZ bit stream, LSB first, to be
 * mixed under transmit audio the way a CTCSS tone is. Upstream has no DCS
 * transmitter — its main.cpp still lists a DCS decoder as a TODO — so the
 * 134.4 baud default comes from the CDCSS standard and not from Mayhem; pass an
 * explicit rate if you need a different one. */
class DcsEncoder {
   public:
    void configure(uint16_t code,
                   float sample_rate_hz,
                   float baud = 134.4f,
                   float amplitude = 1.0f);
    void reset();

    uint32_t word() const { return word_; }
    uint16_t code() const { return code_; }

    float process_one();
    void process(float* out, size_t count);
    /* Same mixing rule as ToneGen::mix(). */
    void mix(float* samples, size_t count, float weight);

   private:
    uint16_t code_{0};
    uint32_t word_{0};
    float amplitude_{1.0f};
    double samples_per_bit_{1.0};
    double next_boundary_{0.0};
    uint64_t sample_index_{0};
    size_t bit_index_{0};
    float level_{0.0f};
    bool primed_{false};
};

/* --- Polyphase interpolating FIR ---------------------------------------------
 *
 * A transmit chain modulates at a rate a few times the channel bandwidth and
 * then lifts the result to whatever the radio is streaming, which on a B200 is
 * hundreds of times higher. Doing that with the linear dsp::Resampler would
 * leave images only ~47 dB down; zero-stuffing and filtering puts them under
 * the design stopband instead.
 *
 * Only the non-zero taps of each polyphase branch are evaluated, so the cost is
 * (taps / interpolation) multiply-accumulates per *output* sample. Taps are
 * scaled by the interpolation factor so the signal level is preserved. */
class FirInterpolateC {
   public:
    FirInterpolateC() = default;
    FirInterpolateC(std::vector<float> taps, size_t interpolation);

    void configure(std::vector<float> taps, size_t interpolation);
    void reset();

    size_t interpolation() const { return interpolation_; }
    size_t taps() const { return taps_.size(); }

    /* Appends count * interpolation samples to `out`. Returns how many. */
    size_t process(const cfloat* in, size_t count, std::vector<cfloat>& out);

   private:
    std::vector<float> taps_;
    std::vector<cfloat> history_;  /* 2x branch length, duplicated window */
    size_t interpolation_{1};
    size_t branch_len_{1};
    size_t pos_{0};
};

}  // namespace dsp

#endif /*__MB200_MODULATE_H__*/
