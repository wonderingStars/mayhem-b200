/*
 * mayhem-b200 — icon grid menu.
 *
 * A port of firmware/application/ui/ui_btngrid.hpp: a scrolling grid of
 * labelled icon tiles with a highlighted cursor. This is Mayhem's real home
 * screen and the shape every category menu takes.
 *
 * The tile widget is ui::NewButton from ui_widget_extra.hpp, ported verbatim
 * from upstream. The grid uses GridTile, a two-method subclass, because the
 * firmware's event dispatcher bubbles an unhandled key or encoder tick up the
 * parent chain and this project's does not: the grid wires each tile's `on_dir`
 * hook (which upstream provides for exactly this) for keys, and GridTile hands
 * an unclaimed encoder tick and the focus-on-press to its parent.
 *
 * The SD-card reload signal is gone — there is no removable card whose
 * appearance should repopulate the menu.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2019 Elia Yehuda (z4ziggy)
 * Copyright (C) 2024 u-foka
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __HOST_UI_BTNGRID_H__
#define __HOST_UI_BTNGRID_H__

#include "ui.hpp"
#include "ui_painter.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"  /* NewButton */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

/* One cell of the grid. Everything it draws is NewButton's; the two overrides
 * exist only because this project's event dispatcher, unlike the firmware's,
 * does not walk the parent chain looking for a handler. */
class GridTile : public NewButton {
   public:
    using NewButton::NewButton;

    /* Hands an encoder tick the tile does not want to the grid. */
    bool on_encoder(const EncoderEvent delta) override;

    /* Takes focus on press, so the grid's cursor follows the pointer; Button
     * does the same for the same reason. */
    bool on_touch(const TouchEvent event) override;
};

struct GridItem {
    std::string text;
    Color color;
    const Bitmap* bitmap;
    std::function<void(void)> on_select;
};

/* Apps named in the blacklist are dropped as they are added to a grid. The
 * firmware reads /SETTINGS/blacklist off the SD card; here it is
 * <data dir>/SETTINGS/blacklist. Missing file means "nothing hidden". */
void load_blacklist();

/* Replaces the loaded blacklist with `raw` (newline- or comma-separated app
 * names). Exposed so the behaviour is testable without touching the disk. */
void set_blacklist(std::string_view raw);

/* True if `name` is currently hidden. */
bool is_blacklisted(std::string_view name);

class BtnGridView : public View {
   public:
    explicit BtnGridView(Rect new_parent_rect = {0, 0, screen_width, screen_height - 16},
                         bool keep_highlight = false);
    ~BtnGridView() override;

    void add_items(std::initializer_list<GridItem> new_items, bool inhibit_update = false);
    void add_item(const GridItem& new_item, bool inhibit_update = false);
    void insert_item(const GridItem& new_item, size_t position, bool inhibit_update = false);

    /* Tiles per row. Unlike upstream this re-runs the layout immediately, so a
     * subclass constructor calling set_max_rows(2) does not have to wait for the
     * navigation stack to hand it a parent rect before the grid is correct. */
    void set_max_rows(int rows);
    int rows() const;
    void clear();

    size_t item_count() const { return menu_items.size(); }
    /* Number of tiles the current parent rect can show at once. */
    size_t displayed_max_items() const { return displayed_max; }
    /* Index of the first item on screen. */
    size_t offset_index() const { return offset; }

    NewButton* item_view(size_t index) const;

    bool show_arrows{true};  // flag used to hide arrows in main menu
    void show_arrows_enabled(bool enabled);

    bool set_highlighted(int32_t new_value, bool force_update = false);
    uint32_t highlighted_index() const;

    /* Runs the highlighted item's callback, if it has one. */
    void select_highlighted();

    void set_parent_rect(const Rect new_parent_rect) override;

    bool arrow_up_enabled{false};
    bool arrow_down_enabled{false};
    void set_arrow_up_enabled(bool enabled);
    void set_arrow_down_enabled(bool enabled);
    void show_hide_arrows();

    void on_focus() override;
    void on_blur() override;
    void on_show() override;
    void on_hide() override;
    bool on_key(const KeyEvent event) override;
    bool on_encoder(const EncoderEvent event) override;

    void reload_items();
    void update_items();

    void set_btn_height_fixed(uint8_t h) { button_h = h; }

    /* Tile background. The firmware reads this from persistent memory
     * (pmem::menu_color, default grey); there is no settings store yet, so it is
     * a plain member with the same default. */
    void set_menu_color(Color c);
    Color menu_color() const { return menu_color_; }

    void page_up();
    void page_down();

   protected:
    virtual void on_populate() = 0;

   private:
    /* Destroys and recreates the tile widgets for the current parent rect and
     * rows-per-screen, then reassigns item data to them. */
    void rebuild_item_views();

    /* Drops focus if it currently sits on a tile that is about to be destroyed —
     * the focus manager keeps a raw pointer and would otherwise dangle. */
    void release_item_focus();

    /* Moves focus onto the tile showing the highlighted item, if it is on
     * screen and the grid has been drawn at least once. */
    void focus_highlighted_view();

    int rows_{3};
    bool keep_highlight{false};

    std::vector<GridItem> menu_items{};
    std::vector<std::unique_ptr<GridTile>> menu_item_views{};

    Button button_pgup{{0, 1324, 120, 16}, "       "};
    Button button_pgdown{{121, 1324, 119, 16}, "         "};

    int button_w{screen_width / rows_};
    int button_h{48};
    size_t displayed_max{0};
    size_t highlighted_item{0};
    size_t offset{0};
    Color menu_color_{Color::grey()};
};

}  // namespace ui

#endif /*__HOST_UI_BTNGRID_H__*/
