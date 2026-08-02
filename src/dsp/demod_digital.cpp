/*
 * mayhem-b200 — digital (data) demodulators.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "demod_digital.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kPiF = 3.14159265358979323846f;

double sinc(double x) {
    if (std::fabs(x) < 1e-12) return 1.0;
    return std::sin(kPi * x) / (kPi * x);
}

/* Gray code over 1 or 2 bits; for both widths the encode and decode maps are
 * the same function, v ^ (v >> 1). */
uint8_t gray(uint8_t v) {
    return static_cast<uint8_t>(v ^ (v >> 1));
}

size_t force_odd(size_t n) {
    return ((n % 2) == 0) ? (n + 1) : n;
}

}  // namespace

/* ===========================================================================
 * OOK / ASK
 * ===========================================================================*/

void OokSlicer::configure(float samples_per_symbol) {
    if (!(samples_per_symbol > 0.0f)) samples_per_symbol = 1.0f;

    /* Upstream: factor_sq(-1 / (8 * sps)) with factor_sq(db) = 10^(db/5).
     * The threshold therefore loses 1/8 dB of power per symbol period. */
    const double db = -1.0 / (8.0 * static_cast<double>(samples_per_symbol));
    leak_ = static_cast<float>(std::pow(10.0, db / 5.0));
    threshold_ = 0.0f;
}

void OokSlicer::reset() {
    threshold_ = 0.0f;
}

bool OokSlicer::process_mag2(float mag2) {
    /* Upstream attenuates by >> 3, described there as "approximation of
     * (-4.5 dB)^2" — one eighth of the power. */
    const float mag2_attenuated = mag2 * 0.125f;
    threshold_ = std::max(threshold_ * leak_, mag2_attenuated);
    return mag2 > threshold_;
}

bool OokSlicer::process(cfloat sample) {
    const float mag2 = sample.real() * sample.real() + sample.imag() * sample.imag();
    return process_mag2(mag2);
}

void OokDemod::configure(float sample_rate_hz, float symbol_rate_hz) {
    if (!(sample_rate_hz > 0.0f) || !(symbol_rate_hz > 0.0f)) {
        configured_ = false;
        return;
    }

    samples_per_symbol_ = sample_rate_hz / symbol_rate_hz;
    if (samples_per_symbol_ < 2.0f) samples_per_symbol_ = 2.0f;

    slicer_.configure(samples_per_symbol_);

    /* OOKClockRecovery: a Q32 phase accumulator ticking once per symbol, with
     * the early-late gate's error scaled by 2/8 of a symbol per unit. */
    const double nominal =
        std::round(4294967296.0 / static_cast<double>(samples_per_symbol_));
    inc_nominal_ = static_cast<uint32_t>(std::min(nominal, 4294967295.0));
    inc_k_ = static_cast<uint32_t>(
        std::round(static_cast<double>(inc_nominal_) * (2.0 / 8.0) /
                   static_cast<double>(samples_per_symbol_)));

    /* The gate's history is 32 bits wide and it uses 2 * (sps/2) of them. */
    size_t gate_sps = static_cast<size_t>(std::lround(samples_per_symbol_));
    gate_sps = std::clamp<size_t>(gate_sps, 2, 32);
    detector_ = PhaseDetectorEarlyLateGate{gate_sps};

    configured_ = true;
    reset();
}

void OokDemod::reset() {
    slicer_.reset();
    accumulator_.reset();
    accumulator_.set_inc(inc_nominal_);
    slicer_history_ = 0;
}

void OokDemod::feed_slice(bool sliced, std::vector<uint8_t>& out) {
    slicer_history_ = (slicer_history_ << 1) | (sliced ? 1u : 0u);

    if (accumulator_()) {
        const auto result = detector_(slicer_history_);
        /* Unsigned wrap on a negative error is upstream's behaviour and is
         * what makes a "too late" gate slow the accumulator down. */
        accumulator_.set_inc(inc_nominal_ +
                             static_cast<uint32_t>(result.error) * inc_k_);
        out.push_back(result.symbol ? uint8_t{1} : uint8_t{0});
    }
}

void OokDemod::process(const cfloat* in, size_t count, std::vector<uint8_t>& out) {
    if (!configured_ || in == nullptr) return;
    for (size_t i = 0; i < count; i++) feed_slice(slicer_.process(in[i]), out);
}

void OokDemod::process_mag2(const float* mag2, size_t count, std::vector<uint8_t>& out) {
    if (!configured_ || mag2 == nullptr) return;
    for (size_t i = 0; i < count; i++) feed_slice(slicer_.process_mag2(mag2[i]), out);
}

/* ===========================================================================
 * 2FSK / GFSK
 * ===========================================================================*/

void FskDemod::configure(float sample_rate_hz, float symbol_rate_hz, float deviation_hz) {
    if (!(sample_rate_hz > 0.0f) || !(symbol_rate_hz > 0.0f)) {
        configured_ = false;
        return;
    }

    sample_rate_hz_ = sample_rate_hz;
    symbol_rate_hz_ = symbol_rate_hz;
    deviation_hz_ = (deviation_hz > 0.0f) ? deviation_hz : (symbol_rate_hz * 0.5f);

    const float sps = sample_rate_hz / symbol_rate_hz;

    /* Smooth the discriminator before the timing loop; see the member's note. */
    size_t taps = static_cast<size_t>(std::lround(2.0f * sps)) + 1;
    taps = force_odd(std::clamp<size_t>(taps, 9, 129));
    post_filter_.configure(
        design_lowpass_fixed(0.7 * static_cast<double>(symbol_rate_hz),
                             static_cast<double>(sample_rate_hz), taps));

    /* Peak followers relax by this fraction of the current range per sample,
     * i.e. a time constant of ~64 symbols. */
    peak_decay_ = 1.0f / std::max(1.0f, sps * 64.0f);

    clock_recovery_.configure(sample_rate_hz, symbol_rate_hz, DeadbandErrorFilter{1.0f / 16.0f});
    clock_recovery_.set_symbol_handler([this](const float symbol) { this->on_symbol(symbol); });

    configured_ = true;
    reset();
}

void FskDemod::reset() {
    clock_recovery_.reset();
    post_filter_.reset();
    prev_ = cfloat{0.0f, 0.0f};
    last_freq_hz_ = 0.0f;
    peak_max_ = 0.0f;
    peak_min_ = 0.0f;
    primed_ = false;
}

void FskDemod::on_symbol(float symbol) {
    if (out_ == nullptr) return;
    const bool bit = (symbol >= 0.0f) != invert_;
    out_->push_back(bit ? uint8_t{1} : uint8_t{0});
}

void FskDemod::process(const cfloat* in, size_t count, std::vector<uint8_t>& out) {
    if (!configured_ || in == nullptr) return;

    out_ = &out;
    const float hz_per_radian = sample_rate_hz_ / (2.0f * kPiF);

    for (size_t i = 0; i < count; i++) {
        if (!primed_) {
            prev_ = in[i];
            primed_ = true;
            continue;
        }

        /* arg(current * conj(previous)) is the phase advanced in one sample. */
        const cfloat d = in[i] * std::conj(prev_);
        prev_ = in[i];
        const float freq_hz = std::atan2(d.imag(), d.real()) * hz_per_radian;
        last_freq_hz_ = freq_hz;

        const float smoothed = post_filter_.process_one(freq_hz);

        const float range = peak_max_ - peak_min_;
        if (smoothed > peak_max_)
            peak_max_ = smoothed;
        else
            peak_max_ -= range * peak_decay_;
        if (smoothed < peak_min_)
            peak_min_ = smoothed;
        else
            peak_min_ += range * peak_decay_;

        const float centre = 0.5f * (peak_max_ + peak_min_);
        clock_recovery_((smoothed - centre) / std::max(deviation_hz_, 1e-6f));
    }

    out_ = nullptr;
}

/* ===========================================================================
 * AFSK
 * ===========================================================================*/

void AfskDemod::configure(float audio_rate_hz, Standard standard) {
    switch (standard) {
        case Standard::Bell103Originate:
            configure(audio_rate_hz, 1270.0f, 1070.0f, 300.0f);
            break;
        case Standard::Bell103Answer:
            configure(audio_rate_hz, 2225.0f, 2025.0f, 300.0f);
            break;
        case Standard::Bell202:
        default:
            configure(audio_rate_hz, 1200.0f, 2200.0f, 1200.0f);
            break;
    }
}

void AfskDemod::configure(float audio_rate_hz, float mark_hz, float space_hz, float baud) {
    if (!(audio_rate_hz > 0.0f) || !(baud > 0.0f)) {
        configured_ = false;
        return;
    }

    audio_rate_hz_ = audio_rate_hz;
    mark_hz_ = mark_hz;
    space_hz_ = space_hz;
    baud_ = baud;
    samples_per_bit_ = audio_rate_hz / baud;

    /* Delay-and-multiply: after the lowpass, a tone at f settles at
     * cos(2*pi*f*D/fs). Pick the D that pushes mark and space furthest apart.
     * Upstream hard-codes D = samples_per_bit/2, which is near-optimal for
     * Bell 202 at 24 kHz and useless for Bell 103's 200 Hz spacing. */
    const size_t max_delay =
        std::max<size_t>(1, static_cast<size_t>(std::floor(samples_per_bit_)));
    double best_separation = -1.0;
    size_t best_delay = 1;
    double best_mark = 0.0;
    double best_space = 0.0;

    for (size_t d = 1; d <= max_delay; d++) {
        const double wm = 2.0 * kPi * static_cast<double>(mark_hz) *
                          static_cast<double>(d) / static_cast<double>(audio_rate_hz);
        const double ws = 2.0 * kPi * static_cast<double>(space_hz) *
                          static_cast<double>(d) / static_cast<double>(audio_rate_hz);
        const double cm = std::cos(wm);
        const double cs = std::cos(ws);
        const double separation = std::fabs(cm - cs);
        if (separation > best_separation) {
            best_separation = separation;
            best_delay = d;
            best_mark = cm;
            best_space = cs;
        }
    }

    delay_ = best_delay;
    delay_line_.assign(delay_, 0.0f);
    delay_pos_ = 0;

    /* Arrange for a mark tone to always end up on the negative side, which is
     * the sense upstream's `sample_filtered < -20` slicer assumes. */
    polarity_ = (best_mark < best_space) ? 1.0f : -1.0f;

    /* Remove the sum-frequency product of the multiply. Upstream's two-tap
     * plus one-pole does not do this for arbitrary tone pairs. */
    size_t taps = static_cast<size_t>(std::lround(4.0f * samples_per_bit_)) + 1;
    taps = force_odd(std::clamp<size_t>(taps, 15, 255));
    lowpass_.configure(design_lowpass_fixed(static_cast<double>(baud),
                                            static_cast<double>(audio_rate_hz), taps));

    peak_decay_ = 1.0f / std::max(1.0f, samples_per_bit_ * 32.0f);

    /* 16-bit bit-clock accumulator, upstream proc_aprsrx. */
    phase_inc_ = static_cast<uint32_t>(std::lround(65536.0 * static_cast<double>(baud) /
                                                   static_cast<double>(audio_rate_hz)));
    if (phase_inc_ == 0) phase_inc_ = 1;

    configured_ = true;
    reset();
}

void AfskDemod::reset() {
    std::fill(delay_line_.begin(), delay_line_.end(), 0.0f);
    delay_pos_ = 0;
    lowpass_.reset();
    peak_max_ = 0.0f;
    peak_min_ = 0.0f;
    sample_bits_ = 0;
    phase_ = 0;
    prev_ = cfloat{0.0f, 0.0f};
    primed_ = false;
}

void AfskDemod::slice_and_clock(float filtered, std::vector<uint8_t>& out) {
    const float range = peak_max_ - peak_min_;
    if (filtered > peak_max_)
        peak_max_ = filtered;
    else
        peak_max_ -= range * peak_decay_;
    if (filtered < peak_min_)
        peak_min_ = filtered;
    else
        peak_min_ += range * peak_decay_;

    const float centre = 0.5f * (peak_max_ + peak_min_);
    const float value = polarity_ * (filtered - centre);

    sample_bits_ = (sample_bits_ << 1) | ((value < 0.0f) ? 1u : 0u);

    /* A clean transition is 0011 or 1100 in the last four slicer outputs; pull
     * the sampling phase towards half a bit after it. */
    if ((((sample_bits_ >> 2) ^ sample_bits_) & 3u) == 3u) {
        if (phase_ < 0x8000u)
            phase_ += 0x800u;
        else
            phase_ -= 0x800u;
    }

    phase_ += phase_inc_;
    if (phase_ >= 0x10000u) {
        phase_ &= 0xFFFFu;
        const int ones = std::popcount(sample_bits_ & 0xFFu);
        out.push_back((ones >= 5) ? uint8_t{1} : uint8_t{0});
    }
}

void AfskDemod::process_audio(const float* in, size_t count, std::vector<uint8_t>& out) {
    if (!configured_ || in == nullptr || delay_line_.empty()) return;

    for (size_t i = 0; i < count; i++) {
        const float x = in[i];
        const float delayed = delay_line_[delay_pos_];
        delay_line_[delay_pos_] = x;
        delay_pos_ = (delay_pos_ + 1) % delay_line_.size();

        slice_and_clock(lowpass_.process_one(x * delayed), out);
    }
}

void AfskDemod::process(const cfloat* in, size_t count, std::vector<uint8_t>& out) {
    if (!configured_ || in == nullptr) return;

    /* FM-demodulate to audio first: AFSK rides on an FM carrier. The absolute
     * scale is irrelevant — the correlator is quadratic in it and the slicer
     * tracks its own level. */
    for (size_t i = 0; i < count; i++) {
        if (!primed_) {
            prev_ = in[i];
            primed_ = true;
            continue;
        }
        const cfloat d = in[i] * std::conj(prev_);
        prev_ = in[i];
        const float audio = std::atan2(d.imag(), d.real());

        const float delayed = delay_line_[delay_pos_];
        delay_line_[delay_pos_] = audio;
        delay_pos_ = (delay_pos_ + 1) % delay_line_.size();

        slice_and_clock(lowpass_.process_one(audio * delayed), out);
    }
}

/* ===========================================================================
 * BPSK / QPSK
 * ===========================================================================*/

void PskDemod::configure(float sample_rate_hz, float symbol_rate_hz, PskOrder order) {
    if (!(sample_rate_hz > 0.0f) || !(symbol_rate_hz > 0.0f)) {
        configured_ = false;
        return;
    }

    sample_rate_hz_ = sample_rate_hz;
    symbol_rate_hz_ = symbol_rate_hz;
    order_ = order;

    /* 1/8 rather than the 1/16 the FSK path uses: QPSK's decision regions are
     * 90 degrees wide instead of 180, so a mistimed symbol also corrupts the
     * carrier loop's error, and the timing loop has to clear a half-symbol
     * offset inside the preamble. Measured over timing offsets 0..9 samples at
     * 10 samples per symbol, 1/32 leaves QPSK unable to acquire from offsets
     * near half a symbol; 1/8 acquires from every offset. */
    clock_recovery_.configure(sample_rate_hz, symbol_rate_hz, DeadbandErrorFilter{1.0f / 8.0f});
    clock_recovery_.set_symbol_handler([this](const cfloat symbol) { this->on_symbol(symbol); });

    const size_t sps = std::max<size_t>(
        2, static_cast<size_t>(std::lround(sample_rate_hz / symbol_rate_hz)));
    set_receive_filter(design_root_raised_cosine(0.35, sps, 6));

    configured_ = true;
    reset();
}

void PskDemod::set_carrier_loop_gains(float alpha, float beta) {
    alpha_ = alpha;
    beta_ = beta;
}

void PskDemod::set_timing_loop_gain(float weight, float deadband) {
    clock_recovery_.configure(sample_rate_hz_, symbol_rate_hz_,
                              DeadbandErrorFilter{weight, deadband});
    clock_recovery_.set_symbol_handler([this](const cfloat symbol) { this->on_symbol(symbol); });
}

void PskDemod::set_receive_filter(std::vector<float> taps) {
    filter_enabled_ = !taps.empty();
    if (filter_enabled_) receive_filter_.configure(std::move(taps), 1);
}

void PskDemod::reset() {
    clock_recovery_.reset();
    receive_filter_.reset();
    filtered_.clear();
    phase_ = 0.0f;
    frequency_ = 0.0f;
    magnitude_ = 1.0f;
    prev_index_ = 0;
    have_prev_ = false;
}

float PskDemod::carrier_frequency_offset_hz() const {
    /* frequency_ is radians of phase advance per symbol. */
    return frequency_ * symbol_rate_hz_ / (2.0f * kPiF);
}

void PskDemod::on_symbol(cfloat symbol) {
    const cfloat derotated = symbol * std::polar(1.0f, -phase_);

    /* Slow AGC so the decision points sit on the unit circle. */
    const float mag = std::abs(derotated);
    magnitude_ += 0.05f * (mag - magnitude_);
    const float scale = (magnitude_ > 1e-9f) ? (1.0f / magnitude_) : 1.0f;
    const cfloat y = derotated * scale;

    uint8_t index = 0;
    cfloat decision{1.0f, 0.0f};
    if (order_ == PskOrder::Bpsk) {
        index = (y.real() >= 0.0f) ? uint8_t{0} : uint8_t{1};
        decision = (index == 0) ? cfloat{1.0f, 0.0f} : cfloat{-1.0f, 0.0f};
    } else {
        /* exp(j*(pi/4 + k*pi/2)): k = 0 (+,+), 1 (-,+), 2 (-,-), 3 (+,-). */
        const bool re_neg = (y.real() < 0.0f);
        const bool im_neg = (y.imag() < 0.0f);
        if (!re_neg && !im_neg)
            index = 0;
        else if (re_neg && !im_neg)
            index = 1;
        else if (re_neg && im_neg)
            index = 2;
        else
            index = 3;
        decision = std::polar(1.0f, kPiF * 0.25f + static_cast<float>(index) * kPiF * 0.5f);
    }

    /* Decision-directed phase error, in radians. */
    const cfloat error_vector = y * std::conj(decision);
    const float error = std::atan2(error_vector.imag(), error_vector.real());

    frequency_ += beta_ * error;
    phase_ += frequency_ + alpha_ * error;
    while (phase_ > kPiF) phase_ -= 2.0f * kPiF;
    while (phase_ < -kPiF) phase_ += 2.0f * kPiF;

    if (out_ == nullptr) return;

    const uint8_t bits_per_symbol = static_cast<uint8_t>(order_);
    const uint8_t modulus = static_cast<uint8_t>(1u << bits_per_symbol);

    uint8_t value = 0;
    if (differential_) {
        if (!have_prev_) {
            /* The first symbol is the phase reference and carries no data. */
            have_prev_ = true;
            prev_index_ = index;
            return;
        }
        const uint8_t difference =
            static_cast<uint8_t>((index + modulus - prev_index_) % modulus);
        prev_index_ = index;
        value = gray(difference);
    } else {
        value = gray(index);
    }

    for (uint8_t b = bits_per_symbol; b > 0; b--) {
        out_->push_back(static_cast<uint8_t>((value >> (b - 1)) & 1u));
    }
}

void PskDemod::process(const cfloat* in, size_t count, std::vector<uint8_t>& out) {
    if (!configured_ || in == nullptr) return;

    out_ = &out;
    if (filter_enabled_) {
        filtered_.clear();
        receive_filter_.process(in, count, filtered_);
        for (const cfloat s : filtered_) clock_recovery_(s);
    } else {
        for (size_t i = 0; i < count; i++) clock_recovery_(in[i]);
    }
    out_ = nullptr;
}

/* ===========================================================================
 * Pulse shaping
 * ===========================================================================*/

std::vector<float> design_raised_cosine(double rolloff, size_t sps, size_t span_symbols) {
    if (sps == 0) sps = 1;
    if (span_symbols == 0) span_symbols = 1;
    rolloff = std::clamp(rolloff, 0.0, 1.0);

    const size_t n = 2 * span_symbols * sps + 1;
    const double centre = static_cast<double>(n - 1) / 2.0;

    std::vector<float> taps(n);
    double peak = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double t = (static_cast<double>(i) - centre) / static_cast<double>(sps);
        double h;
        const double denom = 1.0 - (2.0 * rolloff * t) * (2.0 * rolloff * t);
        if (rolloff > 1e-9 && std::fabs(denom) < 1e-9) {
            h = (kPi / 4.0) * sinc(1.0 / (2.0 * rolloff));
        } else {
            h = sinc(t) * std::cos(kPi * rolloff * t) / denom;
        }
        taps[i] = static_cast<float>(h);
        peak = std::max(peak, std::fabs(h));
    }

    if (peak > 1e-12) {
        for (auto& v : taps) v = static_cast<float>(v / peak);
    }
    return taps;
}

std::vector<float> design_root_raised_cosine(double rolloff, size_t sps, size_t span_symbols) {
    if (sps == 0) sps = 1;
    if (span_symbols == 0) span_symbols = 1;
    rolloff = std::clamp(rolloff, 0.0, 1.0);

    if (rolloff <= 1e-9) return design_raised_cosine(0.0, sps, span_symbols);

    const size_t n = 2 * span_symbols * sps + 1;
    const double centre = static_cast<double>(n - 1) / 2.0;
    const double quarter = 1.0 / (4.0 * rolloff);

    std::vector<float> taps(n);
    double peak = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double t = (static_cast<double>(i) - centre) / static_cast<double>(sps);
        double h;
        if (std::fabs(t) < 1e-9) {
            h = 1.0 - rolloff + 4.0 * rolloff / kPi;
        } else if (std::fabs(std::fabs(t) - quarter) < 1e-9) {
            h = (rolloff / std::sqrt(2.0)) *
                ((1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * rolloff)) +
                 (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * rolloff)));
        } else {
            const double numerator = std::sin(kPi * t * (1.0 - rolloff)) +
                                     4.0 * rolloff * t * std::cos(kPi * t * (1.0 + rolloff));
            const double denominator =
                kPi * t * (1.0 - (4.0 * rolloff * t) * (4.0 * rolloff * t));
            h = numerator / denominator;
        }
        taps[i] = static_cast<float>(h);
        peak = std::max(peak, std::fabs(h));
    }

    if (peak > 1e-12) {
        for (auto& v : taps) v = static_cast<float>(v / peak);
    }
    return taps;
}

/* ===========================================================================
 * Modulators
 * ===========================================================================*/

std::vector<cfloat> ook_modulate(const std::vector<uint8_t>& bits,
                                 float sample_rate_hz,
                                 float symbol_rate_hz,
                                 float amplitude) {
    std::vector<cfloat> out;
    if (bits.empty() || !(sample_rate_hz > 0.0f) || !(symbol_rate_hz > 0.0f)) return out;

    const double sps = static_cast<double>(sample_rate_hz) / static_cast<double>(symbol_rate_hz);
    const size_t total = static_cast<size_t>(std::llround(sps * static_cast<double>(bits.size())));
    out.resize(total, cfloat{0.0f, 0.0f});

    for (size_t i = 0; i < total; i++) {
        const size_t symbol = static_cast<size_t>(static_cast<double>(i) / sps);
        if (symbol < bits.size() && (bits[symbol] & 1)) out[i] = cfloat{amplitude, 0.0f};
    }
    return out;
}

std::vector<cfloat> fsk_modulate(const std::vector<uint8_t>& bits,
                                 float sample_rate_hz,
                                 float symbol_rate_hz,
                                 float deviation_hz,
                                 float gaussian_bt) {
    std::vector<cfloat> out;
    if (bits.empty() || !(sample_rate_hz > 0.0f) || !(symbol_rate_hz > 0.0f)) return out;

    const double sps = static_cast<double>(sample_rate_hz) / static_cast<double>(symbol_rate_hz);
    const size_t total = static_cast<size_t>(std::llround(sps * static_cast<double>(bits.size())));

    /* Instantaneous frequency, one value per sample. */
    std::vector<double> freq(total, 0.0);
    for (size_t i = 0; i < total; i++) {
        const size_t symbol = static_cast<size_t>(static_cast<double>(i) / sps);
        const bool one = (symbol < bits.size()) && ((bits[symbol] & 1) != 0);
        freq[i] = one ? static_cast<double>(deviation_hz) : -static_cast<double>(deviation_hz);
    }

    if (gaussian_bt > 0.0f) {
        /* Gaussian frequency pulse; sigma in samples from the BT product. */
        const double sigma =
            std::sqrt(std::log(2.0)) / (2.0 * kPi * static_cast<double>(gaussian_bt)) * sps;
        const size_t half = std::max<size_t>(1, static_cast<size_t>(std::ceil(3.0 * sigma)));

        std::vector<double> kernel(2 * half + 1);
        double sum = 0.0;
        for (size_t i = 0; i < kernel.size(); i++) {
            const double t = static_cast<double>(i) - static_cast<double>(half);
            kernel[i] = std::exp(-(t * t) / (2.0 * sigma * sigma));
            sum += kernel[i];
        }
        for (auto& k : kernel) k /= sum;

        std::vector<double> shaped(total, 0.0);
        for (size_t i = 0; i < total; i++) {
            double acc = 0.0;
            for (size_t j = 0; j < kernel.size(); j++) {
                const std::ptrdiff_t idx =
                    static_cast<std::ptrdiff_t>(i + j) - static_cast<std::ptrdiff_t>(half);
                const size_t clamped = static_cast<size_t>(
                    std::clamp<std::ptrdiff_t>(idx, 0, static_cast<std::ptrdiff_t>(total) - 1));
                acc += kernel[j] * freq[clamped];
            }
            shaped[i] = acc;
        }
        freq.swap(shaped);
    }

    out.resize(total);
    double phase = 0.0;
    for (size_t i = 0; i < total; i++) {
        out[i] = cfloat{static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
        phase += 2.0 * kPi * freq[i] / static_cast<double>(sample_rate_hz);
        if (phase > kPi) phase -= 2.0 * kPi;
        if (phase < -kPi) phase += 2.0 * kPi;
    }
    return out;
}

std::vector<float> afsk_modulate(const std::vector<uint8_t>& bits,
                                 float sample_rate_hz,
                                 float mark_hz,
                                 float space_hz,
                                 float baud,
                                 float amplitude) {
    std::vector<float> out;
    if (bits.empty() || !(sample_rate_hz > 0.0f) || !(baud > 0.0f)) return out;

    const double sps = static_cast<double>(sample_rate_hz) / static_cast<double>(baud);
    const size_t total = static_cast<size_t>(std::llround(sps * static_cast<double>(bits.size())));

    out.resize(total);
    double phase = 0.0;
    for (size_t i = 0; i < total; i++) {
        const size_t symbol = static_cast<size_t>(static_cast<double>(i) / sps);
        const bool one = (symbol < bits.size()) && ((bits[symbol] & 1) != 0);
        const double f = one ? static_cast<double>(mark_hz) : static_cast<double>(space_hz);

        out[i] = static_cast<float>(static_cast<double>(amplitude) * std::sin(phase));
        phase += 2.0 * kPi * f / static_cast<double>(sample_rate_hz);
        if (phase > kPi) phase -= 2.0 * kPi;
    }
    return out;
}

std::vector<cfloat> psk_modulate(const std::vector<uint8_t>& bits,
                                 float sample_rate_hz,
                                 float symbol_rate_hz,
                                 PskOrder order,
                                 bool differential,
                                 double rolloff,
                                 size_t span_symbols) {
    std::vector<cfloat> out;
    if (bits.empty() || !(sample_rate_hz > 0.0f) || !(symbol_rate_hz > 0.0f)) return out;

    const size_t sps = std::max<size_t>(
        2, static_cast<size_t>(std::lround(sample_rate_hz / symbol_rate_hz)));
    const uint8_t bits_per_symbol = static_cast<uint8_t>(order);
    const uint8_t modulus = static_cast<uint8_t>(1u << bits_per_symbol);

    /* Constellation indices, most-significant bit of each symbol first. */
    std::vector<uint8_t> indices;
    indices.reserve(bits.size() / bits_per_symbol + 2);

    uint8_t previous = 0;
    if (differential) indices.push_back(previous);  /* phase reference */

    for (size_t i = 0; i < bits.size(); i += bits_per_symbol) {
        uint8_t value = 0;
        for (uint8_t b = 0; b < bits_per_symbol; b++) {
            const size_t index = i + b;
            const uint8_t bit = (index < bits.size()) ? static_cast<uint8_t>(bits[index] & 1) : 0;
            value = static_cast<uint8_t>((value << 1) | bit);
        }
        const uint8_t coded = gray(value);
        if (differential) {
            previous = static_cast<uint8_t>((previous + coded) % modulus);
            indices.push_back(previous);
        } else {
            indices.push_back(coded);
        }
    }

    std::vector<cfloat> symbols(indices.size());
    for (size_t i = 0; i < indices.size(); i++) {
        if (order == PskOrder::Bpsk) {
            symbols[i] = (indices[i] == 0) ? cfloat{1.0f, 0.0f} : cfloat{-1.0f, 0.0f};
        } else {
            symbols[i] = std::polar(1.0f,
                                    kPiF * 0.25f + static_cast<float>(indices[i]) * kPiF * 0.5f);
        }
    }

    const std::vector<float> pulse = design_root_raised_cosine(rolloff, sps, span_symbols);
    const size_t taps = pulse.size();

    out.assign(symbols.size() * sps + taps - 1, cfloat{0.0f, 0.0f});
    for (size_t s = 0; s < symbols.size(); s++) {
        const size_t base = s * sps;
        for (size_t t = 0; t < taps; t++) {
            out[base + t] += symbols[s] * pulse[t];
        }
    }
    return out;
}

}  // namespace dsp
