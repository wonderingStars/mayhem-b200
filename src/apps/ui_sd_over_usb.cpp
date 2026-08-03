/*
 * mayhem-b200 — SD over USB (hardware-limited N/A screen).
 *
 * See ui_sd_over_usb.hpp for why this is not functional on a B200 host.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_sd_over_usb.hpp"

#include "app_context.hpp"
#include "ui_navigation.hpp"

namespace app {

SdUsbStartResult SdOverUsbView::attempt_start() {
    /* No SD card exists on a B200 host, and there is no on-board processor to
     * present one as USB mass storage. The upstream behaviour has no host
     * analogue, so we start nothing and say so. */
    return {false, "No SD card to expose on a B200."};
}

SdOverUsbView::SdOverUsbView() {
    add_children({&console_, &button_run_, &button_back_});

    button_run_.on_select = [this](ui::Button&) {
        /* Mirrors the upstream "Run" button, but it is a deliberate no-op: the
         * decision below never performs any action. */
        const auto result = attempt_start();
        console_.writeln("");
        console_.writeln(STR_COLOR_RED "Unavailable:");
        console_.writeln(STR_COLOR_RED + (" " + result.message));
    };

    button_back_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void SdOverUsbView::on_show() {
    View::on_show();
    button_back_.focus();
    render();
}

void SdOverUsbView::render() {
    console_.clear();
    console_.enable_scrolling(false);

    console_.writeln(STR_COLOR_YELLOW "SD over USB");
    console_.writeln("");
    console_.writeln("On a PortaPack this rebooted");
    console_.writeln("the device into USB mass-");
    console_.writeln("storage mode so a PC could");
    console_.writeln("read its microSD card as a");
    console_.writeln("removable drive.");
    console_.writeln("");
    console_.writeln(STR_COLOR_CYAN "Not applicable on a B200:");
    console_.writeln(" - it has no SD card,");
    console_.writeln(" - it is itself a USB device,");
    console_.writeln(" - no on-board CPU to bridge.");
    console_.writeln("");
    console_.writeln("The host PC's own filesystem");
    console_.writeln("is already directly available");
    console_.writeln("to every app (see File Mgr).");
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Utilities, yellow — matches upstream sdusb's app_location_t::UTILITIES and
 * ui::Color::yellow() icon_color. hardware_limited: this is a PortaPack-only
 * feature with no host equivalent. */
const app::Registrar reg_sdusb{{"sdusb", "SD over USB", app::Category::Utilities,
                                ui::Color::yellow(), &ui::bitmap_icon_dir,
                                [] { return std::make_unique<app::SdOverUsbView>(); },
                                true}};
}  // namespace
