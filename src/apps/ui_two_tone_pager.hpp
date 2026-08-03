/*
 * mayhem-b200 — Two-Tone pager TX (Motorola Quik-Call II style sequential
 * two-tone paging).
 *
 * Ported from firmware/application/external/two_tone_pager/ui_two_tone_pager.*
 * and the baseband/proc_tones.cpp tone processor it drives. Upstream transmits
 * a sequential two-tone page: tone A held for a set duration, an optional gap,
 * then tone B — the pattern a Quik-Call / Fire-Std pager decodes as an address.
 * An optional CTCSS sub-tone can be mixed simultaneously under both tones
 * (proc_tones' "dual tone" mode). The whole thing is FM-modulated onto the
 * carrier (proc_tones does FM: `delta = tone_sample * fm_delta`).
 *
 * What is ported faithfully, and tested against upstream:
 *   - The Motorola tone table (MOTO_FREQS, 45 entries, ×10 Hz) and the CTCSS
 *     table (51 entries) verbatim from upstream.
 *   - tone_delta(freq_x10) = freq_hz · 2^32 / SAMPLE_RATE, the phase increment
 *     proc_tones' 32-bit accumulator uses — computed exactly as upstream's own
 *     TwoTonePagerView::tone_delta (full-precision (freq_x10·2^32)/(SR·10), a
 *     touch finer than the pre-truncated TONES_F2D coefficient macro).
 *   - ms_to_samples(ms) = ms · SAMPLE_RATE / 1000, the baseband sample counts.
 *   - The A → [gap] → B sequence structure and its durations.
 *
 * How it reaches RF on the host: the firmware hands proc_tones a phase-delta and
 * a sample count and the M4 synthesises the FM baseband. Here the sequence is
 * rendered as an audio tone stream (dsp::ToneGen) at the audio rate and fed to
 * radio::TransmitterModel in NFM mode, which resamples, FM-modulates, and drives
 * the B200. The frequencies and timings are identical; only the sample rate of
 * the intermediate representation differs, which is the same deliberate
 * host/firmware split documented in dsp/modulate.hpp.
 *
 * LEGALITY: transmitting pager tones on real paging channels is illegal in most
 * jurisdictions and can trigger real pagers. This app never transmits until the
 * user presses TX, shows the sequence first, and displays a legality warning.
 *
 * RF output requires a USRP B200 and is unverified without hardware; the tone
 * encoder and sequence builder are real and tested.
 *
 * Copyright (C) 2024 PortaPack Mayhem (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_TWO_TONE_PAGER_H__
#define __MB200_UI_TWO_TONE_PAGER_H__

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace app {

/* ==========================================================================
 * UI-free encoder — the deliverable, testable without a display or a radio.
 * Every function here reproduces an upstream computation exactly.
 * ========================================================================== */

namespace two_tone_pager {

/* proc_tones runs its 32-bit phase accumulator at this rate (BasebandThread in
 * proc_tones.hpp, TONES_SAMPLERATE in common/tonesets.hpp). The tone-delta and
 * sample-count math is defined against it so the ported values match upstream
 * bit-for-bit, independent of whatever rate the host audio path runs at. */
inline constexpr uint32_t SAMPLE_RATE = 1'536'000;

/* Motorola two-tone frequencies, ×10 Hz, verbatim from upstream MOTO_FREQS[45]
 * (ui_two_tone_pager.cpp). Group A/B tones share this pool. */
inline constexpr std::array<uint32_t, 45> MOTO_FREQS = {{
    2885, 3047, 3217, 3396, 3586, 3786, 3998, 4221, 4457, 4705,
    4968, 5246, 5539, 5848, 6174, 6519, 6883, 7268, 7674, 8102,
    8555, 9032, 9537, 10073, 10642, 11225, 11247, 11534, 11852, 11885,
    12178, 12514, 12555, 12858, 13258, 13576, 13950, 13996, 14768, 15579,
    16430, 17325, 18262, 19245, 20275,
}};

/* CTCSS sub-audible tones, ×10 Hz. Index 0 = None; 1..50 = standard tones.
 * Verbatim from upstream CTCSS_FREQS[51]. */
inline constexpr std::array<uint32_t, 51> CTCSS_FREQS = {{
    0, 670, 719, 744, 770, 797, 825, 854, 885, 915,
    948, 974, 1000, 1035, 1072, 1109, 1148, 1188, 1230, 1273,
    1318, 1365, 1413, 1462, 1500, 1514, 1567, 1598, 1622, 1655,
    1679, 1713, 1738, 1773, 1799, 1835, 1862, 1899, 1928, 1966,
    1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503,
    2541,
}};

inline constexpr size_t MOTO_TONE_COUNT = MOTO_FREQS.size();  /* 45 */
inline constexpr size_t CTCSS_COUNT = CTCSS_FREQS.size();     /* 51 */

/* Phase increment for proc_tones' 32-bit sine-table accumulator, from a
 * frequency expressed in tenths of a hertz:
 *
 *   delta = freq_hz · 2^32 / SAMPLE_RATE
 *         = (freq_x10 · 2^32) / (SAMPLE_RATE · 10)
 *
 * This is upstream TwoTonePagerView::tone_delta() to the last bit. A zero
 * frequency (the "None" CTCSS) yields a zero delta, which proc_tones treats as
 * silence. */
inline uint32_t tone_delta(uint32_t freq_x10) {
    if (freq_x10 == 0) return 0;
    return static_cast<uint32_t>((static_cast<uint64_t>(freq_x10) << 32) /
                                 (static_cast<uint64_t>(SAMPLE_RATE) * 10ULL));
}

/* Duration in baseband samples, upstream ms_to_samples(). */
inline uint32_t ms_to_samples(uint32_t ms) {
    return static_cast<uint32_t>((static_cast<uint64_t>(ms) *
                                  static_cast<uint64_t>(SAMPLE_RATE)) /
                                 1000ULL);
}

/* One segment of the rendered audio tone stream. `freq_hz` is the main tone
 * (0 = silence/gap). `ctcss_hz` is the sub-tone mixed under it at equal weight
 * (0 = none, matching upstream's single-tone path). `samples` is the length at
 * the audio rate the sequence was built for. */
struct ToneSegment {
    float freq_hz{0.0f};
    float ctcss_hz{0.0f};
    uint32_t samples{0};
};

/* Builds the A → [gap] → B sequence at `audio_rate`, mirroring upstream
 * start_tx(): tone A for dur_a, an optional carrier gap for gap_ms, then tone B
 * for dur_b. The gap is emitted only when gap_ms > 0, exactly as upstream only
 * inserts its silence digit then. When a CTCSS tone is present it rides under
 * both A and B (upstream's dual-tone mode); the gap never carries CTCSS. */
inline std::vector<ToneSegment> build_sequence(float freq_a_hz,
                                               float freq_b_hz,
                                               float ctcss_hz,
                                               uint32_t dur_a_ms,
                                               uint32_t dur_b_ms,
                                               uint32_t gap_ms,
                                               uint32_t audio_rate) {
    auto to_samples = [audio_rate](uint32_t ms) -> uint32_t {
        return static_cast<uint32_t>((static_cast<uint64_t>(ms) *
                                      static_cast<uint64_t>(audio_rate)) /
                                     1000ULL);
    };

    std::vector<ToneSegment> seq;
    seq.push_back({freq_a_hz, ctcss_hz, to_samples(dur_a_ms)});
    if (gap_ms > 0)
        seq.push_back({0.0f, 0.0f, to_samples(gap_ms)});
    seq.push_back({freq_b_hz, ctcss_hz, to_samples(dur_b_ms)});
    return seq;
}

}  // namespace two_tone_pager

}  // namespace app

/* ==========================================================================
 * View — pulled out of the encoder-only build so the tests and a standalone
 * harness can exercise the code above without dragging in the UI or UHD.
 * ========================================================================== */

#ifndef MB200_ENCODER_ONLY

#include "../dsp/modulate.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <atomic>
#include <mutex>
#include <string>

namespace app {

class TwoTonePagerView : public ui::View {
   public:
    TwoTonePagerView();
    ~TwoTonePagerView() override;

    TwoTonePagerView(const TwoTonePagerView&) = delete;
    TwoTonePagerView& operator=(const TwoTonePagerView&) = delete;

    std::string title() const override { return "2-Tone TX"; }
    void focus() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    static constexpr uint32_t kAudioRate = 48'000;
    static constexpr uint32_t kCustomToneIdx =
        static_cast<uint32_t>(two_tone_pager::MOTO_TONE_COUNT);  /* "Custom" */

    void update_summary();
    bool start_tx();
    void stop_tx();

    /* DSP-thread audio callback: renders the built sequence. */
    size_t fill_audio(float* out, size_t count);

    /* Current tone-A/tone-B frequency in Hz (respecting the Custom entry). */
    float freq_a_hz() const;
    float freq_b_hz() const;
    float ctcss_hz() const;

    /* --- Transmit state (touched by the DSP thread once running) --- */
    std::vector<two_tone_pager::ToneSegment> sequence_{};
    dsp::ToneGen tone_main_{};
    dsp::ToneGen tone_ctcss_{};
    size_t seg_index_{0};
    uint32_t seg_pos_{0};
    std::atomic<bool> playing_{false};
    std::atomic<bool> finished_{false};
    std::atomic<uint32_t> seg_progress_{0};

    bool transmitting_{false};

    /* --- Widgets --- */
    ui::Labels labels_{
        {{0 * 8, 1 * 16}, "Tone A:", ui::Color::light_grey()},
        {{16 * 8, 1 * 16}, "Tone B:", ui::Color::light_grey()},
        {{0 * 8, 2 * 16}, "CTCSS:", ui::Color::light_grey()},
        {{0 * 8, 3 * 16}, "A ms:", ui::Color::light_grey()},
        {{12 * 8, 3 * 16}, "B ms:", ui::Color::light_grey()},
        {{0 * 8, 4 * 16}, "Gap ms:", ui::Color::light_grey()},
        {{0 * 8, 6 * 16}, "Freq:", ui::Color::light_grey()},
        {{0 * 8, 7 * 16}, "TX gain:", ui::Color::light_grey()},
    };

    ui::OptionsField options_tone_a_{{7 * 8, 1 * 16}, 8, {}};
    ui::OptionsField options_tone_b_{{23 * 8, 1 * 16}, 8, {}};
    ui::OptionsField options_ctcss_{{7 * 8, 2 * 16}, 9, {}};

    ui::NumberField field_dur_a_{{5 * 8, 3 * 16}, 4, {100, 9900}, 100, ' '};
    ui::NumberField field_dur_b_{{17 * 8, 3 * 16}, 4, {100, 9900}, 100, ' '};
    ui::NumberField field_gap_{{7 * 8, 4 * 16}, 4, {0, 9900}, 100, ' '};

    ui::FrequencyField field_freq_{{5 * 8, 6 * 16}};
    ui::NumberField field_gain_{{8 * 8, 7 * 16}, 3, {0, 89}, 1, ' '};

    ui::Text text_summary_{{0, 9 * 16, 30 * 8, 16}, ""};
    ui::Text text_status_{{0, 10 * 16, 30 * 8, 16}, ""};
    ui::ProgressBar progressbar_{{0, 11 * 16, 30 * 8, 16}};

    ui::Text text_warning_{{0, 13 * 16, 30 * 8, 16}, ""};

    ui::Button button_tx_{{2 * 8, 15 * 16, 12 * 8, 3 * 16}, "TX"};
    ui::Button button_stop_{{16 * 8, 15 * 16, 12 * 8, 3 * 16}, "Stop"};
};

}  // namespace app

#endif  // MB200_ENCODER_ONLY

#endif  // __MB200_UI_TWO_TONE_PAGER_H__
