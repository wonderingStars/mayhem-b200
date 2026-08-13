/*
 * mayhem-b200 — web portal panel provider for Tetra RX.
 *
 * Publishes the signalling lines TetraRxView has already decoded, as the same
 * scrolling log it draws on the 240x320 screen. Nothing here decodes anything:
 * every line is one the app wrote into its own ui::Console after its channel
 * decoder returned a CRC-OK burst.
 *
 * Only the console is published. The eight text fields above it (MCC, MNC, TS,
 * FN, BCC, ENC, LA, PDU and the burst counters) are private members of the view
 * with no accessor, and reaching them would need more than the one sanctioned
 * one-line accessor per app — so they are absent from the panel rather than
 * being guessed at from the console text.
 *
 * The one edit to the app is TetraRxView::console(), a const accessor in the
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
#include "../apps/ui_navigation.hpp"
#include "../apps/ui_tetra_rx.hpp"
#include "../ui/ui_widget.hpp"

#include <cstddef>

namespace remote {

/* Defined in provider_acars.cpp, which explains why the four text-decoder
 * providers share one reader: the 500-line cap, the newest-last order and the
 * STR_COLOR_* stripping have to be identical across the four panels. */
ConsoleData console_data_from(const ui::Console& console);

namespace {

/* The provider is handed nav->top(), which is the frequency step menu whenever
 * the local operator has it open (TetraRxView carries a FrequencyStepView), so
 * walk down for the view that owns the console rather than letting the browser
 * go blank while someone changes the step on the device. */
app::TetraRxView* find_tetra_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::TetraRxView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::TetraRxView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

PanelData tetra_panel(ui::View& view) {
    PanelData panel;

    app::TetraRxView* app_view = find_tetra_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "Tetra RX is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::Console;
    panel.console = console_data_from(app_view->console());
    return panel;
}

const ProviderRegistrar reg_tetra_rx{"tetra_rx", PanelKind::Console, tetra_panel};

}  // namespace
}  // namespace remote
