/*
 * mayhem-b200 — .PPL playlist editor, and the .PPL format helpers and file
 * browser that the Replay app (ui_playlist.*) reuses.
 *
 * Ported from firmware/application/external/playlist_editor/ui_playlist_editor.*
 * (copyleft 2025 zxkmm). The upstream editor sits on the firmware's FileLoadView
 * for browsing and text_prompt for naming; here browsing is a small MenuView
 * lister (FileBrowserView, defined below because the host has no FileLoadView)
 * and file I/O goes through core::fs_utils instead of FatFs.
 *
 * A .PPL playlist is one entry per line:
 *
 *     <capture path>[,<delay milliseconds>]
 *
 * Blank lines and lines beginning with '#' are comments. The delay is how long
 * Replay waits before transmitting that entry. This is exactly what upstream
 * writes, so a .PPL made on a PortaPack loads here and vice versa.
 *
 * Copyright (C) 2025 zxkmm (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_PLAYLIST_EDITOR_H__
#define __MB200_UI_PLAYLIST_EDITOR_H__

#include "../core/file_path.hpp"
#include "../core/fs_utils.hpp"
#include "../core/string_format.hpp"
#include "ui.hpp"
#include "ui_alphanum.hpp"
#include "ui_menu.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace app {

/* --- .PPL format ----------------------------------------------------------
 * Kept as free functions so the parse/write logic is testable without building
 * a View (which needs a display and app::globals()). */

struct PplEntry {
    std::string path{};
    uint32_t delay_ms{0};
};

/* Reads leading decimal digits, stopping at the first non-digit — the host
 * stand-in for upstream's atoi()/parse_int() on the delay column. */
inline uint32_t ppl_parse_uint(std::string_view s) {
    uint32_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') break;
        v = v * 10u + static_cast<uint32_t>(c - '0');
    }
    return v;
}

/* Parses one line into an entry. Returns false for a comment ('#') or a line
 * with no usable path, matching upstream PlaylistView::load_file which skips
 * both. The path is everything before the first comma (trimmed); the delay is
 * the token after it. */
inline bool parse_ppl_line(std::string_view line, PplEntry& out) {
    if (line.empty() || line.front() == '#')
        return false;

    const auto comma = line.find(',');
    std::string path = trim(line.substr(0, comma));
    if (path.empty())
        return false;

    uint32_t delay = 0;
    if (comma != std::string_view::npos) {
        const auto rest = line.substr(comma + 1);
        const auto comma2 = rest.find(',');
        delay = ppl_parse_uint(trim(rest.substr(0, comma2)));
    }

    out.path = std::move(path);
    out.delay_ms = delay;
    return true;
}

/* Serialises one entry, matching upstream PlaylistItemEditView::build_item and
 * PlaylistView::save_file: "<path>,<delay>". */
inline std::string format_ppl_line(const PplEntry& e) {
    return e.path + "," + to_string_dec_uint(e.delay_ms);
}

/* Parses a whole file's lines into entries, skipping comments and blanks. */
inline std::vector<PplEntry> parse_ppl(const std::vector<std::string>& lines) {
    std::vector<PplEntry> out;
    for (const auto& line : lines) {
        PplEntry e;
        if (parse_ppl_line(line, e))
            out.push_back(std::move(e));
    }
    return out;
}

/* Where .PPL files live. The firmware uses /PLAYLISTS on the SD card; here it is
 * a folder under the app data directory. */
inline std::string playlists_directory() {
    return core::data_directory() + "/PLAYLISTS";
}

/* --- FileBrowserView ------------------------------------------------------
 * A minimal replacement for the firmware's FileLoadView: a directory listing in
 * a MenuView, entered/left with the "<dir>/" and ".." rows, filtered by
 * extension. Selecting a file fires on_selected and pops. Shared by both the
 * editor and the Replay app; it lives here (not a new shared header) so no two
 * agents race on the same filename. */
class FileBrowserView : public ui::View {
   public:
    std::function<void(const std::string& path)> on_selected{};

    FileBrowserView(std::string start_dir, std::vector<std::string> extensions);

    std::string title() const override { return "Select file"; }
    void on_show() override;

   private:
    void populate();

    std::string dir_{};
    std::vector<std::string> extensions_{};

    ui::Text text_path_{{0, 0, 240, 16}, ""};
    ui::MenuView menu_{{0, 18, 240, 252}};
    ui::Button button_cancel_{{0, 272, 240, 30}, "Cancel"};
};

/* --- PlaylistItemEditView -------------------------------------------------
 * Edits a single "<path>,<delay>" entry: browse for the capture, set the delay.
 * on_save gets the rebuilt line; on_delete (set only when editing an existing
 * entry, not when inserting) removes it. */
class PlaylistItemEditView : public ui::View {
   public:
    std::function<void(std::string)> on_save{};
    std::function<void()> on_delete{};

    explicit PlaylistItemEditView(std::string item);

    std::string title() const override { return "Edit Item"; }
    void on_show() override;

    /* Reveals the Delete button; call it only for an existing entry. */
    void set_on_delete(std::function<void()> callback);

    /* Exposed for tests: split "path,delay" and rebuild it. */
    void parse_item(std::string_view item);
    std::string build_item() const;

   private:
    void refresh_ui();

    std::string path_{};
    uint32_t delay_{0};

    ui::Labels labels_{
        {{0, 0}, "Path:", ui::Color::light_grey()},
        {{0, 48}, "Delay (ms):", ui::Color::light_grey()},
    };

    ui::TextField field_path_{{0, 18, 240, 16}, "(none)"};
    ui::NumberField field_delay_{{96, 48}, 5, {0, 99999}, 10, ' '};

    ui::Button button_browse_{{0, 80, 116, 30}, "Browse"};
    ui::Button button_delete_{{0, 150, 116, 30}, "Delete"};
    ui::Button button_save_{{124, 150, 116, 30}, "Save"};
};

/* --- PlaylistEditorView ---------------------------------------------------
 * Loads a .PPL into a menu, lets the user create/open/insert/edit/save. Raw
 * lines are kept (comments included) so a hand-edited playlist round-trips. */
class PlaylistEditorView : public ui::View {
   public:
    PlaylistEditorView();

    std::string title() const override { return "PPL Edit"; }
    void on_show() override;

   private:
    void new_file();
    void open_file();
    void load_file(const std::string& path);
    void refresh_menu();
    void refresh_interface();
    void edit_item();
    void insert_item();
    void save_file();

    /* The entry's display name: leaf filename, without the trailing ",delay". */
    static std::string entry_label(const std::string& line);

    std::vector<std::string> lines_{};
    std::string ppl_path_{};
    std::string name_buffer_{};  /* text_prompt needs a live string. */
    bool loaded_{false};

    ui::Labels labels_{
        {{0, 0}, "PPL:", ui::Color::light_grey()},
    };

    ui::Text text_current_{{40, 0, 200, 16}, "(none)"};

    ui::MenuView menu_{{0, 20, 240, 170}};

    ui::Text text_hint_{{0, 192, 240, 16}, "Open or create a playlist"};

    ui::Button button_new_{{0, 210, 116, 28}, "New"};
    ui::Button button_open_{{124, 210, 116, 28}, "Open"};
    ui::Button button_insert_{{0, 242, 116, 28}, "Ins After"};
    ui::Button button_edit_{{124, 242, 116, 28}, "Edit"};
    ui::Button button_save_{{0, 274, 240, 28}, "Save PPL"};
};

}  // namespace app

#endif /*__MB200_UI_PLAYLIST_EDITOR_H__*/
