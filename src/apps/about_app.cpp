/*
 * mayhem-b200 — about / help screen.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "about_app.hpp"

#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

namespace app {

AboutView::AboutView() {
    add_children({&console_, &button_done_});

    button_done_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void AboutView::on_show() {
    View::on_show();
    button_done_.focus();

    console_.clear();
    console_.enable_scrolling(false);

    console_.writeln(STR_COLOR_CYAN "Mayhem B200");
    console_.writeln(std::string{"version "} + kVersion);
    console_.writeln("");
    console_.writeln("PortaPack Mayhem's apps and UI,");
    console_.writeln("running on the host against an");
    console_.writeln("Ettus USRP B200 through UHD.");
    console_.writeln("");
    console_.writeln(STR_COLOR_YELLOW "Controls");
    console_.writeln(" Arrows    nav / tune");
    console_.writeln(" Enter     select");
    console_.writeln(" Esc       back");
    console_.writeln(" Wheel     encoder");
    console_.writeln(" PgUp/Dn   encoder step");
    console_.writeln(" Mouse     touch screen");
    console_.writeln(" F11       window scale");
    console_.writeln("");
    console_.writeln(STR_COLOR_YELLOW "Credits");
    console_.writeln(" UI and app design from the");
    console_.writeln(" PortaPack Mayhem project.");
    console_.writeln(" (C) Jared Boone / ShareBrained,");
    console_.writeln(" Furrtek and Mayhem contributors.");
    console_.writeln("");
    console_.writeln(STR_COLOR_LIGHT_GREY "GPL-2.0-or-later. No warranty.");
}

}  // namespace app
