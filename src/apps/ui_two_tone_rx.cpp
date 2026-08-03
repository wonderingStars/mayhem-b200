/*
 * mayhem-b200 — Two-Tone (Motorola Quik-Call II) sequential paging receiver.
 *
 * Copyright (C) 2024 PortaPack Mayhem (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_two_tone_rx.hpp"

#include "../core/string_format.hpp"
#include "../radio/receiver_model.hpp"
#include "app_context.hpp"
#include "ui_painter.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace app {
namespace two_tone {

/* --- Tables --------------------------------------------------------------- */

const uint32_t kMotoFreqsX10[kMotoToneCount] = {
    2885, 3047, 3217, 3396, 3586, 3786, 3998, 4221, 4457,
    4705, 4968, 5246, 5539, 5848, 6174, 6519, 6883, 7268,
    7674, 8102, 8555, 9032, 9537, 10073, 10642, 11225, 11247,
    11534, 11852, 11885, 12178, 12514, 12555, 12858, 13258, 13576,
    13950, 13996, 14768, 15579, 16430, 17325, 18262, 19245, 20275,
};

const uint32_t kCtcssFreqsX10[kCtcssOptionCount] = {
    0, 670, 719, 744, 770, 797, 825, 854, 885, 915,
    948, 974, 1000, 1035, 1072, 1109, 1148, 1188, 1230, 1273,
    1318, 1365, 1413, 1462, 1500, 1514, 1567, 1598, 1622, 1655,
    1679, 1713, 1738, 1773, 1799, 1835, 1862, 1899, 1928, 1966,
    1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503,
    2541,
};

std::string ctcss_name(const uint32_t freq_x10) {
    if (freq_x10 == 0) return "None";
    return to_string_dec_uint(freq_x10 / 10) + "." + to_string_dec_uint(freq_x10 % 10) + "Hz";
}

namespace {

uint32_t abs_diff_u32(const uint32_t a, const uint32_t b) {
    return (a > b) ? (a - b) : (b - a);
}

}  // namespace

uint32_t moto_index(const uint32_t freq_hz, const uint32_t match_hz) {
    if (freq_hz == 0) return kMotoNone;
    uint32_t best_idx = kMotoNone;
    uint32_t best_diff = match_hz + 1;
    for (size_t i = 0; i < kMotoToneCount; i++) {
        const uint32_t hz = kMotoFreqsX10[i] / 10;
        const uint32_t diff = abs_diff_u32(freq_hz, hz);
        if (diff < best_diff) {
            best_diff = diff;
            best_idx = static_cast<uint32_t>(i);
        }
    }
    return best_idx;
}

std::string format_tone(const uint32_t freq_hz, const uint32_t duration_ms) {
    const uint32_t idx = moto_index(freq_hz, kMotoFinalMatchHz);
    std::string freq_str;
    if (idx != kMotoNone) {
        const uint32_t matched_x10 = kMotoFreqsX10[idx];
        freq_str = to_string_dec_uint(matched_x10 / 10) + "." +
                   to_string_dec_uint(matched_x10 % 10) + "Hz";
    } else {
        freq_str = to_string_dec_uint(freq_hz) + "Hz";
    }
    return freq_str + " " + to_string_dec_uint(duration_ms) + "ms";
}

/* --- FmNoiseSquelch ------------------------------------------------------- */

/* common/dsp_iir_config.hpp non_audio_hpf_config:
 *   scipy.signal.iirdesign(wp=8000/24000, ws=4000/24000, gpass=1, gstop=18,
 *                          ftype='ellip')
 * Designed for a 24 kHz audio rate, which is the rate this app demodulates to.*/
constexpr float kHpfB[3] = {0.51891061f, -0.95714180f, 0.51891061f};
constexpr float kHpfA[3] = {1.00000000f, -0.79878302f, 0.43960231f};

void FmNoiseSquelch::reset() {
    x_ = {0.0f, 0.0f, 0.0f};
    y_ = {0.0f, 0.0f, 0.0f};
}

bool FmNoiseSquelch::execute(const float* audio, const size_t count) {
    if (threshold_squared_ == 0.0f) return true;

    /* "Non-audio" means "noise" here: find the loudest high-frequency sample in
     * the block, exactly as FMSquelch::execute does. */
    float peak_squared = 0.0f;
    for (size_t i = 0; i < count; i++) {
        x_[2] = x_[1];
        x_[1] = x_[0];
        x_[0] = audio[i];
        y_[2] = y_[1];
        y_[1] = y_[0];
        y_[0] = kHpfB[0] * x_[0] + kHpfB[1] * x_[1] + kHpfB[2] * x_[2] -
                kHpfA[1] * y_[1] - kHpfA[2] * y_[2];
        const float s2 = y_[0] * y_[0];
        if (s2 > peak_squared) peak_squared = s2;
    }
    return peak_squared < threshold_squared_;
}

/* --- ToneDetector --------------------------------------------------------- */

namespace {

/* Upstream's thresholds are quoted for a 960-sample window. Goertzel energy
 * grows with the square of the window length, so a host running at a different
 * audio rate scales them rather than silently changing sensitivity. */
constexpr float kMotoEnergyThresholdRef = 1000.0f;
constexpr float kCtcssEnergyThresholdRef = 30.0f;
constexpr size_t kReferenceWindowSamples = 960;
constexpr float kPiF = 3.14159265f;

}  // namespace

void ToneDetector::configure(const float audio_rate_hz, const uint32_t ctcss_freq_x10) {
    sample_rate_ = (audio_rate_hz > 0.0f) ? audio_rate_hz : 24000.0f;
    ctcss_freq_x10_ = ctcss_freq_x10;

    /* 40 ms of audio, the upstream WINDOW_SAMPLES at whatever rate we run. */
    window_samples_ = static_cast<size_t>(
        std::lround(static_cast<double>(sample_rate_) * kDetectWindowMs / 1000.0));
    if (window_samples_ < 16) window_samples_ = 16;
    window_ms_ = static_cast<uint32_t>((1000ULL * window_samples_) /
                                       static_cast<uint32_t>(sample_rate_ + 0.5f));
    if (window_ms_ == 0) window_ms_ = 1;

    const double scale = static_cast<double>(window_samples_) / kReferenceWindowSamples;
    moto_threshold_ = static_cast<float>(kMotoEnergyThresholdRef * scale * scale);
    ctcss_threshold_ = static_cast<float>(kCtcssEnergyThresholdRef * scale * scale);

    /* CTCSS uses the nearest DFT bin, as upstream does. */
    if (ctcss_freq_x10_ > 0) {
        const float freq = static_cast<float>(ctcss_freq_x10_) / 10.0f;
        const float k = std::round(static_cast<float>(window_samples_) * freq / sample_rate_);
        const float omega = 2.0f * kPiF * k / static_cast<float>(window_samples_);
        goertzel_coeff_ = 2.0f * std::cos(omega);
    } else {
        goertzel_coeff_ = 0.0f;
    }

    /* Each MOTO filter is tuned to the EXACT table frequency rather than to a
     * bin centre, so neighbouring entries never share a coefficient. */
    for (size_t i = 0; i < kMotoToneCount; i++) {
        const float freq = static_cast<float>(kMotoFreqsX10[i]) / 10.0f;
        const float omega = 2.0f * kPiF * freq / sample_rate_;
        moto_coeff_[i] = 2.0f * std::cos(omega);
    }

    reset();
}

void ToneDetector::reset() {
    goertzel_s1_ = 0.0f;
    goertzel_s2_ = 0.0f;
    moto_s1_.fill(0.0f);
    moto_s2_.fill(0.0f);
    last_energies_.fill(0.0f);
    window_sample_count_ = 0;
    was_detected_ = false;
    tone_duration_windows_ = 0;
}

void ToneDetector::close_window(std::vector<ToneWindow>& out) {
    /* Detection gate. A carrier is always required — without one the FM demod
     * outputs broadband noise that floods every bin. CTCSS mode adds a second
     * condition, the standard two-condition squelch of a real radio. */
    bool gate_open;
    if (ctcss_freq_x10_ > 0) {
        const float power = goertzel_s1_ * goertzel_s1_ + goertzel_s2_ * goertzel_s2_ -
                            goertzel_coeff_ * goertzel_s1_ * goertzel_s2_;
        gate_open = carrier_open_ && (power > ctcss_threshold_);
        goertzel_s1_ = 0.0f;
        goertzel_s2_ = 0.0f;
    } else {
        gate_open = carrier_open_;
    }

    /* Highest-energy MOTO entry this window; states reset regardless of the
     * gate so every window starts fresh. */
    uint32_t best_idx = kMotoToneCount;
    float best_energy = moto_threshold_;
    for (size_t j = 0; j < kMotoToneCount; j++) {
        const float pwr = moto_s1_[j] * moto_s1_[j] + moto_s2_[j] * moto_s2_[j] -
                          moto_coeff_[j] * moto_s1_[j] * moto_s2_[j];
        last_energies_[j] = pwr;
        moto_s1_[j] = 0.0f;
        moto_s2_[j] = 0.0f;
        if (pwr > best_energy) {
            best_energy = pwr;
            best_idx = static_cast<uint32_t>(j);
        }
    }

    /* Raw estimate from the local energy centroid around the winner; the
     * sequencer does the final table snap from the phase average. */
    uint32_t win_freq_hz = 0;
    if (best_idx < kMotoToneCount) {
        const size_t start = (best_idx > 0) ? (best_idx - 1) : best_idx;
        const size_t end = (best_idx + 1 < kMotoToneCount) ? (best_idx + 1) : best_idx;
        float weight_sum = 0.0f;
        float weighted_freq_x10 = 0.0f;
        for (size_t j = start; j <= end; j++) {
            const float weight = last_energies_[j] - moto_threshold_;
            if (weight > 0.0f) {
                weight_sum += weight;
                weighted_freq_x10 += weight * static_cast<float>(kMotoFreqsX10[j]);
            }
        }
        if (weight_sum > 0.0f) {
            win_freq_hz = static_cast<uint32_t>((weighted_freq_x10 / weight_sum) / 10.0f + 0.5f);
        } else {
            win_freq_hz = kMotoFreqsX10[best_idx] / 10;
        }
    }

    if (gate_open) {
        if (!was_detected_) tone_duration_windows_ = 0;
        tone_duration_windows_++;
        was_detected_ = true;
        out.push_back({win_freq_hz, tone_duration_windows_ * window_ms_, false});
    } else {
        if (was_detected_) {
            out.push_back({0, tone_duration_windows_ * window_ms_, true});
            tone_duration_windows_ = 0;
        }
        was_detected_ = false;
    }
}

void ToneDetector::process(const float* audio, const size_t count,
                           std::vector<ToneWindow>& out) {
    for (size_t i = 0; i < count; i++) {
        const float s = audio[i];

        if (ctcss_freq_x10_ > 0) {
            const float s0 = s + goertzel_coeff_ * goertzel_s1_ - goertzel_s2_;
            goertzel_s2_ = goertzel_s1_;
            goertzel_s1_ = s0;
        }

        for (size_t j = 0; j < kMotoToneCount; j++) {
            const float s0 = s + moto_coeff_[j] * moto_s1_[j] - moto_s2_[j];
            moto_s2_[j] = moto_s1_[j];
            moto_s1_[j] = s0;
        }

        if (++window_sample_count_ >= window_samples_) {
            window_sample_count_ = 0;
            close_window(out);
        }
    }
}

/* --- TwoToneSequencer ----------------------------------------------------- */

void TwoToneSequencer::reset() {
    state_ = State::Idle;
    phase_window_count_ = 0;
    phase_freq_accum_ = 0;
    phase_valid_windows_ = 0;
    phase_last_freq_ = 0;
    phase_last_is_first_ = true;
    t1_avg_freq_ = 0;
    t1_window_count_ = 0;
    t1_transition_candidate_windows_ = 0;
    t2_zero_window_count_ = 0;
}

bool TwoToneSequencer::finalize(const uint32_t t2_avg, const uint32_t t2_dur,
                                DetectedPair& out) {
    const uint32_t t1_dur = t1_window_count_ * window_ms_;
    if (t1_avg_freq_ > 0 && t2_avg > 0 &&
        t1_dur >= 500 && t2_dur >= 500 &&
        moto_index(t1_avg_freq_, kMotoFinalMatchHz) != kMotoNone &&
        moto_index(t2_avg, kMotoFinalMatchHz) != kMotoNone) {
        out = {t1_avg_freq_, t1_dur, t2_avg, t2_dur};
        return true;
    }
    return false;
}

bool TwoToneSequencer::feed(const ToneWindow& msg, DetectedPair& out) {
    if (msg.tone_end) {
        /* Gate closed — finalize whatever phase we are in. phase_last_freq_ is
         * the last window of the phase and is discarded. */
        bool logged = false;
        if (state_ == State::T2Collecting) {
            const uint32_t t2_avg = (phase_valid_windows_ > 0)
                                        ? (phase_freq_accum_ / phase_valid_windows_)
                                        : phase_last_freq_;
            const uint32_t t2_dur = phase_window_count_ * window_ms_;
            logged = finalize(t2_avg, t2_dur, out);
        }
        reset();
        status_.clear();
        return logged;
    }

    const uint32_t freq = msg.freq_hz;
    if (freq == 0) {
        if (state_ == State::T2Collecting) {
            const uint32_t t2_avg = (phase_valid_windows_ > 0)
                                        ? (phase_freq_accum_ / phase_valid_windows_)
                                        : phase_last_freq_;
            const uint32_t t2_dur = phase_window_count_ * window_ms_;

            if (t2_avg > 0 && t2_dur >= 2000) {
                const bool logged = finalize(t2_avg, t2_dur, out);
                reset();
                status_.clear();
                return logged;
            }
        }

        /* One weak window during B is tolerated so a standard 3 s B tone is not
         * lost to a single marginal estimate. */
        if (state_ == State::T2Collecting && t2_zero_window_count_ == 0) {
            t2_zero_window_count_++;
            status_ = "T1:" + format_tone(t1_avg_freq_, t1_window_count_ * window_ms_) + " T2 ?";
            return false;
        }

        reset();
        status_.clear();
        return false;
    }

    t2_zero_window_count_ = 0;

    switch (state_) {
        case State::Idle:
            state_ = State::T1Collecting;
            phase_window_count_ = 1;
            phase_freq_accum_ = 0;
            phase_valid_windows_ = 0;
            phase_last_freq_ = freq;
            phase_last_is_first_ = true;
            status_ = "T1 ...";
            break;

        case State::T1Collecting: {
            /* Skip when phase_last_freq_ is the first (noisy) window — it is
             * marked for discard and must not trigger a false transition. */
            if (phase_last_freq_ > 0 && !phase_last_is_first_) {
                const uint32_t t1_ref_freq = (phase_valid_windows_ > 0)
                                                 ? (phase_freq_accum_ / phase_valid_windows_)
                                                 : phase_last_freq_;
                const uint32_t idx_ref = moto_index(t1_ref_freq, kMotoFinalMatchHz);
                const uint32_t idx_last = moto_index(phase_last_freq_);
                const uint32_t idx_cur = moto_index(freq);
                const uint32_t idx_cur_final = moto_index(freq, kMotoFinalMatchHz);

                bool transition_detected = false;
                if (idx_last != kMotoNone && idx_cur != kMotoNone && idx_last != idx_cur) {
                    transition_detected = true;
                } else if (idx_ref != kMotoNone &&
                           idx_cur_final != kMotoNone &&
                           idx_cur_final != idx_ref &&
                           abs_diff_u32(freq, t1_ref_freq) >= kMotoTransitionDeltaHz) {
                    t1_transition_candidate_windows_++;
                    transition_detected = (t1_transition_candidate_windows_ >= 2);
                } else {
                    t1_transition_candidate_windows_ = 0;
                }

                if (transition_detected) {
                    t1_avg_freq_ = (phase_valid_windows_ > 0)
                                       ? (phase_freq_accum_ / phase_valid_windows_)
                                       : phase_last_freq_;
                    t1_window_count_ = phase_window_count_;

                    /* The transition window becomes B's first window, which is
                     * itself discarded. */
                    state_ = State::T2Collecting;
                    phase_window_count_ = 1;
                    phase_freq_accum_ = 0;
                    phase_valid_windows_ = 0;
                    phase_last_freq_ = freq;
                    phase_last_is_first_ = true;
                    t1_transition_candidate_windows_ = 0;
                    t2_zero_window_count_ = 0;

                    status_ = "T1:" + format_tone(t1_avg_freq_, t1_window_count_ * window_ms_) +
                              " T2...";
                    break;
                }
            }

            if (!phase_last_is_first_ && phase_last_freq_ > 0) {
                phase_freq_accum_ += phase_last_freq_;
                phase_valid_windows_++;
            }
            phase_last_freq_ = freq;
            phase_last_is_first_ = false;
            phase_window_count_++;

            status_ = "T1 " + to_string_dec_uint(phase_window_count_ * window_ms_) + "ms...";
            break;
        }

        case State::T2Collecting:
            if (!phase_last_is_first_ && phase_last_freq_ > 0) {
                phase_freq_accum_ += phase_last_freq_;
                phase_valid_windows_++;
            }
            phase_last_freq_ = freq;
            phase_last_is_first_ = false;
            phase_window_count_++;

            status_ = "T1:" + format_tone(t1_avg_freq_, t1_window_count_ * window_ms_) +
                      " T2 " + to_string_dec_uint(phase_window_count_ * window_ms_) + "ms";
            break;
    }

    return false;
}

}  // namespace two_tone

/* --- View ----------------------------------------------------------------- */

TwoToneRxView::TwoToneRxView()
    : receiver_{*globals().receiver} {
    add_children({&labels_,
                  &field_frequency_,
                  &step_view_,
                  &options_ctcss_,
                  &field_squelch_,
                  &button_startstop_,
                  &button_clear_,
                  &text_status_,
                  &text_tap_,
                  &log_view_,
                  &notes_});

    log_view_.set_parent_rect({0, 84, 240, 160});
    log_view_.on_draw = [](const TwoToneLogEntry& entry, const ui::Rect& target_rect,
                           ui::Painter& painter, const ui::Style& style,
                           ui::RecentEntriesColumns&) {
        std::string line = entry.line;
        const size_t width = static_cast<size_t>(target_rect.width() / 8);
        line.resize(width, ' ');
        painter.draw_string(target_rect.location(), style, line);
    };

    ui::OptionsField::options_t ctcss_options;
    ctcss_options.reserve(two_tone::kCtcssOptionCount);
    for (size_t i = 0; i < two_tone::kCtcssOptionCount; i++) {
        ctcss_options.push_back({two_tone::ctcss_name(two_tone::kCtcssFreqsX10[i]),
                                 static_cast<int32_t>(i)});
    }
    options_ctcss_.set_options(std::move(ctcss_options));
    options_ctcss_.set_by_value(static_cast<int32_t>(ctcss_idx_), false);
    options_ctcss_.on_change = [this](size_t, int32_t v) {
        ctcss_idx_ = static_cast<uint32_t>(v);
        detector_.configure(audio_rate_, two_tone::kCtcssFreqsX10[ctcss_idx_]);
        sequencer_.reset();
    };

    field_squelch_.set_value(static_cast<int32_t>(squelch_val_), false);
    field_squelch_.on_change = [this](int32_t v) {
        squelch_val_ = static_cast<uint32_t>(v);
        receiver_.set_squelch_level(static_cast<uint8_t>(squelch_val_));
    };

    button_startstop_.on_select = [this](ui::Button&) {
        if (running_)
            stop_rx();
        else
            start_rx();
    };

    button_clear_.on_select = [this](ui::Button&) {
        log_entries_.clear();
        log_view_.set_dirty();
        text_status_.set("");
        sequencer_.reset();
    };

    /* Upstream steps the frequency in 12.5 kHz channels. */
    field_frequency_.set_step_index(3);
    field_frequency_.set_value(receiver_.target_frequency(), false);
    field_frequency_.on_change = [this](uint64_t hz) { receiver_.set_target_frequency(hz); };
}

TwoToneRxView::~TwoToneRxView() {
    /* Nothing to tear down: the receiver is shared and the navigation layer
     * owns it. */
}

void TwoToneRxView::on_show() {
    ui::View::on_show();
    button_startstop_.focus();
}

void TwoToneRxView::on_hide() {
    if (running_) stop_rx();
    ui::View::on_hide();
}

void TwoToneRxView::start_rx() {
    receiver_.set_mode(radio::ReceiverModel::Mode::NarrowbandFMAudio);
    receiver_.set_nfm_configuration(radio::ReceiverModel::NfmConfig::Medium11k);
    receiver_.set_squelch_level(static_cast<uint8_t>(squelch_val_));
    if (!receiver_.running()) receiver_.start();

    /* CARRIER_DETECT_THRESHOLD from proc_tonedetect.cpp: fixed, independent of
     * the user squelch, so the gate still closes when the user opens audio. */
    carrier_squelch_.set_threshold(0.20f);
    carrier_squelch_.reset();

    rebuild_front_end();
    sequencer_.reset();

    running_ = true;
    button_startstop_.set_text("Stop");
    text_status_.set("");
}

void TwoToneRxView::stop_rx() {
    running_ = false;
    button_startstop_.set_text("Start");
    text_status_.set("");
    sequencer_.reset();
    detector_.reset();
}

void TwoToneRxView::rebuild_front_end() {
    const double input_rate = receiver_.sampling_rate();
    if (input_rate <= 0.0) return;

    /* Decimate as close to 24 kHz as an integer factor allows, then run the
     * detector at whatever rate that actually is — the Goertzel coefficients
     * are derived from the real rate, so no resampling is needed. */
    size_t decim = static_cast<size_t>(std::lround(input_rate / 24000.0));
    if (decim < 1) decim = 1;
    decimation_ = decim;
    audio_rate_ = static_cast<float>(input_rate / static_cast<double>(decim));

    /* An 11 kHz-ish NBFM channel: 5.5 kHz cutoff with a 2 kHz transition. */
    channel_filter_.configure(dsp::design_lowpass(5500.0, 2000.0, input_rate, 60.0), decim);
    fm_.configure(audio_rate_, 5000.0f);
    detector_.configure(audio_rate_, two_tone::kCtcssFreqsX10[ctcss_idx_]);
    sequencer_.set_window_ms(detector_.window_ms());
    configured_input_rate_ = input_rate;

    text_tap_.set("Audio " + to_string_dec_uint(static_cast<uint64_t>(audio_rate_)) +
                  " Hz  win " + to_string_dec_uint(detector_.window_ms()) + " ms");
}

void TwoToneRxView::log_pair(const two_tone::DetectedPair& pair) {
    TwoToneLogEntry entry{++next_log_serial_};
    entry.line = two_tone::format_tone(pair.t1_freq_hz, pair.t1_duration_ms) + " " +
                 two_tone::format_tone(pair.t2_freq_hz, pair.t2_duration_ms);
    /* Oldest first, so newer pairs appear below — upstream's ordering. */
    log_entries_.push_back(std::move(entry));
    ui::truncate_entries(log_entries_, 64);
    log_view_.set_dirty();
}

void TwoToneRxView::on_frame_sync() {
    ui::View::on_frame_sync();

    if (!running_) return;

    if (receiver_.sampling_rate() != configured_input_rate_) rebuild_front_end();

    /* SAMPLE TAP — the honest note.
     *
     * Upstream's ToneDetectProcessor is handed every baseband sample. The host
     * ReceiverModel exposes only take_spectrum_samples(), a 4096-sample block
     * of the most recent *wideband* input that is refreshed per DSP block and
     * cleared on read; consecutive reads are therefore not contiguous in time.
     * The decode pipeline below (channel lowpass -> FM demod -> Goertzel window
     * -> A/B state machine) is a faithful port and is correct when fed a
     * continuous stream, but with this tap a 40 ms window can straddle a gap
     * and a real pair may be missed.
     *
     * The tap that would make this exact: a channel-rate (post-channel-filter,
     * post-demodulator) ring buffer on ReceiverModel that a decoder drains
     * without loss — i.e. the host equivalent of the M4 handing every audio
     * sample to the processor. */
    if (!receiver_.take_spectrum_samples(samples_, 4096)) return;
    if (samples_.empty()) return;

    channel_filter_.process(samples_.data(), samples_.size(), channel_);
    if (channel_.empty()) return;

    fm_.process(channel_.data(), channel_.size(), audio_);
    if (audio_.empty()) return;

    detector_.set_carrier_open(carrier_squelch_.execute(audio_.data(), audio_.size()));

    windows_.clear();
    detector_.process(audio_.data(), audio_.size(), windows_);

    two_tone::DetectedPair pair{};
    for (const auto& w : windows_) {
        if (sequencer_.feed(w, pair)) log_pair(pair);
    }

    text_status_.set(sequencer_.status());
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream: menu_location RX, icon_color yellow. bitmaps.hpp has no pager-with-
 * receive-arrow glyph; POCSAG's pager icon is the closest honest match for a
 * sequential-paging decoder. */
const app::Registrar reg_two_tone_rx{{"two_tone_rx", "2-Tone RX", app::Category::Receive,
                                      ui::Color::yellow(), &ui::bitmap_icon_pocsag,
                                      [] { return std::make_unique<app::TwoToneRxView>(); }}};
}  // namespace
