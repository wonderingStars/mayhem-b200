/*
 * mayhem-b200 — Mic TX: live microphone transmitter.
 *
 * Ported from firmware/application/apps/ui_mictx.* and its baseband
 * baseband/proc_mictx.* + baseband/dsp_modulate.* . The upstream app captures
 * the PortaPack's on-board codec, modulates it (NFM/WFM/AM/USB/LSB/DSB) with a
 * user-set deviation and an optional CTCSS/DCS sub-tone, and keys the HackRF
 * transmitter from a PTT button (or voice activation). Here the codec is a
 * Windows capture device (audio::AudioIn) and the M4 baseband is the host
 * radio::TransmitterModel driven as an AudioSource; the modulation chain is the
 * same one every host TX app uses (dsp/modulate.hpp).
 *
 * The upstream signal maths that ports exactly, and is what tests/test_mictx.cpp
 * pins down, is factored into three free functions below so it can be checked
 * without a UI or a radio:
 *
 *   - mictx_deviation_hz()     upstream sets FM deviation_hz from field_bw (kHz):
 *                              configure_baseband() passes channel_bandwidth()
 *                              as deviation_hz, and field_bw feeds
 *                              set_channel_bandwidth(v*1000). So bw_khz*1000 Hz.
 *   - mictx_tone_mix_weight()  upstream mixes the CTCSS/tone-key sub-tone at
 *                              persistent_memory::tone_mix()/100 (range 10..99,
 *                              reset 20) — see baseband_api.cpp set_audiotx_config
 *                              and tone_gen.cpp (out = in*(1-w) + tone*w).
 *   - mictx_meter_value()      upstream FM VU: value = 4 * mean|sample_8bit|;
 *                              an int16 audio >>8 makes sample_8bit = float*128,
 *                              so value = 512 * mean|sample_float|, 0..255.
 *
 * LEGALITY: transmitting voice on an arbitrary frequency needs a licence and is
 * illegal in most bands. This app transmits ONLY on an explicit user action (the
 * PTT button, or after the user enables VOX) and shows a standing on-screen
 * warning. No hardware is attached during development, so actual radiation is
 * unverified; the modulation and metering maths are real and tested.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2024 Mark Thompson (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_MICTX_H__
#define __MB200_UI_MICTX_H__

#include "../dsp/ring_buffer.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace app {

/* --- Pure signal maths (declared here so tests can reach them) -------------- */

/* FM deviation the transmitter should use for a UI "TX deviation" of `bw_khz`
 * kilohertz. Upstream carries this value as channel_bandwidth() and hands it to
 * the FM modulator as deviation_hz, so it is simply kHz -> Hz. */
double mictx_deviation_hz(uint32_t bw_khz);

/* CTCSS / tone-key mix weight for a tone-mix percentage. Upstream clamps the
 * stored value to 10..99 and divides by 100. */
float mictx_tone_mix_weight(int percent);

/* Microphone VU value in 0..255 for a block of float samples, after `gain` is
 * applied — the host equivalent of upstream's AudioLevelReportMessage. See the
 * file header for the 512x derivation. A null/empty block reads as silence. */
uint8_t mictx_meter_value(const float* samples, size_t count, float gain);

/* --- View ------------------------------------------------------------------ */

class MicTXView : public ui::View {
   public:
    MicTXView();
    ~MicTXView() override;

    MicTXView(const MicTXView&) = delete;
    MicTXView& operator=(const MicTXView&) = delete;

    std::string title() const override { return "Mic TX"; }
    void focus() override;

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

    /* Upstream Mic_Modulation order, minus the codec-specific extras. Public so
     * the mode-mapping helper in the .cpp can name the values. */
    enum Modulation : int32_t {
        MOD_NFM = 0,
        MOD_WFM = 1,
        MOD_AM = 2,
        MOD_USB = 3,
        MOD_LSB = 4,
        MOD_DSB = 5,
    };

   private:
    bool is_fm_mode() const { return mod_ == MOD_NFM || mod_ == MOD_WFM; }

    void apply_mode();
    void apply_sub_tone();
    void apply_audio_gain();
    void set_tx(bool enable);
    void pump_audio();  /* capture -> meter -> (TX ring), each frame */
    void update_status();

    /* --- state --- */
    int32_t mod_{MOD_NFM};
    uint32_t bw_khz_{10};       /* TX deviation, kHz (FM modes) */
    uint32_t mic_gain_x10_{10}; /* x1.0 */
    int32_t tx_gain_db_{40};
    int32_t tone_value_{-1};    /* index into dsp::tones::ctcss, -1 = none */
    int32_t dcs_code_{0};       /* 0 = off */
    int32_t tone_mix_pct_{20};
    bool vox_enabled_{false};
    int32_t vox_level_{40};     /* 0..255 threshold */
    uint32_t attack_ms_{200};
    uint32_t decay_ms_{800};
    uint64_t tx_frequency_{446'000'000};

    bool transmitting_{false};
    bool ptt_latched_{false};   /* PTT toggle keeps TX on until pressed again */
    uint8_t meter_value_{0};
    uint32_t attack_timer_ms_{0};
    uint32_t decay_timer_ms_{0};

    /* Mic samples the DSP thread pulls when transmitting. Sized for ~340 ms at
     * 48 kHz so a stalled UI frame does not immediately underrun the modulator. */
    dsp::RingBuffer<float> tx_ring_{16384};
    std::vector<float> capture_{};

    /* --- widgets --- */
    ui::Labels labels_{
        {{0, 8}, "Freq:", ui::Color::light_grey()},
        {{0, 24}, "Mode:", ui::Color::light_grey()},
        {{120, 24}, "Dev:", ui::Color::light_grey()},
        {{0, 40}, "MicG:", ui::Color::light_grey()},
        {{120, 40}, "TXg:", ui::Color::light_grey()},
        {{0, 56}, "Tone:", ui::Color::light_grey()},
        {{0, 72}, "DCS:", ui::Color::light_grey()},
        {{120, 72}, "Mix%:", ui::Color::light_grey()},
        {{0, 104}, "Level:", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{48, 8}};

    ui::OptionsField options_mode_{
        {48, 24},
        4,
        {{"NFM", MOD_NFM},
         {"WFM", MOD_WFM},
         {"AM", MOD_AM},
         {"USB", MOD_USB},
         {"LSB", MOD_LSB},
         {"DSB", MOD_DSB}}};

    ui::NumberField field_bw_{
        {160, 24}, 3, {1, 150}, 1, ' '};

    ui::OptionsField options_gain_{
        {48, 40},
        4,
        {{"x0.5", 5}, {"x1.0", 10}, {"x1.5", 15}, {"x2.0", 20}}};

    ui::NumberField field_tx_gain_{
        {160, 40}, 3, {0, 89}, 1, ' '};

    ui::OptionsField options_tone_{
        {48, 56}, 8, {}};

    ui::NumberField field_dcs_{
        {48, 72}, 3, {0, 511}, 1, ' '};

    ui::NumberField field_tone_mix_{
        {176, 72}, 2, {10, 99}, 1, ' '};

    ui::Checkbox check_vox_{
        {0, 88}, 3, "VOX"};

    ui::NumberField field_vox_level_{
        {80, 88}, 3, {0, 255}, 2, ' '};

    ui::VuMeter vumeter_{
        {56, 104, 176, 16}, 16, true};

    ui::Text text_status_{
        {0, 128, 240, 16}, ""};

    ui::Text text_warn_{
        {0, 288, 240, 16}, "TX needs a licence - illegal in most bands"};

    ui::Button tx_button_{
        {0, 256, 240, 24}, "PTT / TX", true};
};

}  // namespace app

#endif /*__MB200_UI_MICTX_H__*/
