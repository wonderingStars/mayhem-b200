/*
 * mayhem-b200 — .PPL playlist editor, file browser and format helpers.
 *
 * Ported from firmware/application/external/playlist_editor/ui_playlist_editor.*
 * (copyleft 2025 zxkmm).
 *
 * Copyright (C) 2025 zxkmm (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_playlist_editor.hpp"

#include "app_context.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace app {

/* --- FileBrowserView ------------------------------------------------------- */

FileBrowserView::FileBrowserView(std::string start_dir, std::vector<std::string> extensions)
    : dir_{std::move(start_dir)},
      extensions_{std::move(extensions)} {
    add_children({&text_path_, &menu_, &button_cancel_});

    button_cancel_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };

    populate();
}

void FileBrowserView::on_show() {
    View::on_show();
    menu_.focus();
}

void FileBrowserView::populate() {
    menu_.clear();

    core::ListOptions options;
    options.extensions = extensions_;
    options.include_directories = true;

    std::vector<core::DirEntry> entries;
    core::list_directory(dir_, entries, options);

    /* Row to step up a level, unless we are already at the top. */
    const std::string parent = core::parent_path(dir_);
    if (!parent.empty() && parent != dir_) {
        menu_.add_item({"..", ui::Color::cyan(), [this, parent]() {
                            dir_ = parent;
                            populate();
                        }});
    }

    /* list_directory sorts directories first, then by name, so one pass keeps
     * folders grouped above files. */
    for (const auto& entry : entries) {
        const std::string name = entry.name;
        if (entry.is_directory) {
            menu_.add_item({name + "/", ui::Color::cyan(), [this, name]() {
                                dir_ = core::path_join(dir_, name);
                                populate();
                            }});
        } else {
            menu_.add_item({name, ui::Color::white(), [this, name]() {
                                const std::string full = core::path_join(dir_, name);
                                if (on_selected) on_selected(full);
                                if (auto* nav = globals().nav) nav->pop();
                            }});
        }
    }

    std::string shown = dir_;
    if (shown.size() > 29) shown = "..." + shown.substr(shown.size() - 26);
    text_path_.set(shown);
    set_dirty();
}

/* --- PlaylistItemEditView -------------------------------------------------- */

PlaylistItemEditView::PlaylistItemEditView(std::string item) {
    add_children({&labels_, &field_path_, &field_delay_,
                  &button_browse_, &button_delete_, &button_save_});

    parse_item(item);

    /* Only an existing entry can be deleted; set_on_delete reveals the button. */
    button_delete_.hidden(true);

    field_delay_.on_change = [this](int32_t v) {
        delay_ = static_cast<uint32_t>(v < 0 ? 0 : v);
    };

    button_browse_.on_select = [this](ui::Button&) {
        auto browser = std::make_unique<FileBrowserView>(
            core::captures_directory(), std::vector<std::string>{".C16", ".C8"});
        browser->on_selected = [this](const std::string& p) {
            path_ = p;
            field_path_.set_text(p);
        };
        if (auto* nav = globals().nav) nav->push(std::move(browser));
    };

    button_delete_.on_select = [this](ui::Button&) {
        if (on_delete) on_delete();
        if (auto* nav = globals().nav) nav->pop();
    };

    button_save_.on_select = [this](ui::Button&) {
        if (path_.empty()) {
            ui::display_modal("Error", "Pick a capture first, or press Back to cancel.");
            return;
        }
        if (on_save) on_save(build_item());
        if (auto* nav = globals().nav) nav->pop();
    };

    refresh_ui();
}

void PlaylistItemEditView::on_show() {
    View::on_show();
    button_save_.focus();
}

void PlaylistItemEditView::set_on_delete(std::function<void()> callback) {
    on_delete = std::move(callback);
    button_delete_.hidden(false);
}

void PlaylistItemEditView::parse_item(std::string_view item) {
    PplEntry e;
    if (parse_ppl_line(item, e)) {
        path_ = e.path;
        delay_ = e.delay_ms;
    } else {
        path_.clear();
        delay_ = 0;
    }
}

std::string PlaylistItemEditView::build_item() const {
    return format_ppl_line({path_, delay_});
}

void PlaylistItemEditView::refresh_ui() {
    field_path_.set_text(path_.empty() ? "(none)" : path_);
    field_delay_.set_value(static_cast<int32_t>(delay_), false);
}

/* --- PlaylistEditorView ---------------------------------------------------- */

PlaylistEditorView::PlaylistEditorView() {
    add_children({&labels_, &text_current_, &menu_, &text_hint_,
                  &button_new_, &button_open_, &button_insert_,
                  &button_edit_, &button_save_});

    menu_.on_highlight = [this](size_t i) {
        if (i < lines_.size())
            text_hint_.set("Edit: " + entry_label(lines_[i]));
    };

    button_new_.on_select = [this](ui::Button&) { new_file(); };
    button_open_.on_select = [this](ui::Button&) { open_file(); };
    button_insert_.on_select = [this](ui::Button&) { insert_item(); };
    button_edit_.on_select = [this](ui::Button&) { edit_item(); };
    button_save_.on_select = [this](ui::Button&) { save_file(); };
}

void PlaylistEditorView::on_show() {
    View::on_show();
    refresh_menu();
    if (loaded_ && menu_.item_count() > 0)
        menu_.focus();
    else
        button_open_.focus();
}

void PlaylistEditorView::new_file() {
    auto* nav = globals().nav;
    if (nav == nullptr) return;

    name_buffer_.clear();
    ui::text_prompt(
        *nav, name_buffer_, 64, ENTER_KEYBOARD_MODE_ALPHA,
        [this](std::string& s) {
            if (s.empty()) return;

            std::string fname = s;
            if (core::extension(fname).empty())
                fname += ".PPL";

            core::ensure_directory(playlists_directory());
            ppl_path_ = core::path_join(playlists_directory(), fname);
            core::write_file(ppl_path_, "");

            lines_.clear();
            loaded_ = true;
            text_current_.set(core::filename(ppl_path_));
            text_hint_.set("New playlist. Insert an item.");
            refresh_interface();
        });
}

void PlaylistEditorView::open_file() {
    core::ensure_directory(playlists_directory());
    auto browser = std::make_unique<FileBrowserView>(
        playlists_directory(), std::vector<std::string>{".PPL"});
    browser->on_selected = [this](const std::string& p) { load_file(p); };
    if (auto* nav = globals().nav) nav->push(std::move(browser));
}

void PlaylistEditorView::load_file(const std::string& path) {
    std::vector<std::string> raw;
    const auto result = core::read_lines(path, raw);
    if (!result) {
        ui::display_modal("Error", "Could not read\n" + path);
        return;
    }

    /* Keep comments; drop the blank lines upstream also strips on load. */
    lines_.clear();
    for (auto& line : raw)
        if (!line.empty())
            lines_.push_back(std::move(line));

    ppl_path_ = path;
    loaded_ = true;
    text_current_.set(core::filename(path));
    text_hint_.set(lines_.empty() ? "Empty playlist. Insert an item."
                                  : "Highlight an entry");
    refresh_interface();
}

void PlaylistEditorView::refresh_menu() {
    menu_.clear();
    for (const auto& line : lines_) {
        if (!line.empty() && line.front() == '#') {
            menu_.add_item({line.substr(0, 29), ui::Color::grey(),
                            [this]() { button_insert_.focus(); }});
        } else {
            menu_.add_item({entry_label(line), ui::Color::white(),
                            [this]() { button_edit_.focus(); }});
        }
    }
}

void PlaylistEditorView::refresh_interface() {
    const size_t previous = menu_.highlighted_index();
    refresh_menu();
    set_dirty();
    menu_.set_highlighted(previous);
}

void PlaylistEditorView::edit_item() {
    if (!loaded_ || lines_.empty()) {
        ui::display_modal("Error", "No entry to edit.");
        return;
    }

    const size_t idx = menu_.highlighted_index();
    if (idx >= lines_.size()) return;

    auto view = std::make_unique<PlaylistItemEditView>(lines_[idx]);
    view->set_on_delete([this, idx]() {
        if (idx < lines_.size()) lines_.erase(lines_.begin() + static_cast<ptrdiff_t>(idx));
        refresh_interface();
    });
    view->on_save = [this, idx](std::string new_item) {
        if (idx < lines_.size()) lines_[idx] = std::move(new_item);
        refresh_interface();
    };
    if (auto* nav = globals().nav) nav->push(std::move(view));
}

void PlaylistEditorView::insert_item() {
    if (!loaded_) {
        ui::display_modal("Error", "Create or open a playlist first.");
        return;
    }

    const size_t idx = menu_.highlighted_index();
    auto view = std::make_unique<PlaylistItemEditView>("");
    view->on_save = [this, idx](std::string new_item) {
        if (lines_.empty()) {
            lines_.push_back(std::move(new_item));
        } else {
            const size_t at = std::min(idx + 1, lines_.size());
            lines_.insert(lines_.begin() + static_cast<ptrdiff_t>(at), std::move(new_item));
        }
        refresh_interface();
    };
    if (auto* nav = globals().nav) nav->push(std::move(view));
}

void PlaylistEditorView::save_file() {
    if (ppl_path_.empty()) {
        ui::display_modal("Error", "No playlist loaded.");
        return;
    }
    if (lines_.empty()) {
        ui::display_modal("Error", "Playlist is empty.");
        return;
    }

    std::string content;
    for (const auto& line : lines_) {
        content += line;
        content += '\n';
    }

    core::ensure_directory(core::parent_path(ppl_path_));
    const auto result = core::write_file(ppl_path_, content);
    if (!result) {
        ui::display_modal("Error", "Could not save\n" + ppl_path_);
        return;
    }

    ui::display_modal("Saved", "Saved playlist\n" + ppl_path_);
}

std::string PlaylistEditorView::entry_label(const std::string& line) {
    const auto comma = line.find(',');
    return core::filename(trim(std::string_view{line}.substr(0, comma)));
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_playlist_editor{{
    "playlist_editor", "Playlist Edit", app::Category::Utilities,
    ui::Color::cyan(), &ui::bitmap_icon_notepad,
    [] { return std::make_unique<app::PlaylistEditorView>(); }}};
}  // namespace
