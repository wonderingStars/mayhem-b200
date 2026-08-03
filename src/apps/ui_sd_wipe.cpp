/*
 * mayhem-b200 — Wipe SD card (hardware-limited N/A screen, refuses to act).
 *
 * See ui_sd_wipe.hpp. This app intentionally performs no filesystem operation
 * of any kind. There is no disk_write(), no open, no unlink — the destructive
 * upstream behaviour has no safe host analogue and is refused outright.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_sd_wipe.hpp"

#include "app_context.hpp"
#include "ui_navigation.hpp"

namespace app {

WipeResult WipeSDView::attempt_wipe() {
    /* Hard refusal. We do not touch the host filesystem, and there is no SD
     * card to touch. Return the refusal; perform nothing. */
    return {false, 0, "No SD card; host disk not touched."};
}

WipeSDView::WipeSDView() {
    add_children({&console_, &button_wipe_, &button_back_});

    button_wipe_.on_select = [this](ui::Button&) {
        /* Mirrors the upstream destructive action, but it is a guaranteed
         * no-op: attempt_wipe() writes nothing anywhere. */
        const auto result = attempt_wipe();
        console_.writeln("");
        console_.writeln(STR_COLOR_RED "Refused:");
        console_.writeln(STR_COLOR_RED + (" " + result.message));
    };

    button_back_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void WipeSDView::on_show() {
    View::on_show();
    button_back_.focus();
    render();
}

void WipeSDView::render() {
    console_.clear();
    console_.enable_scrolling(false);

    console_.writeln(STR_COLOR_RED "Wipe SD card");
    console_.writeln("");
    console_.writeln("On a PortaPack this over-");
    console_.writeln("wrote the FAT of the microSD");
    console_.writeln("card with noise - a wipe of");
    console_.writeln("PortaPack-only hardware.");
    console_.writeln("");
    console_.writeln(STR_COLOR_CYAN "Not applicable on a B200:");
    console_.writeln("there is no SD card here.");
    console_.writeln("");
    console_.writeln(STR_COLOR_YELLOW "This app will NOT wipe the");
    console_.writeln(STR_COLOR_YELLOW "host PC's own filesystem.");
    console_.writeln("The action is disabled and");
    console_.writeln("touches nothing.");
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Utilities, red — matches upstream sd_wipe's app_location_t::UTILITIES and
 * ui::Color::red() icon_color. hardware_limited: PortaPack-only, and here it is
 * a hard-refusal screen that performs no destructive action. */
const app::Registrar reg_sd_wipe{{"sd_wipe", "Wipe SD card", app::Category::Utilities,
                                  ui::Color::red(), &ui::bitmap_icon_dir,
                                  [] { return std::make_unique<app::WipeSDView>(); },
                                  true}};
}  // namespace
