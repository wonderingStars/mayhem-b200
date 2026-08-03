/*
 * mayhem-b200 — Notepad / text editor (Category::Utilities).
 *
 * Copyright (C) 2023 Kyle Reed
 * Copyright (C) 2023 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_text_editor.hpp"

#include "app_context.hpp"
#include "file_path.hpp"
#include "string_format.hpp"
#include "theme.hpp"
#include "ui_alphanum.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"

namespace app {

using namespace ui;

/* --- TextViewer ---------------------------------------------------------- */

TextViewer::TextViewer(Rect parent_rect)
    : Widget(parent_rect) {
    set_focusable(true);

    font_style_ = Theme::getInstance()->bg_darkest;  // 8x16
    char_height_ = font_style_->font.line_height();
    char_width_ = font_style_->font.char_width();
    if (char_height_ <= 0) char_height_ = 16;
    if (char_width_ <= 0) char_width_ = 8;
    max_line_ = static_cast<uint32_t>(parent_rect.height() / char_height_);
    max_col_ = static_cast<uint32_t>(parent_rect.width() / char_width_);
    if (max_line_ == 0) max_line_ = 1;
    if (max_col_ == 0) max_col_ = 1;
}

void TextViewer::on_focus() {
    set_dirty();
}

int32_t TextViewer::line_len(int32_t index) {
    if (!model_ || index < 0) return 0;
    if (static_cast<uint32_t>(index) >= model_->line_count()) return 0;
    return static_cast<int32_t>(model_->line_length(static_cast<size_t>(index)));
}

void TextViewer::paint(Painter& painter) {
    const auto r = screen_rect();
    const Style& s = *font_style_;

    painter.fill_rectangle(r, s.background);

    if (!model_ || model_->line_count() == 0)
        return;

    const uint32_t count = static_cast<uint32_t>(model_->line_count());

    // Follow the cursor vertically and horizontally.
    if (cursor_line_ < first_line_)
        first_line_ = cursor_line_;
    else if (cursor_line_ >= first_line_ + max_line_)
        first_line_ = cursor_line_ - max_line_ + 1;

    if (cursor_col_ < first_col_)
        first_col_ = cursor_col_;
    else if (cursor_col_ >= first_col_ + max_col_)
        first_col_ = cursor_col_ - max_col_ + 1;

    for (uint32_t i = 0; i < max_line_; ++i) {
        const uint32_t line = first_line_ + i;
        if (line >= count) break;

        const std::string text = model_->get_text(line, first_col_, max_col_);
        if (!text.empty())
            painter.draw_string(
                {r.left(), r.top() + static_cast<int>(i) * char_height_},
                s, text);
    }

    // Cursor: repaint the character under it inverted.
    if (has_focus() &&
        cursor_line_ >= first_line_ && cursor_line_ < first_line_ + max_line_ &&
        cursor_col_ >= first_col_ && cursor_col_ < first_col_ + max_col_) {
        const int cx = r.left() + static_cast<int>(cursor_col_ - first_col_) * char_width_;
        const int cy = r.top() + static_cast<int>(cursor_line_ - first_line_) * char_height_;
        const std::string cur = model_->get_line(cursor_line_);
        const char ch = (cursor_col_ < cur.size()) ? cur[cursor_col_] : ' ';
        const Style inv = s.invert();
        painter.draw_char({cx, cy}, inv, ch);
    }
}

bool TextViewer::apply_scrolling_constraints(int16_t delta_line, int16_t delta_col) {
    if (!model_ || model_->line_count() == 0)
        return false;

    const int32_t count = static_cast<int32_t>(model_->line_count());

    int32_t new_line = static_cast<int32_t>(cursor_line_) + delta_line;
    int32_t new_col = static_cast<int32_t>(cursor_col_) + delta_col;
    int32_t new_line_length = line_len(new_line);

    if (new_col < 0)
        --new_line;
    else if (new_col >= new_line_length && delta_line == 0) {
        // Only wrap when moving horizontally.
        new_col = 0;
        ++new_line;
    }

    // Snap to first/last line to make navigating easier, and let the focus
    // escape (return false) when already at the boundary.
    if (new_line < 0 && cursor_line_ > 0) {
        new_line = 0;
    } else if (new_line >= count) {
        const int32_t last_line = count - 1;
        if (static_cast<int32_t>(cursor_line_) < last_line)
            new_line = last_line;
    }

    if (new_line < 0 || new_line >= count)
        return false;

    new_line_length = line_len(new_line);

    if (new_line_length == 0)
        new_col = 0;
    else if (new_col >= new_line_length || new_col < 0)
        new_col = new_line_length - 1;

    cursor_line_ = static_cast<uint32_t>(new_line);
    cursor_col_ = static_cast<uint32_t>(new_col);

    if (on_cursor_moved)
        on_cursor_moved();

    return true;
}

bool TextViewer::on_key(const KeyEvent key) {
    int16_t delta_col = 0;
    int16_t delta_line = 0;

    switch (key) {
        case KeyEvent::Left:
            delta_col = -1;
            break;
        case KeyEvent::Right:
            delta_col = 1;
            break;
        case KeyEvent::Up:
            delta_line = -1;
            break;
        case KeyEvent::Down:
            delta_line = 1;
            break;
        case KeyEvent::Select:
            if (on_select) {
                on_select();
                return true;
            }
            return false;
        default:
            return false;
    }

    dir_ = (delta_col != 0) ? ScrollDirection::Horizontal : ScrollDirection::Vertical;
    const bool updated = apply_scrolling_constraints(delta_line, delta_col);

    if (updated)
        redraw();

    return updated;
}

bool TextViewer::on_encoder(const EncoderEvent delta) {
    bool updated;
    if (dir_ == ScrollDirection::Horizontal) {
        updated = apply_scrolling_constraints(0, static_cast<int16_t>(delta));
    } else {
        updated = apply_scrolling_constraints(static_cast<int16_t>(delta * 16), 0);
    }

    if (updated)
        redraw();

    return updated;
}

void TextViewer::cursor_home() {
    cursor_col_ = 0;
    if (on_cursor_moved) on_cursor_moved();
    redraw();
}

void TextViewer::cursor_end() {
    const int32_t len = line_len(static_cast<int32_t>(cursor_line_));
    cursor_col_ = (len > 0) ? static_cast<uint32_t>(len - 1) : 0;
    if (on_cursor_moved) on_cursor_moved();
    redraw();
}

void TextViewer::reset_cursor() {
    cursor_line_ = 0;
    cursor_col_ = 0;
    first_line_ = 0;
    first_col_ = 0;
    redraw();
}

void TextViewer::clamp_cursor() {
    if (!model_ || model_->line_count() == 0) {
        cursor_line_ = 0;
        cursor_col_ = 0;
        redraw();
        return;
    }

    const uint32_t count = static_cast<uint32_t>(model_->line_count());
    if (cursor_line_ >= count)
        cursor_line_ = count - 1;

    const int32_t len = line_len(static_cast<int32_t>(cursor_line_));
    if (len <= 0)
        cursor_col_ = 0;
    else if (cursor_col_ >= static_cast<uint32_t>(len))
        cursor_col_ = static_cast<uint32_t>(len - 1);

    if (on_cursor_moved) on_cursor_moved();
    redraw();
}

/* --- TextEditorView ------------------------------------------------------ */

TextEditorView::TextEditorView() {
    add_children({
        &viewer,
        &text_position,
        &text_size,
        &button_open,
        &button_edit,
        &button_addline,
        &button_delline,
        &button_save,
        &button_exit,
    });

    text_position.set_style(Theme::getInstance()->option_active);
    text_size.set_style(Theme::getInstance()->option_active);

    viewer.on_select = [this]() { show_edit_line(); };
    viewer.on_cursor_moved = [this]() { update_position(); };

    button_open.on_select = [this](Button&) { show_open_prompt(); };

    button_edit.on_select = [this](Button&) { show_edit_line(); };

    button_addline.on_select = [this](Button&) {
        if (!model_.is_open()) {
            display_modal("Notepad", "Open a file first.");
            return;
        }
        model_.insert_line(viewer.line());
        viewer.clamp_cursor();
        refresh_ui();
    };

    button_delline.on_select = [this](Button&) {
        if (!model_.is_open() || model_.line_count() == 0) {
            display_modal("Notepad", "Nothing to delete.");
            return;
        }
        model_.delete_line(viewer.line());
        viewer.clamp_cursor();
        refresh_ui();
    };

    button_save.on_select = [this](Button&) {
        if (!model_.is_open()) {
            display_modal("Notepad", "Nothing to save.");
            return;
        }
        if (model_.save(path_)) {
            refresh_ui();
        } else {
            display_modal("Save Error", "Could not write:\n" + path_);
        }
    };

    button_exit.on_select = [this](Button&) { request_exit(); };
}

void TextEditorView::on_show() {
    View::on_show();

    if (!pending_open_path_.empty()) {
        const std::string p = pending_open_path_;
        pending_open_path_.clear();
        open_file(p);
    }

    refresh_ui();

    if (model_.is_open() && model_.line_count() > 0)
        viewer.focus();
    else
        button_open.focus();
}

void TextEditorView::open_file(const std::string& path) {
    model_.close();
    viewer.clear_model();
    path_.clear();

    if (path.empty()) {
        display_modal("Notepad", "No path given.");
        return;
    }

    // Host addition: create a new empty file if it does not exist, so Notepad can
    // start a file without a file manager. Upstream opens existing files only.
    if (!core::exists(path)) {
        const auto w = core::write_file(path, "");
        if (!w) {
            display_modal("Notepad", "Cannot create:\n" + path + "\n" + w.message);
            return;
        }
    }

    if (!core::is_regular_file(path)) {
        display_modal("Notepad", "Not a file:\n" + path);
        return;
    }

    if (!model_.open(path)) {
        display_modal("Read Error", "Cannot open file:\n" + path);
        return;
    }

    path_ = path;
    viewer.set_model(&model_);
    viewer.reset_cursor();
}

void TextEditorView::show_open_prompt() {
    auto* nav = globals().nav;
    if (!nav) return;

    if (open_path_buffer_.empty())
        open_path_buffer_ = core::data_directory() + "/";

    text_prompt(
        *nav,
        open_path_buffer_,
        max_path_length,
        ENTER_KEYBOARD_MODE_ALPHA,
        [this](std::string& buffer) {
            // Defer the open (which may show a modal) until this view is on top
            // again — the host navigation has no set_on_pop continuation.
            pending_open_path_ = buffer;
        });
}

void TextEditorView::show_edit_line() {
    auto* nav = globals().nav;
    if (!nav) return;

    if (!model_.is_open()) {
        display_modal("Notepad", "Open a file first.");
        return;
    }

    // An empty file has no lines; give the user one to type into.
    if (model_.line_count() == 0) {
        model_.append_line("");
        viewer.set_model(&model_);
        viewer.clamp_cursor();
        refresh_ui();
    }

    const size_t ln = viewer.line();
    if (ln >= model_.line_count())
        return;

    edit_line_index_ = ln;
    edit_line_buffer_ = model_.get_line(ln);

    text_prompt(
        *nav,
        edit_line_buffer_,
        viewer.col(),
        max_edit_length,
        ENTER_KEYBOARD_MODE_ALPHA,
        [this](std::string& buffer) {
            model_.set_line(edit_line_index_, buffer);
        });
}

void TextEditorView::refresh_ui() {
    if (model_.is_open()) {
        update_position();
        std::string s = model_.dirty() ? "*" : "";
        s += "Lines:" + to_string_dec_uint(model_.line_count()) +
             " (" + to_string_file_size(model_.total_size()) + ")";
        text_size.set(s);
    } else {
        text_position.set("No file. Press Open.");
        text_size.set("Open creates it if new.");
    }
}

void TextEditorView::update_position() {
    if (model_.is_open()) {
        text_position.set(
            "Ln " + to_string_dec_uint(viewer.line() + 1) +
            ", Col " + to_string_dec_uint(viewer.col() + 1));
    }
}

void TextEditorView::request_exit() {
    auto* nav = globals().nav;
    if (!nav) return;

    if (model_.is_open() && model_.dirty()) {
        display_modal(
            *nav, "Notepad", "Save changes before exit?", YESNO,
            [this, nav](bool save_it) {
                if (save_it)
                    model_.save(path_);
                // choose() pops the modal; this pop removes the editor beneath it.
                nav->pop();
            });
    } else {
        nav->pop();
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_notepad{{
    "notepad",
    "Notepad",
    app::Category::Utilities,
    ui::Color::green(),
    &ui::bitmap_icon_notepad,
    [] { return std::make_unique<app::TextEditorView>(); },
    false,
}};
}  // namespace
