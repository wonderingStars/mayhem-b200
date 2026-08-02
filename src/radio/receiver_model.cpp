/*
 * mayhem-b200 — receive chain.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "receiver_model.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace radio {

namespace {

/* Channel-filter bandwidths, chosen to match the labels Mayhem uses. */
struct ChannelSpec {
    double bandwidth_hz;   /* two-sided, for the complex channel filter */
    double audio_hz;       /* audio lowpass corner */
    double deviation_hz;   /* FM only */
    double deemph_us;      /* FM only, 0 = none */
};

ChannelSpec spec_for_am(ReceiverModel::AmConfig cfg) {
    switch (cfg) {
        case ReceiverModel::AmConfig::DSB6k: return {6000.0, 3000.0, 0.0, 0.0};
        case ReceiverModel::AmConfig::USB:   return {2800.0, 2800.0, 0.0, 0.0};
        case ReceiverModel::AmConfig::LSB:   return {2800.0, 2800.0, 0.0, 0.0};
        case ReceiverModel::AmConfig::CW:    return {1400.0, 1400.0, 0.0, 0.0};
        case ReceiverModel::AmConfig::DSB9k:
        default:                             return {9000.0, 4500.0, 0.0, 0.0};
    }
}

ChannelSpec spec_for_nfm(ReceiverModel::NfmConfig cfg) {
    /* Deviation follows the usual pairing for these channel spacings. */
    switch (cfg) {
        case ReceiverModel::NfmConfig::Narrow8k5: return {8500.0, 3000.0, 2500.0, 750.0};
        case ReceiverModel::NfmConfig::Wide16k:   return {16000.0, 3400.0, 5000.0, 750.0};
        case ReceiverModel::NfmConfig::Medium11k:
        default:                                  return {11000.0, 3200.0, 3500.0, 750.0};
    }
}

ChannelSpec spec_for_wfm(ReceiverModel::WfmConfig cfg) {
    switch (cfg) {
        case ReceiverModel::WfmConfig::Narrow180k: return {180000.0, 15000.0, 75000.0, 50.0};
        case ReceiverModel::WfmConfig::Wide200k:
        default:                                   return {200000.0, 15000.0, 75000.0, 50.0};
    }
}

/* Samples pulled from the RX ring per DSP iteration. Big enough to amortise the
 * per-block overhead, small enough that a mode change feels immediate. */
constexpr size_t kBlockSamples = 8192;

/* How many raw samples the spectrum tap keeps. Matches the largest FFT the
 * waterfall uses. */
constexpr size_t kSpectrumSamples = 4096;

}  // namespace

ReceiverModel::ReceiverModel(UsrpRadio& radio, audio::AudioOut& audio_out)
    : radio_{radio},
      audio_{audio_out} {
    spectrum_buffer_.resize(kSpectrumSamples);
}

ReceiverModel::~ReceiverModel() {
    stop();
}

const char* ReceiverModel::mode_label(Mode m) {
    switch (m) {
        case Mode::AMAudio: return "AM";
        case Mode::NarrowbandFMAudio: return "NFM";
        case Mode::WidebandFMAudio: return "WFM";
        case Mode::SpectrumAnalysis: return "SPEC";
    }
    return "?";
}

std::string ReceiverModel::mode_name() const {
    switch (mode_) {
        case Mode::AMAudio:
            switch (am_config_) {
                case AmConfig::DSB9k: return "AM DSB 9k";
                case AmConfig::DSB6k: return "AM DSB 6k";
                case AmConfig::USB: return "AM USB";
                case AmConfig::LSB: return "AM LSB";
                case AmConfig::CW: return "AM CW";
            }
            return "AM";
        case Mode::NarrowbandFMAudio:
            switch (nfm_config_) {
                case NfmConfig::Narrow8k5: return "NFM 8k5";
                case NfmConfig::Medium11k: return "NFM 11k";
                case NfmConfig::Wide16k: return "NFM 16k";
            }
            return "NFM";
        case Mode::WidebandFMAudio:
            return (wfm_config_ == WfmConfig::Wide200k) ? "WFM 200k" : "WFM 180k";
        case Mode::SpectrumAnalysis:
            return "SPEC";
    }
    return "?";
}

double ReceiverModel::channel_bandwidth() const {
    switch (mode_) {
        case Mode::AMAudio: return spec_for_am(am_config_).bandwidth_hz;
        case Mode::NarrowbandFMAudio: return spec_for_nfm(nfm_config_).bandwidth_hz;
        case Mode::WidebandFMAudio: return spec_for_wfm(wfm_config_).bandwidth_hz;
        case Mode::SpectrumAnalysis: return sample_rate_;
    }
    return 0.0;
}

/* --- Control --------------------------------------------------------------- */

bool ReceiverModel::start() {
    if (running_.load()) return true;

    radio_.set_rx_rate(sample_rate_);
    sample_rate_ = radio_.rx_rate();

    /* Let the analog filter pass the whole captured band; anything narrower is
     * the channel filter's job and doing it in the AD936x would fight the NCO. */
    radio_.set_rx_bandwidth(sample_rate_ * 0.8);

    retune_if_needed();
    chain_dirty_.store(true);

    if (!radio_.start_rx()) return false;

    stop_.store(false);
    running_.store(true);
    dsp_thread_ = std::thread(&ReceiverModel::dsp_thread_main, this);
    return true;
}

void ReceiverModel::stop() {
    /* Close the capture file before the DSP thread goes away, so a recording is
     * always flushed rather than truncated. */
    stop_capture();

    if (running_.load()) {
        stop_.store(true);
        if (dsp_thread_.joinable()) dsp_thread_.join();
        running_.store(false);
    } else if (dsp_thread_.joinable()) {
        dsp_thread_.join();
    }

    radio_.stop_rx();
}

void ReceiverModel::set_target_frequency(uint64_t hz) {
    target_frequency_ = hz;
    retune_if_needed();
}

void ReceiverModel::retune_if_needed() {
    const double target = static_cast<double>(target_frequency_);
    const double lo = radio_.rx_frequency();
    const double offset = target - lo;

    /* Keep the wanted signal inside the middle 80% of the captured band; retune
     * the LO only when it would otherwise drift towards the filter skirts. */
    const double window = sample_rate_ * 0.4;

    const bool move_lo = (lo == 0.0) || (std::fabs(offset) > window);
    if (move_lo) radio_.set_rx_frequency(target);

    /* The NCO is the DSP thread's, so take the chain lock before touching it.
     * Tuning happens on the UI thread and mixing happens on the DSP thread;
     * without this they race on the oscillator's phase state. */
    std::lock_guard<std::mutex> g{chain_mutex_};
    if (move_lo) {
        nco_.set_frequency(0.0, sample_rate_);
    } else {
        /* Mixing by -offset brings the signal from +offset down to baseband. */
        nco_.set_frequency(-offset, sample_rate_);
    }
}

/* The mode selectors are written from the UI thread and read by the DSP thread
 * inside its chain_mutex_ section, so they take the same lock. They are driven
 * by user input, so the cost is irrelevant. */

void ReceiverModel::set_mode(Mode mode) {
    {
        std::lock_guard<std::mutex> g{chain_mutex_};
        if (mode == mode_) return;
        mode_ = mode;
    }
    chain_dirty_.store(true);
}

void ReceiverModel::set_am_configuration(AmConfig cfg) {
    {
        std::lock_guard<std::mutex> g{chain_mutex_};
        if (cfg == am_config_) return;
        am_config_ = cfg;
    }
    chain_dirty_.store(true);
}

void ReceiverModel::set_nfm_configuration(NfmConfig cfg) {
    {
        std::lock_guard<std::mutex> g{chain_mutex_};
        if (cfg == nfm_config_) return;
        nfm_config_ = cfg;
    }
    chain_dirty_.store(true);
}

void ReceiverModel::set_wfm_configuration(WfmConfig cfg) {
    {
        std::lock_guard<std::mutex> g{chain_mutex_};
        if (cfg == wfm_config_) return;
        wfm_config_ = cfg;
    }
    chain_dirty_.store(true);
}

void ReceiverModel::set_gain(double db) { radio_.set_rx_gain(db); }
double ReceiverModel::gain() const { return radio_.rx_gain(); }

void ReceiverModel::set_agc(bool enabled) {
    hw_agc_ = enabled;
    radio_.set_rx_agc(enabled);
}

void ReceiverModel::set_audio_agc(bool enabled) {
    std::lock_guard<std::mutex> g{chain_mutex_};
    audio_agc_enabled_ = enabled;
    agc_.set_enabled(enabled);
}

void ReceiverModel::set_squelch_level(uint8_t level_0_99) {
    std::lock_guard<std::mutex> g{chain_mutex_};
    squelch_level_ = std::min<uint8_t>(level_0_99, 99);
    squelch_.set_level(squelch_level_);
}

void ReceiverModel::set_volume(uint8_t volume_0_99) { audio_.set_volume(volume_0_99); }
uint8_t ReceiverModel::volume() const { return audio_.volume(); }

void ReceiverModel::set_sampling_rate(double hz) {
    if (hz == sample_rate_) return;

    const bool was_running = running_.load();
    if (was_running) stop();

    sample_rate_ = radio_.set_rx_rate(hz);
    radio_.set_rx_bandwidth(sample_rate_ * 0.8);
    chain_dirty_.store(true);

    if (was_running) start();
}

bool ReceiverModel::take_spectrum_samples(std::vector<dsp::cfloat>& out, size_t count) {
    std::lock_guard<std::mutex> g{spectrum_mutex_};
    if (!spectrum_ready_ || count == 0) return false;
    if (count > spectrum_buffer_.size()) count = spectrum_buffer_.size();

    out.assign(spectrum_buffer_.end() - static_cast<ptrdiff_t>(count),
               spectrum_buffer_.end());
    spectrum_ready_ = false;
    return true;
}

/* --- IQ capture ------------------------------------------------------------ */

bool ReceiverModel::start_capture(const std::string& path_stem) {
    stop_capture();

    std::lock_guard<std::mutex> g{capture_mutex_};
    capture_error_.clear();

    const std::string data_path = path_stem + ".C16";
    const std::string meta_path = path_stem + ".TXT";

    auto file = std::make_unique<std::ofstream>(data_path, std::ios::binary | std::ios::trunc);
    if (!file->is_open()) {
        capture_error_ = "cannot create " + data_path;
        return false;
    }

    /* A large stream buffer keeps the DSP thread from hitting the disk on every
     * block; at 2.4 Msps this path moves ~9.6 MB/s. */
    static thread_local std::vector<char> unused;
    file->rdbuf()->pubsetbuf(nullptr, 1 << 20);

    /* Metadata in Mayhem's format, so captures open in its viewer and in the
     * usual host-side tooling. */
    std::ofstream meta(meta_path, std::ios::trunc);
    if (!meta.is_open()) {
        capture_error_ = "cannot create " + meta_path;
        return false;
    }
    meta << "center_frequency=" << target_frequency_ << "\n";
    meta << "sample_rate=" << static_cast<uint64_t>(std::llround(sample_rate_)) << "\n";
    meta.close();

    capture_file_ = std::move(file);
    capture_path_ = data_path;
    captured_bytes_.store(0);
    capturing_.store(true);
    return true;
}

void ReceiverModel::stop_capture() {
    std::lock_guard<std::mutex> g{capture_mutex_};
    capturing_.store(false);
    if (capture_file_) {
        capture_file_->flush();
        capture_file_->close();
        capture_file_.reset();
    }
}

/* --- Chain construction ---------------------------------------------------- */

void ReceiverModel::rebuild_chain() {
    std::lock_guard<std::mutex> g{chain_mutex_};

    ChannelSpec spec{};
    switch (mode_) {
        case Mode::AMAudio: spec = spec_for_am(am_config_); break;
        case Mode::NarrowbandFMAudio: spec = spec_for_nfm(nfm_config_); break;
        case Mode::WidebandFMAudio: spec = spec_for_wfm(wfm_config_); break;
        case Mode::SpectrumAnalysis: spec = {sample_rate_, 0.0, 0.0, 0.0}; break;
    }

    /* Pick a channel rate at least 2.5x the channel bandwidth so the filter has
     * room for its transition band, then decimate by the largest integer that
     * still clears it. */
    const double min_channel_rate = std::max(spec.bandwidth_hz * 2.5, 24000.0);
    size_t decim = 1;
    if (sample_rate_ > min_channel_rate)
        decim = static_cast<size_t>(std::floor(sample_rate_ / min_channel_rate));
    if (decim < 1) decim = 1;

    const double channel_rate = sample_rate_ / static_cast<double>(decim);
    channel_rate_.store(channel_rate);

    if (mode_ == Mode::SpectrumAnalysis) {
        /* Nothing to demodulate; the DSP thread only feeds the spectrum tap. */
        return;
    }

    /* Channel filter: half the channel bandwidth on each side of DC, with a
     * transition band a quarter as wide again. */
    const double cutoff = spec.bandwidth_hz / 2.0;
    const double transition = std::max(cutoff * 0.25, 500.0);
    channel_filter_.configure(
        dsp::design_lowpass(cutoff, transition, sample_rate_, 60.0, 1023), decim);

    /* Audio side: decimate the demodulated audio down towards 48 kHz. */
    size_t audio_decim = 1;
    if (channel_rate > 48000.0)
        audio_decim = static_cast<size_t>(std::floor(channel_rate / 48000.0));
    if (audio_decim < 1) audio_decim = 1;

    const double audio_rate = channel_rate / static_cast<double>(audio_decim);
    const double audio_cutoff = std::min(spec.audio_hz, audio_rate * 0.45);
    audio_filter_.configure(
        dsp::design_lowpass(audio_cutoff, audio_cutoff * 0.3, channel_rate, 60.0, 511),
        audio_decim);

    switch (mode_) {
        case Mode::AMAudio:
            if (am_config_ == AmConfig::USB || am_config_ == AmConfig::CW) {
                ssb_.configure(static_cast<float>(channel_rate),
                               dsp::SsbDemod::Sideband::Upper, 127);
            } else if (am_config_ == AmConfig::LSB) {
                ssb_.configure(static_cast<float>(channel_rate),
                               dsp::SsbDemod::Sideband::Lower, 127);
            } else {
                am_.configure(static_cast<float>(channel_rate));
            }
            break;

        case Mode::NarrowbandFMAudio:
        case Mode::WidebandFMAudio:
            fm_.configure(static_cast<float>(channel_rate),
                          static_cast<float>(spec.deviation_hz));
            break;

        default:
            break;
    }

    if (spec.deemph_us > 0.0)
        deemph_.configure(static_cast<float>(spec.deemph_us), static_cast<float>(audio_rate));
    else
        deemph_.configure(0.0f, static_cast<float>(audio_rate));

    agc_.configure(static_cast<float>(audio_rate), 5.0f, 300.0f, 0.35f, 64.0f);
    agc_.set_enabled(audio_agc_enabled_);

    squelch_.configure(static_cast<float>(channel_rate));
    squelch_.set_level(squelch_level_);

    resampler_.configure(audio_rate, static_cast<double>(audio::sample_rate));

    nco_.set_frequency(nco_.frequency(), sample_rate_);
}

/* --- DSP thread ------------------------------------------------------------ */

void ReceiverModel::dsp_thread_main() {
    std::vector<dsp::cfloat> raw(kBlockSamples);
    std::vector<dsp::cfloat> mixed(kBlockSamples);
    std::vector<dsp::cfloat> channel;
    std::vector<float> demodulated;
    std::vector<float> audio_block;
    std::vector<float> resampled;

    channel.reserve(kBlockSamples);
    demodulated.reserve(kBlockSamples);
    audio_block.reserve(kBlockSamples);
    resampled.reserve(kBlockSamples);

    auto& ring = radio_.rx_ring();

    while (!stop_.load()) {
        if (chain_dirty_.exchange(false)) rebuild_chain();

        const size_t got = ring.read(raw.data(), raw.size());
        if (got == 0) {
            /* Nothing buffered yet — yield rather than spin. At 2.4 Msps a full
             * block arrives every ~3.4 ms, so 1 ms keeps latency low. */
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        /* Spectrum tap: keep the most recent raw samples, pre-channel-filter,
         * so the waterfall shows the whole captured band. */
        {
            std::lock_guard<std::mutex> g{spectrum_mutex_};
            const size_t n = std::min(got, spectrum_buffer_.size());
            /* Slide the buffer and append the newest n samples. */
            if (n < spectrum_buffer_.size()) {
                std::move(spectrum_buffer_.begin() + static_cast<ptrdiff_t>(n),
                          spectrum_buffer_.end(), spectrum_buffer_.begin());
            }
            std::copy(raw.data() + (got - n), raw.data() + got,
                      spectrum_buffer_.end() - static_cast<ptrdiff_t>(n));
            spectrum_ready_ = true;
        }

        /* Capture tap, before any channel filtering, so the file holds the full
         * captured band exactly as the radio delivered it. */
        if (capturing_.load()) {
            std::lock_guard<std::mutex> g{capture_mutex_};
            if (capture_file_ && capture_file_->good()) {
                capture_scratch_.resize(got * 2);
                for (size_t i = 0; i < got; i++) {
                    /* fc32 samples are nominally in [-1, 1]; scale to int16 and
                     * clamp so an overdriven front end saturates rather than
                     * wrapping to the opposite rail. */
                    const float re = std::clamp(raw[i].real(), -1.0f, 1.0f);
                    const float im = std::clamp(raw[i].imag(), -1.0f, 1.0f);
                    capture_scratch_[i * 2 + 0] = static_cast<int16_t>(std::lrint(re * 32767.0f));
                    capture_scratch_[i * 2 + 1] = static_cast<int16_t>(std::lrint(im * 32767.0f));
                }
                const auto bytes = capture_scratch_.size() * sizeof(int16_t);
                capture_file_->write(reinterpret_cast<const char*>(capture_scratch_.data()),
                                     static_cast<std::streamsize>(bytes));
                if (capture_file_->good())
                    captured_bytes_ += bytes;
                else
                    capturing_.store(false);  /* disk full or gone: stop cleanly */
            }
        }

        std::lock_guard<std::mutex> g{chain_mutex_};

        if (mode_ == Mode::SpectrumAnalysis) continue;

        nco_.mix(raw.data(), mixed.data(), got);

        channel.clear();
        channel_filter_.process(mixed.data(), got, channel);
        if (channel.empty()) continue;

        const float level = dsp::rms(channel.data(), channel.size());
        channel_level_db_.store(dsp::to_db(level));
        const bool open = squelch_.update(level);
        squelch_open_.store(open);

        demodulated.clear();
        switch (mode_) {
            case Mode::AMAudio:
                if (am_config_ == AmConfig::USB || am_config_ == AmConfig::LSB ||
                    am_config_ == AmConfig::CW) {
                    ssb_.process(channel.data(), channel.size(), demodulated);
                } else {
                    am_.process(channel.data(), channel.size(), demodulated);
                }
                break;

            case Mode::NarrowbandFMAudio:
            case Mode::WidebandFMAudio:
                fm_.process(channel.data(), channel.size(), demodulated);
                break;

            default:
                break;
        }

        if (demodulated.empty()) continue;

        audio_block.clear();
        audio_filter_.process(demodulated.data(), demodulated.size(), audio_block);
        if (audio_block.empty()) continue;

        deemph_.process(audio_block.data(), audio_block.size());
        agc_.process(audio_block.data(), audio_block.size());

        /* A closed squelch mutes rather than stopping the chain, so filter and
         * AGC state stay warm and the first syllable after it opens is clean. */
        if (!open) std::fill(audio_block.begin(), audio_block.end(), 0.0f);

        resampled.clear();
        resampler_.process(audio_block.data(), audio_block.size(), resampled);
        if (resampled.empty()) continue;

        const size_t written = audio_.write(resampled.data(), resampled.size());
        if (written < resampled.size())
            audio_dropped_ += static_cast<uint32_t>(resampled.size() - written);
    }
}

}  // namespace radio
