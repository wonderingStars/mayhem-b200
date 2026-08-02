/*
 * mayhem-b200 — scrolling menu list.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __HOST_UI_MENU_H__
#define __HOST_UI_MENU_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <functional>
#include <string>
#include <vector>

namespace ui {

struct MenuItem {
    std::string text;
    Color color;
    std::function<void()> on_select;
};

/* A vertical list of MenuItems with a highlighted cursor. Height is whatever
 * the parent rect allows; the list scrolls to keep the cursor visible. */
class MenuView : public Widget {
   public:
    static constexpr Dim item_height = 24;

    explicit MenuView(Rect parent_rect, bool keep_highlight = false);

    void add_item(MenuItem item);
    void add_items(std::vector<MenuItem> items);
    void clear();

    size_t item_count() const { return items_.size(); }

    size_t highlighted_index() const { return index_; }
    bool set_highlighted(size_t new_index);

    /* Activates the highlighted item. */
    void select_highlighted();

    void paint(Painter& painter) override;
    bool on_key(const KeyEvent key) override;
    bool on_encoder(const EncoderEvent delta) override;
    bool on_touch(const TouchEvent event) override;

    /* Called whenever the cursor moves — apps use it to preview a selection. */
    std::function<void(size_t)> on_highlight{};

   private:
    size_t visible_items() const;
    void scroll_to_cursor();

    std::vector<MenuItem> items_{};
    size_t index_{0};
    size_t offset_{0};
    bool keep_highlight_{false};
};

}  // namespace ui

#endif /*__HOST_UI_MENU_H__*/
