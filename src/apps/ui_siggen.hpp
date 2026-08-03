/*
 * mayhem-b200 — Signal generator (siggen) TX.
 *
 * Ported from firmware/application/external/siggen/ and the baseband/proc_siggen
 * tone processor it drives. Upstream siggen synthesises a modulating waveform
 * (sine / triangle / saw up / saw down / square / pseudo-noise) and puts it on
 * the carrier with a selectable modulation (CW, FM, DSB, AM). proc_siggen builds
 * the waveform from a 32-bit phase accumulator into a 256-entry sine table and a
 * 16-bit Fibonacci LFSR (taps 16/15/13/4, seed 0xACE1) for the noise shape.
 *
 * On the host that waveform generator is dsp::ToneGen (dsp/modulate.hpp), which
 * is the same six shapes and the same LFSR, in float — see its header for why
 * the fixed-point details of proc_siggen are a hardware cost and not part of the
 * specification. The tone drives radio::TransmitterModel, which supplies the
 * modulation (CW carrier, FM, DSB, AM). This is the "use ToneGen + the
 * transmitter" path the port was scoped to.
 *
 * Added over upstream: a linear frequency SWEEP of the tone (start → end by a
 * step, dwelling on each step, looping) — the "tone/sweep generator" the port
 * asked for. The sweep math is UI-free and tested.
 *
 * Not ported: upstream's BPSK / QPSK / Pulsed-CW modulations. Those are direct
 * phase/OOK constructions proc_siggen writes straight into int8 IQ; the host
 * transmitter has no equivalent single-tone digital mode, and faking them with
 * an audio path would misrepresent them, so they are omitted rather than stubbed
 * (an honest gap, per the porting contract). The analog subset — CW, FM, DSB,
 * AM 100%, AM 50% — is complete.
 *
 * LEGALITY: a signal generator radiates a carrier that can interfere with
 * licensed services. This app never transmits until the user presses TX and
 * shows a legality reminder on screen.
 *
 * RF output requires a USRP B200 and is unverified without hardware; the
 * waveform and sweep generators are real and tested.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (original)
 * Copyright (C) 2016 Furrtek (original)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SIGGEN_H__
#define __MB200_UI_SIGGEN_H__

#include <algorithm>
#include <cstdint>

namespace app {

/* ==========================================================================
 * UI-free sweep math — the added deliverable, testable without a display.
 * ========================================================================== */

namespace siggen {

/* Number of discrete frequency steps in a linear sweep from `start_hz` to
 * `end_hz` inclusive, stepping by |`step_hz`|. A zero step (or start == end) is
 * a single fixed point. Direction is inferred from start vs end, so a descending
 * sweep (start > end) has the same count as its ascending mirror. */
inline uint32_t sweep_step_count(uint32_t start_hz, uint32_t end_hz, uint32_t step_hz) {
    if (step_hz == 0) return 1;
    const uint32_t lo = std::min(start_hz, end_hz);
    const uint32_t hi = std::max(start_hz, end_hz);
    return (hi - lo) / step_hz + 1;
}

/* Frequency at step `index` (clamped to the last step). Ascending sweeps count
 * up from start; descending sweeps count down from start. The final step never
 * overshoots the [lo, hi] span: with an unaligned range the last step is the
 * largest multiple of the step within it, not `end_hz` itself. */
inline uint32_t sweep_frequency_at(uint32_t start_hz, uint32_t end_hz,
                                   uint32_t step_hz, uint32_t index) {
    const uint32_t n = sweep_step_count(start_hz, end_hz, step_hz);
    if (index >= n) index = n - 1;
    if (start_hz <= end_hz)
        return start_hz + index * step_hz;
    return start_hz - index * step_hz;
}

}  // namespace siggen

}  // namespace app

/* ==========================================================================
 * View — excluded from the encoder-only build used by tests / harness.
 * ========================================================================== */

#ifndef MB200_ENCODER_ONLY

#include "../dsp/modulate.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <atomic>
#include <chrono>
#include <string>

namespace app {

class SigGenView : public ui::View {
   public:
    SigGenView();
    ~SigGenView() override;

    SigGenView(const SigGenView&) = delete;
    SigGenView& operator=(const SigGenView&) = delete;

    std::string title() const override { return "Signal gen"; }
    void focus() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    static constexpr uint32_t kAudioRate = 48'000;

    /* Modulation modes offered by the host (the analog subset of upstream). */
    enum Mod : int32_t { ModCW = 0, ModFM = 1, ModDSB = 2, ModAM100 = 3, ModAM50 = 4 };

    void update_visibility();
    void update_summary();
    bool start_tx();
    void stop_tx();

    /* DSP-thread audio callback: renders the tone, sweeping if enabled. */
    size_t fill_audio(float* out, size_t count);

    /* --- Transmit state (read by the DSP thread once running) --- */
    dsp::ToneGen tone_{};
    dsp::ToneGen::Shape shape_{dsp::ToneGen::Shape::Sine};
    float base_freq_hz_{1000.0f};
    bool sweep_on_{false};
    uint32_t sweep_start_{1000}, sweep_end_{2000}, sweep_step_{100};
    uint32_t dwell_samples_{4800};   /* set at start from dwell ms */
    uint32_t sweep_count_{1};
    uint64_t elapsed_{0};
    float cur_freq_hz_{1000.0f};

    bool transmitting_{false};
    uint32_t stop_after_s_{0};       /* 0 = run until Stop */
    std::chrono::steady_clock::time_point tx_start_{};

    /* --- Widgets --- */
    ui::Labels labels_{
        {{0 * 8, 1 * 16}, "Mod:", ui::Color::light_grey()},
        {{0 * 8, 2 * 16}, "Shape:", ui::Color::light_grey()},
        {{0 * 8, 3 * 16}, "Tone:", ui::Color::light_grey()},
        {{18 * 8, 3 * 16}, "Hz", ui::Color::light_grey()},
        {{0 * 8, 5 * 16}, "Sweep:", ui::Color::light_grey()},
        {{0 * 8, 6 * 16}, "Start:", ui::Color::light_grey()},
        {{0 * 8, 7 * 16}, "End:", ui::Color::light_grey()},
        {{0 * 8, 8 * 16}, "Step:", ui::Color::light_grey()},
        {{14 * 8, 8 * 16}, "Dwell:", ui::Color::light_grey()},
        {{25 * 8, 8 * 16}, "ms", ui::Color::light_grey()},
        {{0 * 8, 11 * 16}, "Freq:", ui::Color::light_grey()},
        {{0 * 8, 12 * 16}, "Gain:", ui::Color::light_grey()},
        {{13 * 8, 12 * 16}, "Stop:", ui::Color::light_grey()},
        {{26 * 8, 12 * 16}, "s", ui::Color::light_grey()},
    };

    ui::OptionsField options_mod_{
        {5 * 8, 1 * 16},
        11,
        {{"CW (nomod)", ModCW},
         {"FM", ModFM},
         {"DSB", ModDSB},
         {"AM 100%", ModAM100},
         {"AM 50%", ModAM50}}};

    ui::OptionsField options_shape_{
        {7 * 8, 2 * 16},
        12,
        {{"Sine", 0},
         {"Triangle", 1},
         {"Saw up", 2},
         {"Saw down", 3},
         {"Square", 4},
         {"Pseudo Noise", 5}}};

    ui::NumberField field_tone_{{6 * 8, 3 * 16}, 5, {1, 20000}, 10, ' '};

    ui::Checkbox check_sweep_{{7 * 8, 5 * 16}, 3, "on"};
    ui::NumberField field_start_{{7 * 8, 6 * 16}, 5, {1, 20000}, 10, ' '};
    ui::NumberField field_end_{{7 * 8, 7 * 16}, 5, {1, 20000}, 10, ' '};
    ui::NumberField field_step_{{7 * 8, 8 * 16}, 5, {1, 10000}, 10, ' '};
    ui::NumberField field_dwell_{{21 * 8, 8 * 16}, 4, {1, 5000}, 10, ' '};

    ui::FrequencyField field_freq_{{6 * 8, 11 * 16}};
    ui::NumberField field_gain_{{6 * 8, 12 * 16}, 3, {0, 89}, 1, ' '};
    ui::Checkbox check_stop_{{19 * 8, 12 * 16}, 2, ""};
    ui::NumberField field_stop_{{22 * 8, 12 * 16}, 3, {1, 999}, 1, ' '};

    ui::Text text_summary_{{0, 14 * 16, 30 * 8, 16}, ""};
    ui::Text text_status_{{0, 15 * 16, 30 * 8, 16}, ""};
    ui::Text text_warning_{{0, 16 * 16, 30 * 8, 16}, ""};

    ui::Button button_tx_{{2 * 8, 17 * 16 + 8, 12 * 8, 3 * 16}, "TX"};
    ui::Button button_stop_tx_{{16 * 8, 17 * 16 + 8, 12 * 8, 3 * 16}, "Stop"};
};

}  // namespace app

#endif  // MB200_ENCODER_ONLY

#endif  // __MB200_UI_SIGGEN_H__
