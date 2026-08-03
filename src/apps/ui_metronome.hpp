/*
 * mayhem-b200 — Metronome utility.
 *
 * Host port of PortaPack Mayhem's external/metronome app (copyleft 2024 zxkmm).
 * The firmware drove the beat from a ChibiOS thread that called
 * chThdSleepMilliseconds() between baseband beep requests. On the host there is
 * no second core and no baseband: instead the tone is synthesised here and
 * streamed to the audio output, and the tempo is driven by the audio sample
 * clock (which is exact) rather than by the ~60 Hz UI frame tick. The timing and
 * beep maths live in metronome_dsp / MetronomeEngine so they can be unit tested.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_METRONOME_H__
#define __MB200_UI_METRONOME_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace app {

/* ---- pure metronome timing / beep maths (tested in test_metronome.cpp) ----
 *
 * Values follow firmware/application/external/metronome/ui_metronome.cpp:
 *   base_interval = (60 * 1000) / bpm   (ms per quarter note)
 *   actual        = (base_interval * 4) / beat_unit
 * where beat_unit is the denominator of the time signature (the field after the
 * "/") and beats_per_measure is the numerator (the field before it). The first
 * beat of every measure is accented. */
namespace metronome_dsp {

/* Milliseconds between beats, using upstream's two-step integer formula. */
uint32_t beat_interval_ms(uint32_t bpm, uint32_t beat_unit);

/* Exact number of audio samples between beats: 240 * rate / (bpm * beat_unit).
 * Derived from beat_seconds = (60/bpm) * (4/beat_unit) = 240/(bpm*beat_unit). */
uint32_t samples_per_beat(uint32_t bpm, uint32_t beat_unit, uint32_t rate);

/* Number of tone samples for a beep of beep_ms at the given rate. */
uint32_t beep_samples(uint32_t beep_ms, uint32_t rate);

/* First beat of each measure (index % beats_per_measure == 0) is accented.
 * beats_per_measure is clamped to >= 1. */
bool is_accent(uint32_t beat_index, uint32_t beats_per_measure);

/* Tap tempo: BPM implied by a beat interval in ms (0 when interval is 0). */
uint16_t bpm_from_interval_ms(uint32_t interval_ms);

}  // namespace metronome_dsp

/* Sample-accurate metronome tone generator. The tempo comes out exact because
 * one beat is always samples_per_beat() samples long; live config changes take
 * effect at the next beat boundary. */
class MetronomeEngine {
   public:
    /* Sets the tone parameters. Does not touch playback position, so it is safe
     * to call every frame with the current field values. */
    void configure(uint32_t bpm,
                   uint32_t beats_per_measure,
                   uint32_t beat_unit,
                   uint32_t accent_hz,
                   uint32_t unaccent_hz,
                   uint32_t beep_ms,
                   uint32_t rate);

    /* Rewind to the downbeat (accented first beat) of measure 0. */
    void reset();

    /* Fill out[0..n) with the next n samples, advancing the position. Returns
     * how many beats began within this call. */
    uint32_t render(float* out, uint32_t n);

    uint32_t beat_index() const { return beat_index_; }
    uint32_t beat_in_measure() const;  /* 0-based position within the measure */
    bool current_is_accent() const { return accent_; }
    uint32_t rate() const { return rate_; }

   private:
    uint32_t bpm_{120};
    uint32_t beats_per_measure_{4};
    uint32_t beat_unit_{4};
    uint32_t accent_hz_{880};
    uint32_t unaccent_hz_{440};
    uint32_t beep_ms_{100};
    uint32_t rate_{48000};

    uint32_t pos_in_beat_{0};  /* sample index within the current beat */
    uint32_t spb_{0};          /* samples in current beat (latched at start) */
    uint32_t beep_len_{0};     /* tone samples in current beat (latched) */
    uint32_t cur_freq_{0};     /* frequency for current beat (latched) */
    uint32_t beat_index_{0};
    bool accent_{true};
    float amplitude_{0.6f};
};

class MetronomeView : public ui::View {
   public:
    MetronomeView();

    std::string title() const override { return "Metronome"; }

    void focus() override;
    void on_show() override;
    void on_frame_sync() override;

   private:
    void toggle_play();
    void play();
    void stop_play();
    void register_tap();

    MetronomeEngine engine_{};
    bool playing_{false};
    double pending_samples_{0.0};  /* estimate of unplayed samples in the ring */
    uint64_t last_sync_ms_{0};
    std::vector<float> render_buf_;

    /* Tap tempo state. */
    uint64_t last_tap_ms_{0};
    std::deque<uint32_t> tap_intervals_{};

    ui::Labels labels_{
        {{0, 8}, "BPM", ui::Color::light_grey()},
        {{0, 32}, "Time sig", ui::Color::light_grey()},
        {{120, 32}, "/", ui::Color::light_grey()},
        {{0, 56}, "Accent Hz", ui::Color::light_grey()},
        {{0, 80}, "Beat Hz", ui::Color::light_grey()},
        {{0, 104}, "Beep ms", ui::Color::light_grey()},
        {{0, 128}, "Volume", ui::Color::light_grey()},
    };

    ui::NumberField field_bpm_{{88, 8}, 4, {1, 1000}, 1, ' '};
    ui::Button button_tap_{{160, 4, 72, 24}, "TAP"};

    ui::NumberField field_beats_{{88, 32}, 2, {1, 99}, 1, ' '};      /* numerator */
    ui::NumberField field_beat_unit_{{128, 32}, 2, {1, 99}, 1, ' '}; /* denominator */

    ui::NumberField field_accent_hz_{{88, 56}, 5, {380, 24000}, 20, ' '};
    ui::NumberField field_beat_hz_{{88, 80}, 5, {380, 24000}, 20, ' '};
    ui::NumberField field_beep_ms_{{88, 104}, 3, {10, 999}, 1, ' '};
    ui::NumberField field_volume_{{88, 128}, 3, {0, 99}, 1, ' '};

    ui::ProgressBar progressbar_{{0, 160, 240, 24}};

    ui::Button button_play_stop_{{20, 204, 200, 40}, "PLAY"};
    ui::Button button_back_{{72, 256, 96, 28}, "Back"};
};

}  // namespace app

#endif /*__MB200_UI_METRONOME_H__*/
