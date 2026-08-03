/*
 * mayhem-b200 — app registry.
 *
 * The firmware keeps one central `appList` array (application/ui_navigation.cpp)
 * that every menu reads. Editing one shared array does not parallelise, so here
 * each app registers *itself* from its own translation unit with a file-scope
 * Registrar, exactly the way the test harness registers tests. Adding an app is
 * a self-contained new .cpp — no shared file to edit, no merge conflict.
 *
 * IMPORTANT build note: this only works because the app sources are compiled
 * into an OBJECT library (see CMakeLists.txt). In a STATIC library the linker
 * discards any object nothing references, and an app reached only through its
 * Registrar is exactly that — its constructor would never run and the app would
 * silently vanish from the menu.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_APP_REGISTRY_H__
#define __MB200_APP_REGISTRY_H__

#include "ui.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ui {
class View;
struct Bitmap;
}  // namespace ui

namespace app {

/* Mirrors the firmware's app_location_t. Home holds the category tiles plus the
 * few apps that sit at the top level; Debug is a sub-page of Settings. */
enum class Category : uint8_t {
    Home = 0,
    Receive,
    Transmit,
    Transceiver,
    Utilities,
    Games,
    Settings,
    Debug,
};

const char* category_name(Category c);

struct AppEntry {
    std::string id;            /* short name, e.g. "adsbrx" — matches upstream */
    std::string display_name;  /* menu caption, e.g. "ADS-B" */
    Category category;
    ui::Color color;
    const ui::Bitmap* icon;    /* may be null; the menu falls back to a generic */
    std::function<std::unique_ptr<ui::View>()> factory;

    /* Set for apps that need hardware a B200 does not have. The app still
     * appears (so the menu matches Mayhem) but shows an explanation instead of
     * pretending to work. Purely informational; the factory still runs. */
    bool hardware_limited{false};
};

class AppRegistry {
   public:
    static AppRegistry& instance();

    void add(AppEntry entry);

    /* All apps in a category, sorted by display name (as the firmware sorts). */
    std::vector<const AppEntry*> by_category(Category c) const;

    const AppEntry* by_id(std::string_view id) const;

    const std::vector<AppEntry>& all() const { return entries_; }
    size_t size() const { return entries_.size(); }

   private:
    std::vector<AppEntry> entries_;
};

/* File-scope helper: declare `const app::Registrar reg{{...}};` in an anonymous
 * namespace in each app's .cpp. */
struct Registrar {
    explicit Registrar(AppEntry entry);
};

}  // namespace app

#endif /*__MB200_APP_REGISTRY_H__*/
