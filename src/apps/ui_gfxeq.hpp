/*
 * mayhem-b200 — gfxEQ, the audio spectrum bar visualiser.
 *
 * Ported from firmware/application/external/gfxeq (RocketGod / HTotoo). The
 * widget itself (GraphEq) is Phase A's ui_widget_extra.hpp; what this file adds
 * is the app around it and, more importantly, the *producer* of the
 * AudioSpectrum it eats.
 *
 * Where the spectrum comes from
 * -----------------------------
 * On a PortaPack the M4 runs baseband/proc_wfm_audio.cpp, which taps the 96 kHz
 * audio (right after the second CIC decimator, before the <15 kHz audio filter —
 * so the 19 kHz pilot and the 38 kHz stereo subcarrier are still in it), pushes
 * 256 samples into a 256-point FFT and posts an AudioSpectrumMessage. One bin is
 * therefore 96000/256 = 375 Hz, which is exactly what GraphEq's band table
 * assumes when it divides by 48000/128.
 *
 * There is no M4 here and no message queue. radio::ReceiverModel demodulates
 * internally and hands audio straight to waveOut; it exposes no audio tap. What
 * it does expose is take_spectrum_samples() — the raw, pre-channel-filter
 * wideband IQ. So this app re-implements the WFM chain on that tap:
 *
 *   raw IQ @ Fs -> NCO (LO offset) -> channel FIR /4 -> 384 kHz
 *               -> FM discriminator -> audio FIR /4 -> 96 kHz
 *               -> 256-point FFT -> upstream's dB->byte curve -> AudioSpectrum
 *
 * The two intermediate rates (384 kHz demod, 96 kHz spectrum feed) are upstream's
 * own, so the display means the same thing it does on a PortaPack.
 *
 * There is no squelch anywhere in this path, which is upstream's behaviour too:
 * a phase discriminator is amplitude-blind, so tuning off a station does not
 * darken the display — it fills it with the distorted remains of whatever the
 * channel filter is still passing. tests/test_gfxeq.cpp pins both cases.
 *
 * IDEAL TAP: a ReceiverModel::take_audio_samples(buf, n) returning the audio the
 * receiver has *already* demodulated. That would remove this second demodulator
 * entirely and would let gfxEQ visualise AM and NFM too. Until that exists, the
 * app demodulates WFM itself and only works in WFM.
 *
 * Copyright (C) 2025 RocketGod, HTotoo (original app and widget)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_GFXEQ_H__
#define __MB200_UI_GFXEQ_H__

#include "../core/settings.hpp"
#include "../dsp/demod.hpp"
#include "../dsp/fft.hpp"
#include "../dsp/fir.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_navigation.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* --- AudioSpectrumAnalyzer -------------------------------------------------
 * The host stand-in for the AudioSpectrum block of proc_wfm_audio.cpp.
 *
 * Feed it mono audio at kAudioRate; every kFftSize samples it publishes a fresh
 * ui::AudioSpectrum. The dB scale is upstream's exactly:
 *
 *   upstream: complex_audio[i] = int16_audio[i] / 32
 *             X = FFT_256(complex_audio)            (unnormalised)
 *             mag2 = |X / 32768|^2
 *             db   = 10*log10(mag2)                 (mag2_to_dbv_norm)
 *             v    = clamp(db * 5 + 255, 0, 255)
 *
 * With float audio s[] in [-1,1], int16_audio = s * 32768, so |X|/32768 reduces
 * to |FFT_256(s)| / 32 — that is kFftMagScale. A 0.25-amplitude sine therefore
 * reads exactly 0 dB / 255, and every 0.2 dB below that costs one count. */
class AudioSpectrumAnalyzer {
   public:
    static constexpr size_t kFftSize = 256;
    static constexpr double kAudioRate = 96000.0;
    /* 96000 / 256. GraphEq's FREQUENCY_BANDS are quoted in Hz against this. */
    static constexpr float kBinHz = 375.0f;
    /* proc_wfm_audio.cpp's mag_scale. */
    static constexpr float kMagScale = 5.0f;
    /* 1024/32768: upstream's /32 pre-scale against its /32768 reference. */
    static constexpr float kFftMagScale = 1.0f / 32.0f;

    AudioSpectrumAnalyzer();

    /* Appends audio; returns true if a new spectrum was published by this call.
     * A null pointer or a zero count is a no-op, not a crash. */
    bool feed(const float* samples, size_t count);

    void reset();

    const ui::AudioSpectrum& spectrum() const { return spectrum_; }

    /* Samples buffered towards the next transform. */
    size_t pending() const { return pending_count_; }

    /* Upstream's dB -> 0..255 curve. Exposed because it is the piece worth
     * testing on its own.
     *
     * Deviation: upstream assigns a negative float to an unsigned int and then
     * clamps, which is only well defined because the M4's VCVT saturates to 0.
     * Clamping in float first gives the same answers without the UB. */
    static uint8_t db_to_byte(float db);

    /* Index of the bin holding `hz`, matching GraphEq's own arithmetic. */
    static size_t bin_for_hz(float hz);

   private:
    void analyze(const float* block);

    dsp::Fft fft_{kFftSize};
    std::vector<dsp::cfloat> work_{};
    std::array<float, kFftSize> buffer_{};
    size_t pending_count_{0};
    ui::AudioSpectrum spectrum_{};
};

/* --- WfmAudioTap -----------------------------------------------------------
 * Wideband IQ in, 96 kHz mono audio out. This is the host equivalent of
 * proc_wfm_audio's decim_0 -> decim_1 -> FM -> audio_dec_1 -> audio_dec_2,
 * collapsed into two FIR decimators because the host designs its taps at run
 * time and does not need the fixed 4/2/2/2 cascade. */
class WfmAudioTap {
   public:
    /* Upstream's WFM demodulator input rate. */
    static constexpr double kDemodRate = 384000.0;
    /* set_wfm_configuration(1) in upstream's constructor: the 180 kHz filter. */
    static constexpr double kChannelBandwidth = 180000.0;
    /* Broadcast FM peak deviation. */
    static constexpr double kDeviation = 75000.0;

    void configure(double input_rate_hz);
    void reset();

    bool configured() const { return configured_; }

    /* Offset of the wanted signal above the LO, in Hz. The tap mixes it down to
     * baseband, exactly as ReceiverModel's own NCO does. */
    void set_offset(double offset_hz);
    double offset() const { return offset_hz_; }

    double input_rate() const { return input_rate_; }
    double channel_rate() const { return channel_rate_; }
    double audio_rate() const { return audio_rate_; }
    size_t channel_decimation() const { return channel_decimation_; }
    size_t audio_decimation() const { return audio_decimation_; }

    /* Replaces `out` with the audio produced by this block. Returns its size. */
    size_t process(const dsp::cfloat* in, size_t count, std::vector<float>& out);

   private:
    dsp::Nco nco_{};
    dsp::FirDecimateC channel_{};
    dsp::FmDemod fm_{};
    dsp::FirDecimateR audio_{};
    dsp::Resampler resampler_{};

    std::vector<dsp::cfloat> mixed_{};
    std::vector<dsp::cfloat> channel_out_{};
    std::vector<float> demodulated_{};
    std::vector<float> decimated_{};

    double input_rate_{0.0};
    double channel_rate_{0.0};
    double audio_rate_{0.0};
    double offset_hz_{0.0};
    size_t channel_decimation_{1};
    size_t audio_decimation_{1};
    bool needs_resample_{false};
    bool configured_{false};
};

/* --- Themes ----------------------------------------------------------------
 * The twenty (base, peak) colour pairs the MOOD button cycles, verbatim from
 * upstream's `themes` array. */
struct GfxEqTheme {
    ui::Color base;
    ui::Color peak;
};

size_t gfxeq_theme_count();
/* Wraps, so callers can just increment an index. */
const GfxEqTheme& gfxeq_theme(size_t index);

/* --- The app --------------------------------------------------------------- */

class GfxEqView : public ui::View {
   public:
    GfxEqView();
    ~GfxEqView() override;

    std::string title() const override { return "gfxEQ"; }

    void on_show() override;
    void on_frame_sync() override;

    bool on_key(const ui::KeyEvent key) override;
    bool on_encoder(const ui::EncoderEvent delta) override;

    /* Upstream's default: 93.1 MHz. */
    static constexpr uint64_t kDefaultFrequency = 93'100'000;
    /* 1.536 Msps is 4 x 384 kHz x ... : it decimates by 4 to upstream's demod
     * rate and again by 4 to its 96 kHz spectrum rate, and one 4096-sample tap
     * read is then exactly one 256-point transform. */
    static constexpr double kSampleRate = 1'536'000.0;
    static constexpr size_t kTapSamples = 4096;

   private:
    void cycle_theme();
    void apply_theme();
    void update_status();

    radio::ReceiverModel& receiver_;

    uint32_t current_theme_{0};
    uint64_t frequency_value_{kDefaultFrequency};

    /* Declared after the two values above so the bindings are valid when the
     * store loads them in its constructor. Same store name as upstream. */
    core::SettingsStore settings_{
        "rx_gfx_eq",
        {{"theme", &current_theme_}, {"frequency", &frequency_value_}}};

    ui::FrequencyField field_frequency_{{0, 4}};

    ui::Labels labels_{
        {{84, 4}, "G", ui::Color::light_grey()},
        {{116, 4}, "V", ui::Color::light_grey()},
    };

    ui::NumberField field_gain_{{94, 4}, 2, {0, 76}, 1, ' '};
    ui::NumberField field_volume_{{128, 4}, 2, {0, 99}, 1, ' '};
    ui::Button button_mood_{{152, 4, 84, 16}, "MOOD"};

    ui::GraphEq graph_{{2, 24, 236, 260}};
    ui::Text text_status_{{0, 286, 240, 16}, ""};

    AudioSpectrumAnalyzer analyzer_{};
    WfmAudioTap tap_{};

    std::vector<dsp::cfloat> samples_{};
    std::vector<float> audio_{};

    double last_offset_{0.0};
    uint32_t frame_counter_{0};
    uint32_t starved_frames_{0};
    bool offset_valid_{false};
};

}  // namespace app

#endif /*__MB200_UI_GFXEQ_H__*/
