/*
 * mayhem-b200 — web portal panel provider for FLEX RX.
 *
 * Publishes the pages FlexRxView has already decoded, as the same scrolling log
 * it draws on the 240x320 screen. Nothing here decodes anything: every line is
 * one the app wrote into its own ui::Console with flex::format_packet_line(),
 * so the browser and the device cannot disagree about a page.
 *
 * The one edit to the app is FlexRxView::console(), a const accessor in the
 * same style as AprsTableView::entries() (src/apps/ui_aprs_rx.hpp), because the
 * widget is otherwise private and a provider handed a ui::View& cannot reach
 * it. It is read-only and nothing here mutates the app.
 *
 * THREADING: a provider is only ever called from AppBridge::refresh(), i.e. on
 * the UI thread, so walking the view stack and reading the console here races
 * nothing — see app_bridge.hpp's file header.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/app_context.hpp"
#include "../apps/ui_flex_rx.hpp"
#include "../apps/ui_navigation.hpp"
#include "../ui/ui_widget.hpp"

#include <cstddef>

namespace remote {

/* Defined in provider_acars.cpp, which explains why the four text-decoder
 * providers share one reader: the 500-line cap, the newest-last order and the
 * STR_COLOR_* stripping have to be identical across the four panels. */
ConsoleData console_data_from(const ui::Console& console);

namespace {

/* The provider is handed nav->top(). FlexRxView pushes nothing of its own, but
 * the frequency field's step menu and anything the navigation bar puts up sit
 * above it, so walk down for the view that owns the console rather than letting
 * the browser go blank the moment someone touches the device. */
app::FlexRxView* find_flex_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::FlexRxView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::FlexRxView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

PanelData flex_panel(ui::View& view) {
    PanelData panel;

    app::FlexRxView* app_view = find_flex_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "FLEX RX is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::Console;
    panel.console = console_data_from(app_view->console());
    return panel;
}

const ProviderRegistrar reg_flex_rx{"flex_rx", PanelKind::Console, flex_panel};

}  // namespace
}  // namespace remote
