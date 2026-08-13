/*
 * mayhem-b200 — app registry.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_registry.hpp"

/* ui::View is only forward-declared in the header; the factory wrapper in
 * add() owns a std::unique_ptr<ui::View> for the length of one statement, and
 * destroying one needs the complete type. */
#include "ui_widget.hpp"

#include <algorithm>
#include <utility>

namespace app {

const char* category_name(Category c) {
    switch (c) {
        case Category::Home: return "Home";
        case Category::Receive: return "Receive";
        case Category::Transmit: return "Transmit";
        case Category::Transceiver: return "Transceiver";
        case Category::Utilities: return "Utilities";
        case Category::Games: return "Games";
        case Category::Settings: return "Settings";
        case Category::Debug: return "Debug";
    }
    return "?";
}

AppRegistry& AppRegistry::instance() {
    /* Meyers singleton: constructed on first use, so it is alive before any
     * file-scope Registrar runs regardless of static init order. */
    static AppRegistry registry;
    return registry;
}

void AppRegistry::add(AppEntry entry) {
    /* A duplicate id means the same app was registered twice — keep the first
     * and drop the rest rather than showing it twice in the menu. */
    for (const auto& e : entries_) {
        if (e.id == entry.id) return;
    }

    /* Wrap the factory once, here, so that every push site stamps the app id
     * onto the view it pushes without knowing it has to. Three unrelated
     * places build app views — the home/category menus (main_menu.cpp), the
     * scanner, and the web portal's launch queue — and the factory is the
     * only thing all three go through, as well as the only thing that knows
     * which app a `ui::View` is. The web portal reads the stamp back off the
     * navigation stack (see ui::View::app_id). Nothing about what gets pushed
     * changes; an entry with no factory is left exactly as it was. */
    if (entry.factory) {
        entry.factory = [id = entry.id, inner = std::move(entry.factory)]() {
            auto view = inner();
            if (view) view->set_app_id(id);
            return view;
        };
    }

    entries_.push_back(std::move(entry));
}

std::vector<const AppEntry*> AppRegistry::by_category(Category c) const {
    std::vector<const AppEntry*> out;
    for (const auto& e : entries_) {
        if (e.category == c) out.push_back(&e);
    }
    std::sort(out.begin(), out.end(), [](const AppEntry* a, const AppEntry* b) {
        return a->display_name < b->display_name;
    });
    return out;
}

const AppEntry* AppRegistry::by_id(std::string_view id) const {
    for (const auto& e : entries_) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

Registrar::Registrar(AppEntry entry) {
    AppRegistry::instance().add(std::move(entry));
}

}  // namespace app
