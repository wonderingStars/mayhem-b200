/*
 * mayhem-b200 — RTTY (Baudot/ITA2 radioteletype) receiver.
 *
 * Ported from firmware/application/external/rtty_rx/ (ui_rtty_rx.*, baudot.*)
 * and firmware/baseband/proc_rtty_rx.{hpp,cpp}. Upstream splits the work across
 * two cores: the M4 runs the decimation chain, the FM discriminator, the
 * envelope tracker, the Schmitt slicer and the asynchronous-UART bit framer,
 * then pushes raw 5-bit Baudot codes to the M0 in an RTTYDataMessage; the M0
 * runs BaudotCoder and prints characters into a Console.
 *
 * There is no second core here, so the whole chain lives in RttyDemodulator and
 * runs on the UI thread from RttyRxView::on_frame_sync().
 *
 * PIPELINE  (mirrors proc_rtty_rx.cpp block for block)
 *
 *   complex baseband @ channel rate
 *     -> FM discriminator (dsp::FmDemod, deviation 9 kHz as upstream sets it)
 *     -> floating envelope tracker (val_max / val_min with periodic decay)
 *     -> midpoint subtraction + one-pole LPF (alpha 1/4, upstream's >>2)
 *     -> spread squelch (open above 600, close below 300, upstream units)
 *     -> Schmitt trigger at +/- spread/8, with the slow polarity auto-flip
 *     -> start/stop UART framer: WAIT_START, CHECK_START, READ_BITS, WAIT_STOP
 *     -> 5-bit Baudot code
 *     -> BaudotDecoder (ITA2 letters/figures with USOS shift handling)
 *
 * UNITS.  Upstream is fixed point: its FM demodulator emits int16 audio and the
 * decoder works on `fm_val = sample * 32`, so a full-scale discriminator swing
 * is 32768 * 32 = 1048576 counts, and every threshold in proc_rtty_rx.cpp (600,
 * 300, 200, ...) is expressed against that. dsp::FmDemod emits floats where
 * +/-1.0 is the configured deviation. To keep the ported constants literally
 * identical to upstream's, the float discriminator output is multiplied by
 * kUpstreamFullScale and the demodulator is configured with upstream's 9 kHz
 * deviation — so a 170 Hz shift lands at the same +/-9902 counts the M4 sees.
 *
 * DEVIATIONS FROM UPSTREAM, and why:
 *
 *  1. Upstream's sample counters (MIN_VALID_PULSE 200, MAX_VALID_PULSE 800, the
 *     7200-sample polarity timer, the 12000-sample squelch timeout, the 528
 *     sample default bit width) are all hard-coded for its fixed 24 kHz demod
 *     rate. A B200 streams at whatever rate the user picked, so the channel
 *     rate here is not exactly 24 kHz and those constants are scaled by
 *     rate/24000. At 24 kHz they are bit-for-bit upstream's.
 *  2. The first discriminator output after a reset is discarded. dsp::FmDemod
 *     starts with prev_ = 1+0j, so the first sample of a stream measures the
 *     phase difference against an arbitrary reference and can be a full-scale
 *     spike; feeding that to the envelope tracker sets val_max ~100x too high
 *     and blinds the slicer for seconds while the decay unwinds it.
 *  3. Envelope outlier guard: a sample whose magnitude exceeds four times the
 *     expected half-shift does not update val_max/val_min (it is still sliced).
 *     The host's sample tap is not continuous (see the note in the .cpp), so
 *     every block boundary is a phase discontinuity and produces exactly the
 *     spike described in (2). Upstream's M4 has a gapless stream and needs no
 *     such guard.
 *  4. Upstream keeps candidate_width, candidate_hits, squelch_closed_timer and
 *     is_squelched as file-scope globals in proc_rtty_rx.cpp; here they are
 *     members, so two decoders can coexist (the tests rely on that).
 *
 * Copyright (C) 2026 HTotoo (original app and baseband processor)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_RTTY_RX_H__
#define __MB200_UI_RTTY_RX_H__

#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace app {
namespace rtty {

/* ===========================================================================
 * Baudot / ITA2
 *
 * Straight port of external/rtty_rx/baudot.cpp. The two 32-entry tables are
 * upstream's, character for character, including the deliberate 0 entries at
 * index 0, 27 (the shift codes themselves) and 31.
 * ===========================================================================*/

inline constexpr uint8_t kBaudotFigs = 0x1B;  /* 27 */
inline constexpr uint8_t kBaudotLtrs = 0x1F;  /* 31 */
inline constexpr uint8_t kBaudotSpace = 0x04; /* 4  */

class BaudotDecoder {
   public:
    enum class Shift : uint8_t { Letters = 0, Figures = 1 };

    /* Upstream BaudotCoder::get_char_mapping(). Returns 0 for the codes that
     * have no printable character in that shift. */
    static char char_mapping(bool is_figures, uint8_t index) {
        if (index >= 32) return 0;

        if (is_figures) {
            static const char figures[32] = {
                0, '3', '\n', '-', ' ', '\'', '8', '7',
                '\r', '$', '4', '\a', ',', '!', ':', '(',
                '5', '+', ')', '2', '#', '6', '0', '1',
                '9', '?', '&', 0, '.', '/', '=', 0};
            return figures[index];
        }

        static const char letters[32] = {
            0, 'E', '\n', 'A', ' ', 'S', 'I', 'U',
            '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K',
            'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q',
            'O', 'B', 'G', 0, 'M', 'X', 'V', 0};
        return letters[index];
    }

    /* Upstream BaudotCoder::decode(). Shift codes change state and return 0.
     * With USOS ("unshift on space") enabled a space received while in figures
     * shift also drops back to letters, which is what every HF RTTY operator
     * expects and what upstream defaults to. */
    char decode(uint8_t baudot_code) {
        const uint8_t code = static_cast<uint8_t>(baudot_code & 0x1F);

        if (code == kBaudotFigs) {
            shift_ = Shift::Figures;
            return 0;
        }
        if (code == kBaudotLtrs) {
            shift_ = Shift::Letters;
            return 0;
        }
        if (usos_ && shift_ == Shift::Figures && code == kBaudotSpace) {
            shift_ = Shift::Letters;
            return ' ';
        }
        return char_mapping(shift_ == Shift::Figures, code);
    }

    void set_usos(bool enable) { usos_ = enable; }
    bool usos() const { return usos_; }

    Shift shift() const { return shift_; }
    void reset() { shift_ = Shift::Letters; }

   private:
    Shift shift_{Shift::Letters};
    bool usos_{true};
};

/* Encoder side, used by the tests to build the bit stream a transmitter would
 * send. Port of BaudotCoder::encode(), minus the destination-length plumbing:
 * it returns the codes as a vector because the host has one. */
class BaudotEncoder {
   public:
    std::vector<uint8_t> encode(const std::string& src) {
        std::vector<uint8_t> out;
        out.reserve(src.size() + 8);

        for (char raw : src) {
            const char c = static_cast<char>(
                (raw >= 'a' && raw <= 'z') ? (raw - 'a' + 'A') : raw);

            uint8_t code = 0;
            bool found = false;
            bool target_is_figures = false;

            if (c == ' ') {
                code = kBaudotSpace;
                found = true;
            } else {
                for (uint8_t i = 1; i < 32; i++) {
                    if (BaudotDecoder::char_mapping(false, i) == c) {
                        code = i;
                        found = true;
                        target_is_figures = false;
                        break;
                    }
                }
                if (!found) {
                    for (uint8_t i = 1; i < 32; i++) {
                        if (BaudotDecoder::char_mapping(true, i) == c) {
                            code = i;
                            found = true;
                            target_is_figures = true;
                            break;
                        }
                    }
                }
            }

            if (!found) continue;

            if (target_is_figures && shift_ != BaudotDecoder::Shift::Figures) {
                out.push_back(kBaudotFigs);
                shift_ = BaudotDecoder::Shift::Figures;
            } else if (!target_is_figures &&
                       shift_ != BaudotDecoder::Shift::Letters &&
                       code != kBaudotSpace) {
                out.push_back(kBaudotLtrs);
                shift_ = BaudotDecoder::Shift::Letters;
            }

            out.push_back(code);
        }
        return out;
    }

    void reset() { shift_ = BaudotDecoder::Shift::Letters; }

   private:
    BaudotDecoder::Shift shift_{BaudotDecoder::Shift::Letters};
};

/* ===========================================================================
 * Asynchronous UART framer
 *
 * Port of RTTYRxProcessor::process_demodulated_sample()'s state machine and
 * update_baud_estimation(). Fed one sliced bit per sample; emits a 5-bit code
 * once a frame completes.
 *
 * Upstream deliberately accepts a frame whose stop bit sliced low ("improves
 * reception during fades" — the commented-out `if (current_slicer_bit == 1)` in
 * proc_rtty_rx.cpp), and that behaviour is preserved.
 * ===========================================================================*/

class RttyFramer {
   public:
    /* `baud_centi` is upstream's OptionsField value: hundredths of a baud, with
     * 0 meaning auto-detect (4545 == 45.45 baud). */
    void configure(float sample_rate_hz, uint16_t baud_centi) {
        sample_rate_hz_ = (sample_rate_hz > 0.0f) ? sample_rate_hz : 24000.0f;
        baud_centi_ = baud_centi;

        /* Upstream's counters are all quoted at its fixed 24 kHz demod rate. */
        const float k = sample_rate_hz_ / 24000.0f;
        min_valid_pulse_ = static_cast<uint32_t>(200.0f * k);
        max_valid_pulse_ = static_cast<uint32_t>(800.0f * k);
        polarity_limit_ = static_cast<uint32_t>(7200.0f * k);
        squelch_timeout_ = static_cast<uint32_t>(12000.0f * k);

        /* Upstream's 528 is 24000/45.4545 rounded down. */
        default_bit_width_ = static_cast<uint32_t>(sample_rate_hz_ / 45.4545f);
        if (default_bit_width_ < 2) default_bit_width_ = 2;

        reset();
    }

    void reset() {
        if (baud_centi_ > 0) {
            const float real_baud = static_cast<float>(baud_centi_) / 100.0f;
            samples_per_bit_ = static_cast<uint32_t>(sample_rate_hz_ / real_baud);
            if (samples_per_bit_ < 2) samples_per_bit_ = 2;
            estimated_bit_width_ = samples_per_bit_;
        } else {
            samples_per_bit_ = default_bit_width_;
            estimated_bit_width_ = default_bit_width_;
        }

        state_ = State::WaitStart;
        phase_counter_ = 0;
        bit_counter_ = 0;
        shift_reg_ = 0;
        pulse_measure_counter_ = 0;
        last_bit_state_ = 0;
        candidate_width_ = 0;
        candidate_hits_ = 0;
    }

    /* Upstream's squelch-closed reset path: after a long silence, drop back to
     * hunting for a start bit and, in auto mode, forget the baud estimate. */
    void on_squelch_timeout() {
        state_ = State::WaitStart;
        pulse_measure_counter_ = 0;
        if (baud_centi_ == 0) {
            estimated_bit_width_ = default_bit_width_;
            samples_per_bit_ = default_bit_width_;
        }
    }

    void on_polarity_flip() { state_ = State::WaitStart; }

    /* One sliced bit. Returns true and fills `code` when a frame completes. */
    bool process_bit(uint8_t slicer_bit, uint8_t& code) {
        if (baud_centi_ == 0) {
            pulse_measure_counter_++;
            if (slicer_bit != last_bit_state_) {
                update_baud_estimation(pulse_measure_counter_);
                pulse_measure_counter_ = 0;
                last_bit_state_ = slicer_bit;
            }
        }

        switch (state_) {
            case State::WaitStart:
                if (slicer_bit == 0) {
                    phase_counter_ = samples_per_bit_ / 2;
                    state_ = State::CheckStart;
                }
                break;

            case State::CheckStart:
                if (phase_counter_ > 0 && --phase_counter_ == 0) {
                    if (slicer_bit == 0) {
                        phase_counter_ = samples_per_bit_;
                        bit_counter_ = 0;
                        shift_reg_ = 0;
                        state_ = State::ReadBits;
                    } else {
                        state_ = State::WaitStart;
                    }
                }
                break;

            case State::ReadBits:
                if (phase_counter_ > 0 && --phase_counter_ == 0) {
                    /* ITA2 is sent least-significant bit first. */
                    if (slicer_bit)
                        shift_reg_ = static_cast<uint8_t>(shift_reg_ | (1u << bit_counter_));

                    phase_counter_ = samples_per_bit_;
                    bit_counter_++;

                    if (bit_counter_ >= 5) state_ = State::WaitStop;
                }
                break;

            case State::WaitStop:
                if (phase_counter_ > 0 && --phase_counter_ == 0) {
                    code = static_cast<uint8_t>(shift_reg_ & 0x1F);
                    state_ = State::WaitStart;
                    return true;
                }
                break;
        }
        return false;
    }

    uint32_t samples_per_bit() const { return samples_per_bit_; }
    uint32_t estimated_bit_width() const { return estimated_bit_width_; }
    uint32_t polarity_limit() const { return polarity_limit_; }
    uint32_t squelch_timeout() const { return squelch_timeout_; }

    /* Upstream's UI-side baud readout: hundredths of a baud, snapped onto the
     * three standard rates when the estimate is close enough. */
    uint16_t estimated_baud_centi() const {
        if (baud_centi_ != 0) return baud_centi_;
        if (samples_per_bit_ == 0) return 0;

        uint32_t b = static_cast<uint32_t>(sample_rate_hz_ * 100.0f) / samples_per_bit_;
        if (b > 4300 && b < 4700)
            b = 4500;
        else if (b > 4800 && b < 5200)
            b = 5000;
        else if (b > 7200 && b < 7800)
            b = 7500;
        return static_cast<uint16_t>(b);
    }

   private:
    enum class State : uint8_t { WaitStart, CheckStart, ReadBits, WaitStop };

    /* Port of RTTYRxProcessor::update_baud_estimation(). */
    void update_baud_estimation(uint32_t pulse_width) {
        if (pulse_width < min_valid_pulse_ || pulse_width > max_valid_pulse_) return;

        int32_t diff = static_cast<int32_t>(pulse_width) - static_cast<int32_t>(estimated_bit_width_);
        if (diff < 0) diff = -diff;

        if (diff < static_cast<int32_t>(estimated_bit_width_ / 6)) {
            estimated_bit_width_ = (estimated_bit_width_ * 7 + pulse_width) / 8;
            samples_per_bit_ = estimated_bit_width_;
            candidate_hits_ = 0;
        } else {
            int32_t cand_diff = static_cast<int32_t>(pulse_width) - static_cast<int32_t>(candidate_width_);
            if (cand_diff < 0) cand_diff = -cand_diff;

            if (candidate_width_ != 0 && cand_diff < static_cast<int32_t>(candidate_width_ / 8)) {
                candidate_hits_++;
                if (candidate_hits_ >= 3) {
                    estimated_bit_width_ = (candidate_width_ + pulse_width) / 2;
                    samples_per_bit_ = estimated_bit_width_;
                    candidate_hits_ = 0;
                    state_ = State::WaitStart;
                }
            } else {
                candidate_width_ = pulse_width;
                candidate_hits_ = 1;
            }
        }
    }

    float sample_rate_hz_{24000.0f};
    uint16_t baud_centi_{0};

    uint32_t min_valid_pulse_{200};
    uint32_t max_valid_pulse_{800};
    uint32_t polarity_limit_{7200};
    uint32_t squelch_timeout_{12000};
    uint32_t default_bit_width_{528};

    State state_{State::WaitStart};
    uint32_t samples_per_bit_{528};
    uint32_t phase_counter_{0};
    uint8_t bit_counter_{0};
    uint8_t shift_reg_{0};

    uint32_t pulse_measure_counter_{0};
    uint8_t last_bit_state_{0};
    uint32_t estimated_bit_width_{528};
    uint32_t candidate_width_{0};
    uint8_t candidate_hits_{0};
};

/* ===========================================================================
 * Full receive chain
 * ===========================================================================*/

/* Full-scale of upstream's fixed-point discriminator value (int16 * 32). */
inline constexpr float kUpstreamFullScale = 32768.0f * 32.0f;

/* Deviation dsp::FmDemod is configured with, matching upstream's
 * `demod.configure(24000, 9000)`. It only sets the output scale; a 170 Hz shift
 * therefore lands at +/- (85/9000) * kUpstreamFullScale ~= +/-9902 counts, the
 * same numbers proc_rtty_rx.cpp's thresholds were tuned against. */
inline constexpr float kDiscriminatorDeviationHz = 9000.0f;

class RttyDemodulator {
   public:
    /* `sample_rate_hz` is the rate of the complex baseband handed to process().
     * `baud_centi` is hundredths of a baud, 0 for auto-detect.
     * `shift_hz` is the mark/space separation (upstream hard-codes 170). */
    void configure(float sample_rate_hz, uint16_t baud_centi, float shift_hz = 170.0f) {
        sample_rate_hz_ = (sample_rate_hz > 0.0f) ? sample_rate_hz : 24000.0f;
        shift_hz_ = (shift_hz > 0.0f) ? shift_hz : 170.0f;

        fm_.configure(sample_rate_hz_, kDiscriminatorDeviationHz);
        framer_.configure(sample_rate_hz_, baud_centi);

        /* Guard for deviation (3): four times the expected half-shift. */
        envelope_guard_ =
            (shift_hz_ * 2.0f / kDiscriminatorDeviationHz) * kUpstreamFullScale;

        reset();
    }

    void reset() {
        fm_.reset();
        framer_.reset();
        baudot_.reset();

        val_max_ = -200000.0f;
        val_min_ = 200000.0f;
        decay_timer_ = 0;
        fm_val_smoothed_ = 0.0f;
        is_squelched_ = true;
        squelch_closed_timer_ = 0;
        polarity_timer_ = 0;
        inverted_polarity_ = false;
        current_slicer_bit_ = 1;
        primed_ = false;
        scratch_.clear();
    }

    /* Complex baseband in, decoded text out (appended). */
    void process(const dsp::cfloat* in, size_t count, std::string& out) {
        if (count == 0) return;
        scratch_.clear();
        fm_.process(in, count, scratch_);

        size_t first = 0;
        if (!primed_ && !scratch_.empty()) {
            /* Deviation (2): the first difference after a reset is meaningless. */
            first = 1;
            primed_ = true;
        }
        for (size_t i = first; i < scratch_.size(); i++)
            process_discriminator_sample(scratch_[i] * kUpstreamFullScale, out);
    }

    /* Discriminator output straight in, in upstream's counts (+/-1048576 is
     * +/-9 kHz). The tests drive this to exercise the slicer and framer without
     * going through dsp::FmDemod. */
    void process_counts(const float* in, size_t count, std::string& out) {
        for (size_t i = 0; i < count; i++) process_discriminator_sample(in[i], out);
    }

    bool squelched() const { return is_squelched_; }
    bool inverted_polarity() const { return inverted_polarity_; }
    float envelope_spread() const { return val_max_ - val_min_; }
    uint16_t estimated_baud_centi() const { return framer_.estimated_baud_centi(); }
    uint32_t samples_per_bit() const { return framer_.samples_per_bit(); }

    BaudotDecoder& baudot() { return baudot_; }
    const BaudotDecoder& baudot() const { return baudot_; }
    RttyFramer& framer() { return framer_; }

   private:
    /* Port of the per-sample body of RTTYRxProcessor::execute(). */
    void process_discriminator_sample(float fm_val, std::string& out) {
        /* 1. Envelope tracking, with the host's outlier guard (deviation 3). */
        if (std::fabs(fm_val) <= envelope_guard_) {
            if (fm_val > val_max_) val_max_ = fm_val;
            if (fm_val < val_min_) val_min_ = fm_val;
        }

        /* 2. Decay: upstream's uint8 counter fires every 256 samples. */
        if (++decay_timer_ == 0) {
            const float spread = val_max_ - val_min_;
            if (spread > 200.0f) {
                const float decay = std::floor(spread / 128.0f) + 1.0f;
                if (val_max_ > val_min_ + decay) val_max_ -= decay;
                if (val_min_ < val_max_ - decay) val_min_ += decay;
            }
        }

        /* 3-4. Centre on the midpoint of the tracked envelope. */
        const float midpoint = (val_max_ + val_min_) * 0.5f;
        const float centered = fm_val - midpoint;

        /* Upstream's `>>= LPF_ALPHA_SHIFT` with a shift of 2. */
        fm_val_smoothed_ += (centered - fm_val_smoothed_) * 0.25f;

        /* 5. Squelch on the envelope spread. */
        const float spread = val_max_ - val_min_;
        if (is_squelched_) {
            if (spread > 600.0f) is_squelched_ = false;
        } else {
            if (spread < 300.0f) is_squelched_ = true;
        }

        slice_and_frame(fm_val_smoothed_, out);
    }

    /* Port of RTTYRxProcessor::process_demodulated_sample(). */
    void slice_and_frame(float sample, std::string& out) {
        if (is_squelched_) {
            squelch_closed_timer_++;
            if (squelch_closed_timer_ > framer_.squelch_timeout()) {
                current_slicer_bit_ = 1;
                inverted_polarity_ = false;
                framer_.on_squelch_timeout();
            }
            return;
        }
        squelch_closed_timer_ = 0;

        const float hysteresis = (val_max_ - val_min_) / 8.0f;

        uint8_t raw_bit = current_slicer_bit_;
        if (inverted_polarity_) raw_bit = static_cast<uint8_t>(!raw_bit);

        if (sample > hysteresis)
            raw_bit = 1;
        else if (sample < -hysteresis)
            raw_bit = 0;

        /* A carrier that sits on the space tone for a third of a second is
         * almost certainly the opposite sideband; upstream flips and re-hunts. */
        if (raw_bit == 0) {
            if (++polarity_timer_ > framer_.polarity_limit()) {
                inverted_polarity_ = !inverted_polarity_;
                polarity_timer_ = 0;
                val_max_ = -200000.0f;
                val_min_ = 200000.0f;
                framer_.on_polarity_flip();
            }
        } else {
            polarity_timer_ = 0;
        }

        current_slicer_bit_ = inverted_polarity_ ? static_cast<uint8_t>(!raw_bit) : raw_bit;

        uint8_t code = 0;
        if (framer_.process_bit(current_slicer_bit_, code)) {
            const char c = baudot_.decode(code);
            if (c != 0) out.push_back(c);
        }
    }

    dsp::FmDemod fm_{};
    RttyFramer framer_{};
    BaudotDecoder baudot_{};
    std::vector<float> scratch_{};

    float sample_rate_hz_{24000.0f};
    float shift_hz_{170.0f};
    float envelope_guard_{40000.0f};

    float val_max_{-200000.0f};
    float val_min_{200000.0f};
    uint8_t decay_timer_{0};
    float fm_val_smoothed_{0.0f};

    bool is_squelched_{true};
    uint32_t squelch_closed_timer_{0};
    uint32_t polarity_timer_{0};
    bool inverted_polarity_{false};
    uint8_t current_slicer_bit_{1};
    bool primed_{false};
};

/* ===========================================================================
 * Host front end: wideband tap -> channel rate
 *
 * A two-stage decimating channeliser. Kept here rather than in a shared header
 * because the porting contract forbids adding shared files; the SSTV receiver
 * has its own copy for the same reason.
 * ===========================================================================*/

class ChannelFrontEnd {
   public:
    /* Decimates `input_rate_hz` down to the lowest rate >= `target_rate_hz`
     * reachable with two integer stages, and reports the rate it landed on. */
    void configure(double input_rate_hz, double target_rate_hz) {
        input_rate_hz_ = (input_rate_hz > 0.0) ? input_rate_hz : 2'400'000.0;
        const double target = (target_rate_hz > 0.0) ? target_rate_hz : 24'000.0;

        /* Stage 1 brings the rate down to at least 4x the target, which keeps
         * both filters short: a single stage at 2.4 Msps would need thousands
         * of taps to reach a 24 kHz output cleanly. */
        const double intermediate_min = target * 4.0;
        size_t d1 = static_cast<size_t>(input_rate_hz_ / intermediate_min);
        if (d1 < 1) d1 = 1;
        if (d1 > 64) d1 = 64;

        const double mid_rate = input_rate_hz_ / static_cast<double>(d1);
        size_t d2 = static_cast<size_t>(mid_rate / target);
        if (d2 < 1) d2 = 1;

        output_rate_hz_ = mid_rate / static_cast<double>(d2);

        if (d1 > 1) {
            const double cutoff = mid_rate * 0.20;
            const double transition = mid_rate * 0.25;
            stage1_.configure(dsp::design_lowpass(cutoff, transition, input_rate_hz_, 60.0), d1);
            stage1_enabled_ = true;
        } else {
            stage1_enabled_ = false;
        }

        if (d2 > 1) {
            const double cutoff = output_rate_hz_ * 0.25;
            const double transition = output_rate_hz_ * 0.25;
            stage2_.configure(dsp::design_lowpass(cutoff, transition, mid_rate, 60.0), d2);
            stage2_enabled_ = true;
        } else {
            stage2_.configure(dsp::design_lowpass(output_rate_hz_ * 0.25,
                                                  output_rate_hz_ * 0.25,
                                                  mid_rate, 60.0),
                              1);
            stage2_enabled_ = true;
        }

        decimation_ = d1 * d2;
        reset();
    }

    void reset() {
        stage1_.reset();
        stage2_.reset();
        nco_.reset();
        mid_.clear();
    }

    void set_offset(double offset_hz) {
        if (offset_hz == offset_hz_) return;
        offset_hz_ = offset_hz;
        nco_.set_frequency(-offset_hz, input_rate_hz_);
    }

    double offset() const { return offset_hz_; }
    double output_rate() const { return output_rate_hz_; }
    size_t decimation() const { return decimation_; }

    /* Mixes the wanted signal to zero, filters and decimates. `out` is cleared
     * first and then filled. */
    void process(std::vector<dsp::cfloat>& in_out, std::vector<dsp::cfloat>& out) {
        out.clear();
        if (in_out.empty()) return;

        nco_.mix(in_out.data(), in_out.data(), in_out.size());

        if (stage1_enabled_) {
            mid_.clear();
            stage1_.process(in_out.data(), in_out.size(), mid_);
            if (stage2_enabled_) stage2_.process(mid_.data(), mid_.size(), out);
            else out = mid_;
        } else {
            if (stage2_enabled_) stage2_.process(in_out.data(), in_out.size(), out);
            else out = in_out;
        }
    }

   private:
    dsp::Nco nco_{};
    dsp::FirDecimateC stage1_{};
    dsp::FirDecimateC stage2_{};
    std::vector<dsp::cfloat> mid_{};

    double input_rate_hz_{2'400'000.0};
    double output_rate_hz_{24'000.0};
    double offset_hz_{0.0};
    size_t decimation_{100};
    bool stage1_enabled_{false};
    bool stage2_enabled_{false};
};

}  // namespace rtty

/* ===========================================================================
 * View
 * ===========================================================================*/

class RttyRxView : public ui::View {
   public:
    RttyRxView();
    ~RttyRxView() override;

    RttyRxView(const RttyRxView&) = delete;
    RttyRxView& operator=(const RttyRxView&) = delete;

    std::string title() const override { return "RTTY"; }

    void on_show() override;
    void on_frame_sync() override;

   private:
    void rebuild_chain();
    void update_status();

    rtty::RttyDemodulator demod_{};
    rtty::ChannelFrontEnd front_end_{};

    std::vector<dsp::cfloat> raw_{};
    std::vector<dsp::cfloat> channel_{};
    std::string decoded_{};

    double configured_input_rate_{0.0};
    uint16_t baud_centi_{0};
    uint32_t frame_counter_{0};
    bool chain_valid_{false};

    ui::Labels labels_{
        {{0, 2}, "Freq", ui::Color::light_grey()},
        {{0, 20}, "Baud", ui::Color::light_grey()},
        {{120, 20}, "Gain", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 2}};
    ui::FrequencyStepView step_view_{{132, 2}, field_frequency_};

    ui::OptionsField options_baud_{
        {40, 20},
        5,
        {{"Auto ", 0},
         {"45   ", 4500},
         {"45.45", 4545},
         {"50   ", 5000},
         {"75   ", 7500},
         {"100  ", 10000},
         {"110  ", 11000},
         {"150  ", 15000},
         {"200  ", 20000}}};

    ui::NumberField field_gain_{{160, 20}, 3, {0, 76}, 1, ' '};

    ui::Text text_status_{{0, 38, 240, 16}, "Shift 170 Hz"};

    ui::Labels notes_{
        {{0, 58}, "Sample tap is not gapless:", ui::Color::yellow()},
        {{0, 74}, "see ui_rtty_rx.cpp. No radio", ui::Color::grey()},
        {{0, 90}, "attached - RF path untested.", ui::Color::grey()},
    };

    ui::Console console_{{0, 110, 240, 194}};
};

}  // namespace app

#endif /*__MB200_UI_RTTY_RX_H__*/
