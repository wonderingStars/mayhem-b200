/*
 * mayhem-b200 — transmit chain.
 *
 * The host counterpart of the firmware's TransmitterModel plus the M4 transmit
 * processors (proc_mictx, proc_audiotx, proc_siggen, proc_replay). Modes mirror
 * Mayhem's microphone transmitter — NFM at three deviations, WFM, AM, DSB, USB
 * and LSB — with an unmodulated-carrier mode and a raw mode for apps that
 * generate their own complex baseband.
 *
 * The signal path, per block:
 *
 *   audio source (48 kHz) -> gain -> resample to modulator rate
 *        -> anti-image / bandwidth-limiting lowpass -> CTCSS or DCS sub-tone
 *        -> modulator -> interpolate to the B200 rate -> NCO -> tx_ring
 *
 * Modulating at the full B200 rate would be wasteful: at 2 Msps an NFM signal
 * occupies less than one percent of the band. So the modulator runs at a rate a
 * few times the channel bandwidth and a polyphase interpolator lifts the result,
 * which is the transmit mirror of what the receiver's channel decimator does.
 *
 * The NCO plays the same role it does on receive: small frequency moves inside
 * the streamed bandwidth are done in the DSP, and the AD936x LO is only touched
 * when the target leaves the window. On transmit it earns its keep twice over,
 * because the LO leakage of a direct-conversion transmitter lands at the LO
 * frequency — offsetting the LO puts that spur outside the transmitted channel.
 *
 * Raw mode exists so replay and arbitrary-waveform apps can drive the same
 * radio: they supply complex samples at the streamed sample rate and the model
 * handles gain, tuning and the ring.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_TRANSMITTER_MODEL_H__
#define __MB200_TRANSMITTER_MODEL_H__

#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "../dsp/modulate.hpp"
#include "usrp_radio.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace radio {

class TransmitterModel {
   public:
    /* Mayhem's microphone transmitter offers FM, AM, DSB, USB and LSB; the two
     * extra entries are the host additions that the firmware handles with
     * separate baseband images (proc_siggen's CW, proc_replay's raw stream). */
    enum class Mode : uint8_t {
        NarrowbandFM = 0,
        WidebandFM = 1,
        AM = 2,
        DSB = 3,
        USB = 4,
        LSB = 5,
        CW = 6,   /* unmodulated carrier */
        Raw = 7,  /* caller-supplied complex baseband */
    };

    /* Deviation/bandwidth pairings for the three narrowband channel spacings,
     * the transmit side of ReceiverModel::NfmConfig. */
    enum class NfmConfig : uint8_t {
        Narrow8k5 = 0,  /* 2.5 kHz deviation */
        Medium11k = 1,  /* 3.5 kHz deviation */
        Wide16k = 2,    /* 5 kHz deviation */
    };

    enum class SubTone : uint8_t { None = 0, Ctcss = 1, Dcs = 2 };

    /* Fills `out` with up to `count` mono samples in [-1, 1] at
     * audio::sample_rate and returns how many it wrote. A short return is
     * treated as silence for the remainder and counted as an underrun. Called
     * on the DSP thread. */
    using AudioSource = std::function<size_t(float* out, size_t count)>;

    /* Fills `out` with up to `count` complex samples at sampling_rate(), scaled
     * so that unit magnitude is full output. Called on the DSP thread. */
    using IqSource = std::function<size_t(cfloat* out, size_t count)>;

    explicit TransmitterModel(UsrpRadio& radio);
    ~TransmitterModel();

    TransmitterModel(const TransmitterModel&) = delete;
    TransmitterModel& operator=(const TransmitterModel&) = delete;

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

    void set_nfm_configuration(NfmConfig cfg);
    NfmConfig nfm_configuration() const { return nfm_config_; }

    /* Overrides the deviation the mode would otherwise imply. Zero restores the
     * mode's default. FM modes only. */
    void set_deviation(double hz);
    double deviation() const;

    /* AM modulation index, 0..1. */
    void set_am_depth(float depth);
    float am_depth() const { return am_depth_; }

    /* Output gain in dB at the AD936x. The B200's TX gain range comes from
     * caps().tx_gain; it is not the same control as amplitude(). */
    void set_gain(double db);
    double gain() const;

    /* Digital scaling of the baseband before it reaches the radio, 0..1.
     * Backing off from 1.0 leaves headroom for the interpolator's overshoot and
     * keeps the DAC out of clipping. */
    void set_amplitude(float amplitude);
    float amplitude() const { return amplitude_; }

    /* Linear gain applied to the audio source before modulation, the equivalent
     * of Mayhem's audio_gain. */
    void set_audio_gain(float gain);
    float audio_gain() const { return audio_gain_; }

    /* Streamed bandwidth. Wider costs CPU in the interpolator and buys nothing
     * for a narrowband mode; it matters for raw playback of a wide capture. */
    void set_sampling_rate(double hz);
    double sampling_rate() const { return sample_rate_; }

    /* Analog TX filter width to ask the device for. Zero (the default) tracks
     * the mode's channel bandwidth so out-of-channel noise is filtered in
     * hardware too.
     *
     * bandwidth() reports the *request*. The AD936x has a minimum filter width
     * of its own — 200 kHz on a B200 — so a narrowband mode always ends up
     * wider than asked; UsrpRadio::tx_bandwidth() is what the device settled
     * on. */
    void set_bandwidth(double hz);
    double bandwidth() const;

    /* --- Sub-audible signalling (FM modes) --- */

    void set_sub_tone_none();
    void set_ctcss(float frequency_hz, float mix_weight = 0.15f);
    void set_dcs(uint16_t code, float mix_weight = 0.15f);
    SubTone sub_tone() const { return sub_tone_; }
    float sub_tone_mix() const { return sub_tone_mix_; }
    float ctcss_frequency() const { return ctcss_frequency_; }
    uint16_t dcs_code() const { return dcs_code_; }

    /* --- Sources --- */

    /* Either may be set at any time, including while running. Passing an empty
     * function removes the source: the audio path then transmits silence (a
     * carrier, for AM) and the raw path transmits nothing. */
    void set_audio_source(AudioSource source);
    void set_iq_source(IqSource source);
    bool has_audio_source() const;
    bool has_iq_source() const;

    /* --- Readouts --- */

    /* Peak absolute value of the most recent audio block, for a VU meter — the
     * host equivalent of Mayhem's AudioLevelReportMessage. */
    float audio_level() const { return audio_level_.load(); }

    /* RMS of the most recent baseband block handed to the radio, in dBFS. */
    float baseband_level_db() const { return baseband_level_db_.load(); }

    /* Blocks where the source could not supply everything asked for. */
    uint32_t source_underruns() const { return source_underruns_.load(); }

    /* Samples the DSP thread could not fit into the TX ring. Non-zero means the
     * radio is not draining, which should not happen while it is streaming. */
    uint32_t dropped() const { return dropped_.load(); }

    uint64_t samples_generated() const { return samples_generated_.load(); }

    /* Rate the modulator itself runs at, and the interpolation factor from
     * there up to sampling_rate(). */
    double modulator_rate() const { return modulator_rate_.load(); }
    size_t interpolation() const { return interpolation_.load(); }

    /* Human-readable mode name, e.g. "NFM 11k". */
    std::string mode_name() const;

    /* Two-sided occupied bandwidth of the current mode, in Hz. */
    double channel_bandwidth() const;

    static const char* mode_label(Mode m);

   private:
    void dsp_thread_main();
    void rebuild_chain();
    void retune_if_needed();
    bool modulates_audio() const;

    UsrpRadio& radio_;

    std::thread dsp_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};

    /* Guards the DSP objects and the configuration they are built from. */
    std::mutex chain_mutex_;
    std::atomic<bool> chain_dirty_{true};

    /* Separate from chain_mutex_ because a source callback belongs to the app
     * and may do slow things — read a file, wait on a decoder. Holding the
     * chain lock across it would stall every UI-thread setter. */
    mutable std::mutex source_mutex_;
    AudioSource audio_source_{};
    IqSource iq_source_{};

    /* Configuration */
    uint64_t target_frequency_{446'000'000};
    uint64_t frequency_step_{12'500};
    Mode mode_{Mode::NarrowbandFM};
    NfmConfig nfm_config_{NfmConfig::Medium11k};
    double sample_rate_{2'000'000.0};
    double deviation_override_{0.0};
    double bandwidth_override_{0.0};
    float am_depth_{1.0f};
    float audio_gain_{1.0f};
    float amplitude_{0.85f};

    SubTone sub_tone_{SubTone::None};
    float sub_tone_mix_{0.15f};
    float ctcss_frequency_{0.0f};
    uint16_t dcs_code_{0};

    /* DSP chain — only touched by the DSP thread, or under chain_mutex_. */
    dsp::Resampler audio_resampler_{};
    dsp::FirR audio_filter_{};
    dsp::ToneGen ctcss_{};
    dsp::DcsEncoder dcs_{};
    dsp::FmModulator fm_{};
    dsp::AmModulator am_{};
    dsp::SsbModulator ssb_{};
    dsp::FirInterpolateC interpolator_{};
    dsp::Nco nco_{};

    /* Audio samples pulled per iteration, sized in rebuild_chain() so one
     * iteration yields roughly a constant number of output samples whatever the
     * streamed rate is, and the number of output samples that produces. The
     * second one sets how much room the TX ring must have before another block
     * is generated. */
    std::atomic<size_t> audio_block_{256};
    std::atomic<size_t> block_output_{8192};

    std::atomic<double> modulator_rate_{0.0};
    std::atomic<size_t> interpolation_{1};
    std::atomic<float> audio_level_{0.0f};
    std::atomic<float> baseband_level_db_{-140.0f};
    std::atomic<uint32_t> source_underruns_{0};
    std::atomic<uint32_t> dropped_{0};
    std::atomic<uint64_t> samples_generated_{0};
};

}  // namespace radio

#endif /*__MB200_TRANSMITTER_MODEL_H__*/
