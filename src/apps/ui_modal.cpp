/*
 * mayhem-b200 — modal message and confirmation dialog.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_modal.hpp"

#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace ui {

std::vector<std::string> wrap_message(const std::string& message, size_t max_chars) {
    std::vector<std::string> lines;

    size_t start = 0;
    while (true) {
        const size_t nl = message.find('\n', start);
        std::string line = message.substr(
            start, (nl == std::string::npos) ? std::string::npos : nl - start);

        /* Strip a trailing CR so text pasted from a Windows file does not gain
         * a stray glyph at the end of every line. */
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (max_chars == 0) {
            lines.push_back(std::move(line));
        } else {
            while (line.length() > max_chars) {
                /* Break at the last space that fits; if the word is longer than
                 * the dialog there is nothing to do but cut it. */
                size_t brk = line.rfind(' ', max_chars);
                if (brk == std::string::npos || brk == 0) brk = max_chars;

                lines.push_back(line.substr(0, brk));

                size_t next = brk;
                while (next < line.length() && line[next] == ' ') next++;
                line = line.substr(next);
            }
            lines.push_back(std::move(line));
        }

        if (nl == std::string::npos) break;
        start = nl + 1;
    }

    return lines;
}

ModalMessageView::ModalMessageView(
    NavigationView& nav,
    std::string title,
    std::string message,
    modal_t type,
    std::function<void(bool)> on_choice,
    bool compact)
    : title_{std::move(title)},
      message_{std::move(message)},
      type_{type},
      on_choice_{std::move(on_choice)},
      compact_{compact},
      nav_{nav} {
    set_style(Theme::getInstance()->bg_darkest);

    if (type_ == YESNO) {
        add_children({&button_yes, &button_no});

        button_yes.on_select = [this](Button&) { choose(true); };
        button_no.on_select = [this](Button&) { choose(false); };
    } else {
        add_child(&button_ok);

        button_ok.on_select = [this](Button&) { choose(true); };
    }
}

void ModalMessageView::choose(bool value) {
    if (on_choice_) on_choice_(value);

    /* ABORT means the app underneath has given up, so it goes too. Both pops
     * are queued and applied by NavigationView::service(). */
    nav_.pop();
    if (type_ == ABORT) nav_.pop();
}

void ModalMessageView::set_parent_rect(const Rect new_parent_rect) {
    View::set_parent_rect(new_parent_rect);

    const auto w = new_parent_rect.width();
    const auto h = new_parent_rect.height();

    /* Buttons sit one line clear of the bottom of the view. Upstream measures
     * from screen_height, which is the same place when the view fills the area
     * below the status bar, but not when it does not. */
    const Coord button_y = static_cast<Coord>(h - UI_POS_HEIGHT(4));
    const Dim button_h = UI_POS_HEIGHT(3);

    button_ok.set_parent_rect({(w - UI_POS_WIDTH(10)) / 2, button_y, UI_POS_WIDTH(10), button_h});
    button_yes.set_parent_rect({(w / 2) - UI_POS_WIDTH(10), button_y, UI_POS_WIDTH(8), button_h});
    button_no.set_parent_rect({(w / 2) + UI_POS_WIDTH(2), button_y, UI_POS_WIDTH(8), button_h});
}

void ModalMessageView::paint(Painter& painter) {
    const auto r = screen_rect();
    const Style s = style();

    painter.fill_rectangle(r, s.background);

    const int line_height = s.font.line_height();
    const int char_w = s.font.char_width();
    if (line_height <= 0 || char_w <= 0) return;

    const auto columns = static_cast<size_t>(std::max(1, (r.width() - 16) / char_w));
    const auto lines = wrap_message(message_, columns);

    /* Vertically centre the block between the top of the view and the buttons,
     * which is where upstream's icon would otherwise have been. */
    const int text_area_top = compact_ ? r.top() + UI_POS_HEIGHT(1) : r.top();
    const int text_area_bottom = r.top() + static_cast<int>(button_ok.parent_rect().top());
    const int block_height = static_cast<int>(lines.size()) * line_height;

    int y = compact_
                ? text_area_top
                : text_area_top + std::max(0, (text_area_bottom - text_area_top - block_height) / 2);

    for (const auto& line : lines) {
        if (y + line_height > text_area_bottom) break;
        painter.draw_string({r.left() + 8, y}, s, line);
        y += line_height;
    }
}

void ModalMessageView::focus() {
    if (type_ == YESNO)
        button_yes.focus();
    else
        button_ok.focus();
}

/* --- display_modal ---------------------------------------------------------- */

void display_modal(NavigationView& nav, const std::string& title, const std::string& message) {
    display_modal(nav, title, message, INFO, nullptr, false);
}

void display_modal(
    NavigationView& nav,
    const std::string& title,
    const std::string& message,
    modal_t type,
    std::function<void(bool)> on_choice,
    bool compact) {
    nav.push(std::make_unique<ModalMessageView>(
        nav, title, message, type, std::move(on_choice), compact));
}

void display_modal(const std::string& title, const std::string& message) {
    display_modal(title, message, INFO, nullptr, false);
}

void display_modal(
    const std::string& title,
    const std::string& message,
    modal_t type,
    std::function<void(bool)> on_choice,
    bool compact) {
    auto* nav = app::globals().nav;
    if (nav == nullptr) return;
    display_modal(*nav, title, message, type, std::move(on_choice), compact);
}

}  // namespace ui
