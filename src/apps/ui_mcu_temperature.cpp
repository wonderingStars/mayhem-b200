/*
 * mayhem-b200 — MCU Temperature (host N/A screen).
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (upstream)
 * Copyright (C) 2026 mayhem-b200 contributors (host N/A screen)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_mcu_temperature.hpp"

#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

namespace app {

std::vector<std::string> McuTemperatureView::status_report() {
    /* Plain text only here so it round-trips through a test unchanged; the view
     * adds the colour escapes when it prints. */
    return {
        "On a PortaPack this plotted the",
        "die temperature of the LPC43xx",
        "microcontroller, sampled once a",
        "second from its on-die sensor.",
        "",
        "A USRP B200 is a USB SDR front",
        "end driven by this host PC. It",
        "has no LPC43xx and no on-board",
        "microcontroller whose die temp",
        "could be read.",
        "",
        "The host CPU's own thermal",
        "sensors are a different device",
        "and are not what this measured,",
        "so there is no equivalent here.",
        "",
        "No MCU temperature to report.",
    };
}

McuTemperatureView::McuTemperatureView() {
    add_children({&console_, &button_done_});

    button_done_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void McuTemperatureView::on_show() {
    View::on_show();
    button_done_.focus();

    console_.clear();
    console_.enable_scrolling(false);

    console_.writeln(STR_COLOR_YELLOW "MCU Temp  (N/A on B200)");
    console_.writeln("");
    for (const auto& line : status_report()) {
        /* Colour the one line that states the result. */
        if (line == "No MCU temperature to report.")
            console_.writeln(STR_COLOR_RED + line);
        else
            console_.writeln(line);
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream: menu_location DEBUG, thermometer icon. Registered so the menu still
 * matches Mayhem; hardware_limited flags it as a PortaPack-only capability that
 * a B200 host cannot provide. */
const app::Registrar reg_mcu_temperature{{"mcu_temperature", "MCU Temp",
                                          app::Category::Debug, ui::Color::yellow(),
                                          &ui::bitmap_icon_thermometer,
                                          [] { return std::make_unique<app::McuTemperatureView>(); },
                                          true}};
}  // namespace
