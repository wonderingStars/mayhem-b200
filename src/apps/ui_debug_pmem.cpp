/*
 * mayhem-b200 — persistent-memory dump (host settings dump).
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (upstream)
 * Copyright (C) 2018 Furrtek (upstream)
 * Copyright (C) 2026 mayhem-b200 contributors (host settings dump)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_debug_pmem.hpp"

#include "app_context.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

#include <sstream>

namespace app {

std::string DebugPMemView::format_settings_dump(const core::Settings& s) {
    const auto sections = s.sections();
    if (sections.empty())
        return "(no settings stored)\n";

    std::string out;
    for (const auto& section : sections) {
        if (!section.empty())
            out += "[" + section + "]\n";
        for (const auto& key : s.keys(section))
            out += "  " + key + ": " + s.get_raw(section, key) + "\n";
    }
    return out;
}

DebugPMemView::DebugPMemView() {
    add_children({&console_, &button_done_});

    button_done_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void DebugPMemView::on_show() {
    View::on_show();
    button_done_.focus();

    console_.clear();
    console_.enable_scrolling(false);

    auto& settings = core::settings();

    console_.writeln(STR_COLOR_CYAN "Host settings dump");
    console_.writeln(STR_COLOR_LIGHT_GREY "(was LPC persistent memory)");
    console_.writeln(settings.path());
    console_.writeln("");

    /* Print the dump line by line. format_settings_dump() newline-terminates
     * each line, so split on '\n' and drop the trailing empty piece. */
    const std::string dump = format_settings_dump(settings);
    std::istringstream stream{dump};
    std::string line;
    while (std::getline(stream, line))
        console_.writeln(line);
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream: menu_location DEBUG, icon_color cyan. Genuinely functional on a host
 * (dumps the settings store), so hardware_limited stays false. */
const app::Registrar reg_debug_pmem{{"debug_pmem", "Debug PMem", app::Category::Debug,
                                     ui::Color::cyan(), &ui::bitmap_icon_debug,
                                     [] { return std::make_unique<app::DebugPMemView>(); }}};
}  // namespace
