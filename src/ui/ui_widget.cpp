/*
 * mayhem-b200 — widget toolkit implementation.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version. See LICENSE.GPL-2.0-or-later.
 */

#include "ui_widget.hpp"

#include "../core/string_format.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace ui {

namespace {

bool dirty_flag = false;

/* The root context. The firmware has one per core; we have one, full stop. */
Context g_context;

}  // namespace

void dirty_set() { dirty_flag = true; }
void dirty_clear() { dirty_flag = false; }
bool is_dirty() { return dirty_flag; }

const std::vector<Widget*> Widget::no_children{};

/* --- Widget ---------------------------------------------------------------- */

Point Widget::screen_pos() const {
    return parent() ? (parent()->screen_pos() + parent_rect().location())
                    : parent_rect().location();
}

Size Widget::size() const { return _parent_rect.size(); }

Rect Widget::screen_rect() const { return {screen_pos(), size()}; }

Rect Widget::parent_rect() const { return _parent_rect; }

void Widget::set_parent_rect(const Rect new_parent_rect) {
    _parent_rect = new_parent_rect;
    set_dirty();
}

Widget* Widget::parent() const { return parent_; }

void Widget::set_parent(Widget* const widget) {
    if (widget == parent_) return;

    if (parent_ != nullptr) {
        /* Leaving a parent means the area we occupied has to be repainted by
         * whatever is underneath. */
        parent_->set_dirty();
    }

    parent_ = widget;
    set_dirty();
}

void Widget::hidden(bool hide) {
    if (hide == flags.hidden) return;

    flags.hidden = hide;

    /* Hiding does not repaint us — it repaints the parent, which owns the
     * background we were covering. */
    if (hide) {
        if (parent()) parent()->set_dirty();
        on_hide();
    } else {
        set_dirty();
        on_show();
    }
}

void Widget::focus() {
    context().focus_manager().set_focus_widget(this);
}

void Widget::on_focus() {}

void Widget::blur() {
    context().focus_manager().set_focus_widget(nullptr);
}

void Widget::on_blur() {}

bool Widget::focusable() const { return flags.focusable; }

void Widget::set_focusable(const bool value) { flags.focusable = value; }

bool Widget::has_focus() const {
    return context().focus_manager().focus_widget() == this;
}

bool Widget::on_key(const KeyEvent) { return false; }
bool Widget::on_encoder(const EncoderEvent) { return false; }
bool Widget::on_touch(const TouchEvent) { return false; }
bool Widget::on_keyboard(const KeyboardEvent) { return false; }

const std::vector<Widget*>& Widget::children() const { return no_children; }

Context& Widget::context() const {
    /* Every widget shares the one root context; the firmware walks to the root
     * widget for this, which amounts to the same thing here. */
    return g_context;
}

void Widget::set_style(const Style* new_style) {
    if (new_style == style_) return;
    style_ = new_style;
    set_dirty();
}

const Style& Widget::style() const {
    if (style_) return *style_;
    if (parent_) return parent_->style();
    return *Theme::getInstance()->bg_darkest;
}

void Widget::set_dirty() {
    flags.dirty = true;
    dirty_set();
}

bool Widget::dirty() const { return flags.dirty; }

void Widget::set_clean() { flags.dirty = false; }

void Widget::drawn(bool v) {
    flags.drawn = v;
    for (auto child : children()) child->drawn(v);
}

bool Widget::highlighted() const { return flags.highlighted; }

void Widget::set_highlighted(const bool value) {
    if (value == flags.highlighted) return;
    flags.highlighted = value;
    set_dirty();
}

void Widget::dirty_overlapping_children_in_rect(const Rect& child_rect) {
    for (auto child : children()) {
        if (!child->hidden() && !child->parent_rect().intersect(child_rect).is_empty())
            child->set_dirty();
    }
}

/* --- View ------------------------------------------------------------------ */

void View::paint(Painter& painter) {
    painter.fill_rectangle(screen_rect(), style().background);
}

void View::add_child(Widget* const widget) {
    if (widget == nullptr) return;
    if (widget->parent() != nullptr) return;  /* already owned */

    widget->set_parent(this);
    children_.push_back(widget);
    widget->on_after_attach();
    set_dirty();
}

void View::add_children(const std::initializer_list<Widget*> children) {
    for (auto child : children) add_child(child);
}

void View::remove_child(Widget* const widget) {
    if (widget == nullptr) return;

    const auto it = std::find(children_.begin(), children_.end(), widget);
    if (it == children_.end()) return;

    widget->on_before_detach();
    invalidate_child(widget);
    children_.erase(it);
    widget->set_parent(nullptr);
}

void View::remove_children(const std::vector<Widget*>& children) {
    for (auto child : children) remove_child(child);
}

const std::vector<Widget*>& View::children() const { return children_; }

std::string View::title() const { return ""; }

void View::on_frame_sync() {
    for (auto child : children_) {
        if (!child->hidden()) child->on_frame_sync();
    }
}

void View::invalidate_child(Widget* const widget) {
    if (widget == nullptr) return;
    /* The child's area has to be repainted by us and by anything overlapping. */
    set_dirty();
    dirty_overlapping_children_in_rect(widget->parent_rect());
}

/* --- Rectangle ------------------------------------------------------------- */

Rectangle::Rectangle(Color c)
    : Rectangle{{}, c} {}

Rectangle::Rectangle(Rect parent_rect, Color c)
    : Widget{parent_rect},
      color{c} {}

void Rectangle::paint(Painter& painter) {
    if (outline_)
        painter.draw_rectangle(screen_rect(), color);
    else
        painter.fill_rectangle(screen_rect(), color);
}

void Rectangle::set_color(const Color c) {
    color = c;
    set_dirty();
}

void Rectangle::set_outline(const bool outline) {
    outline_ = outline;
    set_dirty();
}

/* --- Text ------------------------------------------------------------------ */

Text::Text(Rect parent_rect, std::string text)
    : Widget{parent_rect},
      text{std::move(text)} {}

Text::Text(Rect parent_rect)
    : Text{parent_rect, {}} {}

void Text::set(std::string_view value) {
    if (text == value) return;
    text = std::string{value};
    set_dirty();
}

std::string Text::get() const { return text; }

void Text::paint(Painter& painter) {
    const auto rect = screen_rect();
    const auto s = style();

    painter.fill_rectangle(rect, s.background);

    if (text.empty()) return;

    /* Render only as many characters as fit, as the firmware does — apps rely
     * on being able to stash a longer string here. */
    const int max_chars = rect.width() / s.font.char_width();
    painter.draw_string(rect.location(), s,
                        std::string_view{text}.substr(0, static_cast<size_t>(std::max(0, max_chars))));
}

bool Text::on_touch(const TouchEvent event) {
    if (event.type == TouchEvent::Type::Start && on_select) {
        on_select(*this);
        return true;
    }
    return false;
}

/* --- Labels ---------------------------------------------------------------- */

Labels::Labels(std::initializer_list<Label> labels)
    : Widget{{0, 0, screen_width, screen_height}},
      labels_{labels} {}

Labels::Labels(Rect parent_rect, std::initializer_list<Label> labels)
    : Widget{parent_rect},
      labels_{labels} {}

void Labels::set_labels(std::initializer_list<Label> labels) {
    labels_ = labels;
    set_dirty();
}

void Labels::paint(Painter& painter) {
    const auto pos = screen_pos();
    for (const auto& label : labels_) {
        painter.draw_string(pos + label.pos,
                            style().font,
                            label.color,
                            style().background,
                            label.text);
    }
}

/* --- Button ---------------------------------------------------------------- */

Button::Button(Rect parent_rect, std::string text, bool instant_exec)
    : Widget{parent_rect},
      text_{std::move(text)},
      instant_exec_{instant_exec} {
    set_focusable(true);
}

Button::Button(Rect parent_rect, std::string text, Color bg, Color fg, bool instant_exec)
    : Widget{parent_rect},
      text_{std::move(text)},
      instant_exec_{instant_exec},
      has_custom_colors_{true},
      bg_{bg},
      fg_{fg} {
    set_focusable(true);
}

void Button::set_text(std::string_view value) {
    if (text_ == value) return;
    text_ = std::string{value};
    set_dirty();
}

std::string Button::text() const { return text_; }

void Button::set_bg_color(Color c) {
    bg_ = c;
    has_custom_colors_ = true;
    set_dirty();
}

void Button::set_fg_color(Color c) {
    fg_ = c;
    has_custom_colors_ = true;
    set_dirty();
}

void Button::paint(Painter& painter) {
    const auto r = screen_rect();
    const auto s = style();
    const bool focused = has_focus() || highlighted();

    Color bg = has_custom_colors_ ? bg_ : s.background;
    Color fg = has_custom_colors_ ? fg_ : s.foreground;
    if (focused) std::swap(bg, fg);

    painter.fill_rectangle(r, bg);
    painter.draw_rectangle(r, fg);

    /* Centre the caption, clipping to the button width. */
    const int cw = s.font.char_width();
    const int max_chars = std::max(0, (r.width() - 2) / cw);
    const std::string_view caption{text_};
    const auto shown = caption.substr(0, static_cast<size_t>(max_chars));

    const int text_w = static_cast<int>(shown.size()) * cw;
    const Point p{r.left() + (r.width() - text_w) / 2,
                  r.top() + (r.height() - s.font.line_height()) / 2};

    painter.draw_string(p, s.font, fg, bg, shown);
}

bool Button::on_key(const KeyEvent key) {
    if (key == KeyEvent::Select) {
        if (on_select) {
            on_select(*this);
            return true;
        }
        return false;
    }

    if (on_dir) return on_dir(*this, key);
    return false;
}

bool Button::on_touch(const TouchEvent event) {
    switch (event.type) {
        case TouchEvent::Type::Start:
            set_highlighted(true);
            focus();
            if (instant_exec_ && on_select) on_select(*this);
            return true;

        case TouchEvent::Type::End:
            set_highlighted(false);
            if (!instant_exec_ && on_select) on_select(*this);
            if (on_touch_release) on_touch_release(*this);
            return true;

        default:
            return false;
    }
}

/* --- Checkbox -------------------------------------------------------------- */

Checkbox::Checkbox(Point parent_pos, size_t length, std::string text, bool small_text)
    : Widget{{parent_pos, {static_cast<int>((8 * length) + 24),
                           small_text ? 24 : 28}}},
      text_{std::move(text)},
      small_{small_text} {
    set_focusable(true);
}

void Checkbox::set_text(std::string_view value) {
    if (text_ == value) return;
    text_ = std::string{value};
    set_dirty();
}

bool Checkbox::value() const { return value_; }

void Checkbox::set_value(const bool value) {
    if (value == value_) return;
    value_ = value;
    set_dirty();
    if (on_select) on_select(*this, value_);
}

void Checkbox::paint(Painter& painter) {
    const auto r = screen_rect();
    const auto s = style();
    const auto& font = small_ ? font::fixed_5x8 : font::fixed_8x16;

    const bool focused = has_focus() || highlighted();
    const Color box_bg = focused ? s.foreground : s.background;
    const Color box_fg = focused ? s.background : s.foreground;

    painter.fill_rectangle(r, s.background);

    /* Box on the left, caption to its right. */
    const Rect box{r.left(), r.top(), 20, 20};
    painter.fill_rectangle(box, box_bg);
    painter.draw_rectangle(box, box_fg);

    if (value_) {
        /* Simple tick: two strokes inside the box. */
        painter.draw_line({box.left() + 4, box.top() + 10},
                          {box.left() + 8, box.top() + 15},
                          Theme::getInstance()->fg_green->foreground);
        painter.draw_line({box.left() + 8, box.top() + 15},
                          {box.left() + 16, box.top() + 4},
                          Theme::getInstance()->fg_green->foreground);
    }

    painter.draw_string({r.left() + 24, r.top() + (20 - font.line_height()) / 2},
                        font, s.foreground, s.background, text_);
}

bool Checkbox::on_key(const KeyEvent key) {
    if (key == KeyEvent::Select) {
        set_value(!value_);
        return true;
    }
    return false;
}

bool Checkbox::on_touch(const TouchEvent event) {
    switch (event.type) {
        case TouchEvent::Type::Start:
            set_highlighted(true);
            focus();
            set_dirty();
            return true;

        case TouchEvent::Type::End:
            set_highlighted(false);
            set_value(!value_);
            return true;

        default:
            return false;
    }
}

/* --- OptionsField ---------------------------------------------------------- */

OptionsField::OptionsField(Point parent_pos, int length, options_t options, bool centered)
    : Widget{{parent_pos, {length * 8, 16}}},
      options_{std::move(options)},
      length_{length},
      centered_{centered} {
    set_focusable(true);
}

void OptionsField::set_options(options_t new_options) {
    options_ = std::move(new_options);
    if (selected_index_ >= options_.size()) selected_index_ = 0;
    set_dirty();
}

size_t OptionsField::selected_index() const { return selected_index_; }

const OptionsField::name_t& OptionsField::selected_index_name() const {
    static const name_t empty{};
    if (selected_index_ >= options_.size()) return empty;
    return options_[selected_index_].first;
}

OptionsField::value_t OptionsField::selected_index_value() const {
    if (selected_index_ >= options_.size()) return 0;
    return options_[selected_index_].second;
}

void OptionsField::set_selected_index(const size_t new_index, bool trigger_change) {
    if (options_.empty()) return;
    const size_t clamped = std::min(new_index, options_.size() - 1);
    if (clamped == selected_index_) return;

    selected_index_ = clamped;
    set_dirty();
    if (trigger_change && on_change) on_change(selected_index_, selected_index_value());
}

bool OptionsField::set_by_value(value_t v, bool trigger_change) {
    for (size_t i = 0; i < options_.size(); i++) {
        if (options_[i].second == v) {
            set_selected_index(i, trigger_change);
            return true;
        }
    }
    return false;
}

void OptionsField::set_by_nearest_value(value_t v, bool trigger_change) {
    if (options_.empty()) return;

    size_t best = 0;
    int64_t best_delta = INT64_MAX;
    for (size_t i = 0; i < options_.size(); i++) {
        const int64_t delta = std::abs(static_cast<int64_t>(options_[i].second) -
                                       static_cast<int64_t>(v));
        if (delta < best_delta) {
            best_delta = delta;
            best = i;
        }
    }
    set_selected_index(best, trigger_change);
}

void OptionsField::set_length(int len) {
    length_ = len;
    set_parent_rect({parent_rect().location(), {len * 8, 16}});
}

void OptionsField::paint(Painter& painter) {
    const auto r = screen_rect();
    /* Style has const members, so it is built once rather than reassigned. */
    const Style s = (has_focus() || highlighted()) ? style().invert() : style();

    painter.fill_rectangle(r, s.background);

    if (options_.empty()) return;

    std::string_view name{selected_index_name()};
    if (static_cast<int>(name.size()) > length_)
        name = name.substr(0, static_cast<size_t>(length_));

    int x = r.left();
    if (centered_)
        x += (r.width() - static_cast<int>(name.size()) * s.font.char_width()) / 2;

    painter.draw_string({x, r.top()}, s, name);
}

bool OptionsField::on_encoder(const EncoderEvent delta) {
    if (options_.empty()) return false;

    const int64_t n = static_cast<int64_t>(selected_index_) + delta;
    const int64_t last = static_cast<int64_t>(options_.size()) - 1;
    set_selected_index(static_cast<size_t>(std::clamp<int64_t>(n, 0, last)));
    return true;
}

bool OptionsField::on_key(const KeyEvent key) {
    if (key == KeyEvent::Select) {
        if (on_show_options) {
            on_show_options();
            return true;
        }
        /* Without a chooser attached, Select advances the option and wraps —
         * this is what makes the field usable from the keyboard alone. */
        if (!options_.empty())
            set_selected_index((selected_index_ + 1) % options_.size());
        return true;
    }
    return false;
}

bool OptionsField::on_touch(const TouchEvent event) {
    if (event.type == TouchEvent::Type::Start) {
        focus();
        return true;
    }
    return false;
}

/* --- NumberField ----------------------------------------------------------- */

NumberField::NumberField(Point parent_pos,
                         int length,
                         range_t range,
                         int32_t step,
                         char fill_char,
                         bool can_loop)
    : Widget{{parent_pos, {length * 8, 16}}},
      range_{range},
      step_{step},
      length_{length},
      fill_char_{fill_char},
      can_loop_{can_loop} {
    set_focusable(true);
    value_ = clip_value(0);
}

int32_t NumberField::value() const { return value_; }

void NumberField::set_value(int32_t new_value, bool trigger_change) {
    new_value = clip_value(new_value);
    if (new_value == value_) return;

    value_ = new_value;
    set_dirty();
    if (trigger_change && on_change) on_change(value_);
}

void NumberField::set_range(const int32_t min, const int32_t max) {
    range_ = {min, max};
    set_value(value_);
    set_dirty();
}

int32_t NumberField::clip_value(int32_t value) const {
    return std::clamp(value, range_.first, range_.second);
}

void NumberField::paint(Painter& painter) {
    const auto r = screen_rect();
    const Style s = (has_focus() || highlighted()) ? style().invert() : style();

    painter.fill_rectangle(r, s.background);
    painter.draw_string(r.location(), s, to_string_dec_int(value_, length_, fill_char_));
}

bool NumberField::on_key(const KeyEvent key) {
    if (key == KeyEvent::Select && on_select) {
        on_select(*this);
        return true;
    }
    return false;
}

bool NumberField::on_encoder(const EncoderEvent delta) {
    int64_t n = static_cast<int64_t>(value_) + static_cast<int64_t>(delta) * step_;

    if (can_loop_) {
        const int64_t span = static_cast<int64_t>(range_.second) - range_.first + 1;
        if (span > 0) {
            /* Positive modulo so downward wrap lands on the top of the range. */
            n = range_.first + ((n - range_.first) % span + span) % span;
        }
    }

    set_value(static_cast<int32_t>(std::clamp<int64_t>(n, range_.first, range_.second)));
    return true;
}

bool NumberField::on_touch(const TouchEvent event) {
    if (event.type == TouchEvent::Type::Start) {
        focus();
        return true;
    }
    return false;
}

/* --- BigFrequency ---------------------------------------------------------- */

namespace {

/* Seven-segment bit patterns and segment rectangles, lifted from the firmware
 * so the readout is pixel-identical. Bit order is A,B,C,D,E,F,G. */
constexpr uint8_t big_segment_font[11] = {
    0b00111111,  // 0
    0b00000110,  // 1
    0b01011011,  // 2
    0b01001111,  // 3
    0b01100110,  // 4
    0b01101101,  // 5
    0b01111101,  // 6
    0b00000111,  // 7
    0b01111111,  // 8
    0b01101111,  // 9
    0b01000000,  // -
};

constexpr Rect big_segments[7] = {
    {{4, 0}, {20, 4}},
    {{24, 4}, {4, 20}},
    {{24, 28}, {4, 20}},
    {{4, 48}, {20, 4}},
    {{0, 28}, {4, 20}},
    {{0, 4}, {4, 20}},
    {{4, 24}, {20, 4}},
};

constexpr Dim big_digit_width = 32;

}  // namespace

BigFrequency::BigFrequency(Rect parent_rect, uint64_t frequency)
    : Widget{parent_rect},
      frequency_{frequency} {}

void BigFrequency::set(const uint64_t frequency) {
    if (frequency == frequency_) return;
    frequency_ = frequency;
    set_dirty();
}

void BigFrequency::draw_digit(Painter& painter, Point p, uint8_t digit, Color color) {
    if (digit >= 16) return;  /* "don't draw" code, used for leading zeros */
    uint32_t def = big_segment_font[digit];
    for (size_t s = 0; s < 7; s++) {
        if (def & 1)
            painter.fill_rectangle({p + big_segments[s].location(), big_segments[s].size()}, color);
        def >>= 1;
    }
}

void BigFrequency::paint(Painter& painter) {
    const auto rect = screen_rect();
    const Color color = style().foreground;

    painter.fill_rectangle({{0, rect.location().y()}, {screen_width, 52}},
                           Theme::getInstance()->bg_darkest->background);

    std::array<uint8_t, 7> digits{};
    Point digit_pos;
    size_t leading = 0;

    if (frequency_ == 0) {
        digits.fill(10);  /* ----.--- */
        digit_pos = {(screen_width - ((7 * big_digit_width) + 8)) / 2, rect.location().y()};
    } else {
        uint64_t f = frequency_ / 1000;  /* GMMM.KKK */
        for (size_t i = 0; i < 7; i++) {
            digits[6 - i] = static_cast<uint8_t>(f % 10);
            f /= 10;
        }

        for (leading = 0; leading < 3; leading++) {
            if (digits[leading] == 0)
                digits[leading] = 16;  /* don't draw */
            else
                break;
        }

        digit_pos = {static_cast<Coord>(screen_width - ((7 * big_digit_width) + 8) -
                                        (leading * big_digit_width)) / 2,
                     rect.location().y()};
    }

    for (size_t i = 0; i < 7; i++) {
        draw_digit(painter, digit_pos, digits[i], color);

        if (i == 3) {
            painter.fill_rectangle({digit_pos + Point(34, 48), {4, 4}}, color);
            digit_pos += {big_digit_width + 8, 0};
        } else {
            digit_pos += {big_digit_width, 0};
        }
    }
}

/* --- ProgressBar ----------------------------------------------------------- */

ProgressBar::ProgressBar(Rect parent_rect)
    : Widget{parent_rect} {}

void ProgressBar::set_max(const uint32_t max) {
    max_ = (max == 0) ? 1 : max;
    set_dirty();
}

void ProgressBar::set_value(const uint32_t value) {
    const uint32_t clamped = std::min(value, max_);
    if (clamped == value_) return;
    value_ = clamped;
    set_dirty();
}

void ProgressBar::paint(Painter& painter) {
    const auto r = screen_rect();
    const auto s = style();

    const int filled = static_cast<int>(
        (static_cast<uint64_t>(r.width() - 2) * value_) / max_);

    painter.fill_rectangle({r.left() + 1, r.top() + 1, filled, r.height() - 2},
                           s.foreground);
    painter.fill_rectangle({r.left() + 1 + filled, r.top() + 1,
                            r.width() - 2 - filled, r.height() - 2},
                           s.background);
    painter.draw_rectangle(r, s.foreground);
}

/* --- Console --------------------------------------------------------------- */

Console::Console(Rect parent_rect)
    : Widget{parent_rect} {}

int Console::visible_lines() const {
    return std::max(1, size().height() / font::fixed_8x16.line_height());
}

int Console::columns() const {
    return std::max(1, size().width() / font::fixed_8x16.char_width());
}

void Console::clear(bool clear_screen) {
    lines_.clear();
    pending_.clear();
    if (clear_screen) set_dirty();
}

void Console::write(std::string_view message) {
    for (char c : message) {
        if (c == '\n') {
            lines_.push_back(pending_);
            pending_.clear();
        } else if (c != '\r') {
            pending_.push_back(c);
            /* Hard-wrap at the console width so a long line cannot be lost. */
            if (static_cast<int>(pending_.size()) >= columns()) {
                lines_.push_back(pending_);
                pending_.clear();
            }
        }
    }

    while (lines_.size() > max_lines_) lines_.pop_front();
    set_dirty();
}

void Console::writeln(std::string_view message) {
    write(message);
    write("\n");
}

void Console::enable_scrolling(bool enable) { scrolling_ = enable; }

void Console::on_show() { set_dirty(); }

void Console::paint(Painter& painter) {
    const auto r = screen_rect();
    const auto s = style();

    painter.fill_rectangle(r, s.background);

    const int rows = visible_lines();
    const int line_h = s.font.line_height();

    /* Build the visible window: the tail of the buffer, plus the partial line
     * currently being accumulated. */
    std::vector<const std::string*> shown;
    shown.reserve(static_cast<size_t>(rows));

    const bool has_pending = !pending_.empty();
    const int total = static_cast<int>(lines_.size()) + (has_pending ? 1 : 0);
    int start = scrolling_ ? std::max(0, total - rows) : 0;

    for (int i = start; i < total && static_cast<int>(shown.size()) < rows; i++) {
        if (i < static_cast<int>(lines_.size()))
            shown.push_back(&lines_[static_cast<size_t>(i)]);
        else
            shown.push_back(&pending_);
    }

    int y = r.top();
    for (const auto* line : shown) {
        painter.draw_string({r.left(), y}, s, truncate(*line, static_cast<size_t>(columns())));
        y += line_h;
    }
}

/* --- ActivityDot ----------------------------------------------------------- */

ActivityDot::ActivityDot(Rect parent_rect, Color color)
    : Widget{parent_rect},
      color_{color} {}

void ActivityDot::toggle() {
    on_ = !on_;
    set_dirty();
}

void ActivityDot::reset() {
    if (!on_) return;
    on_ = false;
    set_dirty();
}

void ActivityDot::paint(Painter& painter) {
    painter.fill_rectangle(screen_rect(), on_ ? color_ : style().background);
}

/* --- VuMeter --------------------------------------------------------------- */

VuMeter::VuMeter(Rect parent_rect, uint32_t LEDs, bool show_max)
    : Widget{parent_rect},
      leds_{LEDs == 0 ? 1 : LEDs},
      show_max_{show_max} {
    split_ = static_cast<uint8_t>((leds_ * 2) / 3);
}

void VuMeter::set_value(const uint8_t new_value) {
    if (new_value == value_) return;
    value_ = new_value;
    if (new_value > max_) max_ = new_value;
    set_dirty();
}

void VuMeter::set_mark(const uint8_t new_mark) {
    if (new_mark == mark_) return;
    mark_ = new_mark;
    set_dirty();
}

void VuMeter::paint(Painter& painter) {
    const auto r = screen_rect();
    const auto s = style();

    painter.fill_rectangle(r, s.background);

    const int bar_h = std::max(1, r.height() / static_cast<int>(leds_));
    const uint32_t lit = (static_cast<uint32_t>(value_) * leds_) / 256;
    const uint32_t max_led = (static_cast<uint32_t>(max_) * leds_) / 256;
    const uint32_t mark_led = (static_cast<uint32_t>(mark_) * leds_) / 256;

    /* Bottom-up bargraph: green below the split, yellow above, red at the top. */
    for (uint32_t i = 0; i < leds_; i++) {
        const int y = r.bottom() - static_cast<int>(i + 1) * bar_h;
        Color c = s.background;

        if (i < lit) {
            if (i >= leds_ - 1)
                c = Theme::getInstance()->fg_red->foreground;
            else if (i >= split_)
                c = Theme::getInstance()->fg_yellow->foreground;
            else
                c = Theme::getInstance()->fg_green->foreground;
        } else if (show_max_ && i == max_led && max_led > 0) {
            c = Theme::getInstance()->fg_light->foreground;
        } else if (mark_ && i == mark_led) {
            c = Theme::getInstance()->fg_darkcyan->foreground;
        }

        painter.fill_rectangle({r.left(), y, r.width(), bar_h - 1}, c);
    }
}

}  // namespace ui
