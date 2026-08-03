/*
 * mayhem-b200 — home and category menus.
 *
 * Mayhem's home screen is an icon grid, not a list. The top level shows the
 * category tiles (Receive, Transmit, Transceiver, Utilities, Games, Settings)
 * plus any app registered directly under Home, and each category is its own
 * icon-grid sub-page populated from the app registry.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_MAIN_MENU_H__
#define __MB200_MAIN_MENU_H__

#include "app_registry.hpp"
#include "ui.hpp"
#include "ui_btngrid.hpp"

#include <string>

namespace app {

/* A grid page filled from one registry category. Pushing an app is deferred to
 * the navigation layer, so selecting a tile is safe even though it may destroy
 * this menu. */
class CategoryMenuView : public ui::BtnGridView {
   public:
    explicit CategoryMenuView(Category category);

    std::string title() const override { return title_; }

   protected:
    void on_populate() override;

   private:
    Category category_;
    std::string title_;
};

/* The home grid: category tiles first, then Home-category apps. */
class MainMenuView : public ui::BtnGridView {
   public:
    MainMenuView();

    std::string title() const override { return "Mayhem B200"; }

   protected:
    void on_populate() override;
};

}  // namespace app

#endif /*__MB200_MAIN_MENU_H__*/
