/*
 * mayhem-b200 — Audio Test (tone generator + output level meter).
 *
 * Host port of PortaPack Mayhem's external/audio_test app (copyright 2024 Mark
 * Thompson). The firmware generated the beep on the M4 via the audio DMA's
 * beep_start()/beep_stop() (baseband/audio_dma.cpp) and drove the WM8731 codec
 * at the chosen sample rate. On the host there is no baseband and no codec: the
 * tone is synthesised here (AudioTestTone) and streamed to globals().audio_out,
 * which runs at a fixed 48 kHz (audio::sample_rate). The frequency, duration,
 * volume and speaker toggle all behave as upstream; the sample-rate selector
 * sets the valid frequency band exactly as the firmware did
 * (field_frequency.set_range(v/128, v/2)), but playback itself is always 48 kHz.
 *
 * Because the level meter reads globals().audio_out->take_peak(), which is
 * sampled AFTER the volume/mute step in the waveOut callback, the bar shows the
 * real level heading to the sound card, not a synthetic figure.
 *
 * Copyright (C) 2024 Mark Thompson (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_AUDIO_TEST_H__
#define __MB200_UI_AUDIO_TEST_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* ---- pure audio-test maths (tested in tests/test_audio_test.cpp) ----------
 *
 * These are the parts that can be checked without a sound card: the tone
 * generator's frequency (verified by zero-crossing count) and amplitude bound,
 * the sample-rate -> frequency-band mapping, the duration -> sample count, and
 * the output-level meter scaling. */
namespace audio_test_dsp {

/* Upstream sets the frequency field's range to [sample_rate/128, sample_rate/2]
 * whenever the sample rate changes (external/audio_test/ui_audio_test.cpp:56).
 * sample_rate/2 is the Nyquist limit; sample_rate/128 is upstream's low bound. */
inline uint32_t freq_min_for_rate(uint32_t rate) { return rate / 128; }
inline uint32_t freq_max_for_rate(uint32_t rate) { return rate / 2; }

/* Number of audio samples in a beep of `ms` milliseconds at `rate`. Zero ms is
 * upstream's "continuous" sentinel and is handled by the caller, not here. */
inline uint32_t duration_samples(uint32_t ms, uint32_t rate) {
    return static_cast<uint32_t>((static_cast<uint64_t>(ms) * rate) / 1000ull);
}

/* Convert a linear peak amplitude in [0, 1] to dBFS, clamped to a practical
 * [floor, 0] window. A peak of 0 (silence) returns the floor rather than -inf. */
inline float peak_to_dbfs(float peak, float floor_db = -60.0f) {
    if (peak <= 0.0f) return floor_db;
    float db = 20.0f * std::log10(peak);
    if (db < floor_db) db = floor_db;
    if (db > 0.0f) db = 0.0f;
    return db;
}

/* Map a dBFS reading onto a 0..255 VU-meter bar over [floor_db, 0], clamped so
 * a reading at or below the floor pins to 0 and 0 dBFS pins to 255. */
inline uint8_t dbfs_to_bar255(float db, float floor_db = -60.0f) {
    if (floor_db >= 0.0f) return 0;
    if (db <= floor_db) return 0;
    if (db >= 0.0f) return 255;
    const float frac = (db - floor_db) / (0.0f - floor_db);
    long v = std::lround(static_cast<double>(frac) * 255.0);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return static_cast<uint8_t>(v);
}

}  // namespace audio_test_dsp

/* Sample-accurate sine tone generator. Phase is kept in cycles [0, 1) as a
 * double so the pitch stays exact over long runs; a frequency of 0 (upstream's
 * divide-by-zero guard) produces silence. */
class AudioTestTone {
   public:
    /* Sets pitch, playback rate and amplitude. Does not touch the phase, so it
     * is safe to call every frame; call reset() to restart cleanly. */
    void configure(uint32_t freq, uint32_t rate, float amplitude);

    void reset() { phase_ = 0.0; }

    /* Fill out[0..n) with the next n samples, advancing the phase. */
    void render(float* out, uint32_t n);

    uint32_t freq() const { return freq_; }
    uint32_t rate() const { return rate_; }
    float amplitude() const { return amplitude_; }

   private:
    uint32_t freq_{1000};
    uint32_t rate_{48000};
    float amplitude_{0.6f};
    double phase_{0.0};       /* cycles, [0, 1) */
    double increment_{0.0};   /* cycles per sample = freq / rate */
};

class AudioTestView : public ui::View {
   public:
    AudioTestView();
    ~AudioTestView() override;

    AudioTestView(const AudioTestView&) = delete;
    AudioTestView& operator=(const AudioTestView&) = delete;

    std::string title() const override { return "Audio Test"; }

    void focus() override;
    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    /* (Re)start or stop the tone from the current field values. */
    void update_beep();
    /* Apply the frequency band and step for the selected sample rate. */
    void apply_sample_rate(int32_t rate);

    static constexpr float kAmplitude = 0.6f;
    static constexpr float kMeterFloorDb = -60.0f;

    AudioTestTone tone_{};
    bool beep_{false};
    /* Samples left to play for a one-shot beep; -1 means continuous. */
    int64_t remaining_samples_{-1};
    std::vector<float> render_buf_;

    ui::Labels labels_{
        {{0, 8}, "Sample rate", ui::Color::light_grey()},
        {{0, 32}, "Frequency Hz", ui::Color::light_grey()},
        {{0, 56}, "Step", ui::Color::light_grey()},
        {{0, 80}, "Duration ms", ui::Color::light_grey()},
        {{0, 100}, "0 = continuous", ui::Color::grey()},
        {{0, 124}, "Volume", ui::Color::light_grey()},
        {{0, 188}, "Output level", ui::Color::light_grey()},
    };

    ui::OptionsField options_sample_rate_{
        {144, 8},
        5,
        {{"24000", 24000}, {"48000", 48000}, {"12000", 12000}}};

    ui::NumberField field_frequency_{{144, 32}, 5, {1, 24000}, 100, ' ', true};

    ui::OptionsField options_step_{
        {144, 56},
        4,
        {{"   1", 1}, {"  10", 10}, {" 100", 100}, {"1000", 1000}}};

    ui::NumberField field_duration_{{144, 80}, 5, {0, 60000}, 50, ' '};
    ui::NumberField field_volume_{{144, 124}, 3, {0, 99}, 1, ' '};

    ui::Checkbox check_speaker_{{0, 150}, 10, "Speaker"};

    ui::Text text_level_{{0, 208, 240, 16}, "Level   --.- dBFS"};
    ui::VuMeter meter_{{8, 226, 224, 16}, 28, true};

    ui::Text text_status_{{0, 248, 240, 16}, ""};

    ui::Button button_back_{{72, 268, 96, 28}, "Back"};
};

}  // namespace app

#endif /*__MB200_UI_AUDIO_TEST_H__*/
