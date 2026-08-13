/*
 * mayhem-b200 — web portal panel provider for AIS RX.
 *
 * Publishes the ship list AISAppView is already keeping as a generic table
 * panel: the same two columns its RecentEntriesView shows on the 240x320
 * screen, in the same order, with the same per-cell formatting. Every value
 * below is read straight off an AISRecentEntry that src/apps/ais_app.cpp
 * already filled in — the HDLC framing, the FCS check and the ITU-R M.1371
 * field decode all happened long before this file sees an entry — and it is
 * formatted with the app's own ais::format:: helpers rather than re-derived.
 *
 * The one change to the app is AISAppView::entries(), a const accessor added so
 * this file can read `recent_` without the app having to know the portal
 * exists. It lives here rather than in ais_app.cpp for the same reason:
 * ProviderRegistrar (app_bridge.hpp) self-registers at static-init time, so
 * nothing else has to know this file exists.
 *
 * ONLY the ship list is published. The per-ship detail page
 * (AISRecentEntryDetailView) draws its twelve fields straight to a Painter and
 * keeps no structured form of them, but every one of those fields comes from
 * the same AISRecentEntry, so a richer panel would be a bigger row rather than
 * a second data source. The screen's own list is what the columns follow, and
 * that is what is here.
 *
 * THE PANEL IS A GEOTABLE, NOT A PLAIN TABLE. The table half is byte-for-byte
 * what this file published before — same columns, same cells, same order — and
 * the markers are a strict addition beside it. A ship earns a marker ONLY when
 * its stored position passes the app's own validity gate
 * (Latitude/Longitude::is_valid(), which is what ais::format::latlon() branches
 * on before it will print coordinates). A vessel heard only through a message
 * that carries no position, or one broadcasting the 91/181-degree
 * "not available" sentinel, appears in the table and NOT on the map. It is not
 * at 0N 0E.
 *
 * THREADING: a provider is only ever called from AppBridge::refresh(), i.e.
 * on the UI thread, so walking the view tree and reading the entries list here
 * races nothing — see app_bridge.hpp's file header.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/ais_app.hpp"
#include "../apps/app_context.hpp"
#include "../apps/ui_navigation.hpp"

#include <string>
#include <vector>

namespace remote {
namespace {

/* AISAppView's own columns_ initializer, repeated (src/apps/ais_app.hpp).
 * It cannot be read off the running view — RecentEntriesView resolves the
 * zero-width column against the screen width and then only its private header
 * and table hold the object — so the list is duplicated here and pinned by a
 * test, rather than being free to drift. The widths are the app's declared
 * ones; resolving "Name/Call" to whatever is left on a 240-pixel line is a
 * layout decision TableData does not carry. */
ui::RecentEntriesColumns ais_columns() {
    return ui::RecentEntriesColumns{{"MMSI", 9}, {"Name/Call", 0}};
}

/* One row, laid out to agree cell-for-cell with the line AISAppView's own
 * on_draw paints (ais_app.cpp, recent_entries_view_.table().on_draw).
 *
 * One piece of that line is deliberately not copied, because it is the
 * fixed-width screen's alignment rather than part of any value: the whole line
 * is padded out to the pixel width of the row. A browser table lays its own
 * cells out and would only have to strip it again.
 *
 * Anything a ship has not sent stays empty. A vessel heard only through a
 * position report has broadcast neither its name nor its call sign — those
 * arrive in message 5 or 24, which many transponders send once every six
 * minutes — and its Name/Call cell is blank here exactly as it is on the
 * device. The MMSI is the entry's own key, so it is never absent: an
 * AISRecentEntry only exists because a frame carrying that MMSI passed the
 * length and FCS checks. */
std::vector<std::string> ais_row(const app::AISRecentEntry& e) {
    return {
        ais::format::mmsi(e.mmsi),
        /* The app's own choice, tested on the RAW name and reproduced exactly:
         * a vessel that has sent a message 5 whose name field is all '@'
         * padding has a non-empty `name` that ais::format::text() strips to
         * nothing, and the device shows an empty cell rather than falling back
         * to the call sign. Testing the formatted name instead would put a
         * call sign in the browser that the screen does not show. */
        e.name.empty() ? ais::format::text(e.call_sign) : ais::format::text(e.name),
    };
}

/* The provider is handed nav->top(), which is the GeoMapView whenever the
 * operator has opened a ship's map from the detail page (the detail view
 * itself is a child widget of AISAppView and does not touch the nav stack, but
 * its "See on map" button pushes one). AISAppView stays on the stack
 * underneath it, so walk down for it rather than letting the browser's view go
 * blank the moment someone touches the device. */
app::AISAppView* find_ais_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::AISAppView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::AISAppView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

}  // namespace

/* Exposed rather than left in the anonymous namespace above so that
 * tests/test_provider_ais_ble.cpp can drive it with entries a real
 * AISRecentEntry::update() produced from a real ais::Packet. The formatting is
 * the part that has to keep agreeing with the device screen, and that is only
 * worth asserting against the app's own output. */
TableData ais_table_data(const app::AISRecentEntries& entries) {
    return table_data_from_entries(ais_columns(), entries, ais_row);
}

/* The map half. Exposed for the same reason as ais_table_data(): the rule about
 * which ships do and do not get a marker is the part worth asserting, and it is
 * only worth asserting against entries a real AISRecentEntry::update() built.
 *
 * Nothing here decodes or re-derives a position. The degrees conversion is the
 * app's own ais::format::latlon_float(), which is what AISRecentEntryDetailView
 * feeds its ui::GeoMarker (ais_app.cpp) — re-deriving "1/10000 minute to
 * degrees" here would be a second implementation to keep in step. */
std::vector<GeoTableMarker> ais_geo_markers(const app::AISRecentEntries& entries,
                                            size_t max_markers) {
    std::vector<GeoTableMarker> markers;
    size_t n = 0;
    for (const auto& e : entries) {
        if (n >= max_markers) break;
        n++;

        const auto& pos = e.last_position;
        /* The app's own gate, not a re-invented one. */
        if (!pos.latitude.is_valid() || !pos.longitude.is_valid()) continue;

        GeoTableMarker mk;
        mk.lat = static_cast<double>(ais::format::latlon_float(pos.latitude.normalized()));
        mk.lon = static_cast<double>(ais::format::latlon_float(pos.longitude.normalized()));

        /* The marker has to be identifiable against its own table row, so the
         * label is that row's Name/Call cell — already '@'-padding-stripped by
         * ais::format::text() — falling back to the MMSI, which is the entry's
         * key and can never be absent. (AISRecentEntryDetailView's own GeoMarker
         * tag uses the RAW call sign, which for a transponder padding the field
         * with '@' would put "@@@@@@@" on the map.) */
        const auto row = ais_row(e);
        mk.label = row[1].empty() ? row[0] : row[1];

        /* 511 is ITU-R M.1371's "not available" for true heading, and it is the
         * default AISPosition is constructed with; anything at or above it means
         * the ship has not reported one. Absent, not zero: a marker drawn on a
         * heading of 000 is a claim the vessel is pointing due north. */
        if (pos.true_heading < 511) mk.heading_deg = static_cast<double>(pos.true_heading);

        mk.kind = "vessel";
        markers.push_back(std::move(mk));
    }
    return markers;
}

namespace {

PanelData ais_panel(ui::View& view) {
    PanelData panel;

    app::AISAppView* app_view = find_ais_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "AIS RX is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::GeoTable;
    panel.geotable.table = ais_table_data(app_view->entries());
    /* The same 200-row ceiling table_data_from_entries() applies, so a marker
     * can never refer to a row the table does not carry. */
    panel.geotable.markers = ais_geo_markers(app_view->entries(), 200);
    return panel;
}

const ProviderRegistrar reg_ais{"ais", ais_panel};

}  // namespace
}  // namespace remote
