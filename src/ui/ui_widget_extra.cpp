/*
 * mayhem-b200 — the rest of the PortaPack widget set (implementation).
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2025 HTotoo
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version. See LICENSE.GPL-2.0-or-later.
 */

#include "ui_widget_extra.hpp"

#include "../core/string_format.hpp"
#include "ui_font_fixed_8x16.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace ui {

namespace {

/* The firmware keeps these two in application/string_format.cpp. They are only
 * used by SymField, so they live here rather than widening the host's
 * string_format API. Semantics are upstream's, including the "unknown digit
 * reads as 0" behaviour that SymField relies on for Custom symbol lists. */
uint8_t char_to_uint(char c, uint8_t radix) {
    uint8_t v = 0;

    if (c >= '0' && c <= '9')
        v = static_cast<uint8_t>(c - '0');
    else if (c >= 'A' && c <= 'F')
        v = static_cast<uint8_t>(c - 'A' + 10);
    else if (c >= 'a' && c <= 'f')
        v = static_cast<uint8_t>(c - 'a' + 10);

    return v < radix ? v : uint8_t{0};
}

char uint_to_char(uint8_t val, uint8_t radix) {
    if (val >= radix)
        return 0;

    if (val < 10)
        return static_cast<char>('0' + val);

    return static_cast<char>('A' + val - 10);
}

/* firmware/application/bitmap.hpp's bitmap_tab_edge — the 8x24 sloped right
 * edge of a tab. Reproduced here because the host has no bitmap.hpp and Tab is
 * the only widget that needs it. */
constexpr uint8_t tab_edge_data[] = {
    0x00, 0x01, 0x01, 0x03, 0x03, 0x03, 0x07, 0x07,
    0x07, 0x0F, 0x0F, 0x0F, 0x1F, 0x1F, 0x1F, 0x1F,
    0x3F, 0x3F, 0x3F, 0x7F, 0x7F, 0x7F, 0xFF, 0xFF};

constexpr Bitmap bitmap_tab_edge{{8, 24}, tab_edge_data};

}  // namespace

/* --- LiveDateTime ---------------------------------------------------------- */

LiveDateTime::LiveDateTime(Rect parent_rect)
    : Widget{parent_rect} {
    refresh();
}

void LiveDateTime::refresh() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    datetime_.year = static_cast<uint16_t>(tm.tm_year + 1900);
    datetime_.month = static_cast<uint8_t>(tm.tm_mon + 1);
    datetime_.day = static_cast<uint8_t>(tm.tm_mday);
    datetime_.hour = static_cast<uint8_t>(tm.tm_hour);
    datetime_.minute = static_cast<uint8_t>(tm.tm_min);
    datetime_.second = static_cast<uint8_t>(tm.tm_sec);

    format();
}

void LiveDateTime::set_datetime(const DateTime& dt) {
    datetime_ = dt;
    format();
}

void LiveDateTime::format() {
    const std::string before = text;

    text.clear();
    if (!hide_clock) {
        if (date_enabled) {
            text = to_string_dec_uint(datetime_.year, 4, '0') + "-" +
                   to_string_dec_uint(datetime_.month, 2, '0') + "-" +
                   to_string_dec_uint(datetime_.day, 2, '0') + " ";
        } else {
            /* Upstream pads so the time stays put when the date is hidden. */
            text = "           ";
        }

        text += to_string_dec_uint(datetime_.hour, 2, '0') + ":" +
                to_string_dec_uint(datetime_.minute, 2, '0');

        if (seconds_enabled)
            text += ":" + to_string_dec_uint(datetime_.second, 2, '0');
    }

    if (text != before) set_dirty();
}

/* The firmware repainted from an RTC second-tick signal. Per porting rule 5 the
 * host does its per-frame work here instead, and only redraws when the rendered
 * string actually changes. */
void LiveDateTime::on_frame_sync() {
    refresh();
}

void LiveDateTime::paint(Painter& painter) {
    const auto rect = screen_rect();
    const auto s = style();

    painter.fill_rectangle(rect, s.background);
    painter.draw_string(rect.location(), s, text);
}

void LiveDateTime::set_hide_clock(bool new_value) {
    hide_clock = new_value;
    format();
}

void LiveDateTime::set_date_enabled(bool new_value) {
    date_enabled = new_value;
    format();
}

void LiveDateTime::set_seconds_enabled(bool new_value) {
    seconds_enabled = new_value;
    format();
}

/* --- ButtonWithEncoder ----------------------------------------------------- */

ButtonWithEncoder::ButtonWithEncoder(Rect parent_rect, std::string text, bool instant_exec)
    : Widget{parent_rect},
      text_{std::move(text)},
      instant_exec_{instant_exec} {
    set_focusable(true);
}

void ButtonWithEncoder::set_text(const std::string& value) {
    text_ = value;
    set_dirty();
}

std::string ButtonWithEncoder::text() const { return text_; }

int32_t ButtonWithEncoder::get_encoder_delta() const { return encoder_delta_; }

void ButtonWithEncoder::set_encoder_delta(const int32_t delta) { encoder_delta_ = delta; }

void ButtonWithEncoder::paint(Painter& painter) {
    const auto r = screen_rect();

    Color bg, fg;
    if (has_focus() || highlighted()) {
        bg = style().foreground;
        fg = Theme::getInstance()->bg_darkest->background;
    } else {
        bg = Theme::getInstance()->bg_medium->background;
        fg = style().foreground;
    }

    const Style paint_style{style().font, bg, fg};

    /* Bevel: light along the top, dark down the right and along the bottom. */
    painter.draw_rectangle({r.location(), {r.width(), 1}},
                           Theme::getInstance()->bg_light->background);
    painter.draw_rectangle({r.left(), r.top() + r.height() - 1, r.width(), 1},
                           Theme::getInstance()->bg_dark->background);
    painter.draw_rectangle({r.left() + r.width() - 1, r.top(), 1, r.height()},
                           Theme::getInstance()->bg_dark->background);

    painter.fill_rectangle({r.left(), r.top() + 1, r.width() - 1, r.height() - 2},
                           paint_style.background);

    const auto label_r = paint_style.font.size_of(text_);
    painter.draw_string({r.left() + (r.width() - label_r.width()) / 2,
                         r.top() + (r.height() - label_r.height()) / 2},
                        paint_style, text_);
}

void ButtonWithEncoder::on_focus() {
    if (on_highlight) on_highlight(*this);
}

bool ButtonWithEncoder::on_key(const KeyEvent key) {
    if (key == KeyEvent::Select) {
        if (on_select) {
            on_select(*this);
            return true;
        }
    } else {
        if (on_dir) return on_dir(*this, key);
    }

    return false;
}

bool ButtonWithEncoder::on_keyboard(const KeyboardEvent key) {
    if (key == 32 || key == 10) {
        if (on_select) {
            on_select(*this);
            return true;
        }
    }
    return false;
}

bool ButtonWithEncoder::on_touch(const TouchEvent event) {
    switch (event.type) {
        case TouchEvent::Type::Start:
            set_highlighted(true);
            set_dirty();
            if (on_touch_press) on_touch_press(*this);
            if (on_select && instant_exec_) on_select(*this);
            return true;

        case TouchEvent::Type::End:
            set_highlighted(false);
            set_dirty();
            if (on_touch_release) on_touch_release(*this);
            if (on_select && !instant_exec_) on_select(*this);
            return true;

        default:
            return false;
    }
}

bool ButtonWithEncoder::on_encoder(const EncoderEvent delta) {
    if (delta != 0) {
        encoder_delta_ += delta;
        delta_change_ = true;
        if (on_change) on_change();
    } else {
        delta_change_ = false;
    }
    return true;
}

/* --- NewButton ------------------------------------------------------------- */

NewButton::NewButton(Rect parent_rect, std::string text, const Bitmap* bitmap)
    : NewButton{parent_rect, std::move(text), bitmap,
                Theme::getInstance()->fg_darkcyan->foreground} {}

NewButton::NewButton(Rect parent_rect,
                     std::string text,
                     const Bitmap* bitmap,
                     Color color,
                     bool vertical_center)
    : Widget{parent_rect},
      color_{color},
      text_{std::move(text)},
      bitmap_{bitmap},
      vertical_center_{vertical_center} {
    set_focusable(true);
}

void NewButton::set_text(const std::string& value) {
    text_ = value;
    set_dirty();
}

std::string NewButton::text() const { return text_; }

void NewButton::set_bitmap(const Bitmap* bitmap) {
    bitmap_ = bitmap;
    set_dirty();
}

const Bitmap* NewButton::bitmap() const { return bitmap_; }

void NewButton::set_color(Color color) {
    color_ = color;
    set_dirty();
}

void NewButton::set_bg_color(Color color) {
    bg_color_ = color;
    set_dirty();
}

void NewButton::set_vertical_center(bool value) {
    vertical_center_ = value;
    set_dirty();
}

Color NewButton::color() const { return color_; }

void NewButton::paint(Painter& painter) {
    if (!bitmap_ && text_.empty()) return;

    const auto r = screen_rect();
    const Style s = paint_style();

    painter.draw_rectangle({r.location(), {r.width(), 1}},
                           Theme::getInstance()->bg_light->background);
    painter.draw_rectangle({r.left(), r.top() + r.height() - 1, r.width(), 1},
                           Theme::getInstance()->bg_dark->background);
    painter.draw_rectangle({r.left() + r.width() - 1, r.top(), 1, r.height()},
                           Theme::getInstance()->bg_dark->background);

    painter.fill_rectangle({r.left(), r.top() + 1, r.width() - 1, r.height() - 2},
                           s.background);

    int y = r.top();

    if (vertical_center_) {
        const int bmp_h = bitmap_ ? bitmap_->size.height() : 0;
        const int txt_h = !text_.empty() ? s.font.line_height() : 0;
        int spacing = 0;
        if (bmp_h > 0 && txt_h > 0) {
            const int content_height = bmp_h + txt_h;
            const int remaining_space = r.height() - content_height;
            spacing = std::max(4, remaining_space / 3);
        }
        const int total_height = bmp_h + txt_h + spacing;
        y += (r.height() - total_height) / 2;

        if (bitmap_) {
            const Point bmp_pos{r.left() + (r.width() / 2) - (bitmap_->size.width() / 2), y};
            y += bitmap_->size.height();
            painter.draw_bitmap(bmp_pos, *bitmap_, color_, s.background);
        }

        if (!text_.empty()) {
            if (bitmap_) y += spacing;

            std::string draw_text = text_;
            auto label_r = s.font.size_of(draw_text);
            if (label_r.width() > r.width() - 2) {
                const size_t max_chars =
                    static_cast<size_t>(std::max(0, (r.width() - 2) / s.font.char_width()));
                draw_text = draw_text.substr(0, max_chars);
                label_r = s.font.size_of(draw_text);
            }
            painter.draw_string({r.left() + (r.width() - label_r.width()) / 2, y},
                                s, draw_text);
        }
    } else {
        if (bitmap_) {
            const Point bmp_pos{r.left() + (r.width() / 2) - (bitmap_->size.width() / 2),
                                r.top() + 6};
            y += bitmap_->size.height() - 6;
            painter.draw_bitmap(bmp_pos, *bitmap_, color_, s.background);
        }

        if (!text_.empty()) {
            std::string draw_text = text_;
            auto label_r = s.font.size_of(draw_text);
            if (label_r.width() > r.width() - 2) {
                const size_t max_chars =
                    static_cast<size_t>(std::max(0, (r.width() - 2) / s.font.char_width()));
                draw_text = draw_text.substr(0, max_chars);
                label_r = s.font.size_of(draw_text);
            }
            painter.draw_string({r.left() + (r.width() - label_r.width()) / 2,
                                 y + (r.height() - label_r.height()) / 2},
                                s, draw_text);
        }
    }
}

Style NewButton::paint_style() {
    MutableStyle s{style()};

    if (has_focus() || highlighted()) {
        s.background = style().foreground;
        s.foreground = Theme::getInstance()->bg_darkest->background;
    } else {
        s.background = bg_color_;
        s.foreground = style().foreground;
    }

    return s;
}

void NewButton::on_focus() {
    if (on_highlight) on_highlight(*this);
}

bool NewButton::on_key(const KeyEvent key) {
    if (key == KeyEvent::Select) {
        if (on_select) {
            on_select();
            return true;
        }
    } else {
        if (on_dir) return on_dir(*this, key);
    }

    return false;
}

bool NewButton::on_keyboard(const KeyboardEvent key) {
    if (key == 32 || key == 10) {
        if (on_select) {
            on_select();
            return true;
        }
    }
    return false;
}

bool NewButton::on_touch(const TouchEvent event) {
    switch (event.type) {
        case TouchEvent::Type::Start:
            set_highlighted(true);
            set_dirty();
            return true;

        case TouchEvent::Type::End:
            set_highlighted(false);
            set_dirty();
            if (on_select) on_select();
            return true;

        default:
            return false;
    }
}

/* --- Image ----------------------------------------------------------------- */

Image::Image()
    : Image{{},
            nullptr,
            Theme::getInstance()->bg_darkest->foreground,
            Theme::getInstance()->bg_darkest->background} {}

Image::Image(const Rect parent_rect,
             const Bitmap* bitmap,
             const Color foreground,
             const Color background)
    : Widget{parent_rect},
      bitmap_{bitmap},
      foreground_{foreground},
      background_{background} {}

void Image::set_bitmap(const Bitmap* bitmap) {
    bitmap_ = bitmap;
    set_dirty();
}

void Image::set_foreground(const Color color) {
    foreground_ = color;
    set_dirty();
}

void Image::set_background(const Color color) {
    background_ = color;
    set_dirty();
}

void Image::invert_colors() {
    std::swap(foreground_, background_);
    set_dirty();
}

void Image::paint(Painter& painter) {
    if (!bitmap_) return;

    /* Also covers ImageButton: focus/highlight swaps the two colours. */
    const bool selected = (has_focus() || highlighted());
    painter.draw_bitmap(screen_pos(), *bitmap_,
                        selected ? background_ : foreground_,
                        selected ? foreground_ : background_);
}

/* --- ImageButton ----------------------------------------------------------- */

ImageButton::ImageButton(const Rect parent_rect,
                         const Bitmap* bitmap,
                         const Color foreground,
                         const Color background)
    : Image{parent_rect, bitmap, foreground, background} {
    set_focusable(true);
}

bool ImageButton::on_key(const KeyEvent key) {
    if (key == KeyEvent::Select) {
        if (on_select) {
            on_select(*this);
            return true;
        }
    }
    return false;
}

bool ImageButton::on_keyboard(const KeyboardEvent key) {
    if (key == 32 || key == 10) {
        if (on_select) {
            on_select(*this);
            return true;
        }
    }
    return false;
}

bool ImageButton::on_touch(const TouchEvent event) {
    switch (event.type) {
        case TouchEvent::Type::Start:
            set_highlighted(true);
            set_dirty();
            return true;

        case TouchEvent::Type::End:
            set_highlighted(false);
            set_dirty();
            if (on_select) on_select(*this);
            return true;

        default:
            return false;
    }
}

/* --- ImageToggle ----------------------------------------------------------- */

ImageToggle::ImageToggle(Rect parent_rect, const Bitmap* bitmap_)
    : ImageToggle{parent_rect,
                  bitmap_,
                  Theme::getInstance()->fg_green->foreground,
                  Theme::getInstance()->fg_light->foreground,
                  Theme::getInstance()->bg_dark->background} {}

ImageToggle::ImageToggle(Rect parent_rect,
                         const Bitmap* bitmap_,
                         Color foreground_true,
                         Color foreground_false,
                         Color background_)
    : ImageToggle{parent_rect,
                  bitmap_,
                  bitmap_,
                  foreground_true,
                  background_,
                  foreground_false,
                  background_} {}

ImageToggle::ImageToggle(Rect parent_rect,
                         const Bitmap* bitmap_true,
                         const Bitmap* bitmap_false,
                         Color foreground_true,
                         Color background_true,
                         Color foreground_false,
                         Color background_false)
    : ImageButton{parent_rect, bitmap_false, foreground_false, background_false},
      bitmap_true_{bitmap_true},
      bitmap_false_{bitmap_false},
      foreground_true_{foreground_true},
      background_true_{background_true},
      foreground_false_{foreground_false},
      background_false_{background_false},
      value_{false} {
    ImageButton::on_select = [this](ImageButton&) { set_value(!value()); };
}

bool ImageToggle::value() const { return value_; }

void ImageToggle::set_value(bool b) {
    if (b == value_) return;

    value_ = b;
    set_bitmap(b ? bitmap_true_ : bitmap_false_);
    set_foreground(b ? foreground_true_ : foreground_false_);
    set_background(b ? background_true_ : background_false_);

    if (on_change) on_change(b);
}

/* --- ImageOptionsField ----------------------------------------------------- */

ImageOptionsField::ImageOptionsField(Rect parent_rect,
                                     Color foreground,
                                     Color background,
                                     options_t options)
    : Widget{parent_rect},
      options_{std::move(options)},
      foreground_{foreground},
      background_{background} {
    set_focusable(true);
}

size_t ImageOptionsField::selected_index() const { return selected_index_; }

size_t ImageOptionsField::selected_index_value() const {
    if (selected_index_ >= options_.size()) return 0;
    return static_cast<size_t>(options_[selected_index_].second);
}

void ImageOptionsField::set_selected_index(const size_t new_index) {
    if (new_index < options_.size()) {
        if (new_index != selected_index()) {
            selected_index_ = new_index;
            if (on_change) on_change(selected_index_, options_[selected_index_].second);
            set_dirty();
        }
    }
}

void ImageOptionsField::set_by_value(value_t v) {
    size_t new_index = 0;
    for (const auto& option : options_) {
        if (option.second == v) {
            set_selected_index(new_index);
            return;
        }
        new_index++;
    }

    /* No exact match: upstream falls back to the first option. */
    set_selected_index(0);
}

void ImageOptionsField::set_options(options_t new_options) {
    options_ = std::move(new_options);

    /* An impossible index first, so selecting 0 always fires on_change. */
    selected_index_ = static_cast<size_t>(-1);
    set_selected_index(0);
    set_dirty();
}

void ImageOptionsField::paint(Painter& painter) {
    const bool selected = (has_focus() || highlighted());
    const Style s = selected ? style().invert() : style();
    const auto r = screen_rect();

    painter.draw_rectangle({r.location(), {r.width() + 4, r.height() + 4}}, s.background);

    /* Host guard: upstream dereferences options[selected] unconditionally, which
     * traps on a field whose options have not been set yet. */
    if (selected_index_ >= options_.size()) return;
    if (options_[selected_index_].first == nullptr) return;

    painter.draw_bitmap({screen_pos().x() + 2, screen_pos().y() + 2},
                        *options_[selected_index_].first, foreground_, background_);
}

void ImageOptionsField::on_focus() {
    if (on_show_options) on_show_options();
}

bool ImageOptionsField::on_encoder(const EncoderEvent delta) {
    /* Deliberately unsigned: stepping below zero wraps to a huge index, which
     * set_selected_index() rejects. That is upstream's "clamp at the ends". */
    set_selected_index(static_cast<size_t>(static_cast<ptrdiff_t>(selected_index_) + delta));
    return true;
}

bool ImageOptionsField::on_keyboard(const KeyboardEvent key) {
    if (key == '+' || key == ' ' || key == 10) return on_encoder(1);
    if (key == '-' || key == 8) return on_encoder(-1);
    return false;
}

bool ImageOptionsField::on_key(const KeyEvent event) {
    if (event == KeyEvent::Select) {
        on_encoder(1);
        return true;
    }
    return false;
}

bool ImageOptionsField::on_touch(const TouchEvent event) {
    if (event.type == TouchEvent::Type::Start) focus();
    return true;
}

/* --- TextEdit -------------------------------------------------------------- */

TextEdit::TextEdit(std::string& str, size_t max_length, Point position, uint32_t length)
    : Widget{{position, {8 * static_cast<int>(length), 16}}},
      text_{str},
      max_length_{std::max<size_t>(max_length, str.length())},
      char_count_{std::max<uint32_t>(length, 1)},
      cursor_pos_{static_cast<uint32_t>(text_.length())},
      insert_mode_{true} {
    set_focusable(true);
}

const std::string& TextEdit::value() const { return text_; }

void TextEdit::set_cursor(uint32_t pos) {
    cursor_pos_ = static_cast<uint32_t>(std::min<size_t>(pos, text_.length()));
    set_dirty();
}

void TextEdit::set_insert_mode() { insert_mode_ = true; }

void TextEdit::set_overwrite_mode() { insert_mode_ = false; }

void TextEdit::char_add(char c) {
    /* Don't grow past max_length when inserting, and don't overwrite past the
     * end of the text. */
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

void TextEdit::char_delete() {
    if (cursor_pos_ == 0) return;

    cursor_pos_--;
    text_.erase(cursor_pos_, 1);
    set_dirty();
}

void TextEdit::paint(Painter& painter) {
    const auto rect = screen_rect();
    const Style text_style = has_focus() ? style().invert() : style();

    /* Scroll the view so the cursor stays visible. */
    uint32_t offset = 0;
    if (cursor_pos_ >= char_count_) offset = cursor_pos_ - char_count_ + 1;

    /* draw_char right up to char_count_ blanks the tail with spaces, which
     * flickers less than filling the rectangle first. */
    for (uint32_t i = 0; i < char_count_; i++) {
        const size_t index = static_cast<size_t>(i) + offset;
        const char c = (index < text_.length()) ? text_[index] : ' ';

        painter.draw_char({rect.location().x() + static_cast<int>(i * char_width),
                           rect.location().y()},
                          text_style, c);
    }

    const int cursor_x =
        static_cast<int>(char_width * (offset > 0 ? char_count_ - 1 : cursor_pos_));
    const Point cursor_point{screen_pos().x() + cursor_x, screen_pos().y()};
    const Style cursor_style = text_style.invert();

    /* Overwrite mode inverts the character under the cursor. */
    if (!insert_mode_ && cursor_pos_ < text_.length())
        painter.draw_char(cursor_point, cursor_style, text_[cursor_pos_]);

    painter.draw_rectangle({cursor_point, {char_width, char_height}},
                           cursor_style.background);
}

bool TextEdit::on_key(const KeyEvent key) {
    if (key == KeyEvent::Left && cursor_pos_ > 0)
        cursor_pos_--;
    else if (key == KeyEvent::Right && cursor_pos_ < text_.length())
        cursor_pos_++;
    else if (key == KeyEvent::Select) {
        if (key_is_long_pressed(key)) {
            /* Long Select deletes everything left of the cursor. */
            text_ = text_.substr(cursor_pos_);
            set_cursor(0);
        } else {
            insert_mode_ = !insert_mode_;
        }
    } else {
        return false;
    }

    set_dirty();
    return true;
}

bool TextEdit::on_keyboard(const KeyboardEvent key) {
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

bool TextEdit::on_encoder(const EncoderEvent delta) {
    int32_t new_pos = static_cast<int32_t>(cursor_pos_) + delta;

    /* The encoder wraps around the ends of the text. */
    if (new_pos < 0)
        new_pos = static_cast<int32_t>(text_.length());
    else if (static_cast<size_t>(new_pos) > text_.length())
        new_pos = 0;

    set_cursor(static_cast<uint32_t>(new_pos));
    return true;
}

bool TextEdit::on_touch(const TouchEvent event) {
    if (event.type == TouchEvent::Type::Start) focus();

    set_dirty();
    return true;
}

/* The firmware enabled and disabled long-press reporting for the Select switch
 * around focus. The host reports long presses for every key all the time (see
 * input.hpp), so there is nothing to configure — the hooks stay for symmetry. */
void TextEdit::on_focus() {}
void TextEdit::on_blur() {}

/* --- TextField ------------------------------------------------------------- */

TextField::TextField(Rect parent_rect, std::string text)
    : Text(parent_rect, std::move(text)) {
    set_focusable(true);
}

const std::string& TextField::get_text() const { return text; }

void TextField::set_text(std::string_view value) {
    set(value);
    if (on_change) on_change(*this);
}

bool TextField::on_key(KeyEvent key) {
    if (key == KeyEvent::Select && on_select) {
        on_select(*this);
        return true;
    }
    return false;
}

bool TextField::on_encoder(EncoderEvent delta) {
    if (on_encoder_change) {
        on_encoder_change(*this, delta);
        return true;
    }
    return false;
}

bool TextField::on_touch(TouchEvent event) {
    if (event.type == TouchEvent::Type::Start) {
        focus();
        return true;
    }
    return false;
}

/* --- FloatField ------------------------------------------------------------ */

FloatField::FloatField(Point parent_pos,
                       int length,
                       range_t range,
                       float step,
                       char fill_char,
                       bool can_loop,
                       uint8_t precision)
    : Widget{{parent_pos, {length * 8, 16}}},
      range_{range},
      step_{step},
      length_{length},
      fill_char_{fill_char},
      can_loop_{can_loop},
      precision_{precision} {
    set_focusable(true);
}

float FloatField::value() const { return value_; }

void FloatField::set_value(float new_value, bool trigger_change) {
    const float lo = range_.first;
    const float hi = range_.second;

    if (can_loop_) {
        if (new_value > hi)
            new_value = lo;
        else if (new_value < lo)
            new_value = hi;
    }
    new_value = std::clamp(new_value, lo, hi);

    if (new_value != value_) {
        value_ = new_value;
        if (on_change && trigger_change) on_change(value_);
        set_dirty();
    }
}

void FloatField::set_range(const float min, const float max) {
    range_.first = min;
    range_.second = max;
    set_value(value_, false);
}

void FloatField::set_step(const float new_step) { step_ = new_step; }

void FloatField::set_precision(uint8_t precision) {
    precision_ = precision;
    set_dirty();
}

void FloatField::paint(Painter& painter) {
    const auto r = screen_rect();
    const Style s = has_focus() ? style().invert() : style();

    std::string text = to_string_decimal(value_, static_cast<int8_t>(precision_));

    const int cw = std::max(1, static_cast<int>(style().font.char_width()));
    const size_t max_chars = static_cast<size_t>(std::max(0, r.width() / cw));

    if (static_cast<size_t>(text.length()) > max_chars)
        text = text.substr(0, max_chars);

    if (fill_char_ && text.length() < max_chars)
        text = std::string(max_chars - text.length(), fill_char_) + text;

    painter.fill_rectangle(r, style().background);
    painter.draw_string(screen_pos(), s, text);
}

bool FloatField::on_key(const KeyEvent key) {
    if (key == KeyEvent::Select) {
        if (on_select) {
            on_select(*this);
            return true;
        }
        return on_encoder(1);
    }
    return false;
}

bool FloatField::on_encoder(const EncoderEvent delta) {
    const float old_value = value_;
    set_value(value_ + (static_cast<float>(delta) * step_));

    if (on_wrap) {
        if ((delta > 0) && (value_ < old_value))
            on_wrap(1);
        else if ((delta < 0) && (value_ > old_value))
            on_wrap(-1);
    }
    return true;
}

bool FloatField::on_keyboard(const KeyboardEvent key) {
    if (key == 10 && on_select) {
        on_select(*this);
        return true;
    }
    if (key == '+' || key == ' ') return on_encoder(1);
    if (key == '-' || key == 8) return on_encoder(-1);
    return false;
}

bool FloatField::on_touch(const TouchEvent event) {
    if (event.type == TouchEvent::Type::Start) focus();
    return true;
}

/* --- SymField -------------------------------------------------------------- */

SymField::SymField(Point parent_pos, size_t length, Type type, bool explicit_edits)
    : Widget{{parent_pos, {static_cast<int>(char_width * length), 16}}},
      type_{type},
      explicit_edits_{explicit_edits} {
    if (length == 0) length = 1;

    selected_ = length - 1;
    value_.resize(length);

    switch (type) {
        case Type::Oct:
            set_symbol_list("01234567");
            break;

        case Type::Dec:
            set_symbol_list("0123456789");
            break;

        case Type::Hex:
            set_symbol_list("0123456789ABCDEF");
            break;

        case Type::Alpha:
            set_symbol_list(" 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");
            break;

        default:
            set_symbol_list("01");
            break;
    }

    set_focusable(true);
}

SymField::SymField(Point parent_pos, size_t length, std::string symbol_list, bool explicit_edits)
    : SymField{parent_pos, length, Type::Custom, explicit_edits} {
    set_symbol_list(std::move(symbol_list));
}

char SymField::get_symbol(size_t index) const {
    if (index >= value_.length()) return 0;
    return value_[index];
}

void SymField::set_symbol(size_t index, char symbol) {
    if (index >= value_.length()) return;
    set_symbol_internal(index, ensure_valid(symbol));
}

size_t SymField::get_offset(size_t index) const {
    if (index >= value_.length()) return 0;

    /* Linear search: symbol lists are tiny. */
    return symbols_.find(value_[index]);
}

void SymField::set_offset(size_t index, size_t offset) {
    if (index >= value_.length() || offset >= symbols_.length()) return;
    set_symbol_internal(index, symbols_[offset]);
}

void SymField::set_symbol_list(std::string symbol_list) {
    if (symbol_list.length() == 0) return;

    symbols_ = std::move(symbol_list);
    ensure_all_symbols();
}

void SymField::set_value(uint64_t value) {
    const uint8_t radix = get_radix();

    /* Host guard: Custom and Alpha have no radix. On the LPC43xx the resulting
     * "% 0" quietly produced zero; on x86 it raises an FP exception. */
    if (radix == 0) return;

    uint64_t v = value;
    for (size_t i = value_.length(); i-- > 0;) {
        const uint8_t digit = static_cast<uint8_t>(v % radix);
        value_[i] = uint_to_char(digit, radix);
        v /= radix;
    }

    if (on_change) on_change(*this);
    set_dirty();
}

void SymField::set_value(std::string_view value) {
    /* Upstream refuses an over-long value rather than guessing which end to
     * truncate. */
    if (value.length() > value_.length()) return;

    /* Right-align in the field; the NUL padding is fixed up by
     * ensure_all_symbols(), which maps it to the first legal symbol. */
    const size_t left_padding = value_.length() - value.length();
    value_ = std::string(left_padding, '\0') + std::string{value};
    ensure_all_symbols();
}

uint64_t SymField::to_integer() const {
    const uint8_t radix = get_radix();
    if (radix == 0) return 0;

    uint64_t v = 0;
    uint64_t mul = 1;

    for (size_t i = value_.length(); i-- > 0;) {
        v += char_to_uint(value_[i], radix) * mul;
        mul *= radix;
    }

    return v;
}

const std::string& SymField::to_string() const { return value_; }

void SymField::paint(Painter& painter) {
    Point p = screen_pos();

    for (size_t n = 0; n < value_.length(); n++) {
        const char c = value_[n];
        MutableStyle paint_style{style()};

        /* Highlighting only means anything while focused. */
        if (has_focus()) {
            if (explicit_edits_) {
                /* Explicit edits invert the whole field rather than one slot. */
                paint_style.invert();
            } else if (n == selected_) {
                paint_style.invert();
            }

            if (editing_ && n == selected_) {
                paint_style.foreground = Theme::getInstance()->bg_darkest->foreground;
                paint_style.background = Theme::getInstance()->fg_blue->foreground;
            }
        }

        painter.draw_char(p, paint_style, c);
        p += {8, 0};
    }
}

bool SymField::on_key(KeyEvent key) {
    /* With explicit edits, everything but Select is declined until editing. */
    if (explicit_edits_ && !editing_) {
        switch (key) {
            case KeyEvent::Select:
                editing_ = true;
                set_dirty();
                return true;

            default:
                return false;
        }
    }

    switch (key) {
        case KeyEvent::Select:
            editing_ = !editing_;
            set_dirty();
            return true;

        case KeyEvent::Left:
            if (selected_ > 0) {
                selected_--;
                set_dirty();
                return true;
            }
            break;

        case KeyEvent::Right:
            if (selected_ < (value_.length() - 1)) {
                selected_++;
                set_dirty();
                return true;
            }
            break;

        case KeyEvent::Up:
            if (editing_) {
                on_encoder(1);
                return true;
            }
            break;

        case KeyEvent::Down:
            if (editing_) {
                on_encoder(-1);
                return true;
            }
            break;

        default:
            break;
    }

    return false;
}

bool SymField::on_encoder(EncoderEvent delta) {
    if (explicit_edits_ && !editing_) return false;
    if (symbols_.empty()) return false;

    int offset = static_cast<int>(get_offset(selected_)) + delta;
    offset = std::clamp(offset, 0, static_cast<int>(symbols_.length()) - 1);
    set_offset(selected_, static_cast<size_t>(offset));

    return true;
}

bool SymField::on_touch(TouchEvent event) {
    if (event.type == TouchEvent::Type::Start) focus();
    return true;
}

char SymField::ensure_valid(char symbol) const {
    if (symbols_.empty()) return symbol;
    const auto pos = symbols_.find(symbol);
    return pos != std::string::npos ? symbol : symbols_[0];
}

void SymField::ensure_all_symbols() {
    const auto before = value_;

    for (auto& c : value_) c = ensure_valid(c);

    if (before != value_) {
        if (on_change) on_change(*this);
        set_dirty();
    }
}

void SymField::set_symbol_internal(size_t index, char symbol) {
    if (value_[index] == symbol) return;

    value_[index] = symbol;
    if (on_change) on_change(*this);
    set_dirty();
}

uint8_t SymField::get_radix() const {
    switch (type_) {
        case Type::Oct:
            return 8;
        case Type::Dec:
            return 10;
        case Type::Hex:
            return 16;
        default:
            return 0;
    }
}

/* --- Waveform -------------------------------------------------------------- */

Waveform::Waveform(Rect parent_rect,
                   int16_t* data,
                   uint32_t length,
                   uint32_t offset,
                   bool digital,
                   Color color,
                   bool clickable)
    : Widget{parent_rect},
      data_{data},
      length_{length},
      offset_{offset},
      digital_{digital},
      color_{color},
      clickable_{clickable} {
    if (clickable) set_focusable(true);
}

void Waveform::set_cursor(const uint32_t i, const int16_t position) {
    if (i < 2) {
        if (position != cursors[i]) {
            cursors[i] = position;
            set_dirty();
        }
        show_cursors = true;
    }
}

void Waveform::set_offset(const uint32_t new_offset) {
    if (new_offset != offset_) {
        offset_ = new_offset;
        set_dirty();
    }
}

void Waveform::set_length(const uint32_t new_length) {
    if (new_length != length_) {
        length_ = new_length;
        set_dirty();
    }
}

bool Waveform::is_paused() const { return paused_; }

void Waveform::set_paused(bool paused) {
    paused_ = paused;
    if (!paused) if_ever_painted_pause = false;
    set_dirty();
}

bool Waveform::is_clickable() const { return clickable_; }

void Waveform::set_data(int16_t* new_data) {
    if (new_data != data_) {
        data_ = new_data;
        set_dirty();
    }
}

bool Waveform::on_key(const KeyEvent key) {
    if (!clickable_) return false;

    if (key == KeyEvent::Select) {
        set_paused(!paused_);
        if (on_select) on_select(*this);
        return true;
    }
    return false;
}

bool Waveform::on_keyboard(const KeyboardEvent key) {
    if (!clickable_) return false;

    if (key == 32 || key == 10) {
        set_paused(!paused_);
        if (on_select) on_select(*this);
        return true;
    }
    return false;
}

bool Waveform::on_touch(const TouchEvent event) {
    if (!clickable_) return false;

    switch (event.type) {
        case TouchEvent::Type::Start:
            focus();
            return true;

        case TouchEvent::Type::End:
            set_paused(!paused_);
            if (on_select) on_select(*this);
            return true;

        default:
            return false;
    }
}

void Waveform::paint(Painter& painter) {
    const auto r = screen_rect();

    if (paused_) {
        /* Paint the "hidden" notice once, then stop paying for the draw. */
        if (dirty() && !if_ever_painted_pause) {
            painter.fill_rectangle_unrolled8(r, Theme::getInstance()->bg_darkest->background);
            painter.draw_string({r.center().x() - 24, r.center().y() - 8}, style(), "WF HIDDEN");
            if_ever_painted_pause = true;
        }

        if (show_cursors) {
            for (uint32_t n = 0; n < 2; n++) {
                painter.draw_vline({std::min(r.width(), static_cast<int>(cursors[n])), r.top()},
                                   r.height(), cursor_colors[n]);
            }
        }
        return;
    }

    const Coord y_offset = static_cast<Coord>(r.top());
    Dim h = static_cast<Dim>(r.height());

    painter.fill_rectangle_unrolled8(r, Theme::getInstance()->bg_darkest->background);

    /* Host guard: upstream walks the buffer unconditionally. A view that has
     * not been handed a buffer yet (or that set length 0) must draw an empty
     * frame, not read through a null pointer. */
    if (!length_ || data_ == nullptr) return;

    const float x_inc = static_cast<float>(r.width()) / static_cast<float>(length_);
    const int16_t* data_start = data_ + offset_;

    if (digital_) {
        /* Digital: every sample is a horizontal run, with a riser between runs
         * at different levels. */
        float x = 0.0f;
        h = static_cast<Dim>(h - 1);
        Coord prev_y = 0;

        for (uint32_t n = 0; n < length_; n++) {
            const Coord y = *(data_start++) ? Coord{0} : static_cast<Coord>(h);

            if (n && y != prev_y)
                painter.draw_vline({static_cast<Coord>(x), y_offset}, h, color_);

            painter.draw_hline({static_cast<Coord>(x), static_cast<Coord>(y_offset + y)},
                               static_cast<int>(std::ceil(x_inc)), color_);

            prev_y = y;
            x += x_inc;
        }
    } else {
        /* Analog: every sample is a Y coordinate, joined by line segments. */
        const float y_scale = static_cast<float>(h - 1) / 65536.0f;
        h = static_cast<Dim>(h / 2);

        Coord prev_x = static_cast<Coord>(r.left());
        float x = static_cast<float>(prev_x) + x_inc;

        Coord prev_y = static_cast<Coord>(y_offset + h -
                                          static_cast<Coord>(*(data_start++) * y_scale));

        for (uint32_t n = 1; n < length_; n++) {
            const Coord y = static_cast<Coord>(y_offset + h -
                                               static_cast<Coord>(*(data_start++) * y_scale));
            painter.draw_line({prev_x, prev_y}, {static_cast<Coord>(x), y}, color_);

            prev_x = static_cast<Coord>(x);
            prev_y = y;
            x += x_inc;
        }
    }

    if (show_cursors) {
        for (uint32_t n = 0; n < 2; n++) {
            painter.draw_vline({std::min(r.width(), static_cast<int>(cursors[n])), y_offset},
                               r.height(), cursor_colors[n]);
        }
    }

    if (clickable_ && has_focus())
        painter.draw_rectangle(r, Theme::getInstance()->fg_light->foreground);
}

/* --- GraphEq --------------------------------------------------------------- */

GraphEq::GraphEq(Rect parent_rect, bool clickable)
    : Widget{parent_rect},
      clickable_{clickable},
      bar_heights(NUM_BARS, 0),
      prev_bar_heights(NUM_BARS, 0) {
    if (clickable) set_focusable(true);
}

void GraphEq::set_parent_rect(const Rect new_parent_rect) {
    Widget::set_parent_rect(new_parent_rect);
    calculate_params();
}

void GraphEq::calculate_params() {
    y_top = static_cast<Dim>(screen_rect().top());
    RENDER_HEIGHT = static_cast<Dim>(parent_rect().height());
    BAR_WIDTH = static_cast<Dim>((parent_rect().width() - (BAR_SPACING * (NUM_BARS - 1))) / NUM_BARS);
    HORIZONTAL_OFFSET = static_cast<Dim>(screen_rect().left());
}

bool GraphEq::is_paused() const { return paused_; }

void GraphEq::set_paused(bool paused) {
    paused_ = paused;
    needs_background_redraw = true;
    set_dirty();
}

bool GraphEq::is_clickable() const { return clickable_; }

Dim GraphEq::bar_height(size_t bar) const {
    if (bar >= bar_heights.size()) return 0;
    return bar_heights[bar];
}

bool GraphEq::on_key(const KeyEvent key) {
    if (!clickable_) return false;

    if (key == KeyEvent::Select) {
        set_paused(!paused_);
        if (on_select) on_select(*this);
        return true;
    }
    return false;
}

bool GraphEq::on_keyboard(const KeyboardEvent key) {
    if (!clickable_) return false;

    if (key == 32 || key == 10) {
        set_paused(!paused_);
        if (on_select) on_select(*this);
        return true;
    }
    return false;
}

bool GraphEq::on_touch(const TouchEvent event) {
    if (!clickable_) return false;

    switch (event.type) {
        case TouchEvent::Type::Start:
            focus();
            return true;

        case TouchEvent::Type::End:
            set_paused(!paused_);
            if (on_select) on_select(*this);
            return true;

        default:
            return false;
    }
}

void GraphEq::set_theme(Color base, Color peak) {
    base_color = base;
    peak_color = peak;
    set_dirty();
}

void GraphEq::update_audio_spectrum(const AudioSpectrum& spectrum) {
    /* 128 bins across the 48 kHz audio spectrum. */
    const float bin_frequency_size = 48000.0f / 128;

    if (RENDER_HEIGHT <= 0) calculate_params();

    for (int bar = 0; bar < NUM_BARS; bar++) {
        const float start_freq = static_cast<float>(FREQUENCY_BANDS[static_cast<size_t>(bar)]);
        const float end_freq = static_cast<float>(FREQUENCY_BANDS[static_cast<size_t>(bar) + 1]);

        const int start_bin = std::max(1, static_cast<int>(start_freq / bin_frequency_size));
        int end_bin = std::min(127, static_cast<int>(end_freq / bin_frequency_size));

        if (start_bin >= end_bin) end_bin = start_bin + 1;
        end_bin = std::min(127, end_bin);

        float total_energy = 0.0f;
        int bin_count = 0;

        for (int bin = start_bin; bin <= end_bin; bin++) {
            total_energy += static_cast<float>(spectrum.db[static_cast<size_t>(bin)]);
            bin_count++;
        }

        const float avg_db = bin_count > 0 ? (total_energy / static_cast<float>(bin_count)) : 0.0f;

        /* Upstream's hand-tuned weighting: lift the highs, then the mids, so
         * the display reads as a V rather than a ramp. */
        float treble_boost = 1.0f;
        if (bar == 10)
            treble_boost = 1.7f;
        else if (bar >= 7)
            treble_boost = 1.3f;

        float mid_boost = 1.0f;
        if (bar == 4 || bar == 5 || bar == 6) mid_boost = 1.2f;

        float amplified_db = avg_db * treble_boost * mid_boost;
        if (amplified_db > 255.0f) amplified_db = 255.0f;

        int target_height =
            static_cast<int>((amplified_db * static_cast<float>(RENDER_HEIGHT)) / 255.0f);
        if (target_height > RENDER_HEIGHT) target_height = RENDER_HEIGHT;

        /* Rise slower than it falls. */
        const float rise_speed = 0.8f;
        const float fall_speed = 1.0f;

        const float current = static_cast<float>(bar_heights[static_cast<size_t>(bar)]);
        const float speed = (target_height > bar_heights[static_cast<size_t>(bar)]) ? rise_speed
                                                                                    : fall_speed;

        bar_heights[static_cast<size_t>(bar)] = static_cast<Dim>(
            current * (1.0f - speed) + static_cast<float>(target_height) * speed);
    }

    set_dirty();
}

void GraphEq::paint(Painter& painter) {
    if (!drawn()) return;

    if (!is_calculated) {
        calculate_params();
        is_calculated = true;
    }

    if (needs_background_redraw) {
        painter.fill_rectangle(screen_rect(), Theme::getInstance()->bg_darkest->background);
        needs_background_redraw = false;
    }

    if (paused_) return;
    if (RENDER_HEIGHT <= 0) return;

    const int num_segments = RENDER_HEIGHT / SEGMENT_HEIGHT;
    const int bottom = screen_rect().bottom();

    for (int bar = 0; bar < NUM_BARS; bar++) {
        const size_t b = static_cast<size_t>(bar);
        const int x = HORIZONTAL_OFFSET + bar * (BAR_WIDTH + BAR_SPACING);
        const int active_segments = (bar_heights[b] * num_segments) / RENDER_HEIGHT;

        /* Erase only the segments that have gone away since the last frame. */
        if (prev_bar_heights[b] > active_segments) {
            const int clear_height = (prev_bar_heights[b] - active_segments) * SEGMENT_HEIGHT;
            const int clear_y = bottom - prev_bar_heights[b] * SEGMENT_HEIGHT;
            painter.fill_rectangle({x, clear_y, BAR_WIDTH, clear_height},
                                   Theme::getInstance()->bg_darkest->background);
        }

        for (int seg = 0; seg < active_segments; seg++) {
            const int y = bottom - (seg + 1) * SEGMENT_HEIGHT;
            if (y < y_top) break;

            const Color segment_color = (seg >= active_segments - 2) ? peak_color : base_color;
            painter.fill_rectangle({x, y, BAR_WIDTH, SEGMENT_HEIGHT - 1}, segment_color);
        }

        prev_bar_heights[b] = static_cast<Dim>(active_segments);
    }
}

/* --- OptionTabView --------------------------------------------------------- */

OptionTabView::OptionTabView(Rect parent_rect) {
    set_parent_rect(parent_rect);

    add_child(&check_enable);
    hidden(true);

    check_enable.on_select = [this](Checkbox&, bool value) { enabled = value; };
}

void OptionTabView::set_enabled(bool value) { check_enable.set_value(value); }

bool OptionTabView::is_enabled() { return check_enable.value(); }

void OptionTabView::set_type(std::string type) {
    check_enable.set_text("Transmit " + type);
}

void OptionTabView::focus() { check_enable.focus(); }

/* --- Tab / TabView --------------------------------------------------------- */

Tab::Tab() {
    set_focusable(true);
}

void Tab::set(uint32_t index, Dim width, std::string text, Color text_color) {
    set_parent_rect({static_cast<Coord>(index * static_cast<uint32_t>(width)), 0, width, 24});

    const int max_chars = std::max(0, (width - 8) / 8);
    text_ = text.substr(0, static_cast<size_t>(max_chars));
    text_color_ = text_color;

    index_ = index;
}

void Tab::paint(Painter& painter) {
    const auto rect = screen_rect();
    const Color color = highlighted() ? Theme::getInstance()->bg_darkest->background
                                      : Theme::getInstance()->bg_medium->background;

    painter.fill_rectangle({rect.left(), rect.top(), rect.width() - 8, rect.height()}, color);

    if (!highlighted())
        painter.draw_hline({rect.left(), rect.top()}, rect.width() - 9,
                           Theme::getInstance()->bg_light->background);

    painter.draw_bitmap({rect.right() - 8, rect.top()}, bitmap_tab_edge, color,
                        Theme::getInstance()->bg_dark->background);

    const auto text_point =
        rect.center() - Point(4, 0) - Point(static_cast<int>(text_.size()) * 8 / 2, 16 / 2);

    painter.draw_string(text_point, {font::fixed_8x16, color, text_color_}, text_);

    if (has_focus())
        painter.draw_hline(text_point + Point(0, 16), static_cast<int>(text_.size()) * 8,
                           Theme::getInstance()->bg_darkest->foreground);
}

bool Tab::on_key(const KeyEvent key) {
    if (key == KeyEvent::Select) {
        auto* view = static_cast<TabView*>(parent());
        if (view) view->set_selected(index_);
        return true;
    }

    return false;
}

bool Tab::on_touch(const TouchEvent event) {
    switch (event.type) {
        case TouchEvent::Type::Start:
            focus();
            set_dirty();
            return true;

        case TouchEvent::Type::End: {
            auto* view = static_cast<TabView*>(parent());
            if (view) view->set_selected(index_);
            return true;
        }

        default:
            return false;
    }
}

TabView::TabView(std::initializer_list<TabDef> tab_definitions) {
    n_tabs = tab_definitions.size();
    if (n_tabs > MAX_TABS) n_tabs = MAX_TABS;

    views.fill(nullptr);

    set_parent_rect({0, 0, screen_width, 3 * 8});

    if (n_tabs == 0) return;

    const Dim tab_width = static_cast<Dim>(screen_width / n_tabs);

    size_t i = 0;
    for (const auto& tab_definition : tab_definitions) {
        tabs[i].set(static_cast<uint32_t>(i), tab_width, tab_definition.text,
                    tab_definition.color);
        views[i] = tab_definition.view;
        add_child(&tabs[i]);
        i++;
        if (i == MAX_TABS) break;
    }
}

void TabView::set_selected(uint32_t index) {
    if (index >= n_tabs) return;

    /* Hide the outgoing view and give its tab back its focusability. */
    if (views[current_tab]) views[current_tab]->hidden(true);

    Tab* tab = &tabs[current_tab];
    tab->set_highlighted(false);
    tab->set_focusable(true);
    tab->set_dirty();

    if (views[index]) views[index]->hidden(false);

    /* The selected tab is not focusable — focus belongs to its content. */
    tab = &tabs[index];
    current_tab = index;
    tab->set_highlighted(true);
    tab->set_focusable(false);
    tab->set_dirty();
}

void TabView::on_show() { set_selected(current_tab); }

/* Focus belongs to the selected tab's content, never to the tab strip. With no
 * tabs, or a tab that has no content view, there is nowhere for it to go — the
 * selected tab itself is deliberately not focusable. */
void TabView::focus() {
    if (n_tabs == 0) return;
    if (views[current_tab]) views[current_tab]->focus();
}

}  // namespace ui
