/*
 * mayhem-b200 — home and category menus.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "main_menu.hpp"

#include "app_context.hpp"
#include "bitmaps.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

namespace app {

namespace {

const ui::Bitmap* icon_or_generic(const ui::Bitmap* icon) {
    return icon ? icon : &ui::bitmap_icon_generic;
}

/* Pushes the view an app entry produces. Kept in one place so both menus behave
 * identically and the deferred-push contract is obvious. */
void launch(const AppEntry* entry) {
    if (entry == nullptr || !entry->factory) return;
    auto* nav = globals().nav;
    if (nav == nullptr) return;
    if (auto view = entry->factory()) nav->push(std::move(view));
}

void add_category_apps(ui::BtnGridView& grid, Category category) {
    for (const auto* entry : AppRegistry::instance().by_category(category)) {
        grid.add_item({entry->display_name,
                       entry->color,
                       icon_or_generic(entry->icon),
                       [entry] { launch(entry); }},
                      true);
    }
}

}  // namespace

/* --- CategoryMenuView ------------------------------------------------------ */

CategoryMenuView::CategoryMenuView(Category category)
    : category_{category},
      title_{category_name(category)} {
    /* Utilities and Games use wider two-per-row tiles, as upstream does. */
    if (category == Category::Utilities || category == Category::Games ||
        category == Category::Settings) {
        set_max_rows(2);
    }
}

void CategoryMenuView::on_populate() {
    /* Debug is a sub-page of Settings, as it is upstream — without this tile
     * nothing registered under Category::Debug would be reachable from the
     * menu at all. */
    if (category_ == Category::Settings) {
        auto* nav = globals().nav;
        add_item({"Debug", ui::Color::light_grey(), &ui::bitmap_icon_debug,
                  [nav] {
                      if (nav) nav->push(std::make_unique<CategoryMenuView>(Category::Debug));
                  }},
                 true);
    }

    add_category_apps(*this, category_);
}

/* --- MainMenuView ---------------------------------------------------------- */

MainMenuView::MainMenuView() {
    /* The home grid never shows the page arrows — everything fits one screen. */
    show_arrows_enabled(false);
}

void MainMenuView::on_populate() {
    auto* nav = globals().nav;

    struct CategoryTile {
        const char* label;
        Category category;
        ui::Color color;
        const ui::Bitmap* icon;
    };

    static const CategoryTile tiles[] = {
        {"Receive", Category::Receive, ui::Color::cyan(), &ui::bitmap_icon_receivers},
        {"Transmit", Category::Transmit, ui::Color::cyan(), &ui::bitmap_icon_transmit},
        {"Transceiver", Category::Transceiver, ui::Color::cyan(), &ui::bitmap_icon_transceivers},
        {"Utilities", Category::Utilities, ui::Color::cyan(), &ui::bitmap_icon_utilities},
        {"Games", Category::Games, ui::Color::cyan(), &ui::bitmap_icon_games},
        {"Settings", Category::Settings, ui::Color::cyan(), &ui::bitmap_icon_setup},
    };

    for (const auto& t : tiles) {
        const Category category = t.category;
        add_item({t.label, t.color, icon_or_generic(t.icon),
                  [nav, category] {
                      if (nav) nav->push(std::make_unique<CategoryMenuView>(category));
                  }},
                 true);
    }

    /* Home-category apps (Recon, Capture, Replay, Looking Glass, ...) sit on the
     * home screen next to the category tiles, exactly as in Mayhem. */
    add_category_apps(*this, Category::Home);
}

}  // namespace app
