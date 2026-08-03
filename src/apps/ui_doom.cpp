/*
 * mayhem-b200 — DOOM (Games), app shell only.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_doom.hpp"

#include "app_context.hpp"
#include "string_format.hpp"
#include "ui_navigation.hpp"

namespace app {

DoomView::DoomView() {
    add_children({&console_, &button_recheck_, &button_back_});

    button_recheck_.on_select = [this](ui::Button&) { refresh(); };
    button_back_.on_select = [this](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void DoomView::focus() {
    button_back_.focus();
}

void DoomView::on_show() {
    View::on_show();
    refresh();
}

void DoomView::refresh() {
    console_.clear(true);
    console_.enable_scrolling(false);

    console_.writeln(STR_COLOR_CYAN "DOOM  -  app shell");
    console_.writeln("");
    console_.writeln("No playable engine is wired");
    console_.writeln("up in this host port.");
    console_.writeln("");
    console_.writeln(STR_COLOR_YELLOW "The upstream Doom app");
    console_.writeln("is a self-contained raycaster");
    console_.writeln("(not id's DOOM, no WAD). Its");
    console_.writeln("game loop is not ported.");
    console_.writeln("");
    console_.writeln(STR_COLOR_YELLOW "IWAD for a real DOOM");

    const auto status = doom_wad::find_wad(doom_wad::wad_directory());
    console_.writeln(std::string{STR_COLOR_LIGHT_GREY} + status.path);

    if (status.present) {
        console_.writeln(std::string{STR_COLOR_GREEN} + "Found, " +
                         to_string_dec_uint(static_cast<uint64_t>(status.size)) +
                         " bytes");
        console_.writeln(status.looks_valid
                             ? STR_COLOR_GREEN "WAD header OK (not loaded)"
                             : STR_COLOR_YELLOW "Header not IWAD/PWAD");
    } else {
        console_.writeln(STR_COLOR_RED "Not found - drop a WAD there.");
        console_.writeln(STR_COLOR_LIGHT_GREY "Engine not implemented.");
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_doom{{
    "doom",
    "Doom",
    app::Category::Games,
    ui::Color::green(),  /* upstream icon_color */
    &ui::bitmap_icon_games,
    [] { return std::make_unique<app::DoomView>(); },
    false  /* not PortaPack-hardware-limited; shell only, states so on screen */
}};
}  // namespace
