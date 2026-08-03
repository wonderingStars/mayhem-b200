/*
 * mayhem-b200 — OOK Editor: edit and replay a raw OOK waveform.
 *
 * Port of the firmware's application/external/ook_editor/* together with the
 * .OOK file format in application/ook_file.*. The app edits a payload — a
 * string of '0' and '1' fragments — plus its transmit parameters (frequency,
 * sample rate, symbol rate, repeat count, inter-repeat pause) and replays it as
 * an OOK burst. Payloads can be saved to and loaded from .OOK files whose
 * layout is byte-compatible with the PortaPack, so a waveform captured on
 * either side loads on the other:
 *
 *     Frequency SampleRate SymbolRate Repeat PauseSymbolDuration Payload
 *
 * where SampleRate is one of the tokens 250k / 1M / 2M / 5M / 10M / 20M.
 *
 * How it differs from the firmware:
 *
 *  - The M4 OOK image is replaced by dsp::OokKeyer streaming into
 *    radio::TransmitterModel in Raw mode; the symbol period is sample_rate /
 *    symbol_rate, exactly as ook_file.cpp's start_ook_file_tx() computes it.
 *  - File I/O uses std::fstream instead of the firmware's File/FileLineReader,
 *    but the on-disk bytes are identical.
 *  - The firmware's "Bug Key" run-length entry helper is not ported; the
 *    payload is edited directly as a 0/1 string. Everything else is here.
 *
 * The file format and the fragment packing are in namespace app::ook_editor
 * with no UI or radio dependency, so tests/test_ook_editor.cpp can round-trip a
 * file and assert the packed bytes against the format spec.
 *
 * LEGALITY: replaying a captured OOK waveform re-transmits whatever it encodes —
 * a garage door, a gate, a car remote. Doing so may be illegal where you are
 * and is your responsibility. The app transmits only when you press Send and
 * shows a warning on screen.
 *
 * Copyright (C) 2024 Samir Sánchez Garnica @sasaga92
 * Copyright (C) 2024 gullradriel (ook_file format)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_OOK_EDITOR_H__
#define __MB200_UI_OOK_EDITOR_H__

#include "../dsp/modulate.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace app {
namespace ook_editor {

/* Parsed contents of an .OOK file. Mirrors ook_file_data. */
struct OokFileData {
    uint64_t frequency{0};
    uint32_t sample_rate{0};
    uint16_t symbol_rate{0};
    uint16_t pause_symbol_duration{0};
    uint16_t repeat{0};
    std::string payload{};
};

/* Map between a sample rate in Hz and the file's token ("250k".."20M").
 * to_token returns "" for an unsupported rate; from_token sets ok=false. */
std::string sample_rate_token(uint32_t hz);
uint32_t sample_rate_from_token(const std::string& token, bool& ok);

/* Read the first line of an .OOK file into `out`. Returns false on a missing
 * file, a malformed line, or an unknown sample-rate token — the same conditions
 * that make read_ook_file() return false upstream. */
bool read_ook_file(const std::string& path, OokFileData& out);

/* Write `data` as a one-line .OOK file. Returns false for an unsupported sample
 * rate or if the file cannot be created. */
bool save_ook_file(const OokFileData& data, const std::string& path);

/* Pack a '0'/'1' payload into bytes, MSB-first (encoders::make_bitstream). */
size_t pack_payload(const std::string& payload, std::vector<uint8_t>& out);

/* Directory the editor keeps .OOK files in. */
std::string ook_directory();

}  // namespace ook_editor

class OOKEditorView : public ui::View {
   public:
    OOKEditorView();
    ~OOKEditorView() override;

    OOKEditorView(const OOKEditorView&) = delete;
    OOKEditorView& operator=(const OOKEditorView&) = delete;

    std::string title() const override { return "OOKEditor"; }

    void focus() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void set_payload(const std::string& payload);
    void draw_waveform();
    void update_from_file(const ook_editor::OokFileData& data);

    void toggle_tx();
    void start_tx();
    void stop_tx();

    void open_file();
    void save_file();

    bool transmitting_{false};
    std::string payload_{};
    std::string name_buffer_{};
    std::vector<uint8_t> bits_{};
    size_t bit_count_{0};
    std::shared_ptr<dsp::OokKeyer> keyer_{};

    static constexpr size_t kWaveformBufferSize = 550;
    int16_t waveform_buffer_[kWaveformBufferSize]{};

    ui::Labels labels_{
        {{0, 0}, "Freq", ui::Color::light_grey()},
        {{0, 2 * 8}, "SampleRate:", ui::Color::light_grey()},
        {{0, 4 * 8}, "SymbolRate:", ui::Color::light_grey()},
        {{17 * 8, 4 * 8}, "Rep:", ui::Color::light_grey()},
        {{0, 6 * 8}, "Pause(us):", ui::Color::light_grey()},
        {{0, 8 * 8}, "Payload:", ui::Color::light_grey()},
        {{0, 15 * 8}, "Waveform:", ui::Color::light_grey()}};

    ui::FrequencyField field_frequency_{{5 * 8, 0}};

    ui::OptionsField field_sample_rate_{
        {12 * 8, 2 * 8},
        5,
        {{"250k", 250000}, {"1M", 1000000}, {"2M", 2000000},
         {"5M", 5000000}, {"10M", 10000000}, {"20M", 20000000}}};

    ui::NumberField field_symbol_rate_{{12 * 8, 4 * 8}, 4, {1, 9999}, 1, '0'};
    ui::NumberField field_repeat_{{21 * 8, 4 * 8}, 3, {1, 999}, 1, '0'};
    ui::NumberField field_pause_{{12 * 8, 6 * 8}, 4, {0, 9999}, 1, '0'};

    ui::Text text_payload_{{0, 10 * 8, ui::screen_width, 16}, ""};

    ui::Button button_set_{{0, 12 * 8, 7 * 8, 24}, "Set"};
    ui::Button button_open_{{8 * 8, 12 * 8, 9 * 8, 24}, "Open"};
    ui::Button button_save_{{18 * 8, 12 * 8, 9 * 8, 24}, "Save"};

    ui::Waveform waveform_{
        {0, 17 * 8, ui::screen_width, 32},
        waveform_buffer_,
        0,
        0,
        true,
        ui::Color::yellow()};

    ui::Text text_status_{{0, 22 * 8, ui::screen_width, 16}, ""};
    ui::ProgressBar progressbar_{{0, 24 * 8, ui::screen_width, 16}};

    ui::Text text_warning_{{0, 26 * 8, ui::screen_width, 16},
                           "TX may be illegal - you are responsible"};

    ui::Button button_send_{{4 * 8, 34 * 8, 16 * 8, 28}, "Send"};
};

}  // namespace app

#endif /*__MB200_UI_OOK_EDITOR_H__*/
