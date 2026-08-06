/*
 * mayhem-b200 — web portal panel provider for EPIRB RX.
 *
 * Publishes the beacon list EpirbRxView already keeps, as the same single
 * "Beacon" column its RecentEntriesTable draws on the 240x320 screen. Nothing
 * here decodes anything: the cell is the EpirbRecentEntry::line string that
 * src/apps/ui_epirb_rx.cpp built with Beacon::summary() when the frame arrived
 * — the biphase-L demodulation, the frame parse, the BCH check and the
 * Maidenhead conversion all happened long before this file sees an entry.
 *
 * The one edit to the app is EpirbRxView::entries(), a const accessor in the
 * same style as AprsTableView::entries() (src/apps/ui_aprs_rx.hpp), because the
 * container is otherwise private and a provider handed a ui::View& cannot reach
 * it. It is read-only and nothing here mutates the app.
 *
 * It lives here rather than in the app's own .cpp so that portal concerns stay
 * out of the ported app; ProviderRegistrar (app_bridge.hpp) is designed for
 * exactly this and self-registers at static-init time, so nothing else has to
 * know this file exists.
 *
 * THREADING: a provider is only ever called from AppBridge::refresh(), i.e. on
 * the UI thread, so walking the view stack and reading the list here races
 * nothing — see app_bridge.hpp's file header.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/app_context.hpp"
#include "../apps/ui_epirb_rx.hpp"
#include "../apps/ui_navigation.hpp"

#include <string>
#include <vector>

namespace remote {
namespace {

/* EpirbRxView's own columns_ initializer, repeated (src/apps/ui_epirb_rx.hpp).
 * It cannot be read off the running view — the RecentEntriesColumns object is a
 * private member — so the list is duplicated here and pinned by a test, rather
 * than being free to drift. The declared width of zero is the app's "take
 * whatever is left of the line" convention, a layout decision TableData does
 * not carry. */
ui::RecentEntriesColumns epirb_columns() {
    return ui::RecentEntriesColumns{{"Beacon", 0}};
}

/* Beacon::summary() embeds the display colour of the OK/KO verdict as a
 * STR_COLOR_* escape (src/ui/ui.hpp: 0x1B followed by one palette byte), which
 * the device's Painter consumes as an attribute rather than drawing. On the
 * wire those bytes are neither text nor layout — the table panel renders cells
 * as plain strings, and JSON would carry them through as escaped control
 * characters — so the pairs come out here.
 *
 * What that costs is only the colour: "OK"/"KO" itself is text and survives, so
 * a failed frame is still legible as failed. The one distinction that does not
 * reach the browser is green (clean) versus yellow (accepted after a
 * single-bit BCH correction), which the app encodes in the colour alone and for
 * which its table has no cell. */
std::string strip_color_escapes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\x1B') {
            i++;  /* also drop the palette byte that follows */
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

/* One row, agreeing cell-for-cell with the line EpirbRxView's on_draw paints.
 *
 * That renderer draws entry.line and then pads it out to the width of the
 * table widget; the padding is the fixed-width screen's alignment rather than
 * part of any value, and a browser table lays its own cells out, so it is not
 * reproduced. Everything else is verbatim.
 *
 * A beacon whose frame produced no summary yet has an empty line, and that is
 * published as an empty cell — never as a stand-in that reads like a decode. */
std::vector<std::string> epirb_row(const app::EpirbRecentEntry& e) {
    return {strip_color_escapes(e.line)};
}

/* The provider is handed nav->top(). EpirbRxView pushes nothing of its own
 * today, but the frequency field's step menu and anything the navigation bar
 * puts up sit above it, so walk down for the view that owns the list rather
 * than letting the browser go blank the moment someone touches the device. */
app::EpirbRxView* find_epirb_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::EpirbRxView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::EpirbRxView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

}  // namespace

/* Exposed rather than left in the anonymous namespace above so that
 * tests/test_provider_epirb_ert.cpp can drive it with entries a real
 * epirb::Beacon produced. The formatting is the part that has to keep agreeing
 * with the device screen, and that is only worth asserting against the app's
 * own output. */
TableData epirb_table_data(const app::EpirbRecentEntries& entries) {
    return table_data_from_entries(epirb_columns(), entries, epirb_row);
}

namespace {

PanelData epirb_panel(ui::View& view) {
    PanelData panel;

    app::EpirbRxView* app_view = find_epirb_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "EPIRB RX is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::Table;
    panel.table = epirb_table_data(app_view->entries());
    return panel;
}

const ProviderRegistrar reg_epirb_rx{"epirb_rx", epirb_panel};

}  // namespace
}  // namespace remote
