/*
 * mayhem-b200 — on-screen keyboard and text entry.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_alphanum.hpp"

#include "theme.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <cstring>

namespace ui {

/* --- EntryTextEdit --------------------------------------------------------------- */

EntryTextEdit::EntryTextEdit(
    std::string& str,
    size_t max_length,
    Point position,
    uint32_t length)
    : Widget{{position, {8 * static_cast<int>(length), 16}}},
      text_{str},
      max_length_{std::max<size_t>(max_length, str.length())},
      char_count_{std::max<uint32_t>(length, 1)},
      cursor_pos_{static_cast<uint32_t>(str.length())},
      insert_mode_{true} {
    set_focusable(true);
}

const std::string& EntryTextEdit::value() const {
    return text_;
}

void EntryTextEdit::set_cursor(uint32_t pos) {
    cursor_pos_ = static_cast<uint32_t>(std::min<size_t>(pos, text_.length()));
    set_dirty();
}

void EntryTextEdit::set_insert_mode() {
    insert_mode_ = true;
    set_dirty();
}

void EntryTextEdit::set_overwrite_mode() {
    insert_mode_ = false;
    set_dirty();
}

void EntryTextEdit::char_add(char c) {
    /* Inserting past max_length would grow the caller's buffer beyond what it
     * asked for; overwriting past the end has nothing to overwrite. */
    if ((text_.length() >= max_length_ && insert_mode_) ||
        (cursor_pos_ >= text_.length() && !insert_mode_))
        return;

    if (insert_mode_)
        text_.insert(cursor_pos_, 1, c);
    else
        text_[cursor_pos_] = c;

    cursor_pos_++;
    set_dirty();
}

void EntryTextEdit::char_delete() {
    if (cursor_pos_ == 0) return;

    cursor_pos_--;
    text_.erase(cursor_pos_, 1);
    set_dirty();
}

void EntryTextEdit::paint(Painter& painter) {
    const auto rect = screen_rect();
    const Style text_style = has_focus() ? style().invert() : style();
    uint32_t offset = 0;

    /* Scroll the window so the cursor stays visible. */
    if (cursor_pos_ >= char_count_) offset = cursor_pos_ - char_count_ + 1;

    for (uint32_t i = 0; i < char_count_; i++) {
        /* Drawing spaces rather than filling the line first produces less
         * flicker on the real display, and costs nothing here. */
        const auto index = static_cast<size_t>(i) + offset;
        const auto c = (index < text_.length()) ? text_[index] : ' ';

        painter.draw_char(
            {rect.location().x() + (static_cast<int>(i) * static_cast<int>(char_width)),
             rect.location().y()},
            text_style, c);
    }

    const int cursor_x =
        static_cast<int>(char_width) *
        static_cast<int>(offset > 0 ? char_count_ - 1 : cursor_pos_);
    const Point cursor_point{screen_pos().x() + cursor_x, screen_pos().y()};
    const Style cursor_style = text_style.invert();

    /* In overwrite mode the character under the cursor is inverted, in insert
     * mode the cursor is a solid block between characters. */
    if (!insert_mode_ && cursor_pos_ < text_.length())
        painter.draw_char(cursor_point, cursor_style, text_[cursor_pos_]);

    const Rect cursor_box{cursor_point, {static_cast<int>(char_width),
                                         static_cast<int>(char_height)}};
    painter.draw_rectangle(cursor_box, cursor_style.background);
}

bool EntryTextEdit::on_key(const KeyEvent key) {
    if (key == KeyEvent::Left && cursor_pos_ > 0) {
        cursor_pos_--;
    } else if (key == KeyEvent::Right && cursor_pos_ < text_.length()) {
        cursor_pos_++;
    } else if (key == KeyEvent::Select) {
        if (key_is_long_pressed(key)) {
            /* Long select clears everything before the cursor. */
            text_ = text_.substr(cursor_pos_);
            set_cursor(0);
        } else {
            insert_mode_ = !insert_mode_;
        }
    } else if (key == KeyEvent::Back) {
        /* The window layer folds Backspace and Esc into Back. Backspace is the
         * only delete key a host keyboard offers, so it wins while there is
         * something to delete; an empty field declines and Esc closes the view
         * as usual. */
        if (cursor_pos_ == 0) return false;
        char_delete();
        return true;
    } else {
        return false;
    }

    set_dirty();
    return true;
}

bool EntryTextEdit::on_keyboard(const KeyboardEvent key) {
    if (key >= 0x20 && key <= 0x7e) {
        char_add(static_cast<char>(key));
        return true;
    }
    if (key == 8) {
        char_delete();
        return true;
    }
    return false;
}

bool EntryTextEdit::on_encoder(const EncoderEvent delta) {
    int32_t new_pos = static_cast<int32_t>(cursor_pos_) + delta;

    /* The encoder wraps around the ends of the text. */
    if (new_pos < 0)
        new_pos = static_cast<int32_t>(text_.length());
    else if (static_cast<size_t>(new_pos) > text_.length())
        new_pos = 0;

    set_cursor(static_cast<uint32_t>(new_pos));
    return true;
}

bool EntryTextEdit::on_touch(const TouchEvent event) {
    if (event.type == TouchEvent::Type::Start) focus();

    set_dirty();
    return true;
}

/* --- TextEntryView ---------------------------------------------------------- */

TextEntryView::TextEntryView(
    NavigationView& nav,
    std::string& str,
    size_t max_length)
    : nav_{nav},
      str_{str},
      text_input{str, max_length, {0, 0}} {
    add_children({&text_input,
                  &button_ok});

    button_ok.on_select = [this](Button&) { confirm(); };
}

void TextEntryView::confirm() {
    if (on_changed) on_changed(str_);
    nav_.pop();
}

void TextEntryView::char_delete() {
    text_input.char_delete();
}

void TextEntryView::char_add(const char c) {
    text_input.char_add(c);
}

void TextEntryView::set_cursor(uint32_t pos) {
    text_input.set_cursor(pos);
}

void TextEntryView::focus() {
    text_input.focus();
}

/* --- AlphanumView ----------------------------------------------------------- */

const AlphanumView::key_set_t AlphanumView::key_sets[3] = {
    {"abc", keys_lower, keys_upper},
    {"123", keys_digit, keys_symbl},
    {"hex", keys_hex, keys_hex}};

AlphanumView::AlphanumView(
    NavigationView& nav,
    std::string& str,
    size_t max_length,
    uint8_t enter_mode)
    : TextEntryView(nav, str, max_length) {
    add_children({
        &button_shift,
        &labels,
        &field_raw,
        &text_raw_to_char,
        &button_delete,
        &button_mode,
    });

    button_shift.on_select = [this](Button&) {
        auto next = static_cast<uint8_t>(shift_mode_) + 1;
        shift_mode_ = static_cast<ShiftMode>(next);

        /* A one-shot shift makes no sense in digits mode: the shifted set is a
         * different keyboard, not a case. */
        if ((mode_ == 1) && (shift_mode_ == ShiftMode::Shift))
            shift_mode_ = ShiftMode::ShiftLock;
        else if (next > static_cast<uint8_t>(ShiftMode::ShiftLock))
            shift_mode_ = ShiftMode::None;

        refresh_keys();
    };

    int16_t n = 0;
    for (auto& button : buttons) {
        button.id = n;
        button.on_select = [this](Button& b) { this->on_button(b); };
        button.on_char = [this](KeyboardEvent e) { return this->on_keyboard(e); };
        button.set_parent_rect({static_cast<Coord>((n % 5) * (screen_width / 5)),
                                static_cast<Coord>((n / 5) * 38 + 24),
                                screen_width / 5, 38});
        add_child(&button);
        n++;
    }

    set_enter_mode(enter_mode);

    button_mode.on_select = [this](Button&) {
        set_mode(mode_ + 1);
    };

    button_delete.on_select = [this](Button&) {
        char_delete();
    };

    field_raw.set_value('0');
    field_raw.on_select = [this](NumberField&) {
        char_add(static_cast<char>(field_raw.value()));
    };

    field_raw.on_change = [this](int32_t) {
        text_raw_to_char.set(std::string{static_cast<char>(field_raw.value())});
    };
}

void AlphanumView::set_enter_mode(uint8_t enter_mode) {
    /* Upstream indexes key_sets with enter_mode directly, so SYMBOLS lands on
     * hex and HEX falls off the end back to letters. Map it properly: symbols
     * are the shifted digit set. */
    switch (enter_mode) {
        case ENTER_KEYBOARD_MODE_DIGITS:
            set_mode(1, ShiftMode::None);
            break;
        case ENTER_KEYBOARD_MODE_SYMBOLS:
            set_mode(1, ShiftMode::ShiftLock);
            break;
        case ENTER_KEYBOARD_MODE_HEX:
            set_mode(2, ShiftMode::None);
            break;
        case ENTER_KEYBOARD_MODE_ALPHA:
        default:
            set_mode(0, ShiftMode::None);
            break;
    }
}

void AlphanumView::set_mode(uint32_t new_mode, ShiftMode new_shift_mode) {
    constexpr uint32_t key_set_count = static_cast<uint32_t>(std::size(key_sets));

    mode_ = (new_mode < key_set_count) ? new_mode : 0;
    shift_mode_ = new_shift_mode;
    refresh_keys();

    /* The mode button is labelled with what it will switch to. */
    const uint32_t next = (mode_ + 1 < key_set_count) ? mode_ + 1 : 0;
    button_mode.set_text(key_sets[next].name);
}

void AlphanumView::refresh_keys() {
    const char* key_list = (shift_mode_ == ShiftMode::None)
                               ? key_sets[mode_].normal
                               : key_sets[mode_].shifted;
    const size_t key_count = std::strlen(key_list);

    size_t n = 0;
    for (auto& button : buttons) {
        /* Short key sets (hex) leave the trailing buttons blank. Upstream gives
         * them a one-character string holding NUL, which paints a glyph. */
        if (n < key_count)
            button.set_text(std::string{key_list[n]});
        else
            button.set_text("");
        n++;
    }

    const auto* theme = Theme::getInstance();
    switch (shift_mode_) {
        case ShiftMode::None:
            button_shift.set_bg_color(theme->bg_dark->background);
            break;
        case ShiftMode::Shift:
            button_shift.set_bg_color(theme->bg_darkest->background);
            break;
        case ShiftMode::ShiftLock:
            button_shift.set_bg_color(theme->fg_blue->foreground);
            break;
    }
    button_shift.set_fg_color(theme->fg_light->foreground);
}

void AlphanumView::on_button(Button& button) {
    const auto text = button.text();
    if (!text.empty() && text[0] != '\0') char_add(text[0]);

    if (shift_mode_ == ShiftMode::Shift) {
        shift_mode_ = ShiftMode::None;
        refresh_keys();
    }
}

bool AlphanumView::on_encoder(const EncoderEvent delta) {
    /* Focus may have been moved with the arrow keys or the mouse since the last
     * turn of the encoder, so start from where it actually is. */
    for (size_t i = 0; i < buttons.size(); i++) {
        if (buttons[i].has_focus()) {
            focused_button_ = static_cast<int16_t>(i);
            break;
        }
    }

    focused_button_ = static_cast<int16_t>(focused_button_ + delta);
    if (focused_button_ < 0)
        focused_button_ = static_cast<int16_t>(buttons.size() - 1);
    else if (focused_button_ >= static_cast<int16_t>(buttons.size()))
        focused_button_ = 0;

    buttons[static_cast<size_t>(focused_button_)].focus();
    return true;
}

bool AlphanumView::on_keyboard(const KeyboardEvent event) {
    return text_input.on_keyboard(event);
}

/* --- text_prompt ------------------------------------------------------------ */

void text_prompt(
    NavigationView& nav,
    std::string& str,
    size_t max_length,
    uint8_t mode,
    std::function<void(std::string&)> on_done) {
    text_prompt(nav, str, static_cast<uint32_t>(str.length()), max_length, mode,
                std::move(on_done));
}

void text_prompt(
    NavigationView& nav,
    std::string& str,
    uint32_t cursor_pos,
    size_t max_length,
    uint8_t mode,
    std::function<void(std::string&)> on_done) {
    /* Built here rather than through push_new<> because the cursor and the
     * callback have to be set before the view goes on the stack. */
    auto view = std::make_unique<AlphanumView>(nav, str, max_length, mode);
    view->set_cursor(cursor_pos);
    view->on_changed = [on_done](std::string& value) {
        if (on_done) on_done(value);
    };
    nav.push(std::move(view));
}

}  // namespace ui
