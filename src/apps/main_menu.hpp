/*
 * mayhem-b200 — main menu.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_MAIN_MENU_H__
#define __MB200_MAIN_MENU_H__

#include "ui.hpp"
#include "ui_menu.hpp"
#include "ui_widget.hpp"

#include <string>

namespace app {

class MainMenuView : public ui::View {
   public:
    MainMenuView();

    std::string title() const override { return "Mayhem B200"; }

    void on_show() override;

   private:
    ui::MenuView menu_{{0, 8, 240, 240}, true};
    ui::Text text_hint_{{0, 264, 240, 16}, ""};
    ui::Text text_device_{{0, 282, 240, 16}, ""};
};

}  // namespace app

#endif /*__MB200_MAIN_MENU_H__*/
