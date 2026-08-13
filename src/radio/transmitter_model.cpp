/*
 * mayhem-b200 — transmit chain.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "transmitter_model.hpp"

#include "../audio/audio_out.hpp"  /* audio::sample_rate */

#include <algorithm>
#include <chrono>
#include <cmath>

namespace radio {

namespace {

/* Occupied bandwidth, audio corner and FM deviation per mode. The narrowband
 * numbers are the transmit side of the pairings ReceiverModel uses, so a
 * transmission made here lands squarely inside the matching receive filter. */
struct ChannelSpec {
    double bandwidth_hz;  /* two-sided occupied bandwidth */
    double audio_hz;      /* audio lowpass corner */
    double deviation_hz;  /* FM only */
};

ChannelSpec spec_for(TransmitterModel::Mode mode, TransmitterModel::NfmConfig nfm) {
    switch (mode) {
        case TransmitterModel::Mode::NarrowbandFM:
            switch (nfm) {
                case TransmitterModel::NfmConfig::Narrow8k5: return {8500.0, 3000.0, 2500.0};
                case TransmitterModel::NfmConfig::Wide16k: return {16000.0, 3400.0, 5000.0};
                case TransmitterModel::NfmConfig::Medium11k:
                default: return {11000.0, 3200.0, 3500.0};
            }
        case TransmitterModel::Mode::WidebandFM:
            return {200000.0, 15000.0, 75000.0};
        case TransmitterModel::Mode::AM:
        case TransmitterModel::Mode::DSB:
            return {9000.0, 4500.0, 0.0};
        case TransmitterModel::Mode::USB:
        case TransmitterModel::Mode::LSB:
            return {2800.0, 2800.0, 0.0};
        case TransmitterModel::Mode::CW:
            /* A key-down carrier is a line, but the analog filter still needs a
             * width to be set to; keep it narrow. */
            return {1000.0, 0.0, 0.0};
        case TransmitterModel::Mode::Raw:
        default:
            return {0.0, 0.0, 0.0};
    }
}

/* Output samples one DSP iteration aims to produce. Big enough to amortise the
 * per-block cost, small enough that a control change is heard immediately. */
constexpr size_t kTargetOutputSamples = 8192;

}  // namespace

TransmitterModel::TransmitterModel(RadioDevice& radio)
    : radio_{radio} {}

TransmitterModel::~TransmitterModel() {
    stop();
}

const char* TransmitterModel::mode_label(Mode m) {
    switch (m) {
        case Mode::NarrowbandFM: return "NFM";
        case Mode::WidebandFM: return "WFM";
        case Mode::AM: return "AM";
        case Mode::DSB: return "DSB";
        case Mode::USB: return "USB";
        case Mode::LSB: return "LSB";
        case Mode::CW: return "CW";
        case Mode::Raw: return "RAW";
    }
    return "?";
}

std::string TransmitterModel::mode_name() const {
    if (mode_ == Mode::NarrowbandFM) {
        switch (nfm_config_) {
            case NfmConfig::Narrow8k5: return "NFM 8k5";
            case NfmConfig::Medium11k: return "NFM 11k";
            case NfmConfig::Wide16k: return "NFM 16k";
        }
        return "NFM";
    }
    return mode_label(mode_);
}

double TransmitterModel::channel_bandwidth() const {
    if (mode_ == Mode::Raw) return sample_rate_;
    return spec_for(mode_, nfm_config_).bandwidth_hz;
}

double TransmitterModel::deviation() const {
    if (deviation_override_ > 0.0) return deviation_override_;
    return spec_for(mode_, nfm_config_).deviation_hz;
}

double TransmitterModel::bandwidth() const {
    if (bandwidth_override_ > 0.0) return bandwidth_override_;
    /* Twice the occupied bandwidth passes the modulation untouched while still
     * filtering wideband noise and whatever the interpolator left behind. The
     * device clamps this to its own achievable range. */
    return channel_bandwidth() * 2.0;
}

bool TransmitterModel::modulates_audio() const {
    return mode_ != Mode::Raw && mode_ != Mode::CW;
}

/* --- Control --------------------------------------------------------------- */

bool TransmitterModel::start() {
    if (running_.load()) return true;

    radio_.set_tx_rate(sample_rate_);
    sample_rate_ = radio_.tx_rate();
    radio_.set_tx_bandwidth(bandwidth());

    retune_if_needed();
    chain_dirty_.store(true);

    if (!radio_.start_tx()) return false;

    stop_.store(false);
    running_.store(true);
    dsp_thread_ = std::thread(&TransmitterModel::dsp_thread_main, this);
    return true;
}

void TransmitterModel::stop() {
    if (running_.load()) {
        stop_.store(true);
        if (dsp_thread_.joinable()) dsp_thread_.join();
        running_.store(false);
    } else if (dsp_thread_.joinable()) {
        dsp_thread_.join();
    }

    radio_.stop_tx();
}

void TransmitterModel::set_target_frequency(uint64_t hz) {
    target_frequency_ = hz;
    retune_if_needed();
}

void TransmitterModel::retune_if_needed() {
    const double target = static_cast<double>(target_frequency_);
    const double lo = radio_.tx_frequency();

    /* Keep the transmitted signal inside the middle 80% of the streamed band so
     * the interpolation filter's skirts never clip it. */
    const double window = sample_rate_ * 0.4;

    const bool move_lo = (lo == 0.0) || (std::fabs(target - lo) > window);

    double offset = target - lo;
    if (move_lo) {
        /* set_tx_frequency() returns what the hardware actually reached; with
         * UHD's default tune policy that is the request, but a clamp at a band
         * edge would leave a residual that the NCO should still make up. */
        offset = target - radio_.set_tx_frequency(target);
    }

    /* An NCO shift beyond Nyquist is meaningless — that happens only when the
     * device clamped the request far away, and pinning it is honest. */
    offset = std::clamp(offset, -window, window);

    /* Tuning happens on the UI thread, mixing on the DSP thread; without this
     * they race on the oscillator's phase state. Mixing by +offset moves the
     * baseband up to sit `offset` above the LO. */
    std::lock_guard<std::mutex> g{chain_mutex_};
    nco_.set_frequency(offset, sample_rate_);
}

void TransmitterModel::set_mode(Mode mode) {
    {
        std::lock_guard<std::mutex> g{chain_mutex_};
        if (mode == mode_) return;
        mode_ = mode;
    }
    chain_dirty_.store(true);
}

void TransmitterModel::set_nfm_configuration(NfmConfig cfg) {
    {
        std::lock_guard<std::mutex> g{chain_mutex_};
        if (cfg == nfm_config_) return;
        nfm_config_ = cfg;
    }
    chain_dirty_.store(true);
}

void TransmitterModel::set_deviation(double hz) {
    {
        std::lock_guard<std::mutex> g{chain_mutex_};
        if (hz < 0.0) hz = 0.0;
        if (hz == deviation_override_) return;
        deviation_override_ = hz;
    }
    chain_dirty_.store(true);
}

void TransmitterModel::set_am_depth(float depth) {
    {
        std::lock_guard<std::mutex> g{chain_mutex_};
        am_depth_ = std::clamp(depth, 0.0f, 1.0f);
        am_.set_depth(am_depth_);
    }
}

/* Validated in the shared layer for the same reason the receive side is: a
 * backend is not a reliable place to catch this, since NetworkRadio only clamps
 * against caps while its link is down. See capability_policy.hpp. */
void TransmitterModel::set_gain(double db) {
    gain_choice_ = choose_tx_gain(radio_.caps(), db);
    if (!gain_choice_.should_apply()) return;
    radio_.set_tx_gain(gain_choice_.gain_db);
}

double TransmitterModel::gain() const { return radio_.tx_gain(); }

void TransmitterModel::set_amplitude(float amplitude) {
    amplitude_ = std::clamp(amplitude, 0.0f, 1.0f);
}

void TransmitterModel::set_audio_gain(float gain) {
    audio_gain_ = std::max(gain, 0.0f);
}

void TransmitterModel::set_sampling_rate(double hz) {
    if (hz == sample_rate_) return;

    const bool was_running = running_.load();
    if (was_running) stop();

    sample_rate_ = radio_.set_tx_rate(hz);
    radio_.set_tx_bandwidth(bandwidth());
    chain_dirty_.store(true);

    if (was_running) start();
}

void TransmitterModel::set_bandwidth(double hz) {
    bandwidth_override_ = std::max(hz, 0.0);
    if (running_.load()) radio_.set_tx_bandwidth(bandwidth());
}

/* --- Sub-audible signalling ------------------------------------------------- */

void TransmitterModel::set_sub_tone_none() {
    {
        std::lock_guard<std::mutex> g{chain_mutex_};
        sub_tone_ = SubTone::None;
    }
    chain_dirty_.store(true);
}

void TransmitterModel::set_ctcss(float frequency_hz, float mix_weight) {
    {
        std::lock_guard<std::mutex> g{chain_mutex_};
        sub_tone_ = SubTone::Ctcss;
        ctcss_frequency_ = std::max(frequency_hz, 0.0f);
        sub_tone_mix_ = std::clamp(mix_weight, 0.0f, 1.0f);
    }
    chain_dirty_.store(true);
}

void TransmitterModel::set_dcs(uint16_t code, float mix_weight) {
    {
        std::lock_guard<std::mutex> g{chain_mutex_};
        sub_tone_ = SubTone::Dcs;
        dcs_code_ = static_cast<uint16_t>(code & 0x1FFu);
        sub_tone_mix_ = std::clamp(mix_weight, 0.0f, 1.0f);
    }
    chain_dirty_.store(true);
}

/* --- Sources ---------------------------------------------------------------- */

void TransmitterModel::set_audio_source(AudioSource source) {
    std::lock_guard<std::mutex> g{source_mutex_};
    audio_source_ = std::move(source);
}

void TransmitterModel::set_iq_source(IqSource source) {
    std::lock_guard<std::mutex> g{source_mutex_};
    iq_source_ = std::move(source);
}

bool TransmitterModel::has_audio_source() const {
    std::lock_guard<std::mutex> g{source_mutex_};
    return static_cast<bool>(audio_source_);
}

bool TransmitterModel::has_iq_source() const {
    std::lock_guard<std::mutex> g{source_mutex_};
    return static_cast<bool>(iq_source_);
}

/* --- Chain construction ----------------------------------------------------- */

void TransmitterModel::rebuild_chain() {
    std::lock_guard<std::mutex> g{chain_mutex_};

    const ChannelSpec spec = spec_for(mode_, nfm_config_);

    /* One iteration should produce roughly kTargetOutputSamples whatever the
     * streamed rate is, so the loop's cadence does not change with bandwidth
     * and the ring never has to swallow a block far bigger than expected. */
    const double audio_per_block = static_cast<double>(kTargetOutputSamples) *
                                   static_cast<double>(audio::sample_rate) /
                                   std::max(sample_rate_, 1.0);
    const size_t audio_block =
        std::clamp<size_t>(static_cast<size_t>(std::lround(audio_per_block)), 8, 8192);
    audio_block_.store(audio_block);

    if (!modulates_audio()) {
        /* Raw and CW hand samples straight to the radio at its own rate. */
        modulator_rate_.store(sample_rate_);
        interpolation_.store(1);
        interpolator_.configure({1.0f}, 1);
        block_output_.store(kTargetOutputSamples);
        if (running_.load()) radio_.set_tx_bandwidth(bandwidth());
        return;
    }

    /* Ceiling plus a sample of slack: the linear resampler can emit one extra
     * output per block depending on where its phase sits. */
    block_output_.store(static_cast<size_t>(
                            std::ceil(static_cast<double>(audio_block) * sample_rate_ /
                                      static_cast<double>(audio::sample_rate))) +
                        1);

    /* Modulate at a few times the occupied bandwidth, then interpolate. Four
     * times leaves the modulation products well clear of the folding edge; a
     * higher factor would only move work into the expensive stage. */
    const double min_modulator_rate =
        std::max(spec.bandwidth_hz * 4.0, static_cast<double>(audio::sample_rate));

    size_t interp = 1;
    if (sample_rate_ > min_modulator_rate)
        interp = static_cast<size_t>(std::floor(sample_rate_ / min_modulator_rate));
    if (interp < 1) interp = 1;

    const double mod_rate = sample_rate_ / static_cast<double>(interp);
    modulator_rate_.store(mod_rate);
    interpolation_.store(interp);

    /* 48 kHz audio up to the modulator rate. The linear interpolator leaves
     * images at multiples of 48 kHz roughly 47 dB down; the lowpass that
     * follows is what actually removes them, and it doubles as the transmit
     * audio bandwidth limit. */
    audio_resampler_.configure(static_cast<double>(audio::sample_rate), mod_rate);
    audio_filter_.configure(
        dsp::design_lowpass(spec.audio_hz, spec.audio_hz * 0.5, mod_rate, 60.0, 511));

    if (interp > 1) {
        /* The modulated signal only occupies +/-bandwidth/2, so the images the
         * zero-stuffing creates sit almost a whole modulator rate away: the
         * transition band can be very wide and the filter correspondingly
         * short. */
        const double cutoff = std::min(spec.bandwidth_hz * 0.75, mod_rate * 0.45);
        const double transition = std::max(mod_rate * 0.4, 1000.0);
        interpolator_.configure(
            dsp::design_lowpass(cutoff, transition, sample_rate_, 60.0, 32 * interp + 1),
            interp);
    } else {
        interpolator_.configure({1.0f}, 1);
    }

    const double deviation_hz = (deviation_override_ > 0.0) ? deviation_override_
                                                            : spec.deviation_hz;

    switch (mode_) {
        case Mode::NarrowbandFM:
        case Mode::WidebandFM:
            fm_.configure(static_cast<float>(mod_rate), static_cast<float>(deviation_hz));
            break;

        case Mode::AM:
            am_.configure(static_cast<float>(mod_rate), am_depth_,
                          dsp::AmModulator::Variant::AM);
            break;

        case Mode::DSB:
            am_.configure(static_cast<float>(mod_rate), am_depth_,
                          dsp::AmModulator::Variant::DSB);
            break;

        case Mode::USB:
        case Mode::LSB: {
            /* A Hilbert transformer of N taps is only accurate above roughly
             * 2*fs/N; aim that corner at 300 Hz, the bottom of the speech band,
             * and cap the cost. */
            size_t taps = static_cast<size_t>(std::lround(2.0 * mod_rate / 300.0)) | 1u;
            taps = std::clamp<size_t>(taps, 63, 511);
            if ((taps % 2) == 0) taps += 1;
            ssb_.configure(static_cast<float>(mod_rate),
                           (mode_ == Mode::USB) ? dsp::SsbModulator::Sideband::Upper
                                                : dsp::SsbModulator::Sideband::Lower,
                           taps);
            break;
        }

        default:
            break;
    }

    ctcss_.configure(ctcss_frequency_, static_cast<float>(mod_rate),
                     dsp::ToneGen::Shape::Sine, 1.0f);
    dcs_.configure(dcs_code_, static_cast<float>(mod_rate));

    nco_.set_frequency(nco_.frequency(), sample_rate_);

    if (running_.load()) radio_.set_tx_bandwidth(bandwidth());
}

/* --- DSP thread ------------------------------------------------------------- */

void TransmitterModel::dsp_thread_main() {
    std::vector<float> audio;
    std::vector<float> shaped;
    std::vector<cfloat> modulated;
    std::vector<cfloat> output;

    auto& ring = radio_.tx_ring();

    while (!stop_.load()) {
        if (chain_dirty_.exchange(false)) rebuild_chain();

        Mode mode;
        {
            std::lock_guard<std::mutex> g{chain_mutex_};
            mode = mode_;
        }

        /* Pace against the radio: it drains the ring as fast as UHD accepts
         * samples, so free space is the only back-pressure signal there is.
         * Two blocks of slack absorbs a scheduling hiccup without letting the
         * DSP run so far ahead that the audio lags the operator. */
        const size_t block = block_output_.load();
        const size_t headroom = std::min<size_t>(block * 2, ring.capacity() / 2);
        if (ring.space() < headroom) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        output.clear();

        if (mode == Mode::Raw) {
            const size_t want = std::min<size_t>(block, ring.space());
            output.assign(want, cfloat{0.0f, 0.0f});

            size_t got = 0;
            {
                std::lock_guard<std::mutex> g{source_mutex_};
                if (iq_source_) got = iq_source_(output.data(), output.size());
            }

            if (got == 0) {
                /* Nothing to send. The radio's TX thread already pads with
                 * zeros, so idling here is quieter than writing silence. */
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (got < output.size()) {
                source_underruns_++;
                output.resize(got);
            }
        } else if (mode == Mode::CW) {
            output.assign(std::min<size_t>(block, ring.space()), cfloat{1.0f, 0.0f});
        } else {
            const size_t want = audio_block_.load();
            audio.assign(want, 0.0f);

            size_t got = 0;
            {
                std::lock_guard<std::mutex> g{source_mutex_};
                if (audio_source_) got = audio_source_(audio.data(), audio.size());
            }
            if (got < audio.size()) {
                source_underruns_++;
                std::fill(audio.begin() + static_cast<ptrdiff_t>(got), audio.end(), 0.0f);
            }

            /* Silence still modulates: an FM or AM transmitter with no audio
             * radiates a carrier, and dropping out entirely would key the PA
             * off between syllables. */

            std::lock_guard<std::mutex> g{chain_mutex_};

            float peak = 0.0f;
            for (auto& s : audio) {
                s = std::clamp(s * audio_gain_, -1.0f, 1.0f);
                peak = std::max(peak, std::fabs(s));
            }
            audio_level_.store(peak);

            shaped.clear();
            audio_resampler_.process(audio.data(), audio.size(), shaped);
            if (shaped.empty()) continue;

            /* In place: FirR::process reads each input before writing its
             * output, so aliasing the buffers is safe. */
            audio_filter_.process(shaped.data(), shaped.size(), shaped.data());

            if (mode_ == Mode::NarrowbandFM || mode_ == Mode::WidebandFM) {
                if (sub_tone_ == SubTone::Ctcss)
                    ctcss_.mix(shaped.data(), shaped.size(), sub_tone_mix_);
                else if (sub_tone_ == SubTone::Dcs)
                    dcs_.mix(shaped.data(), shaped.size(), sub_tone_mix_);
            }

            modulated.clear();
            switch (mode_) {
                case Mode::NarrowbandFM:
                case Mode::WidebandFM:
                    fm_.process(shaped.data(), shaped.size(), modulated);
                    break;
                case Mode::AM:
                case Mode::DSB:
                    am_.process(shaped.data(), shaped.size(), modulated);
                    break;
                case Mode::USB:
                case Mode::LSB:
                    ssb_.process(shaped.data(), shaped.size(), modulated);
                    break;
                default:
                    break;
            }

            if (modulated.empty()) continue;
            interpolator_.process(modulated.data(), modulated.size(), output);
        }

        if (output.empty()) continue;

        {
            std::lock_guard<std::mutex> g{chain_mutex_};
            nco_.mix(output.data(), output.data(), output.size());
        }

        /* Final scaling and a hard magnitude limit. The interpolator can
         * overshoot a constant-envelope signal by a few percent, and a sample
         * outside the unit circle is a clipped sample at the DAC. */
        for (auto& s : output) {
            s *= amplitude_;
            const float mag = std::abs(s);
            if (mag > 1.0f) s /= mag;
        }

        baseband_level_db_.store(dsp::to_db(dsp::rms(output.data(), output.size())));

        const size_t written = ring.write(output.data(), output.size());
        samples_generated_ += written;
        if (written < output.size())
            dropped_ += static_cast<uint32_t>(output.size() - written);
    }
}

}  // namespace radio
