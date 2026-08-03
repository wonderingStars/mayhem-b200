/*
 * mayhem-b200 — icon grid menu.
 *
 * Ported from firmware/application/ui/ui_btngrid.cpp; the tile widget it draws
 * with is ui::NewButton from ui_widget_extra.hpp. Scrolling, offset rounding,
 * arrow-button behaviour and the paging rules are upstream's; see
 * ui_btngrid.hpp for the list of deliberate host differences.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2019 Elia Yehuda (z4ziggy)
 * Copyright (C) 2023 Mark Thompson
 * Copyright (C) 2024 u-foka
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_btngrid.hpp"

#include "file_path.hpp"
#include "theme.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace ui {

/* --- GridTile -------------------------------------------------------------- */

bool GridTile::on_encoder(const EncoderEvent delta) {
    /* The firmware's dispatcher walks the parent chain looking for a handler;
     * this project's does not, so the tile hands the tick to its grid itself. */
    auto* p = parent();
    return (p != nullptr) && p->on_encoder(delta);
}

bool GridTile::on_touch(const TouchEvent event) {
    /* Taking focus on press is what moves the grid's cursor to the tile under
     * the pointer; Button does the same thing for the same reason. Everything
     * else — the highlight and the select on release — is NewButton's. */
    if (event.type == TouchEvent::Type::Start) focus();
    return NewButton::on_touch(event);
}

/* --- Blacklist ------------------------------------------------------------- */

namespace {

/* Held with a leading and trailing comma so a whole-name match is a plain
 * substring search for ",<name>,", exactly as upstream does it. */
std::string blacklist_data{};

}  // namespace

void set_blacklist(std::string_view raw) {
    if (raw.empty()) {
        blacklist_data.clear();
        return;
    }

    blacklist_data.assign(raw.size() + 2, ',');
    std::copy(raw.begin(), raw.end(), blacklist_data.begin() + 1);

    for (char& c : blacklist_data) {
        if (c == '\r' || c == '\n') c = ',';
    }
}

void load_blacklist() {
    blacklist_data.clear();

    const std::string path = core::data_directory() + "/SETTINGS/blacklist";
    std::ifstream f{path, std::ios::binary};
    if (!f) return;  /* no file means nothing is hidden */

    const std::string raw{std::istreambuf_iterator<char>{f},
                          std::istreambuf_iterator<char>{}};
    set_blacklist(raw);
}

bool is_blacklisted(std::string_view name) {
    if (blacklist_data.empty()) return false;

    std::string needle{','};
    needle.append(name);
    needle.push_back(',');

    if (blacklist_data.size() < needle.size()) return false;
    return blacklist_data.find(needle) != std::string::npos;
}

/* --- BtnGridView ----------------------------------------------------------- */

BtnGridView::BtnGridView(Rect new_parent_rect, bool keep_highlight)
    : keep_highlight{keep_highlight} {
    set_focusable(true);

    if (screen_height == 480) button_h = 64;

    button_pgup.set_focusable(false);
    button_pgup.on_select = [this](Button&) { page_up(); };

    button_pgdown.set_focusable(false);
    button_pgdown.on_select = [this](Button&) { page_down(); };

    button_pgup.set_style(Theme::getInstance()->bg_darkest_small);
    button_pgdown.set_style(Theme::getInstance()->bg_darkest_small);

    set_parent_rect(new_parent_rect);

    add_child(&button_pgup);
    add_child(&button_pgdown);
}

BtnGridView::~BtnGridView() {
    /* The focus manager holds a raw pointer; the tiles are about to die. */
    auto& fm = context().focus_manager();
    for (auto& item : menu_item_views) {
        if (fm.focus_widget() == item.get()) {
            fm.set_focus_widget(nullptr);
            break;
        }
    }
}

void BtnGridView::set_max_rows(int rows) {
    if (rows < 1) rows = 1;
    if (rows == rows_) return;

    rows_ = rows;
    rebuild_item_views();
}

int BtnGridView::rows() const {
    return rows_;
}

void BtnGridView::set_menu_color(Color c) {
    if (c.v == menu_color_.v) return;
    menu_color_ = c;
    update_items();
}

void BtnGridView::set_parent_rect(const Rect new_parent_rect) {
    View::set_parent_rect(new_parent_rect);
    rebuild_item_views();
}

void BtnGridView::rebuild_item_views() {
    const int height = parent_rect().size().height();
    const int space_available = height - 16;  // leave space for arrows

    /* Upstream would happily compute zero rows for a short parent rect and then
     * show nothing at all; one row is the least confusing floor. */
    int rows_on_screen = (button_h > 0) ? (height / button_h) : 0;
    if (rows_on_screen < 1) rows_on_screen = 1;
    displayed_max = static_cast<size_t>(rows_on_screen) * static_cast<size_t>(rows_);

    button_pgup.set_parent_rect({0, space_available, screen_width / 2, 16});
    button_pgdown.set_parent_rect({screen_width / 2, space_available, screen_width / 2, 16});

    release_item_focus();

    for (auto& item : menu_item_views) remove_child(item.get());
    menu_item_views.clear();

    button_w = screen_width / rows_;

    for (size_t c = 0; c < displayed_max; c++) {
        auto item = std::make_unique<GridTile>();
        item->set_vertical_center(true);
        item->set_parent_rect({static_cast<int>(c % static_cast<size_t>(rows_)) * button_w,
                               static_cast<int>(c / static_cast<size_t>(rows_)) * button_h,
                               button_w, button_h});
        add_child(item.get());
        menu_item_views.push_back(std::move(item));
    }

    update_items();
}

void BtnGridView::release_item_focus() {
    auto& fm = context().focus_manager();
    auto* focused = fm.focus_widget();
    if (focused == nullptr) return;

    for (auto& item : menu_item_views) {
        if (item.get() == focused) {
            fm.set_focus_widget(nullptr);
            return;
        }
    }
}

void BtnGridView::set_arrow_up_enabled(bool enabled) {
    if (!show_arrows) return;

    if (enabled) {
        if (!arrow_up_enabled) {
            arrow_up_enabled = true;
            button_pgup.set_text("< PREV");
        }
    } else {
        if (arrow_up_enabled) {
            arrow_up_enabled = false;
            button_pgup.set_text("      ");
        }
    }
}

void BtnGridView::set_arrow_down_enabled(bool enabled) {
    if (!show_arrows) return;

    if (enabled) {
        if (!arrow_down_enabled) {
            arrow_down_enabled = true;
            button_pgdown.set_text("NEXT >");
        }
    } else {
        if (arrow_down_enabled) {
            arrow_down_enabled = false;
            button_pgdown.set_text("      ");
        }
    }
}

void BtnGridView::clear() {
    menu_items.clear();

    release_item_focus();
    for (auto& item : menu_item_views) remove_child(item.get());
    menu_item_views.clear();
}

void BtnGridView::add_items(std::initializer_list<GridItem> new_items, bool inhibit_update) {
    for (const auto& item : new_items) {
        if (!is_blacklisted(item.text)) menu_items.push_back(item);
    }

    if (!inhibit_update) update_items();
}

void BtnGridView::add_item(const GridItem& new_item, bool inhibit_update) {
    if (is_blacklisted(new_item.text)) return;

    menu_items.push_back(new_item);
    if (!inhibit_update) update_items();
}

void BtnGridView::insert_item(const GridItem& new_item, size_t position, bool inhibit_update) {
    if (is_blacklisted(new_item.text)) return;

    if (position < menu_items.size()) {
        menu_items.insert(menu_items.begin() + static_cast<std::ptrdiff_t>(position), new_item);
    } else {
        menu_items.push_back(new_item);
    }

    if (!inhibit_update) update_items();
}

void BtnGridView::show_hide_arrows() {
    /* No items: no paging, and no size-1 underflow. */
    if (menu_items.empty()) {
        set_arrow_up_enabled(false);
        set_arrow_down_enabled(false);
        return;
    }

    set_arrow_up_enabled(highlighted_item != 0);
    set_arrow_down_enabled(highlighted_item != (menu_items.size() - 1));
}

void BtnGridView::reload_items() {
    /* on_hide() drops the tile widgets; rebuild before repopulating so a
     * hide/show round trip does not leave an empty grid. */
    if (menu_item_views.empty()) rebuild_item_views();

    menu_items.clear();
    on_populate();
    set_highlighted(static_cast<int32_t>(highlighted_item), true);
    show_hide_arrows();

    set_dirty();  // redraw the now potentially empty space as well
}

void BtnGridView::update_items() {
    size_t i = 0;

    for (auto& item : menu_item_views) {
        const size_t index = i + offset;

        if (index >= menu_items.size()) {
            item->hidden(true);
            item->set_text(" ");
            item->set_bitmap(nullptr);
            item->on_select = []() {};
            item->on_dir = nullptr;
            item->on_highlight = nullptr;
            item->set_dirty();
        } else {
            item->hidden(false);
            item->set_text(menu_items[index].text);
            item->set_bitmap(menu_items[index].bitmap);
            item->set_color(menu_items[index].color);
            item->set_bg_color(menu_color_);
            item->on_select = menu_items[index].on_select;
            /* Direction keys reach the grid through the tile, because the host
             * dispatcher does not bubble unhandled keys to the parent. */
            item->on_dir = [this](NewButton&, KeyEvent key) { return on_key(key); };
            item->on_highlight = [this, index](NewButton&) {
                highlighted_item = index;
                show_hide_arrows();
            };
            item->set_dirty();
        }

        i++;
    }
}

NewButton* BtnGridView::item_view(size_t index) const {
    if (index >= menu_item_views.size()) return nullptr;
    return menu_item_views[index].get();
}

void BtnGridView::show_arrows_enabled(bool enabled) {
    show_arrows = enabled;
    if (!enabled) {
        remove_child(&button_pgup);
        remove_child(&button_pgdown);
    }
}

void BtnGridView::focus_highlighted_view() {
    if (!drawn()) return;
    if (highlighted_item < offset) return;

    if (auto* v = item_view(highlighted_item - offset)) v->focus();
}

bool BtnGridView::set_highlighted(int32_t new_value, bool force_update) {
    const int32_t item_count = static_cast<int32_t>(menu_items.size());

    /* Nothing to highlight when the list is empty. */
    if (item_count == 0) {
        highlighted_item = 0;
        offset = 0;
        show_hide_arrows();
        if (force_update) update_items();
        return false;
    }

    if (new_value < 0) return false;
    if (new_value >= item_count) new_value = item_count - 1;

    const size_t value = static_cast<size_t>(new_value);
    const size_t row_size = static_cast<size_t>(rows_);
    bool needs_update = false;

    if ((value > offset) && ((value - offset) >= displayed_max)) {
        /* Shift the grid up, rounding the new offset up to a whole row. */
        highlighted_item = value;
        offset = value - displayed_max + row_size;
        offset -= (offset % row_size);
        needs_update = true;
        /* Repaint everything only when scrolling the last row up leaves a blank
         * tile at the bottom — otherwise the display flickers for nothing. */
        if ((new_value + rows_ >= item_count) && (item_count % rows_) != 0) set_dirty();
    } else if (value < offset) {
        /* Shift the grid down. All tiles get repainted, so no set_dirty(). */
        highlighted_item = value;
        offset = (value / row_size) * row_size;
        needs_update = true;
    } else {
        highlighted_item = value;
    }

    /* Normalise the offset so a shrunken list still fills the screen. */
    if (offset + displayed_max > static_cast<size_t>(item_count)) {
        offset = (static_cast<size_t>(item_count) >= displayed_max)
                     ? static_cast<size_t>(item_count) - displayed_max
                     : 0;
        needs_update = true;
    }

    if (needs_update || force_update) update_items();

    focus_highlighted_view();
    show_hide_arrows();

    return true;
}

uint32_t BtnGridView::highlighted_index() const {
    return static_cast<uint32_t>(highlighted_item);
}

void BtnGridView::select_highlighted() {
    if (highlighted_item >= menu_items.size()) return;

    /* Copy before calling: an item usually pushes a view, and the navigation
     * stack may destroy this grid while the handler is still on the stack. */
    const auto handler = menu_items[highlighted_item].on_select;
    if (handler) handler();
}

void BtnGridView::on_focus() {
    if (menu_items.empty()) return;
    if (highlighted_item < offset) return;

    if (auto* v = item_view(highlighted_item - offset)) v->focus();
}

void BtnGridView::on_blur() {
    if (keep_highlight) return;
    if (menu_items.empty()) return;
    if (highlighted_item < offset) return;

    if (auto* v = item_view(highlighted_item - offset)) v->set_highlighted(false);
}

void BtnGridView::on_show() {
    View::on_show();
    reload_items();

    /* Give a tile initial keyboard focus. Upstream relied on the firmware's
     * event dispatcher bubbling the first key up to the grid; this project's
     * dispatcher routes only to the focused widget, so without this the grid
     * would ignore the keyboard until the pointer touched a tile. */
    if (item_count() > 0) {
        const size_t visible = highlighted_index() - offset_index();
        if (auto* v = item_view(visible)) v->focus();
    }
}

void BtnGridView::on_hide() {
    View::on_hide();
    clear();
    set_arrow_up_enabled(false);
    set_arrow_down_enabled(false);
}

bool BtnGridView::on_key(const KeyEvent key) {
    switch (key) {
        case KeyEvent::Up:
            return set_highlighted(static_cast<int32_t>(highlighted_item) - rows_);

        case KeyEvent::Down:
            return set_highlighted(static_cast<int32_t>(highlighted_item) + rows_);

        case KeyEvent::Right:
            return set_highlighted(static_cast<int32_t>(highlighted_item) + 1);

        case KeyEvent::Left:
            return set_highlighted(static_cast<int32_t>(highlighted_item) - 1);

        case KeyEvent::Select:
            select_highlighted();
            return true;

        default:
            return false;
    }
}

bool BtnGridView::on_encoder(const EncoderEvent event) {
    return set_highlighted(static_cast<int32_t>(highlighted_item) + event);
}

void BtnGridView::page_up() {
    if (!arrow_up_enabled) return;

    const size_t item_count = menu_items.size();
    if (item_count == 0) return;

    const size_t new_offset = (offset > displayed_max) ? (offset - displayed_max) : 0;

    /* Already at the top: put the cursor on the first item instead. */
    if (new_offset == offset) {
        set_highlighted(0);
        return;
    }

    const bool was_visible =
        (highlighted_item >= new_offset) && (highlighted_item < new_offset + displayed_max);

    offset = new_offset;
    update_items();

    if (was_visible) {
        focus_highlighted_view();
    } else {
        /* Last item on the new page, clamped to the last item overall. */
        const size_t last_on_page = std::min(new_offset + displayed_max, item_count) - 1;
        set_highlighted(static_cast<int32_t>(last_on_page));
    }
}

void BtnGridView::page_down() {
    if (!arrow_down_enabled) return;

    const size_t item_count = menu_items.size();
    if (item_count == 0) return;

    const size_t max_offset = (item_count > displayed_max) ? (item_count - displayed_max) : 0;
    const size_t new_offset = std::min(offset + displayed_max, max_offset);

    /* Already at the bottom: put the cursor on the last item instead. */
    if (new_offset == offset) {
        set_highlighted(static_cast<int32_t>(item_count - 1));
        return;
    }

    const bool was_visible =
        (highlighted_item >= new_offset) && (highlighted_item < new_offset + displayed_max);

    offset = new_offset;
    update_items();

    if (was_visible) {
        focus_highlighted_view();
    } else {
        set_highlighted(static_cast<int32_t>(new_offset));
    }
}

}  // namespace ui
