/*
 * mayhem-b200 — App Manager tests.
 *
 * On a host the App Manager is a read-only listing of the compiled-in apps, so
 * the thing to test is that the listing accounts for the whole registry — every
 * registered app appears exactly once, in the registry's order, carrying the
 * same category and hardware_limited flag. The two apps this wave adds are used
 * as anchors: hard_reset (functional) and app_manager (hardware-limited) must
 * both be present, with the flags they were registered with.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_registry.hpp"
#include "ui_app_manager.hpp"

#include <set>
#include <string>

using app::AppRegistry;

TEST(app_manager_lists_every_registered_app) {
    const auto rows = app::app_manager_rows();
    CHECK_EQ(rows.size(), AppRegistry::instance().size());
}

TEST(app_manager_rows_match_the_registry_in_order) {
    const auto rows = app::app_manager_rows();
    const auto& all = AppRegistry::instance().all();

    CHECK_EQ(rows.size(), all.size());
    for (size_t i = 0; i < rows.size() && i < all.size(); i++) {
        CHECK_STR_EQ(rows[i].id, all[i].id);
        CHECK_STR_EQ(rows[i].display_name, all[i].display_name);
        CHECK(rows[i].category == all[i].category);
        CHECK(rows[i].hardware_limited == all[i].hardware_limited);
    }
}

TEST(app_manager_lists_each_app_once) {
    std::set<std::string> seen;
    for (const auto& r : app::app_manager_rows()) {
        const bool inserted = seen.insert(r.id).second;
        if (!inserted)
            ::mb200test::report_failure(__FILE__, __LINE__,
                                        "app listed twice: " + r.id);
    }
    CHECK_EQ(seen.size(), app::app_manager_rows().size());
}

TEST(app_manager_shows_this_waves_two_apps) {
    /* Both .cpp files link into the test runner, so their registrars have run;
     * if the object library were mislinked these would vanish. */
    const auto rows = app::app_manager_rows();

    auto find = [&](const char* id) -> const app::AppManagerRow* {
        for (const auto& r : rows)
            if (r.id == id) return &r;
        return nullptr;
    };

    const auto* hard_reset = find("hard_reset");
    CHECK(hard_reset != nullptr);
    if (hard_reset) {
        CHECK(hard_reset->category == app::Category::Settings);
        /* Hard Reset genuinely works on a host. */
        CHECK(hard_reset->hardware_limited == false);
    }

    const auto* app_manager = find("app_manager");
    CHECK(app_manager != nullptr);
    if (app_manager) {
        CHECK(app_manager->category == app::Category::Settings);
        /* SD-card app management does not apply on a host. */
        CHECK(app_manager->hardware_limited == true);
    }
}

TEST(app_manager_rows_carry_the_registry_flag_faithfully) {
    /* Whatever the registry says about each app, the listing repeats verbatim —
     * the App Manager never second-guesses the hardware_limited flag. */
    for (const auto& r : app::app_manager_rows()) {
        const auto* entry = AppRegistry::instance().by_id(r.id);
        CHECK(entry != nullptr);
        if (entry) CHECK(r.hardware_limited == entry->hardware_limited);
    }
}
