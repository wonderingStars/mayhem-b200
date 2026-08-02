/*
 * mayhem-b200 — scrolling menu list.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_menu.hpp"

#include "theme.hpp"

#include <algorithm>

namespace ui {

MenuView::MenuView(Rect parent_rect, bool keep_highlight)
    : Widget{parent_rect},
      keep_highlight_{keep_highlight} {
    set_focusable(true);
}

void MenuView::add_item(MenuItem item) {
    items_.push_back(std::move(item));
    set_dirty();
}

void MenuView::add_items(std::vector<MenuItem> items) {
    for (auto& item : items) items_.push_back(std::move(item));
    set_dirty();
}

void MenuView::clear() {
    items_.clear();
    index_ = 0;
    offset_ = 0;
    set_dirty();
}

size_t MenuView::visible_items() const {
    return static_cast<size_t>(std::max(1, size().height() / item_height));
}

void MenuView::scroll_to_cursor() {
    const size_t rows = visible_items();
    if (index_ < offset_)
        offset_ = index_;
    else if (index_ >= offset_ + rows)
        offset_ = index_ - rows + 1;
}

bool MenuView::set_highlighted(size_t new_index) {
    if (items_.empty()) return false;
    if (new_index >= items_.size()) return false;
    if (new_index == index_) return true;

    index_ = new_index;
    scroll_to_cursor();
    set_dirty();
    if (on_highlight) on_highlight(index_);
    return true;
}

void MenuView::select_highlighted() {
    if (index_ >= items_.size()) return;
    /* Copy the callback before invoking: activating an item usually pushes a
     * new view, which can destroy this menu while the handler is on the stack. */
    auto handler = items_[index_].on_select;
    if (handler) handler();
}

void MenuView::paint(Painter& painter) {
    const auto r = screen_rect();
    const auto& s = style();
    const auto& font = font::fixed_8x16;

    painter.fill_rectangle(r, s.background);

    const size_t rows = visible_items();
    const bool show_cursor = keep_highlight_ || has_focus();

    for (size_t row = 0; row < rows; row++) {
        const size_t i = offset_ + row;
        if (i >= items_.size()) break;

        const auto& item = items_[i];
        const bool selected = (i == index_) && show_cursor;

        const Rect line{r.left(), r.top() + static_cast<int>(row) * item_height,
                        r.width(), item_height};

        const Color bg = selected ? item.color : s.background;
        const Color fg = selected ? s.background : item.color;

        painter.fill_rectangle(line, bg);
        painter.draw_string({line.left() + 4, line.top() + (item_height - font.line_height()) / 2},
                            font, fg, bg, item.text);
    }
}

bool MenuView::on_key(const KeyEvent key) {
    switch (key) {
        case KeyEvent::Up:
            if (index_ == 0) return false;  /* let focus leave the menu upwards */
            return set_highlighted(index_ - 1);

        case KeyEvent::Down:
            if (index_ + 1 >= items_.size()) return false;
            return set_highlighted(index_ + 1);

        case KeyEvent::Select:
        case KeyEvent::Right:
            select_highlighted();
            return true;

        default:
            return false;
    }
}

bool MenuView::on_encoder(const EncoderEvent delta) {
    if (items_.empty()) return false;

    const int64_t last = static_cast<int64_t>(items_.size()) - 1;
    const int64_t n = std::clamp<int64_t>(static_cast<int64_t>(index_) + delta, 0, last);
    return set_highlighted(static_cast<size_t>(n));
}

bool MenuView::on_touch(const TouchEvent event) {
    if (event.type != TouchEvent::Type::End) {
        if (event.type == TouchEvent::Type::Start) focus();
        return true;
    }

    const auto r = screen_rect();
    const int row = (event.point.y() - r.top()) / item_height;
    if (row < 0) return true;

    const size_t i = offset_ + static_cast<size_t>(row);
    if (i >= items_.size()) return true;

    if (i == index_) {
        select_highlighted();
    } else {
        set_highlighted(i);
    }
    return true;
}

}  // namespace ui
