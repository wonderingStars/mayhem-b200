/*
 * mayhem-b200 — modulators, keyers and tone tables.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "modulate.hpp"

#include <algorithm>
#include <cmath>

namespace dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

/* Keeps a radian phase accumulator in [0, 2pi) without a fmod. */
inline void wrap_phase(double& phase) {
    if (phase >= kTwoPi)
        phase -= kTwoPi * std::floor(phase / kTwoPi);
    else if (phase < 0.0)
        phase += kTwoPi * (std::floor(-phase / kTwoPi) + 1.0);
}

/* Upper tail of the standard normal, Q(x) = 1/2 erfc(x / sqrt(2)). */
inline double q_function(double x) {
    return 0.5 * std::erfc(x / std::sqrt(2.0));
}

}  // namespace

/* --- design_gaussian_pulse -------------------------------------------------- */

std::vector<float> design_gaussian_pulse(double bt,
                                         double samples_per_symbol,
                                         size_t span_symbols) {
    if (samples_per_symbol <= 0.0) return {1.0f};
    if (span_symbols < 1) span_symbols = 1;
    if (bt <= 0.0) return {1.0f};

    size_t n = static_cast<size_t>(std::lround(static_cast<double>(span_symbols) *
                                               samples_per_symbol));
    if (n < 1) n = 1;

    /* t is measured in symbol periods, centred on the pulse the same way the
     * firmware's BLE table is: offsets (k - n/2) / samples_per_symbol. */
    const double a = kTwoPi * bt / std::sqrt(std::log(2.0));
    const double centre = static_cast<double>(n) / 2.0;

    std::vector<float> taps(n);
    double sum = 0.0;
    for (size_t k = 0; k < n; k++) {
        const double t = (static_cast<double>(k) - centre) / samples_per_symbol;
        const double v = 0.5 * (q_function(a * (t - 0.5)) - q_function(a * (t + 0.5)));
        taps[k] = static_cast<float>(v);
        sum += v;
    }

    /* Unit sum: a run of identical symbols must reach exactly full deviation. */
    if (sum > 1e-12) {
        for (auto& t : taps) t = static_cast<float>(t / sum);
    }

    return taps;
}

/* --- ToneGen ---------------------------------------------------------------- */

void ToneGen::configure(float frequency_hz, float sample_rate_hz, Shape shape, float amplitude) {
    sample_rate_hz_ = (sample_rate_hz > 0.0f) ? sample_rate_hz : 48000.0f;
    shape_ = shape;
    amplitude_ = amplitude;
    set_frequency(frequency_hz);
    reset();
}

void ToneGen::set_frequency(float frequency_hz) {
    frequency_hz_ = frequency_hz;
    phase_step_ = (sample_rate_hz_ > 0.0f)
                      ? static_cast<double>(frequency_hz) / static_cast<double>(sample_rate_hz_)
                      : 0.0;
}

void ToneGen::reset() {
    phase_ = 0.0;
    lfsr_ = 0xACE1u;
}

float ToneGen::process_one() {
    float v;

    if (shape_ == Shape::Noise) {
        /* 16-bit Fibonacci LFSR, taps 16/15/13/4 — the polynomial
         * proc_siggen.cpp uses. Any non-zero seed works; the period is 65535. */
        const uint16_t bit = static_cast<uint16_t>(
            ((lfsr_ >> 0) ^ (lfsr_ >> 1) ^ (lfsr_ >> 3) ^ (lfsr_ >> 12)) & 1u);
        lfsr_ = static_cast<uint16_t>((lfsr_ >> 1) | (bit << 15));
        /* Map the 16-bit state onto [-1, 1). */
        v = (static_cast<float>(lfsr_) / 32768.0f) - 1.0f;
        return v * amplitude_;
    }

    const double p = phase_;  /* turns, [0, 1) */

    switch (shape_) {
        case Shape::Sine:
            v = static_cast<float>(std::sin(kTwoPi * p));
            break;
        case Shape::Triangle:
            /* +1 at a quarter turn, -1 at three quarters, matching the sine's
             * sign so a shape change does not invert the modulation. */
            v = static_cast<float>((p < 0.5) ? (4.0 * p - 1.0) : (3.0 - 4.0 * p));
            break;
        case Shape::SawUp:
            v = static_cast<float>(2.0 * p - 1.0);
            break;
        case Shape::SawDown:
            v = static_cast<float>(1.0 - 2.0 * p);
            break;
        case Shape::Square:
            v = (p < 0.5) ? 1.0f : -1.0f;
            break;
        default:
            v = 0.0f;
            break;
    }

    phase_ += phase_step_;
    if (phase_ >= 1.0)
        phase_ -= std::floor(phase_);
    else if (phase_ < 0.0)
        phase_ += std::floor(-phase_) + 1.0;

    return v * amplitude_;
}

void ToneGen::process(float* out, size_t count) {
    for (size_t i = 0; i < count; i++) out[i] = process_one();
}

void ToneGen::mix(float* samples, size_t count, float weight) {
    /* Upstream bails out on a zero phase increment rather than a zero frequency;
     * the two are the same thing for any sane sample rate. */
    if (frequency_hz_ == 0.0f && shape_ != Shape::Noise) return;

    const float input_weight = 1.0f - weight;
    for (size_t i = 0; i < count; i++)
        samples[i] = samples[i] * input_weight + process_one() * weight;
}

/* --- FmModulator ------------------------------------------------------------ */

void FmModulator::configure(float sample_rate_hz, float deviation_hz) {
    sample_rate_hz_ = (sample_rate_hz > 0.0f) ? sample_rate_hz : 48000.0f;
    deviation_hz_ = deviation_hz;
    /* An audio value of 1.0 must advance the phase by 2*pi*deviation/fs radians
     * per sample; dsp::FmDemod divides the same quantity back out. */
    radians_per_unit_ = kTwoPi * static_cast<double>(deviation_hz) /
                        static_cast<double>(sample_rate_hz_);
    reset();
}

void FmModulator::reset() { phase_ = 0.0; }

void FmModulator::process(const float* audio, size_t count, cfloat* out) {
    for (size_t i = 0; i < count; i++) {
        phase_ += static_cast<double>(audio[i]) * radians_per_unit_;
        wrap_phase(phase_);
        out[i] = cfloat{static_cast<float>(std::cos(phase_)),
                        static_cast<float>(std::sin(phase_))};
    }
}

void FmModulator::process(const float* audio, size_t count, std::vector<cfloat>& out) {
    const size_t base = out.size();
    out.resize(base + count);
    process(audio, count, out.data() + base);
}

/* --- AmModulator ------------------------------------------------------------ */

void AmModulator::configure(float /*sample_rate_hz*/, float depth, Variant variant) {
    variant_ = variant;
    set_depth(depth);
}

void AmModulator::set_depth(float depth) {
    depth_ = std::clamp(depth, 0.0f, 1.0f);
    /* Scale the carrier so the peak envelope, carrier * (1 + depth), is 1.0. */
    carrier_ = 1.0f / (1.0f + depth_);
}

void AmModulator::reset() {}

void AmModulator::process(const float* audio, size_t count, cfloat* out) {
    if (variant_ == Variant::DSB) {
        for (size_t i = 0; i < count; i++) out[i] = cfloat{audio[i], 0.0f};
        return;
    }

    for (size_t i = 0; i < count; i++) {
        /* A modulating signal beyond +/-1 would take the envelope negative,
         * which is overmodulation: clamp instead, so the carrier is never
         * inverted. */
        const float a = std::clamp(audio[i], -1.0f, 1.0f);
        out[i] = cfloat{carrier_ * (1.0f + depth_ * a), 0.0f};
    }
}

void AmModulator::process(const float* audio, size_t count, std::vector<cfloat>& out) {
    const size_t base = out.size();
    out.resize(base + count);
    process(audio, count, out.data() + base);
}

/* --- SsbModulator ----------------------------------------------------------- */

void SsbModulator::configure(float /*sample_rate_hz*/, Sideband sideband, size_t hilbert_taps) {
    if (hilbert_taps < 15) hilbert_taps = 15;
    if ((hilbert_taps % 2) == 0) hilbert_taps += 1;

    hilbert_.configure(design_hilbert(hilbert_taps));
    group_delay_ = (hilbert_taps - 1) / 2;
    delay_.configure(group_delay_);
    sideband_ = sideband;
    reset();
}

void SsbModulator::reset() {
    hilbert_.reset();
    delay_.reset();
}

void SsbModulator::process(const float* audio, size_t count, cfloat* out) {
    const float sign = (sideband_ == Sideband::Upper) ? +1.0f : -1.0f;

    for (size_t i = 0; i < count; i++) {
        const float i_delayed = delay_.process(audio[i]);
        const float q_shifted = hilbert_.process_one(audio[i]);
        out[i] = cfloat{i_delayed, sign * q_shifted};
    }
}

void SsbModulator::process(const float* audio, size_t count, std::vector<cfloat>& out) {
    const size_t base = out.size();
    out.resize(base + count);
    process(audio, count, out.data() + base);
}

/* --- Bit access ------------------------------------------------------------- */

bool bit_at(const uint8_t* data, size_t bit_index) {
    if (data == nullptr) return false;
    return ((data[bit_index >> 3] << (bit_index & 7)) & 0x80) != 0;
}

/* --- OokKeyer --------------------------------------------------------------- */

void OokKeyer::configure(float sample_rate_hz, float symbol_rate_hz) {
    if (sample_rate_hz <= 0.0f || symbol_rate_hz <= 0.0f) {
        samples_per_symbol_ = 1.0;
    } else {
        samples_per_symbol_ = static_cast<double>(sample_rate_hz) /
                              static_cast<double>(symbol_rate_hz);
    }
    reset();
}

void OokKeyer::set_levels(float low, float high) {
    low_ = low;
    high_ = high;
}

void OokKeyer::set_repeat(uint32_t repeat, uint32_t pause_symbols) {
    repeat_ = std::max<uint32_t>(repeat, 1);
    pause_symbols_ = pause_symbols;
    reset();
}

void OokKeyer::set_data(const uint8_t* data, size_t bit_count) {
    if (data == nullptr || bit_count == 0) {
        data_.clear();
        bit_count_ = 0;
    } else {
        const size_t bytes = (bit_count + 7) / 8;
        data_.assign(data, data + bytes);
        bit_count_ = bit_count;
    }
    reset();
}

void OokKeyer::reset() {
    level_ = low_;
    sample_index_ = 0;
    next_boundary_ = 0.0;
    symbol_index_ = 0;
    repeats_sent_ = 0;
    done_ = (bit_count_ == 0);
}

size_t OokKeyer::total_samples() const {
    if (bit_count_ == 0) return 0;
    const double symbols = static_cast<double>(repeat_) * static_cast<double>(bit_count_) +
                           static_cast<double>(repeat_ - 1) * static_cast<double>(pause_symbols_);
    return static_cast<size_t>(std::ceil(symbols * samples_per_symbol_));
}

void OokKeyer::advance_symbol() {
    if (symbol_index_ < bit_count_) {
        level_ = bit_at(data_.data(), symbol_index_) ? high_ : low_;
    } else {
        /* Inter-repeat pause: hold the low level, as proc_ook does with
         * cur_bit = 0. */
        level_ = low_;
    }

    symbol_index_++;
    next_boundary_ += samples_per_symbol_;

    const bool more_repeats = (repeats_sent_ + 1) < repeat_;
    const size_t period = bit_count_ + (more_repeats ? pause_symbols_ : 0);
    if (symbol_index_ >= period) {
        repeats_sent_++;
        symbol_index_ = 0;
    }
}

size_t OokKeyer::process(cfloat* out, size_t count) {
    size_t written = 0;

    while (written < count) {
        if (static_cast<double>(sample_index_) >= next_boundary_) {
            if (done_ || repeats_sent_ >= repeat_ || bit_count_ == 0) {
                done_ = true;
                break;
            }
            advance_symbol();
        }

        out[written++] = cfloat{level_, 0.0f};
        sample_index_++;
    }

    return written;
}

/* --- FskKeyer --------------------------------------------------------------- */

void FskKeyer::configure(float sample_rate_hz, float symbol_rate_hz, float deviation_hz) {
    sample_rate_hz_ = (sample_rate_hz > 0.0f) ? sample_rate_hz : 48000.0f;
    deviation_hz_ = deviation_hz;

    samples_per_symbol_ = (symbol_rate_hz > 0.0f)
                              ? static_cast<double>(sample_rate_hz_) /
                                    static_cast<double>(symbol_rate_hz)
                              : 1.0;

    radians_per_unit_ = kTwoPi * static_cast<double>(deviation_hz) /
                        static_cast<double>(sample_rate_hz_);

    /* Re-derive the shaping filter: its tap count depends on the samples per
     * symbol we just recomputed. */
    set_gaussian(bt_, span_symbols_);
}

void FskKeyer::set_gaussian(float bt, size_t span_symbols) {
    bt_ = bt;
    span_symbols_ = std::max<size_t>(span_symbols, 1);

    if (bt <= 0.0f) {
        shaping_ = false;
        group_delay_ = 0;
        shaper_.configure({1.0f});
    } else {
        auto taps = design_gaussian_pulse(bt, samples_per_symbol_, span_symbols_);
        shaper_.configure(taps);
        shaping_ = true;
        group_delay_ = (taps.size() - 1) / 2;
    }

    reset();
}

void FskKeyer::set_data(const uint8_t* data, size_t bit_count) {
    if (data == nullptr || bit_count == 0) {
        data_.clear();
        bit_count_ = 0;
    } else {
        const size_t bytes = (bit_count + 7) / 8;
        data_.assign(data, data + bytes);
        bit_count_ = bit_count;
    }
    reset();
}

void FskKeyer::set_repeat(uint32_t repeat, uint32_t pause_symbols) {
    repeat_ = std::max<uint32_t>(repeat, 1);
    pause_symbols_ = pause_symbols;
    reset();
}

void FskKeyer::reset() {
    symbol_ = 0.0f;
    sample_index_ = 0;
    next_boundary_ = 0.0;
    symbol_index_ = 0;
    repeats_sent_ = 0;
    done_ = (bit_count_ == 0);
    flush_remaining_ = done_ ? 0 : group_delay_;
    phase_ = 0.0;
    shaper_.reset();
}

void FskKeyer::advance_symbol() {
    if (symbol_index_ < bit_count_) {
        /* proc_fsk: a 1 bit adds +shift, a 0 bit subtracts it. */
        symbol_ = bit_at(data_.data(), symbol_index_) ? +1.0f : -1.0f;
    } else {
        /* Inter-repeat pause: unmodulated carrier. */
        symbol_ = 0.0f;
    }

    symbol_index_++;
    next_boundary_ += samples_per_symbol_;

    const bool more_repeats = (repeats_sent_ + 1) < repeat_;
    const size_t period = bit_count_ + (more_repeats ? pause_symbols_ : 0);
    if (symbol_index_ >= period) {
        repeats_sent_++;
        symbol_index_ = 0;
    }
}

size_t FskKeyer::process(cfloat* out, size_t count) {
    size_t written = 0;

    while (written < count) {
        float value;

        if (!done_ && static_cast<double>(sample_index_) >= next_boundary_) {
            if (repeats_sent_ >= repeat_ || bit_count_ == 0)
                done_ = true;
            else
                advance_symbol();
        }

        if (done_) {
            /* Push zeros through the shaper so the trailing half of the last
             * Gaussian pulse still reaches the output. */
            if (flush_remaining_ == 0) break;
            flush_remaining_--;
            value = 0.0f;
        } else {
            value = symbol_;
            sample_index_++;
        }

        if (shaping_) value = shaper_.process_one(value);

        phase_ += static_cast<double>(value) * radians_per_unit_;
        wrap_phase(phase_);
        out[written++] = cfloat{static_cast<float>(std::cos(phase_)),
                                static_cast<float>(std::sin(phase_))};
    }

    return written;
}

/* --- Tone tables ------------------------------------------------------------ */

namespace tones {

/* application/tone_key.cpp, ascending by frequency. */
const std::array<CtcssTone, 50> ctcss = {{
    {"1 XZ", 67.0f},    {"39 WZ", 69.3f},   {"2 XA", 71.9f},    {"3 WA", 74.4f},
    {"4 XB", 77.0f},    {"5 WB", 79.7f},    {"6 YZ", 82.5f},    {"7 YA", 85.4f},
    {"8 YB", 88.5f},    {"9 ZZ", 91.5f},    {"10 ZA", 94.8f},   {"11 ZB", 97.4f},
    {"12 1Z", 100.0f},  {"13 1A", 103.5f},  {"14 1B", 107.2f},  {"15 2Z", 110.9f},
    {"16 2A", 114.8f},  {"17 2B", 118.8f},  {"18 3Z", 123.0f},  {"19 3A", 127.3f},
    {"20 3B", 131.8f},  {"21 4Z", 136.5f},  {"22 4A", 141.3f},  {"23 4B", 146.2f},
    {"24 5Z", 151.4f},  {"25 5A", 156.7f},  {"40 --", 159.8f},  {"26 5B", 162.2f},
    {"41 --", 165.5f},  {"27 6Z", 167.9f},  {"42 --", 171.3f},  {"28 6A", 173.8f},
    {"43 --", 177.3f},  {"29 6B", 179.9f},  {"44 --", 183.5f},  {"30 7Z", 186.2f},
    {"45 --", 189.9f},  {"31 7A", 192.8f},  {"46 --", 196.6f},  {"47 --", 199.5f},
    {"32 M1", 203.5f},  {"48 8Z", 206.5f},  {"33 M2", 210.7f},  {"34 M3", 218.1f},
    {"35 M4", 225.7f},  {"49 9Z", 229.1f},  {"36 M5", 233.6f},  {"37 M6", 241.8f},
    {"38 M7", 250.3f},  {"50 0Z", 254.1f},
}};

int ctcss_index(float hz, float tolerance_hz) {
    int best = -1;
    float best_diff = tolerance_hz;

    for (size_t i = 0; i < ctcss.size(); i++) {
        const float diff = std::fabs(ctcss[i].frequency_hz - hz);
        if (diff <= best_diff) {
            best_diff = diff;
            best = static_cast<int>(i);
        }
    }

    return best;
}

/* common/tonesets.hpp, converted from phase increments back to hertz. */
const std::array<float, 16> ccir = {{1981.0f, 1124.0f, 1197.0f, 1275.0f,
                                     1358.0f, 1446.0f, 1540.0f, 1640.0f,
                                     1747.0f, 1860.0f, 2400.0f, 930.0f,
                                     2247.0f, 991.0f, 2110.0f, 1055.0f}};

const std::array<float, 16> eia = {{600.0f, 741.0f, 882.0f, 1023.0f,
                                    1164.0f, 1305.0f, 1446.0f, 1587.0f,
                                    1728.0f, 1869.0f, 2151.0f, 2433.0f,
                                    2010.0f, 2292.0f, 459.0f, 1091.0f}};

const std::array<float, 16> zvei = {{2400.0f, 1060.0f, 1160.0f, 1270.0f,
                                     1400.0f, 1530.0f, 1670.0f, 1830.0f,
                                     2000.0f, 2200.0f, 2800.0f, 810.0f,
                                     970.0f, 885.0f, 2600.0f, 680.0f}};

/* Columns 1209/1336/1477/1633 Hz, rows 697/770/852/941 Hz, in the character
 * order "0123456789ABCD#*" that upstream's dtmf_deltas uses. */
const std::array<std::array<float, 2>, 16> dtmf = {{
    {{1336.0f, 941.0f}},  /* 0 */
    {{1209.0f, 697.0f}},  /* 1 */
    {{1336.0f, 697.0f}},  /* 2 */
    {{1477.0f, 697.0f}},  /* 3 */
    {{1209.0f, 770.0f}},  /* 4 */
    {{1336.0f, 770.0f}},  /* 5 */
    {{1477.0f, 770.0f}},  /* 6 */
    {{1209.0f, 852.0f}},  /* 7 */
    {{1336.0f, 852.0f}},  /* 8 */
    {{1477.0f, 852.0f}},  /* 9 */
    {{1633.0f, 697.0f}},  /* A */
    {{1633.0f, 770.0f}},  /* B */
    {{1633.0f, 852.0f}},  /* C */
    {{1633.0f, 941.0f}},  /* D */
    {{1477.0f, 941.0f}},  /* # */
    {{1209.0f, 941.0f}},  /* * */
}};

const std::array<float, 6> roger_beep = {{1475.0f, 740.0f, 587.0f,
                                          1109.0f, 831.0f, 740.0f}};

uint32_t dcs_word(uint16_t code) {
    /* Generator of the (23,12) Golay code, x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1.
     * Checked against every one of the 512 entries in upstream's dcs_parity[]. */
    constexpr uint32_t kGolayGenerator = 0xC75u;

    const uint32_t data = 0x800u | (code & 0x1FFu);  /* the fixed 100 plus the code */

    uint32_t remainder = data << 11;
    for (int bit = 22; bit >= 11; bit--) {
        if ((remainder >> bit) & 1u)
            remainder ^= kGolayGenerator << (bit - 11);
    }
    const uint32_t parity = remainder & 0x7FFu;

    return (parity << 12) | (0x4u << 9) | (code & 0x1FFu);
}

}  // namespace tones

/* --- DcsEncoder ------------------------------------------------------------- */

void DcsEncoder::configure(uint16_t code, float sample_rate_hz, float baud, float amplitude) {
    code_ = static_cast<uint16_t>(code & 0x1FFu);
    word_ = tones::dcs_word(code_);
    amplitude_ = amplitude;
    samples_per_bit_ = (sample_rate_hz > 0.0f && baud > 0.0f)
                           ? static_cast<double>(sample_rate_hz) / static_cast<double>(baud)
                           : 1.0;
    reset();
}

void DcsEncoder::reset() {
    sample_index_ = 0;
    next_boundary_ = 0.0;
    bit_index_ = 0;
    level_ = 0.0f;
    primed_ = false;
}

float DcsEncoder::process_one() {
    if (!primed_ || static_cast<double>(sample_index_) >= next_boundary_) {
        /* LSB first: bit 0 of the 23-bit word is the low bit of the code. */
        const bool bit = ((word_ >> bit_index_) & 1u) != 0;
        level_ = bit ? amplitude_ : -amplitude_;
        bit_index_ = (bit_index_ + 1) % 23;
        next_boundary_ += samples_per_bit_;
        primed_ = true;
    }

    sample_index_++;
    return level_;
}

void DcsEncoder::process(float* out, size_t count) {
    for (size_t i = 0; i < count; i++) out[i] = process_one();
}

void DcsEncoder::mix(float* samples, size_t count, float weight) {
    const float input_weight = 1.0f - weight;
    for (size_t i = 0; i < count; i++)
        samples[i] = samples[i] * input_weight + process_one() * weight;
}

/* --- FirInterpolateC -------------------------------------------------------- */

FirInterpolateC::FirInterpolateC(std::vector<float> taps, size_t interpolation) {
    configure(std::move(taps), interpolation);
}

void FirInterpolateC::configure(std::vector<float> taps, size_t interpolation) {
    if (taps.empty()) taps = {1.0f};
    interpolation_ = (interpolation == 0) ? 1 : interpolation;

    branch_len_ = (taps.size() + interpolation_ - 1) / interpolation_;
    if (branch_len_ < 1) branch_len_ = 1;

    /* Pad to a whole number of polyphase branches so every branch has the same
     * length and the inner loop needs no bounds test. */
    taps_.assign(branch_len_ * interpolation_, 0.0f);

    /* Zero-stuffing divides the average power by the interpolation factor;
     * scaling the taps back up keeps the passband gain at unity. */
    const float scale = static_cast<float>(interpolation_);
    for (size_t i = 0; i < taps.size(); i++) taps_[i] = taps[i] * scale;

    history_.assign(branch_len_ * 2, cfloat{0.0f, 0.0f});
    pos_ = 0;
}

void FirInterpolateC::reset() {
    std::fill(history_.begin(), history_.end(), cfloat{0.0f, 0.0f});
    pos_ = 0;
}

size_t FirInterpolateC::process(const cfloat* in, size_t count, std::vector<cfloat>& out) {
    const size_t n = branch_len_;
    const size_t l = interpolation_;

    out.reserve(out.size() + count * l);

    for (size_t i = 0; i < count; i++) {
        history_[pos_] = in[i];
        history_[pos_ + n] = in[i];
        pos_ = (pos_ + 1) % n;

        /* w[0] is the oldest sample, w[n-1] the newest, so x[k-j] is w[n-1-j]. */
        const cfloat* w = &history_[pos_];

        for (size_t phase = 0; phase < l; phase++) {
            float acc_r = 0.0f;
            float acc_i = 0.0f;
            for (size_t j = 0; j < n; j++) {
                const float t = taps_[phase + j * l];
                const cfloat& s = w[n - 1 - j];
                acc_r += s.real() * t;
                acc_i += s.imag() * t;
            }
            out.emplace_back(acc_r, acc_i);
        }
    }

    return count * l;
}

}  // namespace dsp
