/*
 * mayhem-b200 — web portal panel provider for APRS RX.
 *
 * Publishes the app's Stations tab as a generic table panel: the same four
 * columns AprsTableView shows on the 240x320 screen, in the same order, with
 * the same per-cell formatting. Every value is read straight off an
 * AprsRecentEntry that src/apps/ui_aprs_rx.cpp already filled in — the AX.25
 * framing, the address parsing and the position decode all happened long
 * before this file sees an entry — and the app is not modified at all:
 * AprsTableView::entries() is already public because AprsRxView's own tests
 * and its details view need it.
 *
 * It lives here rather than in the app's own .cpp so that portal concerns stay
 * out of the ported app; ProviderRegistrar (app_bridge.hpp) is designed for
 * exactly this and self-registers at static-init time, so nothing else has to
 * know this file exists.
 *
 * ONLY the Stations tab is published. The Stream tab's raw-packet console is a
 * private member of AprsStreamView with no accessor, so there is nothing to
 * read without editing the app; a console panel for it would need that
 * accessor to exist first.
 *
 * THE PANEL IS A GEOTABLE, NOT A PLAIN TABLE. The table half is byte-for-byte
 * what this file published before — same four columns, same cells, same order —
 * and the markers are a strict addition beside it. A station earns a marker
 * ONLY when AprsRecentEntry::has_position is set, which is the same flag the
 * table's own "Loc" column shows a '*' for. A station heard only through a
 * status or telemetry packet is in the table and NOT on the map; it is
 * emphatically not at 0N 0E.
 *
 * THREADING: a provider is only ever called from AppBridge::refresh(), i.e.
 * on the UI thread, so walking the view tree and reading the entries list here
 * races nothing — see app_bridge.hpp's file header.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/app_context.hpp"
#include "../apps/ui_aprs_rx.hpp"
#include "../apps/ui_navigation.hpp"
#include "../core/string_format.hpp"

#include <string>
#include <vector>

namespace remote {
namespace {

/* AprsTableView's own columns_ initializer, repeated (src/apps/ui_aprs_rx.hpp).
 * It cannot be read off the running view — the RecentEntriesColumns object is
 * held only by private members of the table and its header — so the list is
 * duplicated here and pinned by a test, rather than being free to drift.
 * The widths are the app's declared ones; RecentEntriesView resolves the
 * zero-width Source column to whatever is left on screen, which is a layout
 * decision TableData does not carry. */
ui::RecentEntriesColumns aprs_columns() {
    return ui::RecentEntriesColumns{{"Source", 0}, {"Loc", 6}, {"Hits", 4}, {"Time", 8}};
}

/* One row, laid out to agree cell-for-cell with the line AprsTableView's own
 * on_draw paints.
 *
 * Two pieces of that line are deliberately not copied, because they are the
 * fixed-width screen's alignment rather than part of any value: the source is
 * padded out to the column width, and the hit count is right-justified in four
 * characters. A browser table lays its own cells out and would only have to
 * strip both again. The "999+" ceiling is not alignment — it is a real limit
 * on what the app will show — so it stays.
 *
 * Anything a station has not sent stays empty. A station heard only through a
 * status or telemetry packet has no position, and its Loc cell is blank here
 * exactly as it is on the device; it is emphatically not at 0N 0E. */
std::vector<std::string> aprs_row(const app::AprsRecentEntry& e) {
    return {
        e.source_formatted,
        /* Upstream draws bitmap_target in this column and the host draws a
         * green '*' there (the host bitmap set has no such icon); '*' is what
         * the operator actually sees, so it is what goes on the wire. */
        e.has_position ? std::string{"*"} : std::string{},
        (e.hits <= 999) ? to_string_dec_uint(e.hits) : std::string{"999+"},
        e.time_string,
    };
}

/* The provider is handed nav->top(), which is the GeoMapView whenever the
 * operator has opened a station's map from the details page. AprsRxView stays
 * on the stack underneath it, so walk down for it rather than letting the
 * browser's view go blank the moment someone touches the device. */
app::AprsRxView* find_aprs_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::AprsRxView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::AprsRxView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

/* The stations table is the Stations tab's content view — a child widget of
 * AprsRxView, not anything the navigation stack knows about — so it is reached
 * through Widget::children(). The search is recursive because which level it
 * sits at is AprsRxView's business rather than this file's. */
app::AprsTableView* find_table_view(ui::Widget& widget) {
    for (auto* child : widget.children()) {
        if (child == nullptr) continue;
        if (auto* table = dynamic_cast<app::AprsTableView*>(child)) return table;
        if (auto* found = find_table_view(*child)) return found;
    }
    return nullptr;
}

}  // namespace

/* Exposed rather than left in the anonymous namespace above so that
 * tests/test_remote_api.cpp can drive it with entries a real
 * AprsTableView::on_packet() produced. The formatting is the part that has to
 * keep agreeing with the device screen, and that is only worth asserting
 * against the app's own output. */
TableData aprs_table_data(const app::AprsRecentEntries& entries) {
    return table_data_from_entries(aprs_columns(), entries, aprs_row);
}

/* The map half. Exposed for the same reason as aprs_table_data(): the rule
 * about which stations do and do not get a marker is the part worth asserting.
 *
 * No conversion happens here. AprsPosition already holds decimal degrees the
 * app's own AX.25 position decode produced, and there is no heading field on an
 * APRS entry at all — a course in a position report is not parsed into the
 * entry — so heading_deg is absent rather than zero. */
std::vector<GeoTableMarker> aprs_geo_markers(const app::AprsRecentEntries& entries,
                                             size_t max_markers) {
    std::vector<GeoTableMarker> markers;
    size_t n = 0;
    for (const auto& e : entries) {
        if (n >= max_markers) break;
        n++;

        /* The app's own flag, the same one the Loc column reads. */
        if (!e.has_position) continue;

        GeoTableMarker mk;
        mk.lat = static_cast<double>(e.pos.latitude);
        mk.lon = static_cast<double>(e.pos.longitude);
        /* The table's Source cell, so a marker and its row name the same
         * station. */
        mk.label = e.source_formatted;
        mk.kind = "station";
        markers.push_back(std::move(mk));
    }
    return markers;
}

namespace {

PanelData aprs_panel(ui::View& view) {
    PanelData panel;

    app::AprsRxView* app_view = find_aprs_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "APRS RX is not the open app.";
        return panel;
    }

    app::AprsTableView* table_view = find_table_view(*app_view);
    if (table_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "APRS RX has no stations table.";
        return panel;
    }

    panel.kind = PanelKind::GeoTable;
    panel.geotable.table = aprs_table_data(table_view->entries());
    /* The same 200-row ceiling table_data_from_entries() applies, so a marker
     * can never refer to a row the table does not carry. */
    panel.geotable.markers = aprs_geo_markers(table_view->entries(), 200);
    return panel;
}

const ProviderRegistrar reg_aprs_rx{"aprsrx", PanelKind::GeoTable, aprs_panel};

}  // namespace
}  // namespace remote
