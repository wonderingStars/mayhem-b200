/*
 * mayhem-b200 — app registry tests.
 *
 * These guard the seams between independently written apps. Apps self-register
 * from their own translation units, so nothing else checks that the ids are
 * unique, that every app is actually reachable from the menu, or that the
 * registrars ran at all — a static-library link would silently drop them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_registry.hpp"
#include "ui_widget.hpp"  /* complete ui::View, needed to destroy the factory's unique_ptr */

#include <set>
#include <string>
#include <vector>

using app::AppRegistry;
using app::Category;

namespace {

/* Categories the menu tree actually exposes. Home is the top grid; Receive,
 * Transmit, Transceiver, Utilities, Games and Settings are its tiles; Debug is
 * a sub-page of Settings. An app registered to anything else is unreachable. */
const std::vector<Category> reachable_categories{
    Category::Home, Category::Receive, Category::Transmit, Category::Transceiver,
    Category::Utilities, Category::Games, Category::Settings, Category::Debug,
};

bool is_reachable(Category c) {
    for (auto r : reachable_categories) {
        if (r == c) return true;
    }
    return false;
}

}  // namespace

TEST(registry_registrars_actually_ran) {
    /* If this fails the app sources were linked as a static library and the
     * linker discarded the objects — see the note in app_registry.hpp. */
    CHECK(AppRegistry::instance().size() > 0);
}

TEST(registry_ids_are_unique) {
    std::set<std::string> seen;
    for (const auto& e : AppRegistry::instance().all()) {
        const bool inserted = seen.insert(e.id).second;
        if (!inserted) {
            ::mb200test::report_failure(__FILE__, __LINE__,
                                        "duplicate app id: " + e.id);
        }
    }
}

TEST(registry_entries_are_well_formed) {
    for (const auto& e : AppRegistry::instance().all()) {
        if (e.id.empty())
            ::mb200test::report_failure(__FILE__, __LINE__, "app with empty id");
        if (e.display_name.empty())
            ::mb200test::report_failure(__FILE__, __LINE__,
                                        "app '" + e.id + "' has no display name");
        if (!e.factory)
            ::mb200test::report_failure(__FILE__, __LINE__,
                                        "app '" + e.id + "' has no factory");
    }
}

TEST(every_app_is_reachable_from_the_menu) {
    /* An app in a category the menu never shows is dead code the user cannot
     * reach. Registering Debug apps without a Debug page caused exactly this. */
    for (const auto& e : AppRegistry::instance().all()) {
        if (!is_reachable(e.category)) {
            ::mb200test::report_failure(
                __FILE__, __LINE__,
                "app '" + e.id + "' is in category '" +
                    app::category_name(e.category) + "', which no menu shows");
        }
    }
}

TEST(registry_lookup_by_id) {
    const auto& registry = AppRegistry::instance();
    if (registry.size() == 0) return;

    const auto& first = registry.all().front();
    const auto* found = registry.by_id(first.id);
    CHECK(found != nullptr);
    if (found) CHECK_STR_EQ(found->id, first.id);

    CHECK(registry.by_id("no-such-app-id-exists") == nullptr);
}

TEST(registry_by_category_is_sorted_by_display_name) {
    for (auto c : reachable_categories) {
        const auto apps = AppRegistry::instance().by_category(c);
        for (size_t i = 1; i < apps.size(); i++) {
            if (apps[i - 1]->display_name > apps[i]->display_name) {
                ::mb200test::report_failure(
                    __FILE__, __LINE__,
                    std::string{"category "} + app::category_name(c) +
                        " is not sorted: '" + apps[i - 1]->display_name +
                        "' before '" + apps[i]->display_name + "'");
            }
        }
    }
}

TEST(registry_by_category_partitions_all_apps) {
    /* Every app appears in exactly one category listing, and the listings
     * together account for every registered app. */
    size_t total = 0;
    for (auto c : reachable_categories) total += AppRegistry::instance().by_category(c).size();

    CHECK_EQ(total, AppRegistry::instance().size());
}

TEST(registry_ignores_a_duplicate_id) {
    auto& registry = AppRegistry::instance();
    const size_t before = registry.size();
    if (before == 0) return;

    const std::string existing_id = registry.all().front().id;
    registry.add({existing_id, "Impostor", Category::Utilities,
                  ui::Color::white(), nullptr, [] { return nullptr; }});

    CHECK_EQ(registry.size(), before);
    /* The original entry survives; the duplicate is the one dropped. */
    const auto* found = registry.by_id(existing_id);
    CHECK(found != nullptr);
    if (found) CHECK(found->display_name != "Impostor");
}

TEST(known_core_apps_are_registered) {
    /* A canary: these shipped before the app waves, so if the registry is
     * healthy at all they must be present. Catches a build that half-linked. */
    const auto& registry = AppRegistry::instance();
    for (const char* id : {"audio", "capture", "about"}) {
        if (registry.by_id(id) == nullptr) {
            ::mb200test::report_failure(__FILE__, __LINE__,
                                        std::string{"core app missing: "} + id);
        }
    }
}
