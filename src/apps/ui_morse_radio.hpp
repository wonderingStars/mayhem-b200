/*
 * mayhem-b200 — Morse (CW) receiver.
 *
 * Port of the firmware's application/external/morse_radio/* (view + decoder)
 * together with its baseband processor baseband/proc_morse.{hpp,cpp}. Upstream
 * splits the job across two cores:
 *
 *   M4 (proc_morse.cpp)  channel filter -> AM / SSB / FM demod -> per-sample
 *                        squelch, gain, hard limiter, DC blocker -> integer
 *                        Goertzel tone detector (60-sample blocks) -> tone
 *                        on/off state durations in microseconds, plus a
 *                        zero-crossing measurement of the CW tone frequency.
 *   M0 (ui_morse_radio)  accumulate consecutive same-sign durations
 *                        (ProcessSignal) -> MorseDecoder: element/gap timing
 *                        with an adaptive time unit -> morse table lookup.
 *
 * On the host there is no second core and no message queue, so the same two
 * halves live in one file: `morse::ToneProcessor` is proc_morse's per-sample
 * loop and `morse::SignalAccumulator` + `morse::Decoder` are the view's half.
 * Both are plain classes with no UI or radio dependency so they can be tested
 * against injected sample and duration sequences (tests/test_morse_radio.cpp).
 *
 * Faithfulness notes, all verified against the upstream sources:
 *
 *  - The Goertzel runs in the same int32/int64 fixed point as upstream,
 *    including its small-angle cosine approximation (cos w ~ 1 - w^2/2) and the
 *    >>14 scaling. The power thresholds (noise_floor * sensitivity, and the
 *    absolute floor of 150000) are calibrated to that scaling, so converting
 *    the filter to float would silently change the detector's sensitivity.
 *  - duration_us = duration_samples * 250 / 3 is upstream's, and is only exact
 *    at the 12 kHz audio rate: 60 samples -> 5000 us. In upstream's NFM mode
 *    the audio rate is 24 kHz, so every reported duration is 2x too long. That
 *    is an upstream bug; it is ported unchanged (the decoder's adaptive time
 *    unit absorbs a constant scale factor, which is presumably why it was never
 *    noticed) and flagged here rather than silently "fixed".
 *  - MorseDecoder used C99 variable-length arrays (`uint32_t x[ring.size()]`),
 *    which MSVC does not accept. The ring's capacity is a fixed 40, so those
 *    become arrays of 40 — same values, same iteration counts.
 *  - Upstream's on_data() loops `for (i = 0; i <= message->state_cnt; ++i)`
 *    with state_cnt == 1, so it also feeds the always-zero state_durations[1]
 *    into the decoder. handleInput() discards anything in (-5, 5), so that
 *    extra call is a no-op; the port simply iterates the durations actually
 *    produced.
 *
 * Copyright (C) 2025, 2026 Pezsma
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_MORSE_RADIO_H__
#define __MB200_UI_MORSE_RADIO_H__

#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* ======================================================================== *
 *  Pure Morse logic — no UI, no radio. Ported from morsedecoder.hpp,       *
 *  ui_morse_radio.cpp::ProcessSignal and proc_morse.cpp.                   *
 * ======================================================================== */
namespace morse {

/* Fixed 40-entry ring, port of MorseRingBuffer. Oldest element is index 0;
 * pushing into a full ring drops the oldest. */
class RingBuffer40 {
   public:
    static constexpr size_t capacity = 40;

    void push_back(const uint32_t& value) {
        data_[head_] = value;
        head_ = (head_ + 1) % capacity;
        if (count_ < capacity)
            count_++;
        else
            tail_ = (tail_ + 1) % capacity;  /* overwrite oldest */
    }

    void pop_front() {
        if (count_ > 0) {
            tail_ = (tail_ + 1) % capacity;
            count_--;
        }
    }

    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const uint32_t& front() const { return data_[tail_]; }

    uint32_t operator[](size_t idx) const { return data_[(tail_ + idx) % capacity]; }

    void copy_to_array(uint32_t* out) const {
        for (size_t i = 0; i < count_; ++i) out[i] = (*this)[i];
    }

    void clear() {
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }

   private:
    uint32_t data_[capacity]{};
    size_t head_{0};
    size_t tail_{0};
    size_t count_{0};
};

/* The International Morse table upstream ships: 26 letters, 10 digits and 14
 * punctuation marks, in upstream's order. */
struct TableEntry {
    const char* code;
    const char* letter;
};

inline constexpr TableEntry table[] = {
    {".-", "A"},
    {"-...", "B"},
    {"-.-.", "C"},
    {"-..", "D"},
    {".", "E"},
    {"..-.", "F"},
    {"--.", "G"},
    {"....", "H"},
    {"..", "I"},
    {".---", "J"},
    {"-.-", "K"},
    {".-..", "L"},
    {"--", "M"},
    {"-.", "N"},
    {"---", "O"},
    {".--.", "P"},
    {"--.-", "Q"},
    {".-.", "R"},
    {"...", "S"},
    {"-", "T"},
    {"..-", "U"},
    {"...-", "V"},
    {".--", "W"},
    {"-..-", "X"},
    {"-.--", "Y"},
    {"--..", "Z"},
    {".----", "1"},
    {"..---", "2"},
    {"...--", "3"},
    {"....-", "4"},
    {".....", "5"},
    {"-....", "6"},
    {"--...", "7"},
    {"---..", "8"},
    {"----.", "9"},
    {"-----", "0"},
    {".-.-.-", "."},
    {"..--..", "?"},
    {"-.-.--", "!"},
    {"--..--", ","},
    {"-...-", "="},
    {"-..-.", "/"},
    {".--.-.", "@"},
    {"---...", ":"},
    {"-....-", "-"},
    {".----.", "'"},
    {".-..-.", "\""},
    {"-.--.", "("},
    {"-.--.-", ")"},
    {".-.-.", "+"}};

inline constexpr size_t table_size = sizeof(table) / sizeof(table[0]);

/* Morse code -> character. An unknown sequence comes back wrapped in braces,
 * exactly as upstream does, so the console shows what was heard. */
inline std::string lookup(const std::string& seq) {
    for (size_t i = 0; i < table_size; i++)
        if (seq == table[i].code) return table[i].letter;
    return "{" + seq + "}";
}

/* Element/gap timing with an adaptive time unit. Port of MorseDecoder. */
class Decoder {
   public:
    struct Result {
        std::string text{};
        double confidence{0.0};
        bool isValid() const { return !text.empty(); }
    };

    Decoder() = default;

    void resetLearning() {
        time_unit_ms_ = 119.0;
        current_sequence_.clear();
        last_sequence_.clear();
        last_confidence_ = 0.0;
        pulse_history_.clear();
        pulse_gaps_.clear();
    }

    /* One tone-on (positive) or tone-off (negative) interval, in milliseconds.
     * A character is emitted on the first gap at or beyond the inter-character
     * threshold; a gap at or beyond the inter-word threshold also appends a
     * space. Intervals inside +/-5 ms are ignored as noise. */
    Result handleInput(int32_t duration_ms) {
        Result result{};

        if (duration_ms < 5 && duration_ms > -5) return result;

        if (duration_ms > 0) {
            pulse_history_.push_back(static_cast<uint32_t>(duration_ms));
            const double dah_prob = getDahProbability(static_cast<uint32_t>(duration_ms));
            current_sequence_ += (dah_prob > 0.5) ? '-' : '.';
            last_confidence_ = (dah_prob > 0.5) ? dah_prob : (1.0 - dah_prob);
            last_sequence_ = current_sequence_;
        } else {
            const uint32_t gap_duration = static_cast<uint32_t>(-duration_ms);
            pulse_gaps_.push_back(gap_duration);

            if (gap_duration >= getInterCharThreshold() && !current_sequence_.empty()) {
                result.text = lookup(current_sequence_);
                result.confidence = (result.text[0] != '{') ? last_confidence_ : 0.0;
                if (gap_duration >= getInterWordThreshold()) result.text += " ";
                current_sequence_.clear();
            }
        }

        updateLearning();
        return result;
    }

    double getInterElementThreshold() const { return time_unit_ms_ * 0.8; }
    double getInterCharThreshold() const { return time_unit_ms_ * 2.5; }
    double getInterWordThreshold() const { return time_unit_ms_ * 6.0; }

    double getCurrentTimeUnit() const { return time_unit_ms_; }
    const std::string& getLastSequence() const { return last_sequence_; }
    const std::string& currentSequence() const { return current_sequence_; }

    /* Probability that a `duration_ms` key-down is a dash rather than a dot.
     * Linear between 1.5 and 2.5 time units. */
    double getDahProbability(uint32_t duration_ms) const {
        const double start_interp = 1.5 * time_unit_ms_;
        const double end_interp = 2.5 * time_unit_ms_;
        if (duration_ms <= start_interp) return 0.0;
        if (duration_ms >= end_interp) return 1.0;
        return (static_cast<double>(duration_ms) - start_interp) / (end_interp - start_interp);
    }

    /* Words per minute from the current time unit, as the view computes it:
     * PARIS is 50 units, and upstream derives it from the dah length. */
    uint16_t wpm() const {
        const float dah_time = static_cast<float>(time_unit_ms_) * 3.0f;
        if (dah_time <= 0.0f) return 0;
        return static_cast<uint16_t>(3600.0f / (dah_time + 18.0f) + 0.5f);
    }

    /* --- exposed for tests --------------------------------------------- */

    /* Index of the widest relative jump in a sorted duration list: the split
     * between the dot cluster and the dash cluster. 0 means "no clean split". */
    static size_t findDecisionBoundary(const uint32_t* sorted_data, size_t sorted_data_size) {
        if (sorted_data_size < 4) return 0;

        size_t best_split_index = 0;
        uint32_t max_diff = 0;

        for (size_t i = 1; i < sorted_data_size; ++i) {
            const uint32_t diff = sorted_data[i] - sorted_data[i - 1];
            if (diff > sorted_data[i - 1] * 0.5 && diff > max_diff) {
                max_diff = diff;
                best_split_index = i;
            }
        }
        return best_split_index;
    }

    bool calculatePulseUnit(double& unit, double& confidence) const {
        if (pulse_history_.size() < 10) return false;

        const size_t n = pulse_history_.size();
        uint32_t sorted_pulses[RingBuffer40::capacity]{};
        pulse_history_.copy_to_array(sorted_pulses);
        sort_uint32(sorted_pulses, n);

        const size_t split_index = findDecisionBoundary(sorted_pulses, n);
        if (split_index == 0 || split_index < 3 || (n - split_index) < 2) return false;

        const double dit_sum = sum_uint32_range(sorted_pulses, 0, split_index);
        const double dah_sum = sum_uint32_range(sorted_pulses, split_index, n);

        const double avg_dit = dit_sum / static_cast<double>(split_index);
        const double avg_dah = dah_sum / static_cast<double>(n - split_index);
        if (avg_dah <= avg_dit) return false;

        const double ratio = avg_dah / avg_dit;
        if (ratio > 1.5 && ratio < 5.0) {
            unit = avg_dit;
            double tmpabs = ratio - 3.0;
            if (tmpabs < 0) tmpabs *= -1;
            tmpabs /= 3.0;
            tmpabs = 1.0 - tmpabs;
            if (tmpabs < 0) tmpabs = 0;
            confidence = tmpabs;  /* 0..1 */
            return true;
        }
        return false;
    }

    bool calculateGapUnit(double& unit, double& confidence) const {
        if (pulse_gaps_.size() < 10) return false;

        const double threshold = getInterElementThreshold();
        double valid_gaps[RingBuffer40::capacity]{};
        size_t valid_count = 0;
        for (size_t i = 0; i < pulse_gaps_.size(); i++) {
            const double gap = pulse_gaps_[i];
            if (gap <= threshold) valid_gaps[valid_count++] = gap;
        }
        if (valid_count < 2) return false;

        const double sum = sum_double_range(valid_gaps, 0, valid_count);
        unit = sum / static_cast<double>(valid_count);
        confidence = 0.8;
        return true;
    }

    size_t pulseCount() const { return pulse_history_.size(); }
    size_t gapCount() const { return pulse_gaps_.size(); }

   private:
    std::string current_sequence_{};
    std::string last_sequence_{};
    double time_unit_ms_{119.0};
    double last_confidence_{0.0};
    RingBuffer40 pulse_history_{};
    RingBuffer40 pulse_gaps_{};

    static double sum_uint32_range(const uint32_t* data, size_t start, size_t end) {
        double sum = 0.0;
        for (size_t i = start; i < end; i++) sum += data[i];
        return sum;
    }

    static double sum_double_range(const double* data, size_t start, size_t end) {
        double sum = 0.0;
        for (size_t i = start; i < end; i++) sum += data[i];
        return sum;
    }

    /* Insertion sort, as upstream — the list never exceeds 40 entries. */
    static void sort_uint32(uint32_t* data, size_t size) {
        if (size < 2) return;
        for (size_t i = 1; i < size; i++) {
            const uint32_t key = data[i];
            size_t j = i;
            while (j > 0 && data[j - 1] > key) {
                data[j] = data[j - 1];
                j--;
            }
            data[j] = key;
        }
    }

    static double clamp_double(double value, double min_val, double max_val) {
        if (value < min_val) return min_val;
        if (value > max_val) return max_val;
        return value;
    }

    /* Blend the pulse-derived and gap-derived unit estimates into the running
     * time unit, rate-limited to +/-25% per update and with a learning rate that
     * rises the further the estimate is from the 160 ms default. */
    void updateLearning() {
        double pulse_unit = -1.0, pulse_confidence = 0.0;
        double gap_unit = -1.0, gap_confidence = 0.0;

        const bool pulse_success = calculatePulseUnit(pulse_unit, pulse_confidence);
        const bool gap_success = calculateGapUnit(gap_unit, gap_confidence);

        double new_time_unit = -1.0;
        if (pulse_success && pulse_confidence > 0.5) {
            new_time_unit = pulse_unit;
        } else if (pulse_success && gap_success) {
            gap_confidence = 0.2;
            const double total_confidence = pulse_confidence + gap_confidence;
            new_time_unit =
                (pulse_unit * pulse_confidence + gap_unit * gap_confidence) / total_confidence;
        } else if (gap_success) {
            new_time_unit = gap_unit;
        } else {
            return;
        }

        const double max_change = time_unit_ms_ * 0.25;
        new_time_unit =
            clamp_double(new_time_unit, time_unit_ms_ - max_change, time_unit_ms_ + max_change);

        const double DEFAULT_TIME_UNIT = 160.0;
        const double BASE_LEARNING_RATE = 0.05;
        const double MAX_LEARNING_RATE = 0.25;
        double tudeltaabs = new_time_unit - DEFAULT_TIME_UNIT;
        if (tudeltaabs < 0) tudeltaabs *= -1;
        const double deviation_from_default = tudeltaabs / DEFAULT_TIME_UNIT;
        double tpp = deviation_from_default * 2.0;
        if (tpp > 1) tpp = 1;
        const double learning_factor =
            BASE_LEARNING_RATE + (MAX_LEARNING_RATE - BASE_LEARNING_RATE) * tpp;

        time_unit_ms_ = (time_unit_ms_ * (1.0 - learning_factor)) + (new_time_unit * learning_factor);
    }
};

/* Merges the stream of signed microsecond intervals coming out of the tone
 * detector into one interval per key-down / key-up transition, and emits a
 * long silence early (once it passes the inter-word threshold) so a word break
 * does not wait for the next key-down. Port of MorseRadioView::ProcessSignal;
 * upstream reads the threshold from its decoder, here it is a parameter so the
 * class is testable on its own.
 *
 * Returns a duration in MILLISECONDS to hand to Decoder::handleInput, or 0. */
class SignalAccumulator {
   public:
    void reset() {
        last_sign_ = 0;
        accumulator_us_ = 0;
        long_pause_sent_ = false;
    }

    int32_t process(int32_t sig_time_us, double inter_word_threshold_ms) {
        const int8_t sign = (sig_time_us > 0) ? int8_t{1} : ((sig_time_us < 0) ? int8_t{-1} : int8_t{0});
        int32_t result = 0;

        if (last_sign_ == 0) {
            last_sign_ = sign;
            accumulator_us_ = sig_time_us;
            long_pause_sent_ = false;
            return 0;
        }

        if (sign == last_sign_) {
            accumulator_us_ += sig_time_us;
            if (sign < 0) {
                const int32_t threshold_us = static_cast<int32_t>(inter_word_threshold_ms * 1000.0);
                if (!long_pause_sent_ && accumulator_us_ <= -threshold_us) {
                    result = accumulator_us_ / 1000;
                    long_pause_sent_ = true;
                }
            }
        } else {
            /* state change */
            result = long_pause_sent_ ? 0 : (accumulator_us_ / 1000);
            accumulator_us_ = sig_time_us;
            last_sign_ = sign;
            long_pause_sent_ = false;
        }
        return result;
    }

    int32_t accumulator_us() const { return accumulator_us_; }

   private:
    bool long_pause_sent_{false};
    int8_t last_sign_{0};
    int32_t accumulator_us_{0};
};

/* Which demodulator feeds the detector. Values match upstream's
 * MorseProcessor::ModulationMode, which is what the UI's mode index is cast to.
 */
enum class Modulation : uint8_t {
    AM = 0,   /* AM/CW  */
    FM = 1,   /* NFM    */
    DSB = 2,  /* AM/DSB */
    USB = 3,  /* AM/USB */
    LSB = 4,  /* AM/LSB */
};

/* proc_morse.cpp's per-sample half: squelch, gain, hard limiter, DC blocker,
 * the integer Goertzel tone detector and the zero-crossing tone-frequency
 * measurement. Fed float audio in [-1, 1] at 12 kHz (24 kHz in FM). */
class ToneProcessor {
   public:
    void configure(Modulation mode) {
        modulation_ = mode;

        meas_samples_in_period_ = 0;
        meas_last_period_len_ = 0;
        meas_consistency_count_ = 0;
        meas_signal_state_high_ = false;
        meas_freq_accumulator_ = 0.0f;
        meas_freq_count_ = 0;
        ui_update_timer_ = 0;

        s_prev_i_ = 0;
        s_prev2_i_ = 0;
        goertzel_count_ = 0;
        duration_samples_ = 0;
        was_signaling_ = false;

        noise_floor_ = 5000;
        startup_delay_ = 20;
        squelch_is_open_ = true;
        squelch_hold_ = 0;

        dc_average_ = 0.0f;
        clipped_ = false;
        freq_updated_ = false;
        measured_frequency_ = 0;

        current_freq_ = 700.0f;
        update_goertzel_coeff(current_freq_);
    }

    void set_squelch_level(int32_t level) { user_squelch_level_ = level; }
    int32_t squelch_level() const { return user_squelch_level_; }

    Modulation modulation() const { return modulation_; }

    /* Audio sample rate the detector's constants assume. */
    float audio_rate_hz() const { return (modulation_ == Modulation::FM) ? 24000.0f : 12000.0f; }

    /* Runs upstream's execute() loop body over a block of demodulated audio and
     * appends each completed tone-on / tone-off interval, in microseconds
     * (positive = tone present), to `durations_us`. */
    void process_audio(const float* audio, size_t count, std::vector<int32_t>& durations_us) {
        for (size_t i = 0; i < count; i++) {
            const float raw_audio = audio[i];

            int32_t raw_int_abs = static_cast<int32_t>(raw_audio * 32768.0f);
            if (raw_int_abs < 0) raw_int_abs = -raw_int_abs;

            const int32_t audio_threshold = (user_squelch_level_ * user_squelch_level_) * 3;

            if (raw_int_abs > audio_threshold || user_squelch_level_ == 0) {
                squelch_is_open_ = true;
                squelch_hold_ = (modulation_ == Modulation::FM) ? 2400 : 1200;
            } else {
                if (squelch_hold_ > 0)
                    squelch_hold_--;
                else if (modulation_ == Modulation::FM)
                    squelch_is_open_ = false;
            }

            float decode_audio = raw_audio;
            if (modulation_ != Modulation::FM) {
                float gain = 16.0f;
                if (modulation_ == Modulation::USB || modulation_ == Modulation::LSB) gain = 5.0f;
                decode_audio *= gain;

                clipped_ = false;
                if (decode_audio > 1.0f) {
                    decode_audio = 1.0f;
                    clipped_ = true;
                } else if (decode_audio < -1.0f) {
                    decode_audio = -1.0f;
                }
            }

            if (modulation_ != Modulation::FM) {
                dc_average_ = (dc_average_ * 0.95f) + (decode_audio * 0.05f);
                measure_frequency(static_cast<int32_t>((decode_audio - dc_average_) * 32768.0f));
            } else {
                measure_frequency(static_cast<int32_t>(raw_audio * 32768.0f));
            }

            process_decoding(static_cast<int32_t>(decode_audio * 32768.0f), durations_us);
        }
    }

    bool clipped() const { return clipped_; }
    bool squelch_open() const { return squelch_is_open_; }

    /* Rounded-to-5-Hz tone frequency, republished about every 200 ms. */
    bool take_frequency_update(uint32_t& hz) {
        if (!freq_updated_) return false;
        hz = measured_frequency_;
        freq_updated_ = false;
        return true;
    }
    uint32_t measured_frequency() const { return measured_frequency_; }
    float current_freq() const { return current_freq_; }
    int32_t goertzel_coefficient() const { return coeff_int_; }
    int64_t noise_floor() const { return noise_floor_; }

    /* Upstream's Goertzel coefficient, exposed so the fixed-point form can be
     * checked against the exact one in tests. */
    void update_goertzel_coeff(float freq) {
        if (freq < 300.0f) freq = 300.0f;
        if (freq > 2300.0f) freq = 2300.0f;

        const float sample_rate = audio_rate_hz();
        const float omega = 2.0f * 3.14159265358979323846f * freq / sample_rate;
        const float omega_sq = omega * omega;
        const float cos_approx = 1.0f - (omega_sq * 0.5f);
        /* 16384 is the scaling factor for the Goertzel integer math. */
        coeff_int_ = static_cast<int32_t>(2.0f * cos_approx * 16384.0f);
    }

   private:
    void measure_frequency(int32_t sample) {
        const int32_t gate_threshold = (modulation_ == Modulation::FM) ? 4000 : 2000;

        if (sample > gate_threshold || sample < -gate_threshold) {
            if (sample > 0 && !meas_signal_state_high_) {
                /* Rising edge = end of a period. */
                meas_signal_state_high_ = true;

                if (meas_samples_in_period_ > 0) {
                    bool period_is_stable = false;
                    if (meas_last_period_len_ > 0) {
                        const int32_t diff = std::abs(static_cast<int>(meas_samples_in_period_) -
                                                      static_cast<int>(meas_last_period_len_));
                        if (diff <= 2) {
                            meas_consistency_count_++;
                            period_is_stable = true;
                        } else {
                            meas_consistency_count_ = 0;
                        }
                    }
                    meas_last_period_len_ = meas_samples_in_period_;

                    if (period_is_stable && meas_consistency_count_ > 5) {
                        const float base_rate = audio_rate_hz();
                        float inst_freq = base_rate / static_cast<float>(meas_samples_in_period_);
                        if (modulation_ == Modulation::DSB) inst_freq /= 2.0f;
                        if (inst_freq > 250 && inst_freq < 3000) {
                            meas_freq_accumulator_ += inst_freq;
                            meas_freq_count_++;
                        }
                    }
                }
                meas_samples_in_period_ = 0;
            } else if (sample < 0) {
                meas_signal_state_high_ = false;
            }
        } else {
            /* Silence: forget the running period. */
            if (meas_samples_in_period_ > 200) {
                meas_last_period_len_ = 0;
                meas_consistency_count_ = 0;
            }
        }

        meas_samples_in_period_++;
        ui_update_timer_++;

        const uint32_t update_limit = (modulation_ == Modulation::FM) ? 4800u : 2400u;  /* ~200 ms */
        if (ui_update_timer_ > update_limit) {
            if (meas_freq_count_ > 0) {
                const float avg_freq = meas_freq_accumulator_ / static_cast<float>(meas_freq_count_);
                current_freq_ = avg_freq;
                uint32_t stable_disp = static_cast<uint32_t>(avg_freq);
                stable_disp = (stable_disp / 5) * 5;  /* round to 5 Hz for display */
                measured_frequency_ = stable_disp;
                freq_updated_ = true;
            }
            meas_freq_accumulator_ = 0.0f;
            meas_freq_count_ = 0;
            ui_update_timer_ = 0;
        }
    }

    void process_decoding(int32_t sample, std::vector<int32_t>& out) {
        update_goertzel_coeff(current_freq_);

        /* 1. Goertzel recursion, Q14 coefficient. */
        const int64_t s = static_cast<int64_t>(sample) +
                          ((static_cast<int64_t>(coeff_int_) * s_prev_i_) >> 14) - s_prev2_i_;
        s_prev2_i_ = s_prev_i_;
        s_prev_i_ = static_cast<int32_t>(s);
        goertzel_count_++;

        /* 2. Evaluate every 60 samples (5 ms at 12 kHz). */
        if (goertzel_count_ >= 60) {
            if (startup_delay_ > 0) {
                startup_delay_--;
                /* Fast noise-floor learning phase. */
                const int64_t pwr =
                    static_cast<int64_t>(s_prev_i_) * s_prev_i_ +
                    static_cast<int64_t>(s_prev2_i_) * s_prev2_i_ -
                    ((static_cast<int64_t>(s_prev_i_) * s_prev2_i_ * coeff_int_) >> 14);
                noise_floor_ = (noise_floor_ * 15 + pwr) / 16;
            } else {
                const int64_t power =
                    static_cast<int64_t>(s_prev_i_) * s_prev_i_ +
                    static_cast<int64_t>(s_prev2_i_) * s_prev2_i_ -
                    ((static_cast<int64_t>(s_prev_i_) * s_prev2_i_ * coeff_int_) >> 14);

                if (!was_signaling_) noise_floor_ = (noise_floor_ * 127 + power) / 128;

                const int64_t sensitivity = 4 + (user_squelch_level_ / 10);
                const int64_t base_pwr_threshold = noise_floor_ * sensitivity;
                const int64_t current_pwr_threshold =
                    was_signaling_ ? (base_pwr_threshold / 2) : base_pwr_threshold;

                const bool is_tone =
                    squelch_is_open_ && (power > current_pwr_threshold) && (power > 150000);
                const int32_t time_base = 250;

                if (is_tone != was_signaling_) {
                    const int32_t duration_us =
                        static_cast<int32_t>(static_cast<int64_t>(duration_samples_) * time_base / 3);
                    if (duration_us > 10000 || was_signaling_)
                        out.push_back(was_signaling_ ? duration_us : -duration_us);
                    was_signaling_ = is_tone;
                    duration_samples_ = 0;
                }

                /* Long-silence timeout: 28800 samples = 2.4 s at 12 kHz. */
                if (!was_signaling_ && duration_samples_ > 28800) {
                    const int32_t duration_us =
                        static_cast<int32_t>(static_cast<int64_t>(duration_samples_) * time_base / 3);
                    out.push_back(-duration_us);
                    duration_samples_ = 0;
                }
            }

            duration_samples_ += 60;
            s_prev_i_ = 0;
            s_prev2_i_ = 0;
            goertzel_count_ = 0;
        }
    }

    Modulation modulation_{Modulation::AM};
    int32_t user_squelch_level_{0};
    bool squelch_is_open_{true};
    int32_t squelch_hold_{0};

    /* Tone-frequency measurement. */
    uint32_t meas_samples_in_period_{0};
    bool meas_signal_state_high_{false};
    uint32_t meas_last_period_len_{0};
    uint32_t meas_consistency_count_{0};
    float meas_freq_accumulator_{0.0f};
    uint32_t meas_freq_count_{0};
    uint32_t ui_update_timer_{0};
    float current_freq_{700.0f};
    uint32_t measured_frequency_{0};
    bool freq_updated_{false};

    /* Goertzel detector. */
    int32_t coeff_int_{0};
    int32_t s_prev_i_{0};
    int32_t s_prev2_i_{0};
    uint32_t goertzel_count_{0};
    uint32_t duration_samples_{0};
    bool was_signaling_{false};
    int64_t noise_floor_{5000};
    int32_t startup_delay_{20};

    float dc_average_{0.0f};
    bool clipped_{false};
};

}  // namespace morse

/* ======================================================================== *
 *  View                                                                     *
 * ======================================================================== */

class MorseRadioView : public ui::View {
   public:
    MorseRadioView();
    ~MorseRadioView() override;

    MorseRadioView(const MorseRadioView&) = delete;
    MorseRadioView& operator=(const MorseRadioView&) = delete;

    std::string title() const override { return "Morse"; }

    /* --- Read surface for the browser panel (provider_morse.cpp) ------------
     * All read on the UI thread, same thread write_char/on_frame_sync run on
     * (AppBridge::refresh() is UI-thread-only by contract), so no lock. */
    const std::string& decoded_text() const { return decoded_history_; }
    uint16_t decoded_wpm() const { return decoder_.wpm(); }
    uint32_t decoded_tone_hz() const { return last_tone_hz_; }
    bool receiving() const { return receiver_.running(); }

    void focus() override;
    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    /* Upstream's morse_modes, in the order the OptionsField shows them. */
    enum ModeIndex : int32_t {
        MORSE_AM_CW = 0,
        MORSE_NFM = 1,
        MORSE_AM_DSB = 2,
        MORSE_AM_USB = 3,
        MORSE_AM_LSB = 4,
    };

    void set_mode(int32_t mode);
    void rebuild_chain();
    void pump();
    void write_char(const std::string& ch, double confidence);
    void update_tone_readout(uint32_t hz);
    void check_for_timeout();

    radio::ReceiverModel& receiver_;

    morse::Decoder decoder_{};
    morse::SignalAccumulator accumulator_{};
    morse::ToneProcessor processor_{};

    int32_t mode_{MORSE_AM_CW};
    uint32_t frame_counter_{0};

    /* Plain decoded text (no colour markup) for the browser panel, bounded to
     * the most recent characters; the device's own console keeps the coloured
     * full history. Last locked tone, 0 when none. */
    std::string decoded_history_{};
    uint32_t last_tone_hz_{0};
    uint32_t quiet_frames_{0};

    /* Host channel chain. There is no channel tap on radio::ReceiverModel, so
     * this view mixes and filters the wideband spectrum tap down to the
     * detector's audio rate itself — see the comment in pump(). */
    double chain_input_rate_{0.0};
    double chain_offset_hz_{0.0};
    size_t chain_decimation_{1};
    double chain_audio_rate_{12000.0};
    bool chain_valid_{false};

    dsp::Nco nco_{};
    dsp::FirDecimateC channel_filter_{};
    dsp::AmDemod am_{};
    dsp::FmDemod fm_{};
    dsp::SsbDemod ssb_{};

    std::vector<dsp::cfloat> raw_{};
    std::vector<dsp::cfloat> mixed_{};
    std::vector<dsp::cfloat> channel_{};
    std::vector<float> audio_{};
    std::vector<int32_t> durations_{};

    /* --- Widgets --- */
    ui::Labels labels_{
        {{0, 16}, "Sql", ui::Color::light_grey()},
        {{88, 16}, "Mode", ui::Color::light_grey()},
        {{0, 32}, "Speed", ui::Color::light_grey()},
        {{72, 32}, "wpm", ui::Color::light_grey()},
        {{112, 32}, "Tone", ui::Color::light_grey()},
        {{192, 32}, "Hz", ui::Color::light_grey()},
        {{0, 48}, "Last", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{0, 0}};
    ui::NumberField field_gain_{{112, 0}, 3, {0, 76}, 1, ' '};
    ui::Text text_level_{{160, 0, 80, 16}, ""};

    ui::NumberField field_squelch_{{32, 16}, 2, {0, 99}, 1, ' '};
    ui::OptionsField options_mode_{{128, 16},
                                   6,
                                   {{"AM/CW ", MORSE_AM_CW},
                                    {"NFM   ", MORSE_NFM},
                                    {"AM/DSB", MORSE_AM_DSB},
                                    {"AM/USB", MORSE_AM_USB},
                                    {"AM/LSB", MORSE_AM_LSB}}};

    ui::Text text_speed_{{48, 32, 24, 16}, "??"};
    ui::Text text_tone_{{152, 32, 40, 16}, "??"};

    ui::Text text_last_{{40, 48, 200, 16}, ""};
    ui::Text text_clip_{{0, 64, 72, 16}, "clipping"};
    ui::Text text_tap_{{80, 64, 160, 16}, ""};

    ui::Button button_clear_{{0, 80, 48, 24}, "CLR"};
    ui::Checkbox check_log_{{64, 82}, 3, "Log", true};

    ui::Console console_{{0, 112, 240, 192}};
};

}  // namespace app

#endif /*__MB200_UI_MORSE_RADIO_H__*/
