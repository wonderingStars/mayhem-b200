/*
 * mayhem-b200 — instrument tuner (reference-tone generator).
 *
 * Ported from firmware/application/external/tuner/ (copyleft 2024 zxkmm). The
 * upstream Tuner is NOT a receiver app: it plays a reference tone for a chosen
 * instrument string / pitch-fork frequency so the user can tune by ear. The
 * firmware synthesises the tone with the proc_audio_beep baseband image on the
 * M4; on the host there is no M4, so the tone is generated directly and written
 * to the shared audio output (audio::AudioOut).
 *
 * Deliberate, honest host deviation from upstream (see PORTING.md honesty
 * rules): the PortaPack speaker cannot cleanly reproduce very low string
 * frequencies, so upstream stores an "octave_shift" and plays the note an
 * octave up while telling the user. A host waveOut endpoint at 48 kHz plays the
 * real string frequency fine, so this port plays the *real* frequency and shows
 * the octave-shift only for reference. The upstream data table and the
 * real-frequency computation are kept verbatim so both can be tested.
 *
 * Copyright (C) 2024 zxkmm (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_TUNER_H__
#define __MB200_UI_TUNER_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace app {

/* Local mirror of the firmware's audio::Rate. Kept so the upstream note table
 * ports verbatim and so tuner_rate_hz() (upstream's protected_sample_rate) is
 * testable. The host output stage always runs at 48 kHz regardless. */
enum class TunerRate : uint8_t {
    Hz_12000,
    Hz_24000,
    Hz_48000,
};

/* Upstream TunerView::protected_sample_rate(audio::Rate) — an enum-to-Hz map. */
inline uint32_t tuner_rate_hz(TunerRate r) {
    switch (r) {
        case TunerRate::Hz_12000:
            return 12000;
        case TunerRate::Hz_24000:
            return 24000;
        case TunerRate::Hz_48000:
            return 48000;
    }
    return 24000;
}

/* One string / reference pitch, matching upstream Instrument::NoteInfo. */
struct NoteInfo {
    uint16_t frequency;      /* frequency the PortaPack speaker actually plays */
    TunerRate sample_rate;   /* PP-only: rate chosen so the freq is reproducible */
    int8_t octave_shift;     /* PP-only: how far `frequency` is shifted from real */
};

/* Upstream TunerView::paint()'s real-frequency computation: the actual string
 * frequency a note represents, derived from the stored (possibly shifted)
 * frequency. Positive shift doubles, negative halves, per octave. Integer math
 * and int32 accumulator match the visible upstream result for every table
 * entry (e.g. guitar E2: 165, shift -1 -> 82). This is the frequency the host
 * plays, since a 48 kHz endpoint has no PP low-frequency limitation. */
inline int real_note_frequency(int stored_frequency, int octave_shift) {
    int f = stored_frequency;
    if (octave_shift > 0) {
        for (int i = 0; i < octave_shift; ++i) f *= 2;
    } else if (octave_shift < 0) {
        for (int i = 0; i > octave_shift; --i) f /= 2;
    }
    return f;
}

struct Instrument {
    std::string name;
    /* std::map, as upstream, so the on-screen note order (alphabetical by name)
     * matches the firmware exactly. */
    std::map<std::string, NoteInfo> notes;
};

/* The three instrument tables, verbatim from upstream ui_tuner.hpp. Exposed as
 * inline accessors so they are header-only and testable without linking. */
inline const Instrument& tuner_guitar() {
    static const Instrument g{
        "Folk Guitar",
        {
            {"E2", {165, TunerRate::Hz_12000, -1}},  /* actual E2, PP plays E3 */
            {"A2", {110, TunerRate::Hz_12000, 0}},
            {"D3", {147, TunerRate::Hz_12000, 0}},
            {"G3", {196, TunerRate::Hz_24000, 0}},
            {"B3", {247, TunerRate::Hz_24000, 0}},
            {"E4", {330, TunerRate::Hz_24000, 0}},
        }};
    return g;
}

inline const Instrument& tuner_violin() {
    static const Instrument v{
        "Violin 440 Standard, 12ET",
        {
            {"G3", {196, TunerRate::Hz_12000, 0}},
            {"D4", {294, TunerRate::Hz_24000, 0}},
            {"A4", {440, TunerRate::Hz_48000, 0}},
            {"E5", {659, TunerRate::Hz_48000, 0}},
        }};
    return v;
}

inline const Instrument& tuner_pitch_fork() {
    static const Instrument p{
        "Pitch Fork",
        {
            {"12ET A4", {440, TunerRate::Hz_48000, 0}},
            {"Sarti's A4", {436, TunerRate::Hz_48000, 0}},
            {"1858 A4", {435, TunerRate::Hz_48000, 0}},
            {"Verdi's A4", {432, TunerRate::Hz_48000, 0}},
        }};
    return p;
}

/* The instrument selected by an OptionsField index (0/1/2), matching the order
 * the view registers them. Returns nullptr for an out-of-range index. */
inline const Instrument* tuner_instrument_by_index(int index) {
    switch (index) {
        case 0:
            return &tuner_guitar();
        case 1:
            return &tuner_violin();
        case 2:
            return &tuner_pitch_fork();
        default:
            return nullptr;
    }
}

class TunerView : public ui::View {
   public:
    TunerView();
    ~TunerView() override;

    TunerView(const TunerView&) = delete;
    TunerView& operator=(const TunerView&) = delete;

    std::string title() const override { return "Tuner"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void update_notes_for_instrument(const Instrument& instrument);
    void update_current_note();
    void start_tone();
    void stop_tone();

    /* Currently selected note. */
    uint16_t stored_frequency_{440};  /* upstream "frequency" (what PP plays) */
    int8_t octave_shift_{0};
    int played_frequency_{440};       /* real_note_frequency() — what host plays */
    bool playing_{false};

    /* Tone generator state. */
    double phase_{0.0};               /* [0, 1) fractional cycle position */
    std::vector<float> scratch_{};

    ui::Labels labels_{
        {{0, 0}, "Instrument:", ui::Color::light_grey()},
        {{0, 16}, "Note:", ui::Color::light_grey()},
        {{0, 32}, "Plays (Hz):", ui::Color::light_grey()},
        {{0, 48}, "PP tone (Hz):", ui::Color::light_grey()},
        {{0, 64}, "Oct shift:", ui::Color::light_grey()},
        {{0, 80}, "Volume:", ui::Color::light_grey()},
    };

    ui::OptionsField options_instrument_{
        {96, 0},
        14,
        {{"Guitar", 0}, {"Violin", 1}, {"Pitch Fork", 2}}};

    ui::OptionsField options_note_{
        {96, 16},
        14,
        {}};

    ui::Text text_played_frequency_{{112, 32, 80, 16}, ""};
    ui::Text text_stored_frequency_{{112, 48, 80, 16}, ""};
    ui::Text text_octave_shift_{{88, 64, 144, 16}, ""};

    ui::NumberField field_volume_{{64, 80}, 3, {0, 99}, 1, ' '};

    ui::Button button_play_stop_{{16, 112, 208, 32}, "Play"};

    ui::Text text_status_{{0, 176, 240, 16}, ""};

    ui::Labels host_note_{
        {{0, 208}, "Host plays the real string", ui::Color::grey()},
        {{0, 224}, "frequency (48 kHz out); the", ui::Color::grey()},
        {{0, 240}, "PP octave-shift is shown for", ui::Color::grey()},
        {{0, 256}, "reference only.", ui::Color::grey()},
    };
};

}  // namespace app

#endif /*__MB200_UI_TUNER_H__*/
