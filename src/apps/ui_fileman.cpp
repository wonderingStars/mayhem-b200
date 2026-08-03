/*
 * mayhem-b200 — file manager.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_fileman.hpp"

#include "app_context.hpp"
#include "file_path.hpp"
#include "string_format.hpp"
#include "ui_alphanum.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"

#include <cctype>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace app {

namespace {

/* Case-insensitive ASCII equality — same latitude as core's compare_ci and
 * upstream's path_iequal(). */
bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = std::toupper(static_cast<unsigned char>(a[i]));
        const auto cb = std::toupper(static_cast<unsigned char>(b[i]));
        if (ca != cb) return false;
    }
    return true;
}

/* '\\' -> '/', and drop a single trailing '/' (but keep a lone "/"). */
std::string normalize_sep(std::string_view in) {
    std::string s{in};
    for (auto& c : s)
        if (c == '\\') c = '/';
    if (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

std::string leaf_of(const std::string& normalized) {
    const auto pos = normalized.find_last_of('/');
    return pos == std::string::npos ? normalized : normalized.substr(pos + 1);
}

/* File-type colours, mirroring upstream's file_types association table (minus
 * the icons the host bitmap set does not carry). */
ui::Color color_for_extension(const std::string& ext) {
    struct Assoc {
        const char* ext;
        ui::Color color;
    };
    static const Assoc table[] = {
        {".TXT", ui::Color::white()},
        {".PNG", ui::Color::green()},
        {".BMP", ui::Color::green()},
        {".C8", ui::Color::dark_cyan()},
        {".CU8", ui::Color::dark_cyan()},
        {".C16", ui::Color::dark_cyan()},
        {".WAV", ui::Color::dark_magenta()},
        {".PPL", ui::Color::white()},
        {".REM", ui::Color::orange()},
    };
    for (const auto& a : table)
        if (iequals_ascii(ext, a.ext)) return a.color;
    return ui::Color::light_grey();
}

/* "YYYY-MM-DD HH:MM", local time. Empty when the timestamp is unknown. */
std::string format_time(std::time_t t) {
    if (t == 0) return {};
    std::tm tm_buf{};
#if defined(_WIN32)
    if (localtime_s(&tm_buf, &t) != 0) return {};
#else
    if (localtime_r(&t, &tm_buf) == nullptr) return {};
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf) == 0) return {};
    return std::string{buf};
}

}  // namespace

/* --- Pure formatting helpers ----------------------------------------------- */

std::string fileman_size_label(bool is_directory, bool is_parent,
                               std::uintmax_t size, int item_count) {
    if (is_parent) return {};
    if (is_directory) {
        const auto count = item_count < 0 ? 0 : static_cast<std::uint64_t>(item_count);
        return to_string_dec_uint(count);
    }
    return to_string_file_size(static_cast<uint64_t>(size));
}

std::string fileman_relative_display(const std::string& current,
                                     const std::string& root) {
    const std::string c = normalize_sep(current);
    const std::string r = normalize_sep(root);

    if (r.empty()) return c;
    if (iequals_ascii(c, r)) return "/";

    /* Under root: keep the tail, including the separator so it reads "/child". */
    if (c.size() > r.size() && c[r.size()] == '/' &&
        iequals_ascii(std::string_view{c}.substr(0, r.size()), r)) {
        return c.substr(r.size());
    }

    return leaf_of(c);
}

std::string fileman_fit_left(const std::string& text, std::size_t max_cols) {
    if (max_cols == 0) return {};
    if (text.size() <= max_cols) return text;
    return "<" + text.substr(text.size() - (max_cols - 1));
}

std::string fileman_row_text(std::string_view name, bool is_directory,
                             bool is_parent, const std::string& right_label,
                             std::size_t columns) {
    if (columns == 0) return {};

    std::string left;
    if (is_parent)
        left = "..";
    else if (is_directory)
        left = std::string{name} + "/";
    else
        left = std::string{name};

    if (right_label.empty())
        return left.size() > columns ? left.substr(0, columns) : left;

    if (right_label.size() >= columns)
        return right_label.substr(0, columns);

    const std::size_t max_left = columns - right_label.size() - 1;  /* >=1 gap */
    if (left.size() > max_left) left = left.substr(0, max_left);

    const std::size_t pad = columns - left.size() - right_label.size();
    return left + std::string(pad, ' ') + right_label;
}

/* --- FileManagerView ------------------------------------------------------- */

FileManagerView::FileManagerView() {
    root_path_ = core::data_directory();
    core::ensure_directory(root_path_);
    current_path_ = root_path_;

    add_children({&labels_, &text_path_, &menu_, &text_info_,
                  &button_rename_, &button_delete_, &button_cut_, &button_copy_,
                  &button_paste_, &button_clean_, &button_newdir_, &button_newfile_,
                  &button_hidden_, &button_exit_});

    menu_.on_highlight = [this](std::size_t) { update_info(); };

    button_rename_.on_select = [this](ui::Button&) { do_rename(); };
    button_delete_.on_select = [this](ui::Button&) { do_delete(); };
    button_cut_.on_select = [this](ui::Button&) { do_cut(); };
    button_copy_.on_select = [this](ui::Button&) { do_copy(); };
    button_paste_.on_select = [this](ui::Button&) { do_paste(); };
    button_clean_.on_select = [this](ui::Button&) { do_clean(); };
    button_newdir_.on_select = [this](ui::Button&) { do_new_dir(); };
    button_newfile_.on_select = [this](ui::Button&) { do_new_file(); };
    button_hidden_.on_select = [this](ui::Button&) { toggle_hidden(); };
    button_exit_.on_select = [this](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };

    reload();
}

void FileManagerView::on_show() {
    View::on_show();
    if (rows_.empty())
        button_newdir_.focus();
    else
        menu_.focus();
}

/* True when a and b denote the same directory (normalised, case-insensitive). */
static bool paths_equal(const std::string& a, const std::string& b) {
    return core::is_within_directory(a, b) && core::is_within_directory(b, a);
}

void FileManagerView::reload() {
    rows_.clear();
    menu_.clear();

    std::vector<core::DirEntry> entries;
    const auto listed = core::list_directory(current_path_, entries, options_);

    text_path_.set(fileman_fit_left(
        fileman_relative_display(current_path_, root_path_), path_columns));

    if (!listed) {
        text_info_.set(truncate(listed.message, row_columns + 2));
        return;
    }

    if (!paths_equal(current_path_, root_path_)) {
        Row up{};
        up.name = "..";
        up.is_directory = true;
        up.is_parent = true;
        rows_.push_back(std::move(up));
    }

    for (const auto& e : entries) {
        Row row{};
        row.name = e.name;
        row.is_directory = e.is_directory;
        row.size = e.size;
        row.modified = e.modified;
        if (e.is_directory) {
            const int count = core::file_count(core::path_join(current_path_, e.name));
            row.item_count = count < 0 ? 0 : count;
        }
        rows_.push_back(std::move(row));
    }

    rebuild_menu();
    update_info();
}

void FileManagerView::rebuild_menu() {
    for (const auto& row : rows_) {
        const std::string right =
            fileman_size_label(row.is_directory, row.is_parent, row.size, row.item_count);
        const std::string text =
            fileman_row_text(row.name, row.is_directory, row.is_parent, right, row_columns);

        ui::Color color;
        if (row.is_parent)
            color = ui::Color::cyan();
        else if (row.is_directory)
            color = ui::Color::yellow();
        else
            color = color_for_extension(core::extension(row.name));

        menu_.add_item({text, color, [this]() { on_menu_activate(); }});
    }
}

void FileManagerView::update_info() {
    const Row* s = selected();
    if (s == nullptr) {
        text_info_.set("");
        return;
    }
    if (s->is_parent) {
        set_status("Parent directory");
        return;
    }

    std::string info;
    if (s->is_directory) {
        info = to_string_dec_uint(static_cast<std::uint64_t>(s->item_count)) + " item(s)";
    } else {
        info = to_string_file_size(static_cast<uint64_t>(s->size));
        const auto when = format_time(s->modified);
        if (!when.empty()) info += "  " + when;
    }
    set_status(info);
}

void FileManagerView::set_status(const std::string& text) {
    std::string line = text;
    if (clipboard_mode_ != Clip::None && !clipboard_path_.empty()) {
        const char* verb = (clipboard_mode_ == Clip::Cut) ? "Cut:" : "Copy:";
        line = std::string{verb} + core::filename(clipboard_path_);
    }
    text_info_.set(truncate(line, row_columns + 2));
}

const FileManagerView::Row* FileManagerView::selected() const {
    const std::size_t idx = menu_.highlighted_index();
    if (idx >= rows_.size()) return nullptr;
    return &rows_[idx];
}

std::string FileManagerView::selected_full_path() const {
    const Row* s = selected();
    if (s == nullptr) return {};
    if (s->is_parent) return core::parent_path(current_path_);
    return core::path_join(current_path_, s->name);
}

bool FileManagerView::selected_is_actionable() const {
    const Row* s = selected();
    return s != nullptr && !s->is_parent;
}

void FileManagerView::on_menu_activate() {
    const Row* s = selected();
    if (s == nullptr) return;

    if (s->is_parent) {
        go_up();
    } else if (s->is_directory) {
        navigate_to(core::path_join(current_path_, s->name));
    }
    /* A file selection just leaves the info line as-is. */
}

void FileManagerView::navigate_to(const std::string& path) {
    if (!core::is_directory(path)) return;
    /* Never leave the browse root. */
    if (!paths_equal(path, root_path_) && !core::is_within_directory(path, root_path_))
        return;

    current_path_ = path;
    reload();
    menu_.set_highlighted(0);
    menu_.focus();
}

void FileManagerView::go_up() {
    if (paths_equal(current_path_, root_path_)) return;

    std::string parent = core::parent_path(current_path_);
    if (parent.empty() ||
        (!paths_equal(parent, root_path_) && !core::is_within_directory(parent, root_path_)))
        parent = root_path_;

    current_path_ = parent;
    reload();
    menu_.set_highlighted(0);
    menu_.focus();
}

void FileManagerView::do_rename() {
    if (!selected_is_actionable()) {
        show_error("Rename", "Select a file or folder.");
        return;
    }
    auto* nav = globals().nav;
    if (nav == nullptr) return;

    const Row* s = selected();
    const std::string from = selected_full_path();

    name_buffer_ = s->name;
    std::uint32_t cursor = static_cast<std::uint32_t>(name_buffer_.size());
    if (!s->is_directory) {
        const auto dot = name_buffer_.find_last_of('.');
        if (dot != std::string::npos) cursor = static_cast<std::uint32_t>(dot);
    }

    ui::text_prompt(
        *nav, name_buffer_, cursor, max_name_len, ENTER_KEYBOARD_MODE_ALPHA,
        [this, from](std::string& renamed) {
            if (renamed.empty()) return;
            const auto to = core::path_join(current_path_, renamed);
            const auto r = core::rename_path(from, to);
            if (!r) show_error("Rename failed", r.message);
            reload();
        });
}

void FileManagerView::do_delete() {
    if (!selected_is_actionable()) {
        show_error("Delete", "Select a file or folder.");
        return;
    }
    const std::string full = selected_full_path();
    const Row* s = selected();

    if (s->is_directory && !core::is_empty_directory(full)) {
        show_error("Delete", "Folder is not empty.\nUse Clean to empty it first.");
        return;
    }

    /* Surface a guard refusal before the user commits to the modal. */
    const auto allowed = core::may_delete(full);
    if (!allowed) {
        show_error("Delete refused", allowed.message);
        return;
    }

    auto* nav = globals().nav;
    if (nav == nullptr) return;

    const std::string name = core::filename(full);
    ui::display_modal(
        *nav, "Delete", "Delete " + name + "?\nAre you sure?", ui::YESNO,
        [this, full](bool choice) {
            if (!choice) return;
            const auto r = core::delete_path(full);  /* guarded, data-dir only */
            if (!r) show_error("Delete failed", r.message);
            reload();
        });
}

void FileManagerView::do_clean() {
    if (!selected_is_actionable()) {
        show_error("Clean", "Select a folder or a file.");
        return;
    }

    const std::string sel = selected_full_path();
    std::string dir = core::is_directory(sel) ? sel : core::parent_path(sel);
    if (dir.empty()) dir = current_path_;

    if (core::is_empty_directory(dir)) {
        show_error("Clean", "Folder is already empty.");
        return;
    }

    auto* nav = globals().nav;
    if (nav == nullptr) return;

    ui::display_modal(
        *nav, "Clean",
        "Delete ALL files in this\nfolder (sub-folders kept)?", ui::YESNO,
        [this, dir](bool choice) {
            if (!choice) return;

            core::ListOptions files_only;
            files_only.include_directories = false;
            files_only.include_hidden = true;

            std::vector<core::DirEntry> files;
            core::list_directory(dir, files, files_only);

            std::string first_error;
            for (const auto& f : files) {
                const auto r = core::delete_path(core::path_join(dir, f.name));
                if (!r && first_error.empty()) first_error = r.message;
            }
            if (!first_error.empty()) show_error("Clean failed", first_error);
            reload();
        });
}

void FileManagerView::do_cut() {
    if (!selected_is_actionable()) {
        show_error("Cut", "Can't cut that.");
        return;
    }
    clipboard_path_ = selected_full_path();
    clipboard_mode_ = Clip::Cut;
    update_info();
}

void FileManagerView::do_copy() {
    const Row* s = selected();
    if (!selected_is_actionable() || (s != nullptr && s->is_directory)) {
        show_error("Copy", "Can't copy that.\n(Use Cut for folders.)");
        return;
    }
    clipboard_path_ = selected_full_path();
    clipboard_mode_ = Clip::Copy;
    update_info();
}

void FileManagerView::do_paste() {
    if (clipboard_mode_ == Clip::None || clipboard_path_.empty()) {
        show_error("Paste", "Cut or copy something first.");
        return;
    }
    if (!core::exists(clipboard_path_)) {
        show_error("Paste", "The source is gone.");
        clipboard_path_.clear();
        clipboard_mode_ = Clip::None;
        return;
    }

    const std::string leaf = core::filename(clipboard_path_);
    const std::string unique = core::unique_filename(current_path_, leaf);
    if (unique.empty()) {
        show_error("Paste", "No free filename here.");
        return;
    }
    const std::string dest = core::path_join(current_path_, unique);

    core::FsResult r;
    if (clipboard_mode_ == Clip::Cut) {
        if (paths_equal(core::parent_path(clipboard_path_), current_path_)) {
            show_error("Paste", "Already in this folder.");
            return;
        }
        r = core::move_path(clipboard_path_, dest);
    } else {
        r = core::copy_path(clipboard_path_, dest);
    }

    if (!r) {
        show_error("Paste failed", r.message);
        return;
    }

    clipboard_path_.clear();
    clipboard_mode_ = Clip::None;
    reload();
}

void FileManagerView::do_new_dir() {
    auto* nav = globals().nav;
    if (nav == nullptr) return;

    name_buffer_.clear();
    ui::text_prompt(
        *nav, name_buffer_, max_name_len, ENTER_KEYBOARD_MODE_ALPHA,
        [this](std::string& dir_name) {
            if (dir_name.empty()) return;
            const auto r = core::create_directory(core::path_join(current_path_, dir_name));
            if (!r) show_error("New folder failed", r.message);
            reload();
        });
}

void FileManagerView::do_new_file() {
    auto* nav = globals().nav;
    if (nav == nullptr) return;

    name_buffer_.clear();
    ui::text_prompt(
        *nav, name_buffer_, max_name_len, ENTER_KEYBOARD_MODE_ALPHA,
        [this](std::string& file_name) {
            if (file_name.empty()) return;
            const auto r = core::create_file(core::path_join(current_path_, file_name));
            if (!r) show_error("New file failed", r.message);
            reload();
        });
}

void FileManagerView::toggle_hidden() {
    options_.include_hidden = !options_.include_hidden;
    button_hidden_.set_text(options_.include_hidden ? "Hidden on" : "Hidden off");
    reload();
}

void FileManagerView::show_error(const std::string& title, const std::string& message) {
    if (auto* nav = globals().nav) ui::display_modal(*nav, title, message);
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_fileman{{
    "filemanager",
    "File Manager",
    app::Category::Utilities,
    ui::Color::green(),
    &ui::bitmap_icon_dir,
    [] { return std::make_unique<app::FileManagerView>(); },
    false,
}};
}  // namespace
