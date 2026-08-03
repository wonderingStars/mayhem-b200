/*
 * mayhem-b200 — WAV waveform viewer.
 *
 * Ported from firmware/application/external/wav_view/ui_view_wav.{hpp,cpp}
 * (ViewWavView, by Jared Boone / Furrtek / Mark Thompson). It opens an 8- or
 * 16-bit mono WAV, shows its metadata, a peak-envelope overview of the whole
 * file, and a zoomable/scrollable waveform window with two measurement cursors.
 *
 * What maps to the host and what does not (honesty rule, doc/PORTING.md):
 *   - Reading and rendering the waveform is pure file + framebuffer work and is
 *     implemented in full, built on core::WavFileReader and ui::Waveform.
 *   - Upstream also *plays* the file. Its playback path routes audio through the
 *     PortaPack baseband/transmitter (baseband::set_audiotx_config, a ReplayThread
 *     and TXProgress messages) — a firmware-only pipeline with no B200 equivalent.
 *     Playback is therefore not implemented; the view says so on screen rather
 *     than showing a dead Play button.
 *
 * The sample→column peak reduction, the peak→colour-index mapping and the
 * ns-per-pixel scale are factored into wav_view_detail for unit testing.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_VIEW_WAV_H__
#define __MB200_UI_VIEW_WAV_H__

#include "../core/wav_file.hpp"
#include "ui.hpp"
#include "ui_menu.hpp"
#include "ui_painter.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace app {

/* Pure, display-free maths, tested against the spec in test_wav_view.cpp. */
namespace wav_view_detail {

/* Peak (maximum absolute magnitude) of the samples that fall in each of
 * `columns` output columns — the standard waveform-overview envelope reduction.
 *
 * Column i covers the half-open sample range
 *     [ (count * i) / columns , (count * (i + 1)) / columns )
 * so the columns tile the whole file with no gaps or overlap. When there are
 * fewer samples than columns some columns are empty and get a peak of 0.
 *
 * `out` must have room for `columns` values. abs is taken in 32-bit so the
 * INT16_MIN case yields 32768, not a wrapped negative.
 *
 * This is a deliberate improvement on upstream's averaging overview (which its
 * own comment calls "world's worst downsampling"): a peak envelope shows short
 * transients an average would hide. */
inline void peak_columns(const int16_t* samples, size_t count,
                         uint16_t* out, size_t columns) {
    if (out == nullptr || columns == 0) return;

    for (size_t i = 0; i < columns; i++) {
        const size_t lo = (count * i) / columns;
        const size_t hi = (count * (i + 1)) / columns;

        int32_t peak = 0;
        for (size_t s = lo; s < hi; s++) {
            int32_t a = samples[s];
            if (a < 0) a = -a;
            if (a > peak) peak = a;
        }
        out[i] = static_cast<uint16_t>(peak);
    }
}

/* Maps a peak magnitude (0..32768) to an 8-bit colour-LUT index (0..255).
 * 32768 >> 7 = 256, clamped to 255. */
inline uint8_t peak_to_lut_index(uint16_t peak) {
    const uint32_t idx = static_cast<uint32_t>(peak) >> 7;
    return static_cast<uint8_t>(idx > 255 ? 255 : idx);
}

/* Nanoseconds represented by one horizontal pixel at the given sample rate and
 * integer zoom scale, matching upstream ViewWavView::update_scale():
 *     (1e9 / sample_rate) * scale
 * Returns 0 for a zero sample rate (caller guards). */
inline uint64_t ns_per_pixel(uint32_t sample_rate, int32_t scale) {
    if (sample_rate == 0) return 0;
    return (1000000000ULL / sample_rate) * static_cast<uint64_t>(scale);
}

}  // namespace wav_view_detail

/* A tiny file chooser: lists the .WAV files in the captures directory and calls
 * on_pick with the full path. Self-contained here because the host has no shared
 * FileLoadView yet and the porting contract forbids adding one. */
class WavFilePickerView : public ui::View {
   public:
    std::function<void(const std::string&)> on_pick{};

    WavFilePickerView();

    std::string title() const override { return "Open WAV"; }
    void on_show() override;
    void focus() override;

   private:
    std::string directory_{};

    ui::Text header_{{0, 0, 240, 16}, ""};
    ui::Text empty_note_{{0, 40, 240, 48}, ""};
    ui::MenuView menu_{{0, 20, 240, 260}};
    ui::Button button_back_{{72, 280, 96, 22}, "Back"};
};

class WavViewerView : public ui::View {
   public:
    WavViewerView();

    std::string title() const override { return "WAV Viewer"; }

    void paint(ui::Painter& painter) override;
    void on_show() override;
    void focus() override;

   private:
    static constexpr size_t kColumns = 240;
    /* Cap the in-RAM sample buffer so a pathologically long file cannot exhaust
     * memory. 32M int16 samples = 64 MB; longer files load a truncated view. */
    static constexpr size_t kMaxSamples = 32u * 1024u * 1024u;

    void open_picker();
    bool load(const std::string& path);
    void reset_controls();
    void update_scale(int32_t new_scale);
    void refresh_waveform();
    void refresh_measurements();
    void on_pos_time_changed();
    void on_pos_sample_changed();
    void set_status(const std::string& s);

    core::WavFileReader reader_{};
    std::vector<int16_t> samples_{};
    std::vector<int16_t> waveform_buffer_{};
    std::vector<uint16_t> overview_peaks_{};

    bool loaded_{false};
    int32_t scale_{1};
    uint64_t position_{0};
    uint64_t ns_per_pixel_{0};
    bool updating_position_{false};

    ui::Labels labels_{
        {{0, 0}, "File:", ui::Theme::getInstance()->fg_light->foreground},
        {{0, 16}, "Rate:", ui::Theme::getInstance()->fg_light->foreground},
        {{0, 32}, "Title:", ui::Theme::getInstance()->fg_light->foreground},
        {{0, 48}, "Dur:", ui::Theme::getInstance()->fg_light->foreground},
        {{0, 176}, "Pos:", ui::Theme::getInstance()->fg_light->foreground},
        {{72, 176}, ".", ui::Theme::getInstance()->fg_light->foreground},
        {{112, 176}, "s Scl:", ui::Theme::getInstance()->fg_light->foreground},
        {{0, 192}, "Sample:", ui::Theme::getInstance()->fg_light->foreground},
        {{0, 208}, "A:", ui::Theme::getInstance()->fg_darkcyan->foreground},
        {{64, 208}, "B:", ui::Theme::getInstance()->fg_magenta->foreground},
        {{0, 224}, "Delta:", ui::Theme::getInstance()->fg_light->foreground}};

    ui::Text text_filename_{{40, 0, 200, 16}, ""};
    ui::Text text_samplerate_{{40, 16, 200, 16}, ""};
    ui::Text text_title_{{48, 32, 192, 16}, ""};
    ui::Text text_duration_{{40, 48, 200, 16}, ""};

    ui::Button button_open_{{180, 2, 58, 28}, "Open"};

    ui::Waveform waveform_{
        {0, 72, 240, 64},
        nullptr,
        0,
        0,
        false,
        ui::Theme::getInstance()->fg_green->foreground};

    ui::NumberField field_pos_seconds_{{40, 176}, 4, {0, 0}, 1, ' '};
    ui::NumberField field_pos_ms_{{80, 176}, 3, {0, 999}, 1, '0'};
    ui::NumberField field_scale_{{160, 176}, 5, {1, 1}, 1, ' '};
    ui::NumberField field_pos_samples_{{64, 192}, 9, {0, 0}, 1, '0'};
    ui::NumberField field_cursor_a_{{24, 208}, 3, {0, 239}, 1, ' '};
    ui::NumberField field_cursor_b_{{88, 208}, 3, {0, 239}, 1, ' '};

    ui::Text text_delta_{{56, 224, 184, 16}, "-"};
    ui::Text text_status_{{0, 252, 240, 16}, ""};
    ui::Text text_note_{{0, 276, 240, 16}, ""};
};

}  // namespace app

#endif /*__MB200_UI_VIEW_WAV_H__*/
