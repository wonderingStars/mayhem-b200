/*
 * mayhem-b200 — Remote: a user-defined button panel that replays saved
 * OOK / sub-GHz captures. A "universal remote".
 *
 * Ported from firmware/application/external/remote/ui_remote.* . The upstream
 * app binds each grid button to a captured IQ file plus its centre frequency,
 * sample rate, icon and colours, saves the layout to a .REM text file, and
 * replays a button's capture through the transmitter on press. The file format
 * is preserved byte-for-byte so a .REM authored on a PortaPack loads here and
 * vice-versa:
 *
 *   line 1                : the remote's name (trimmed; '#'/blank lines skipped)
 *   each subsequent line  : path,name,icon,bg_color,fg_color,center_freq,rate
 *
 * The send path mirrors PlaylistView: upstream refills the M4's shared buffers
 * with a ReplayThread and the proc_replay baseband drains them; here a
 * radio::ReplayModel paces the file at its own sample rate on a DSP thread,
 * pushes samples into a ring, and radio::TransmitterModel (Raw mode) pulls the
 * ring out through the B200's transmitter. RF output needs a USRP B200 and is
 * unverified without hardware.
 *
 * Copyright (C) 2023 Kyle Reed (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_REMOTE_H__
#define __MB200_UI_REMOTE_H__

#include "../core/file_path.hpp"
#include "../core/iq_file.hpp"
#include "../dsp/ring_buffer.hpp"
#include "../radio/replay_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <complex>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace app {

/* --- Comma-format helpers (upstream string_format/file_reader semantics) ----
 * Kept as free functions, and declared here, so the .REM round trip can be
 * tested directly. These match the firmware's split_string()/join()/parse_int()
 * exactly, which is what makes the file format compatible. */

/* firmware/application/file_reader.cpp: split on `c`, keeping empty fields; a
 * non-empty string always yields at least one column, and a trailing separator
 * yields a trailing empty column. */
std::vector<std::string_view> remote_split(std::string_view str, char c);

/* firmware/common/utility.cpp join(): parts separated by `c`, no trailing
 * separator. */
std::string remote_join(char c, std::initializer_list<std::string_view> parts);

/* firmware/common/convert.hpp parse_int(): decimal, 0 on anything unparseable
 * (strtoull stops at the first non-digit). */
uint64_t remote_parse_uint(std::string_view s);

/* --- Icon and colour tables ------------------------------------------------
 * The stored icon/colour are small integer indices. The colour table matches
 * upstream's RemoteColors 21-entry table exactly. The icon table keeps the same
 * 25 index slots as upstream's RemoteIcons so an index authored on a PortaPack
 * still edits and round-trips here; slots whose bitmap the trimmed host set does
 * not carry map to nullptr (a plain tile), never to a wrong icon. */

size_t remote_icon_count();               /* 25, upstream RemoteIcons::size() */
const ui::Bitmap* remote_icon_at(size_t index);  /* nullptr for slot 0/absent */

size_t remote_color_count();              /* 21, upstream RemoteColors::size() */
ui::Color remote_color_at(size_t index);

/* Where .REM files live. Upstream uses /REMOTES on the SD card; here a folder
 * under the app data directory. */
inline std::string remotes_directory() {
    return core::data_directory() + "/REMOTES";
}

/* --- Data model ------------------------------------------------------------ */

/* One remote button: a capture path plus how to send and draw it. The two
 * radio fields (centre frequency, sample rate) are stored explicitly in the
 * .REM file, the way upstream stores capture_metadata's two members, so a send
 * does not depend on a .TXT sidecar surviving next to the capture. */
struct RemoteEntryModel {
    std::string path{};
    std::string name{};
    uint8_t icon{0};
    uint8_t bg_color{0};
    uint8_t fg_color{0};
    uint64_t center_frequency{0};
    uint32_t sample_rate{0};

    /* "path,name,icon,bg_color,fg_color,center_frequency,sample_rate". */
    std::string to_string() const;

    /* Parses one line. Needs at least 7 comma columns (upstream's guard);
     * columns beyond the seventh are ignored. Returns nullopt on too few. */
    static std::optional<RemoteEntryModel> parse(std::string_view line);
};

/* A whole remote: a name and its buttons. */
struct RemoteModel {
    std::string name{};
    std::vector<RemoteEntryModel> entries{};

    /* Removes the entry `entry` points at (a pointer *into* `entries`), the way
     * upstream's delete_entry does its lookup. Returns false if the pointer is
     * not one of our entries. */
    bool delete_entry(const RemoteEntryModel* entry);

    /* Button lookup helpers. entry_at maps a grid slot to its entry; index_of
     * is the inverse (an entry pointer back to its slot); find_by_name returns
     * the first entry with a matching name. All return null / -1 when absent. */
    RemoteEntryModel* entry_at(size_t index);
    const RemoteEntryModel* entry_at(size_t index) const;
    int index_of(const RemoteEntryModel* entry) const;
    const RemoteEntryModel* find_by_name(std::string_view entry_name) const;

    /* Replaces the model with the file's contents (first line = name, the rest
     * = entries). Returns false only if the file cannot be read. */
    bool load(const std::string& path);

    /* Writes the name then one line per entry. */
    bool save(const std::string& path) const;
};

/* --- Grid button ----------------------------------------------------------- */

/* A NewButton bound to a RemoteEntryModel, drawing it in its stored colours and
 * icon. Short-press sends (on_select2); long-press edits (on_long_select) —
 * exactly upstream's RemoteButton. */
class RemoteButton : public ui::NewButton {
   public:
    std::function<void(RemoteButton&)> on_select2{};
    std::function<void(RemoteButton&)> on_long_select{};

    RemoteButton(ui::Rect parent_rect, RemoteEntryModel* entry);
    RemoteButton(const RemoteButton&) = delete;
    RemoteButton& operator=(const RemoteButton&) = delete;

    bool on_key(ui::KeyEvent key) override;
    void paint(ui::Painter& painter) override;

    RemoteEntryModel* entry() { return entry_; }
    void set_entry(RemoteEntryModel* entry);

   protected:
    ui::Style paint_style() override;

   private:
    /* Hidden: sends go through on_select2 / on_key, not the base on_select. */
    using ui::NewButton::on_select;
    RemoteEntryModel* entry_{nullptr};
};

/* --- Edit view ------------------------------------------------------------- */

/* Edits one button: pick a capture, set frequency, icon and colours. Changes
 * are written straight into the referenced entry; the parent refreshes the grid
 * when this view pops (RemoteAppView::on_show). */
class RemoteEntryEditView : public ui::View {
   public:
    std::function<void(RemoteEntryModel&)> on_delete{};

    explicit RemoteEntryEditView(RemoteEntryModel& entry);

    std::string title() const override { return "Edit Button"; }
    void focus() override;

   private:
    void refresh_ui();
    void load_path(std::string path);

    RemoteEntryModel& entry_;
    std::string name_buffer_{};  /* commit-on-OK buffer for the name prompt */

    ui::Labels labels_{
        {{2 * 8, 1 * 16}, "Name:", ui::Color::light_grey()},
        {{2 * 8, 2 * 16}, "Path:", ui::Color::light_grey()},
        {{2 * 8, 3 * 16}, "Freq:", ui::Color::light_grey()},
        {{2 * 8, 4 * 16}, "Rate:", ui::Color::light_grey()},
        {{2 * 8, 5 * 16}, "Icon:", ui::Color::light_grey()},
        {{2 * 8, 6 * 16}, "FG:", ui::Color::light_grey()},
        {{2 * 8, 7 * 16}, "BG:", ui::Color::light_grey()},
        {{9 * 8, 9 * 16}, "Preview", ui::Color::light_grey()},
    };

    ui::TextField field_name_{{8 * 8, 1 * 16, 20 * 8, 1 * 16}, {}};
    ui::TextField field_path_{{8 * 8, 2 * 16, 20 * 8, 1 * 16}, {}};
    ui::FrequencyField field_freq_{{8 * 8, 3 * 16}};
    ui::Text text_rate_{{8 * 8, 4 * 16, 20 * 8, 1 * 16}, "-"};

    ui::NumberField field_icon_index_{{8 * 8, 5 * 16}, 2, {0, 24}, 1, ' ', true};
    ui::NumberField field_fg_color_index_{{8 * 8, 6 * 16}, 2, {0, 20}, 1, ' ', true};
    ui::NumberField field_bg_color_index_{{8 * 8, 7 * 16}, 2, {0, 20}, 1, ' ', true};

    RemoteButton button_preview_{{10 * 8, 10 * 16, 10 * 8, 3 * 16}, &entry_};

    ui::Button button_delete_{{2 * 8, 17 * 16, 10 * 8, 2 * 16}, "Delete"};
    ui::Button button_done_{{18 * 8, 17 * 16, 10 * 8, 2 * 16}, "Done"};
};

/* --- App view -------------------------------------------------------------- */

class RemoteAppView : public ui::View {
   public:
    RemoteAppView();
    ~RemoteAppView() override;

    RemoteAppView(const RemoteAppView&) = delete;
    RemoteAppView& operator=(const RemoteAppView&) = delete;

    std::string title() const override { return "Remote"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    static constexpr size_t button_cols = 3;
    static constexpr size_t button_rows = 4;
    static constexpr size_t max_buttons = button_cols * button_rows;
    static constexpr ui::Dim button_width = 240 / button_cols;   /* 80 */
    static constexpr ui::Dim button_area_height = 200;
    static constexpr ui::Dim button_height = button_area_height / button_rows; /* 50 */
    static constexpr ui::Coord buttons_top = 20;

    /* Grid */
    void create_buttons();
    void reset_buttons();
    void refresh_ui();

    /* Actions */
    void add_button();
    void edit_button(RemoteButton& btn);
    void send_button(RemoteButton& btn);
    void stop();

    /* Remote file lifecycle */
    void new_remote();
    void open_remote();
    void init_remote();
    bool load_remote(std::string path);
    void save_remote(bool show_errors = true);
    void rename_remote(const std::string& new_name);
    void set_remote_path(std::string path);

    bool is_sending() const { return sending_; }
    void show_error(const std::string& msg) const;
    void log(std::string_view line);

    RemoteModel model_{};
    std::string remote_path_{};
    bool needs_save_{false};
    bool sending_{false};
    RemoteButton* current_btn_{nullptr};
    std::string name_buffer_{};  /* commit-on-OK buffer for title/filename */

    radio::ReplayModel replay_{};
    /* 2^18 samples ≈ 0.5 s at 500 kHz; absorbs disk/scheduler jitter between the
     * pacing thread and the transmitter's pull. Same size PlaylistView uses. */
    dsp::RingBuffer<std::complex<float>> ring_{1u << 18};

    std::vector<std::unique_ptr<RemoteButton>> buttons_{};

    ui::TextField field_title_{{0, 2, 240, 16}, {}};

    ui::TextField field_filename_{{0, 222, 17 * 8, 16}, {}};
    ui::Checkbox check_loop_{{20 * 8, 222}, 4, "Loop"};

    ui::Console console_{{0, 244, 240, 28}};

    ui::Button button_add_{{0, 274, 76, 28}, "Add"};
    ui::Button button_new_{{82, 274, 76, 28}, "New"};
    ui::Button button_open_{{164, 274, 76, 28}, "Open"};
};

}  // namespace app

#endif /*__MB200_UI_REMOTE_H__*/
