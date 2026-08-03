/*
 * mayhem-b200 — external / on-board sensor viewer (host N/A screen).
 *
 * Copyright (C) 2023 Bernd Herzog (upstream extsensors)
 * Copyright (C) 2026 mayhem-b200 contributors (host N/A screen)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_extsensors.hpp"

#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

namespace app {

std::vector<std::string> ExtSensorsView::status_report() {
    /* Plain text only here so it round-trips through a test unchanged; the view
     * adds the colour escapes when it prints. */
    return {
        "On a PortaPack this read sensor",
        "modules on the I2C bus:",
        " GPS   position (lat/lon)",
        " ORI   orientation / tilt",
        " ENV   temp / humidity / press",
        " LUX   ambient light",
        "scanning the bus every 3 s.",
        "",
        "A USRP B200 is a USB SDR front",
        "end. It has no I2C sensor bus",
        "and no on-board sensors, so",
        "sensor data is unavailable.",
        "",
        "Nothing to read on this host.",
    };
}

ExtSensorsView::ExtSensorsView() {
    add_children({&console_, &button_done_});

    button_done_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void ExtSensorsView::on_show() {
    View::on_show();
    button_done_.focus();

    console_.clear();
    console_.enable_scrolling(false);

    console_.writeln(STR_COLOR_YELLOW "Ext Sensors  (N/A on B200)");
    console_.writeln("");
    for (const auto& line : status_report()) {
        /* Colour the one line that states the result. */
        if (line == "sensor data is unavailable.")
            console_.writeln(STR_COLOR_RED + line);
        else
            console_.writeln(line);
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream: menu_location DEBUG, icon_color yellow. Registered so the menu still
 * matches Mayhem; hardware_limited flags it as a PortaPack-only capability. */
const app::Registrar reg_extsensors{{"extsensors", "Ext Sensors", app::Category::Debug,
                                     ui::Color::yellow(), &ui::bitmap_icon_thermometer,
                                     [] { return std::make_unique<app::ExtSensorsView>(); },
                                     true}};
}  // namespace
