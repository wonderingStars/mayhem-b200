/*
 * mayhem-b200 — receive chain.
 *
 * The host counterpart of the firmware's ReceiverModel plus its M4 baseband
 * processors. Modes and their sub-configurations deliberately mirror Mayhem's
 * (AM with DSB/USB/LSB/CW variants, NFM at three deviations, WFM, and a
 * spectrum-only mode) so the on-screen controls mean the same thing.
 *
 * The signal path, per block:
 *
 *   B200 -> NCO fine tune -> channel decimation FIR -> demodulator
 *        -> audio lowpass -> de-emphasis -> AGC -> resample to 48 kHz -> waveOut
 *
 * The NCO matters more here than on a PortaPack: retuning an AD936x costs
 * milliseconds and a settling transient, so small tuning steps inside the
 * captured bandwidth are done in the DSP and the LO is only moved when the
 * target leaves the window.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_RECEIVER_MODEL_H__
#define __MB200_RECEIVER_MODEL_H__

#include "../audio/audio_out.hpp"
#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "usrp_radio.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace radio {

class ReceiverModel {
   public:
    /* Matches ReceiverModel::Mode in the firmware. */
    enum class Mode : uint8_t {
        AMAudio = 0,
        NarrowbandFMAudio = 1,
        WidebandFMAudio = 2,
        SpectrumAnalysis = 3,
    };

    /* AM sub-configurations, as Mayhem presents them. */
    enum class AmConfig : uint8_t {
        DSB9k = 0,
        DSB6k = 1,
        USB = 2,
        LSB = 3,
        CW = 4,
    };

    enum class NfmConfig : uint8_t {
        Narrow8k5 = 0,
        Medium11k = 1,
        Wide16k = 2,
    };

    enum class WfmConfig : uint8_t {
        Wide200k = 0,
        Narrow180k = 1,
    };

    ReceiverModel(UsrpRadio& radio, audio::AudioOut& audio_out);
    ~ReceiverModel();

    ReceiverModel(const ReceiverModel&) = delete;
    ReceiverModel& operator=(const ReceiverModel&) = delete;

    /* --- Control --- */

    bool start();
    void stop();
    bool running() const { return running_.load(); }

    /* Frequency the user asked for. The LO may sit elsewhere; the NCO makes up
     * the difference. */
    void set_target_frequency(uint64_t hz);
    uint64_t target_frequency() const { return target_frequency_; }

    void set_frequency_step(uint64_t hz) { frequency_step_ = hz; }
    uint64_t frequency_step() const { return frequency_step_; }

    void set_mode(Mode mode);
    Mode mode() const { return mode_; }

    void set_am_configuration(AmConfig cfg);
    AmConfig am_configuration() const { return am_config_; }

    void set_nfm_configuration(NfmConfig cfg);
    NfmConfig nfm_configuration() const { return nfm_config_; }

    void set_wfm_configuration(WfmConfig cfg);
    WfmConfig wfm_configuration() const { return wfm_config_; }

    /* Front-end gain in dB. The B200 has one continuous gain control rather
     * than the HackRF's separate LNA/VGA/AMP, so Mayhem's three controls
     * collapse to this one plus the AD936x's own AGC. */
    void set_gain(double db);
    double gain() const;

    void set_agc(bool enabled);
    bool agc() const { return hw_agc_; }

    void set_audio_agc(bool enabled);
    bool audio_agc() const { return audio_agc_enabled_; }

    void set_squelch_level(uint8_t level_0_99);
    uint8_t squelch_level() const { return squelch_level_; }

    void set_volume(uint8_t volume_0_99);
    uint8_t volume() const;

    /* Capture bandwidth. Larger gives a wider waterfall at more CPU cost. */
    void set_sampling_rate(double hz);
    double sampling_rate() const { return sample_rate_; }

    /* --- Readouts --- */

    /* Smoothed channel level in dBFS, after the channel filter — this is the
     * signal-strength figure that tracks the tuned signal rather than the whole
     * captured band. */
    float channel_level_db() const { return channel_level_db_.load(); }

    /* Wideband level straight off the radio, in dBFS. */
    float rf_level_db() const { return radio_.rx_level_db(); }

    bool squelch_open() const { return squelch_open_.load(); }

    double channel_rate() const { return channel_rate_.load(); }

    /* Number of audio samples the DSP thread failed to hand to waveOut because
     * the output ring was full. Non-zero means the machine is not keeping up. */
    uint32_t audio_dropped() const { return audio_dropped_.load(); }

    /* Copies the most recent block of raw (pre-channel-filter) samples for the
     * spectrum display. Returns false if none are ready yet. */
    bool take_spectrum_samples(std::vector<dsp::cfloat>& out, size_t count);

    /* --- IQ capture ---
     * Writes the raw captured band to `<path>.C16` as interleaved int16 I/Q,
     * plus a Mayhem-compatible `<path>.TXT` holding center_frequency and
     * sample_rate. Unlike the PortaPack, capture runs alongside demodulation:
     * the samples are already in the DSP thread's hands, so tapping them costs
     * only the file write.
     *
     * `path_stem` is the file name without extension. */
    bool start_capture(const std::string& path_stem);
    void stop_capture();
    bool capturing() const { return capturing_.load(); }
    uint64_t captured_bytes() const { return captured_bytes_.load(); }
    const std::string& capture_path() const { return capture_path_; }
    const std::string& capture_error() const { return capture_error_; }

    /* Human-readable name of the current mode + sub-config, e.g. "NFM 11k". */
    std::string mode_name() const;

    /* Bandwidth of the current demodulator's channel filter, in Hz. */
    double channel_bandwidth() const;

    static const char* mode_label(Mode m);

   private:
    void dsp_thread_main();
    void rebuild_chain();
    void retune_if_needed();

    UsrpRadio& radio_;
    audio::AudioOut& audio_;

    std::thread dsp_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};

    /* Guards the filter/demod objects against reconfiguration mid-block. */
    std::mutex chain_mutex_;
    std::atomic<bool> chain_dirty_{true};

    /* Configuration */
    uint64_t target_frequency_{100'000'000};
    uint64_t frequency_step_{25'000};
    Mode mode_{Mode::NarrowbandFMAudio};
    AmConfig am_config_{AmConfig::DSB9k};
    NfmConfig nfm_config_{NfmConfig::Medium11k};
    WfmConfig wfm_config_{WfmConfig::Wide200k};
    double sample_rate_{2'400'000.0};
    uint8_t squelch_level_{0};
    bool hw_agc_{false};
    bool audio_agc_enabled_{true};

    /* DSP chain — only touched by the DSP thread, or under chain_mutex_. */
    dsp::Nco nco_{};
    dsp::FirDecimateC channel_filter_{};
    dsp::FirDecimateR audio_filter_{};
    dsp::AmDemod am_{};
    dsp::FmDemod fm_{};
    dsp::SsbDemod ssb_{};
    dsp::Deemphasis deemph_{};
    dsp::AudioAgc agc_{};
    dsp::Squelch squelch_{};
    dsp::Resampler resampler_{};

    std::atomic<double> channel_rate_{0.0};
    std::atomic<float> channel_level_db_{-140.0f};
    std::atomic<bool> squelch_open_{true};
    std::atomic<uint32_t> audio_dropped_{0};

    /* Spectrum tap. */
    std::mutex spectrum_mutex_;
    std::vector<dsp::cfloat> spectrum_buffer_;
    bool spectrum_ready_{false};

    /* Capture sink. Opened and closed on the caller's thread, written only by
     * the DSP thread; capture_mutex_ covers the handover. */
    std::mutex capture_mutex_;
    std::unique_ptr<std::ofstream> capture_file_;
    std::vector<int16_t> capture_scratch_;
    std::string capture_path_{};
    std::string capture_error_{};
    std::atomic<bool> capturing_{false};
    std::atomic<uint64_t> captured_bytes_{0};
};

}  // namespace radio

#endif /*__MB200_RECEIVER_MODEL_H__*/
