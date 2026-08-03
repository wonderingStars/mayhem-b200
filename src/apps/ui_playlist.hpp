/*
 * mayhem-b200 — Replay: transmit a captured IQ file, or a .PPL playlist of them.
 *
 * Ported from firmware/application/apps/ui_playlist.* (the firmware calls the app
 * "Replay"). Upstream refills the M4's shared buffers with a ReplayThread and the
 * proc_replay baseband image drains them at the baseband clock; here the host
 * radio::ReplayModel paces the file at its own sample rate on a DSP thread and
 * pushes samples into a ring, and radio::TransmitterModel (Raw mode) pulls that
 * ring out through the B200's transmitter.
 *
 *   ReplayModel (paces file @ its rate) --push--> ring --pull--> TransmitterModel
 *
 * Playlist looping and the delay-between-tracks are app logic, exactly as
 * upstream's PlaylistView::next_track / send_current_track handle them; the
 * per-file end-of-stream and single-file transport live in ReplayModel.
 *
 * The .PPL format helpers and the file browser this app uses are shared with the
 * playlist editor and declared in ui_playlist_editor.hpp.
 *
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2023 Kyle Reed, zxkmm
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_PLAYLIST_H__
#define __MB200_UI_PLAYLIST_H__

#include "../core/iq_file.hpp"
#include "../dsp/ring_buffer.hpp"
#include "../radio/replay_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_playlist_editor.hpp"  /* PplEntry, parse/format, FileBrowserView */
#include "ui_widget.hpp"

#include <chrono>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace app {

class PlaylistView : public ui::View {
   public:
    PlaylistView();
    ~PlaylistView() override;

    std::string title() const override { return "Replay"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    /* One capture in the playlist: its path, its inter-track delay, and the
     * metadata inspected off disk (rate, centre frequency, duration). */
    struct Entry {
        std::string path{};
        uint32_t delay_ms{0};
        core::IqFileInfo info{};
    };

    /* --- playlist management --- */
    void load_playlist(const std::string& path);
    void add_capture(const std::string& path);
    void delete_entry();
    void save_playlist();
    Entry* current();
    bool at_end() const;

    /* --- transport --- */
    void toggle();
    void start();
    void stop();
    bool next_track();
    void send_current_track();

    void update_ui();
    void log(std::string_view line);

    /* --- state --- */
    radio::ReplayModel replay_{};
    /* 2^18 samples ≈ 0.5 s at 500 kHz; absorbs disk/scheduler jitter between the
     * pacing thread and the transmitter's pull. */
    dsp::RingBuffer<std::complex<float>> ring_{1u << 18};

    std::vector<Entry> entries_{};
    size_t index_{0};
    std::string playlist_path_{};
    bool dirty_{false};

    /* App-level "we are running the playlist" flag; distinct from ReplayModel's
     * per-file state because a track ending is not the playlist ending. */
    bool playing_{false};

    /* Inter-track delay: upstream sleeps the app thread; here we wait without
     * freezing the UI by deferring the play() until the deadline in
     * on_frame_sync(). */
    bool waiting_delay_{false};
    std::chrono::steady_clock::time_point delay_deadline_{};

    /* --- widgets --- */
    ui::Text text_filename_{{0, 0, 240, 16}, "-"};

    ui::Labels labels_{
        {{0, 20}, "Freq", ui::Color::light_grey()},
        {{0, 40}, "Dur", ui::Color::light_grey()},
    };

    ui::FrequencyField field_freq_{{40, 20}};
    ui::Text text_rate_{{160, 20, 80, 16}, "-"};
    ui::Text text_duration_{{40, 40, 96, 16}, "-"};
    ui::Checkbox check_loop_{{160, 40}, 4, "Loop"};

    ui::Text text_track_{{0, 60, 240, 16}, "Open playlist or add capture."};

    ui::ProgressBar bar_track_{{0, 80, 240, 10}};   /* playlist position */
    ui::ProgressBar bar_file_{{0, 92, 240, 10}};    /* within-file transmit */

    ui::Button button_play_{{0, 108, 76, 28}, "Play"};
    ui::Button button_prev_{{82, 108, 76, 28}, "Prev"};
    ui::Button button_next_{{164, 108, 76, 28}, "Next"};

    ui::Button button_add_{{0, 140, 116, 28}, "Add"};
    ui::Button button_delete_{{124, 140, 116, 28}, "Delete"};

    ui::Button button_open_{{0, 172, 116, 28}, "Open"};
    ui::Button button_save_{{124, 172, 116, 28}, "Save"};

    ui::Console console_{{0, 204, 240, 100}};
};

}  // namespace app

#endif /*__MB200_UI_PLAYLIST_H__*/
