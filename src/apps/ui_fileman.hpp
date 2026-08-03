/*
 * mayhem-b200 — file manager.
 *
 * A host port of firmware/application/apps/ui_fileman.* (the FileManagerView).
 * Upstream browses a FatFs SD card and reports FRESULT codes; this browses
 * core::data_directory() through core/fs_utils.hpp and every operation reports
 * an FsResult it can put on screen.
 *
 * Ported behaviour: directory-first sorted listing with file sizes, navigation
 * into sub-directories and back up (never above the browse root), rename via the
 * on-screen keyboard (ui_alphanum.hpp), cut/copy/paste, new folder / new file,
 * "Clean" (delete every file in a folder), a show-hidden toggle, and a
 * modal-confirmed delete (ui_modal.hpp) that goes through the fs_utils delete
 * guard so nothing outside the data directory can be removed.
 *
 * Deliberate deviations from upstream, noted where they matter:
 *  - Browsing is confined to core::data_directory(); ".." never rises above it.
 *    The firmware roams the whole SD card, but the delete guard here refuses
 *    anything outside the data directory, so confining navigation keeps the two
 *    consistent and keeps the app from wandering into the user's profile.
 *  - No pagination. The host MenuView scrolls, so the whole directory is loaded
 *    and the 20-items-per-page / 75-items-max limits upstream needs on a 200 MHz
 *    M0 are gone.
 *  - The action buttons are plain text buttons, not the icon NewButtons upstream
 *    uses — the host bitmap set does not carry rename/cut/copy/... icons.
 *  - Partner-file (.C8 <-> .TXT) rename/delete prompting is dropped: it relies on
 *    the firmware NavigationView::set_on_pop hook the host stack does not have.
 *  - Opening files in specialised viewers (playlist, text editor, IQ trim, image
 *    viewers) is left to those apps; this view browses and manages files.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_FILEMAN_H__
#define __MB200_UI_FILEMAN_H__

#include "ui.hpp"
#include "ui_menu.hpp"
#include "ui_widget.hpp"

#include "fs_utils.hpp"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace app {

/* --- Pure formatting helpers, testable without any UI ---------------------- */

/* Right-hand column label for a listing row:
 *  - parent (".."):  ""   (upstream shows nothing for the parent entry)
 *  - directory:      decimal item count, e.g. "12"
 *  - file:           to_string_file_size(size), e.g. "3kB"
 * A negative item_count is treated as 0. */
std::string fileman_size_label(bool is_directory, bool is_parent,
                               std::uintmax_t size, int item_count);

/* The path shown at the top, expressed relative to the browse root so an
 * absolute host path never fills the line. Purely lexical: separators are
 * normalised to '/', a trailing '/' is ignored, and the comparison is
 * case-insensitive (ASCII). root itself renders as "/", a child renders with a
 * leading '/', and anything not under root falls back to its own leaf name. */
std::string fileman_relative_display(const std::string& current,
                                     const std::string& root);

/* Fits text into max_cols columns of the fixed 8px font. When it does not fit
 * the *left* is dropped (the leaf name matters most) and a '<' marks the cut.
 * max_cols == 0 yields "". */
std::string fileman_fit_left(const std::string& text, std::size_t max_cols);

/* Composes a full menu row in `columns` columns: the left label (the name, with
 * a '/' appended for a directory or ".." for the parent) left-justified, the
 * right label right-justified. The name is truncated (head kept) when the two
 * would collide. columns == 0 yields "". */
std::string fileman_row_text(std::string_view name, bool is_directory,
                             bool is_parent, const std::string& right_label,
                             std::size_t columns);

/* --- The view -------------------------------------------------------------- */

class FileManagerView : public ui::View {
   public:
    FileManagerView();

    std::string title() const override { return "Fileman"; }

    void on_show() override;

   private:
    /* Longest name the rename / new-file / new-folder prompt accepts. */
    static constexpr std::size_t max_name_len = 64;
    /* Columns available in a menu row for the 240px-wide list. */
    static constexpr std::size_t row_columns = 28;
    /* Columns available in the top path line. */
    static constexpr std::size_t path_columns = 24;

    enum class Clip : std::uint8_t { None, Cut, Copy };

    struct Row {
        std::string name{};
        bool is_directory{false};
        bool is_parent{false};  /* the synthetic ".." entry */
        std::uintmax_t size{0};
        std::time_t modified{0};
        int item_count{0};  /* entries inside, for directories */
    };

    /* Rebuilds rows_ from the current directory and repaints the menu. */
    void reload();
    void rebuild_menu();
    void update_info();
    void set_status(const std::string& text);

    const Row* selected() const;
    std::string selected_full_path() const;
    bool selected_is_actionable() const;  /* valid and not ".." */

    void on_menu_activate();
    void navigate_to(const std::string& path);
    void go_up();

    void do_rename();
    void do_delete();
    void do_clean();
    void do_cut();
    void do_copy();
    void do_paste();
    void do_new_dir();
    void do_new_file();
    void toggle_hidden();

    void show_error(const std::string& title, const std::string& message);

    std::string root_path_{};
    std::string current_path_{};
    core::ListOptions options_{};
    std::vector<Row> rows_{};

    std::string clipboard_path_{};
    Clip clipboard_mode_{Clip::None};

    /* Must outlive the text-entry views that edit it. */
    std::string name_buffer_{};

    ui::Labels labels_{
        {{0, 4}, "Path:", ui::Color::light_grey()}};

    ui::Text text_path_{
        {6 * 8, 4, 240 - 6 * 8, 16},
        ""};

    ui::MenuView menu_{
        {0, 22, 240, 168},
        /*keep_highlight*/ true};

    ui::Text text_info_{
        {0, 194, 240, 16},
        ""};

    ui::Button button_rename_{{0, 212, 58, 24}, "Rename"};
    ui::Button button_delete_{{60, 212, 58, 24}, "Delete"};
    ui::Button button_cut_{{120, 212, 58, 24}, "Cut"};
    ui::Button button_copy_{{180, 212, 58, 24}, "Copy"};

    ui::Button button_paste_{{0, 238, 58, 24}, "Paste"};
    ui::Button button_clean_{{60, 238, 58, 24}, "Clean"};
    ui::Button button_newdir_{{120, 238, 58, 24}, "NewDir"};
    ui::Button button_newfile_{{180, 238, 58, 24}, "NewFile"};

    ui::Button button_hidden_{{0, 264, 88, 24}, "Hidden off"};
    ui::Button button_exit_{{180, 264, 58, 24}, "Exit"};
};

}  // namespace app

#endif /*__MB200_UI_FILEMAN_H__*/
