/*
 * mayhem-b200 — App Manager.
 *
 * Port of firmware/application/external/app_manager/. On a PortaPack that app
 * manages the external apps loaded from the SD card as .ppma/.ppmp files: it
 * lists both the built-in apps and the SD-card apps, lets you hide/show entries
 * from the menu (a SETTINGS/blacklist file) and pick one to autostart at boot.
 *
 * On a B200 host there is no SD card and there are no loadable app files: every
 * app is compiled into this executable and self-registers through app_registry.
 * So the install/remove half of the upstream app has nothing to act on and would
 * be a lie to fake. What still makes sense — and is genuinely useful — is a
 * read-only listing of exactly which apps are built in, which category each
 * lives in, and which are hardware-limited (present so the menu matches Mayhem
 * but unable to do their PortaPack job on a host). That is what this view shows.
 *
 * hardware_limited = true: the management this app existed to do (installing,
 * removing, hiding SD-card apps) does not apply on a host.
 *
 * Copyright (C) 2024 zxkmm (original design)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_APP_MANAGER_H__
#define __MB200_UI_APP_MANAGER_H__

#include "ui.hpp"
#include "ui_menu.hpp"
#include "ui_widget.hpp"

#include "app_registry.hpp"

#include <string>
#include <vector>

namespace app {

/* One read-only row of the App Manager list — a flattened copy of an AppEntry
 * with just the fields the view needs. Copied out (rather than holding pointers)
 * so the list is a stable snapshot independent of the registry. */
struct AppManagerRow {
    std::string id;
    std::string display_name;
    Category category;
    ui::Color color;
    const ui::Bitmap* icon;
    bool hardware_limited;
};

/* Enumerates every registered app in the registry's own order. This is the whole
 * of the App Manager's data model on a host — there is nothing to install or
 * remove — so it is exposed for the tests to assert against directly. */
inline std::vector<AppManagerRow> app_manager_rows() {
    std::vector<AppManagerRow> rows;
    const auto& all = AppRegistry::instance().all();
    rows.reserve(all.size());
    for (const auto& e : all) {
        rows.push_back({e.id, e.display_name, e.category, e.color, e.icon,
                        e.hardware_limited});
    }
    return rows;
}

class AppManagerView : public ui::View {
   public:
    AppManagerView();

    std::string title() const override { return "AppMan"; }
    void on_show() override;

   private:
    void populate();
    void update_info(size_t index);

    std::vector<AppManagerRow> rows_{};

    ui::Labels labels_{
        {{1 * 8, 0 * 16}, "Built-in apps (read-only)", ui::Color::light_grey()},
    };

    ui::MenuView menu_view_{
        {0, 1 * 16 + 4, 240, 8 * ui::MenuView::item_height}};

    ui::Text text_info_{
        {0, 1 * 16 + 4 + 8 * ui::MenuView::item_height + 2, 240, 1 * 16}, ""};

    ui::Labels na_labels_{
        {{1 * 8, 15 * 16}, "Apps are compiled in;", ui::Color::light_grey()},
        {{1 * 8, 16 * 16}, "nothing to install/remove.", ui::Color::light_grey()},
    };

    ui::Button button_back_{
        {68, 276, 104, 28}, "Back"};
};

}  // namespace app

#endif /*__MB200_UI_APP_MANAGER_H__*/
