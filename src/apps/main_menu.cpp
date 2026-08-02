/*
 * mayhem-b200 — main menu.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "main_menu.hpp"

#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "about_app.hpp"
#include "analog_audio_app.hpp"
#include "app_context.hpp"
#include "capture_app.hpp"
#include "device_app.hpp"
#include "spectrum_app.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

namespace app {

MainMenuView::MainMenuView() {
    add_children({&menu_, &text_hint_, &text_device_});

    auto* nav = globals().nav;

    menu_.add_items({
        {"Receive audio", ui::Color::green(),
         [nav] { nav->push_new<AnalogAudioView>(); }},
        {"Spectrum", ui::Color::cyan(),
         [nav] { nav->push_new<SpectrumAnalysisView>(); }},
        {"Capture IQ", ui::Color::orange(),
         [nav] { nav->push_new<CaptureView>(); }},
        {"Radio setup", ui::Color::yellow(),
         [nav] { nav->push_new<DeviceView>(); }},
        {"About", ui::Color::light_grey(),
         [nav] { nav->push_new<AboutView>(); }},
    });

    text_hint_.set("Arrows move  Enter select");
}

void MainMenuView::on_show() {
    View::on_show();
    menu_.focus();

    auto* r = globals().radio;
    if (r != nullptr && r->is_open()) {
        const auto& caps = r->caps();
        std::string line = caps.mboard;
        if (!caps.serial.empty()) line += " " + caps.serial;
        text_device_.set(STR_COLOR_GREEN + line);
    } else {
        text_device_.set(STR_COLOR_RED "No USRP connected");
    }
}

}  // namespace app
