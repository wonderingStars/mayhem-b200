/*
 * mayhem-b200 — Morse Practice (Games).
 *
 * Host port of PortaPack Mayhem's external/morse_practice app
 * (copyright 2025 Pezsma). The firmware timed a hardware key press with
 * chTimeNow() and measured dit/dah and gap lengths to decode Morse, adaptively
 * learning the operator's timing unit. On the host there is no TX key and no
 * baseband beep: the "straight key" is the Enter key, its down/up transitions
 * are sampled from the input layer in on_frame_sync(), the same ms-domain
 * timing feeds the decoder unchanged, and an optional sidetone is synthesised to
 * the audio output.
 *
 * The whole decode + scoring engine (MorseDecoder, the Morse table, the
 * confidence-to-colour scoring) lives here as pure, hardware-free logic in the
 * app::morse namespace so it can be unit tested against known keying attempts,
 * exactly as it ran upstream.
 *
 * Upstream deviations, all forced by the host toolchain / platform:
 *   - the two scratch arrays in the learner were C99 VLAs (a GCC extension MSVC
 *     rejects); they are fixed [40] arrays here, which is the ring buffer's
 *     capacity, so the behaviour is identical.
 *   - keying is sampled at the ~60 Hz frame tick instead of a hardware IRQ.
 *
 * Copyright (C) 2025 Pezsma (decoder + Morse table, GPL-2.0-or-later)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_MORSE_PRACTICE_H__
#define __MB200_UI_MORSE_PRACTICE_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* ---- pure Morse decode / scoring engine (tested in test_morse_practice.cpp) --
 *
 * Ported verbatim from firmware/application/external/morse_practice/
 * morsedecoder.hpp, keeping the same 40-slot ring buffers, adaptive timing
 * learner, thresholds and 50-entry Morse table. No firmware or UI dependency. */
namespace morse {

/* Fixed-capacity FIFO of the last 40 pulse or gap durations, oldest at [0]. */
class RingBuffer {
   public:
    static constexpr size_t capacity = 40;

    void push_back(uint32_t value) {
        data_[head_] = value;
        head_ = (head_ + 1) % capacity;
        if (count_ < capacity)
            count_++;
        else
            tail_ = (tail_ + 1) % capacity;  /* overwrite oldest */
    }

    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }

    uint32_t operator[](size_t idx) const {
        return data_[(tail_ + idx) % capacity];
    }

    void copy_to_array(uint32_t* out) const {
        for (size_t i = 0; i < count_; ++i)
            out[i] = (*this)[i];
    }

   private:
    uint32_t data_[capacity]{};
    size_t head_{0};
    size_t tail_{0};
    size_t count_{0};
};

class MorseDecoder {
   public:
    struct DecodeResult {
        std::string text{};
        double confidence{0.0};
        bool isValid() const { return !text.empty(); }
    };

    struct MorseEntry {
        std::string code;
        std::string letter;
    };

    MorseDecoder() = default;

    /* Feed one keying event: a positive duration is a key-down (pulse) in ms, a
     * negative duration is a key-up (gap) in ms. Returns a decoded character
     * once a gap of at least the inter-character threshold ends a sequence. */
    DecodeResult handleInput(int32_t duration_ms) {
        DecodeResult result{"", 0.0};

        if (duration_ms < 5 && duration_ms > -5) return result;

        if (duration_ms > 0) {
            pulse_history_.push_back(static_cast<uint32_t>(duration_ms));
            double dah_prob = getDahProbability(static_cast<uint32_t>(duration_ms));
            current_sequence_ += (dah_prob > 0.5) ? '-' : '.';
            last_confidence_ = (dah_prob > 0.5) ? dah_prob : (1.0 - dah_prob);
            last_sequence_ = current_sequence_;
        } else {
            uint32_t gap_duration = static_cast<uint32_t>(-duration_ms);
            pulse_gaps_.push_back(gap_duration);

            if (gap_duration >= getInterCharThreshold() && !current_sequence_.empty()) {
                result.text = lookupMorse(current_sequence_);
                result.confidence = (result.text[0] != '{') ? last_confidence_ : 0.0;
                if (gap_duration >= getInterWordThreshold()) {
                    result.text += " ";
                }
                current_sequence_ = "";
            }
        }

        updateLearning();
        return result;
    }

    /* Public wrappers over the Morse table, both directions, for testing and for
     * a reference chart. decode() returns "{seq}" for an unknown sequence, the
     * same sentinel the decoder emits on screen. encode() returns "" if there is
     * no code for the character. */
    std::string decode(const std::string& seq) const { return lookupMorse(seq); }

    std::string encode(char letter) const {
        std::string want(1, to_upper(letter));
        for (size_t i = 0; i < morse_table_size_; i++) {
            if (morse_table_[i].letter == want) return morse_table_[i].code;
        }
        return "";
    }

    double getInterElementThreshold() const { return time_unit_ms_ * 0.8; }
    double getInterCharThreshold() const { return time_unit_ms_ * 2.5; }
    double getInterWordThreshold() const { return time_unit_ms_ * 6.0; }

    double getCurrentTimeUnit() const { return time_unit_ms_; }
    std::string getLastSequence() const { return last_sequence_; }

   private:
    static char to_upper(char c) {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }

    std::string current_sequence_{};
    std::string last_sequence_{};
    double time_unit_ms_{119.0};
    double last_confidence_{0.0};
    RingBuffer pulse_history_{};
    RingBuffer pulse_gaps_{};

    std::string lookupMorse(const std::string& seq) const {
        for (size_t i = 0; i < morse_table_size_; i++) {
            if (seq == morse_table_[i].code) return morse_table_[i].letter;
        }
        return "{" + seq + "}";  /* not found */
    }

    double getDahProbability(uint32_t duration_ms) const {
        double start_interp = 1.5 * time_unit_ms_;
        double end_interp = 2.5 * time_unit_ms_;
        if (duration_ms <= start_interp) return 0.0;
        if (duration_ms >= end_interp) return 1.0;
        return (static_cast<double>(duration_ms) - start_interp) /
               (end_interp - start_interp);
    }

    size_t findDecisionBoundary(uint32_t* sorted_data, size_t sorted_data_size) const {
        if (sorted_data_size < 4) return 0;
        size_t best_split_index = 0;
        uint32_t max_diff = 0;
        for (size_t i = 1; i < sorted_data_size; ++i) {
            uint32_t diff = sorted_data[i] - sorted_data[i - 1];
            if (diff > sorted_data[i - 1] * 0.5 && diff > max_diff) {
                max_diff = diff;
                best_split_index = i;
            }
        }
        return best_split_index;
    }

    bool calculatePulseUnit(double& unit, double& confidence) const {
        if (pulse_history_.size() < 10) return false;
        uint32_t sorted_pulses[RingBuffer::capacity];
        pulse_history_.copy_to_array(sorted_pulses);
        sort_uint32(sorted_pulses, pulse_history_.size());

        size_t split_index = findDecisionBoundary(sorted_pulses, pulse_history_.size());
        if (split_index == 0 || split_index < 3 ||
            (pulse_history_.size() - split_index) < 2) {
            return false;
        }
        double dit_sum = sum_uint32_range(sorted_pulses, 0, split_index);
        double dah_sum = sum_uint32_range(sorted_pulses, split_index, pulse_history_.size());

        double avg_dit = dit_sum / split_index;
        double avg_dah = dah_sum / (pulse_history_.size() - split_index);
        if (avg_dah <= avg_dit) return false;

        double ratio = avg_dah / avg_dit;
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
        double threshold = getInterElementThreshold();
        double valid_gaps[RingBuffer::capacity];
        size_t valid_count = 0;
        for (size_t i = 0; i < pulse_gaps_.size(); i++) {
            double gap = pulse_gaps_[i];
            if (gap <= threshold) valid_gaps[valid_count++] = gap;
        }
        if (valid_count < 2) return false;
        double sum = sum_double_range(valid_gaps, 0, valid_count);
        if (valid_count > 0) {
            unit = sum / valid_count;
            confidence = 0.8;
            return true;
        }
        return false;
    }

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

    static void sort_uint32(uint32_t* data, size_t size) {
        if (size < 2) return;
        for (size_t i = 1; i < size; i++) {
            uint32_t key = data[i];
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

    void updateLearning() {
        double pulse_unit = -1.0, pulse_confidence = 0.0;
        double gap_unit = -1.0, gap_confidence = 0.0;

        bool pulse_success = calculatePulseUnit(pulse_unit, pulse_confidence);
        bool gap_success = calculateGapUnit(gap_unit, gap_confidence);

        double new_time_unit = -1.0;
        if (pulse_success && pulse_confidence > 0.5) {
            new_time_unit = pulse_unit;
        } else if (pulse_success && gap_success) {
            gap_confidence = 0.2;
            double total_confidence = pulse_confidence + gap_confidence;
            new_time_unit =
                (pulse_unit * pulse_confidence + gap_unit * gap_confidence) / total_confidence;
        } else if (gap_success) {
            new_time_unit = gap_unit;
        } else {
            return;
        }

        double max_change = time_unit_ms_ * 0.25;
        new_time_unit = clamp_double(new_time_unit, time_unit_ms_ - max_change,
                                     time_unit_ms_ + max_change);
        const double DEFAULT_TIME_UNIT = 160.0;
        const double BASE_LEARNING_RATE = 0.05;
        const double MAX_LEARNING_RATE = 0.25;
        double tudeltaabs = new_time_unit - DEFAULT_TIME_UNIT;
        if (tudeltaabs < 0) tudeltaabs *= -1;
        double deviation_from_default = tudeltaabs / DEFAULT_TIME_UNIT;
        double tpp = deviation_from_default * 2.0;
        if (tpp > 1) tpp = 1;
        double learning_factor =
            BASE_LEARNING_RATE + (MAX_LEARNING_RATE - BASE_LEARNING_RATE) * tpp;

        time_unit_ms_ =
            (time_unit_ms_ * (1.0 - learning_factor)) + (new_time_unit * learning_factor);
    }

    size_t morse_table_size_{50};
    MorseEntry morse_table_[50] = {
        {".-", "A"},     {"-...", "B"},   {"-.-.", "C"},   {"-..", "D"},
        {".", "E"},      {"..-.", "F"},   {"--.", "G"},    {"....", "H"},
        {"..", "I"},     {".---", "J"},   {"-.-", "K"},    {".-..", "L"},
        {"--", "M"},     {"-.", "N"},     {"---", "O"},    {".--.", "P"},
        {"--.-", "Q"},   {".-.", "R"},    {"...", "S"},    {"-", "T"},
        {"..-", "U"},    {"...-", "V"},   {".--", "W"},    {"-..-", "X"},
        {"-.--", "Y"},   {"--..", "Z"},   {".----", "1"},  {"..---", "2"},
        {"...--", "3"},  {"....-", "4"},  {".....", "5"},  {"-....", "6"},
        {"--...", "7"},  {"---..", "8"},  {"----.", "9"},  {"-----", "0"},
        {".-.-.-", "."}, {"..--..", "?"}, {"-.-.--", "!"}, {"--..--", ","},
        {"-...-", "="},  {"-..-.", "/"},  {".--.-.", "@"}, {"---...", ":"},
        {"-....-", "-"}, {".----.", "'"}, {".-..-.", "\""}, {"-.--.", "("},
        {"-.--.-", ")"}, {".-.-.", "+"}};
};

/* Scoring: which of the four console colours a decoded result is written in.
 * Ported from writeCharToConsole() — 0 white (space / no match), 1 red (weak,
 * confidence < 0.8), 2 yellow (confidence < 0.9), 3 green (confident). This is
 * the "score" the trainer gives each keyed character. */
inline uint8_t score_color_id(const std::string& text, double confidence) {
    if (text.empty()) return 0;
    if (text == " ") return 0;
    if (text[0] == '{') return 0;  /* no match */
    if (confidence < 0.8) return 1;
    if (confidence < 0.9) return 2;
    return 3;
}

}  // namespace morse

/* ---- the interactive view ------------------------------------------------- */

class MorsePracticeView : public ui::View {
   public:
    MorsePracticeView();

    std::string title() const override { return "Morse Practice"; }

    void focus() override;
    void on_show() override;
    void on_frame_sync() override;

   private:
    void on_press(uint32_t now);
    void on_release(uint32_t now);
    void write_result(const morse::MorseDecoder::DecodeResult& r);
    void write_char(const std::string& ch, double confidence);
    void update_sidetone(bool keyed, uint32_t now);

    morse::MorseDecoder decoder_{};

    /* Keying state, sampled from the input layer each frame. */
    bool key_down_{false};
    uint32_t start_time_{0};
    uint32_t end_time_{0};
    bool timing_gap_{false};

    /* Sidetone synthesis. */
    double phase_{0.0};
    double pending_samples_{0.0};
    uint32_t last_sync_ms_{0};
    std::vector<float> tone_buf_{};

    ui::Labels labels_{
        {{4, 4}, "Hold ENTER to key Morse", ui::Color::light_grey()},
        {{4, 72}, "Last:", ui::Color::light_grey()},
    };

    ui::Button button_key_{{40, 24, 160, 40}, "KEY"};
    ui::Text text_last_{{52, 72, 184, 16}, ""};
    ui::Console console_{{0, 96, 240, 176}};

    ui::Button button_clear_{{8, 276, 100, 24}, "Clear"};
    ui::Button button_back_{{132, 276, 100, 24}, "Back"};
};

}  // namespace app

#endif /*__MB200_UI_MORSE_PRACTICE_H__*/
