/*
 * mayhem-b200 — web portal panel provider for AIS RX.
 *
 * Publishes the ship list AISAppView is already keeping. Every value below is
 * read straight off an AISRecentEntry that src/apps/ais_app.cpp already filled
 * in — the HDLC framing, the FCS check and the ITU-R M.1371 field decode all
 * happened long before this file sees an entry — and it is formatted with the
 * app's own ais::format:: helpers rather than re-derived.
 *
 * The one change to the app is AISAppView::entries()/packets_valid(), const
 * accessors added so this file can read `recent_` and the decoder's own frame
 * count without the app having to know the portal exists. This file lives here
 * rather than in ais_app.cpp for the same reason: ProviderRegistrar
 * (app_bridge.hpp) self-registers at static-init time, so nothing else has to
 * know it exists.
 *
 * THE PANEL IS AN `ais` PANEL, NOT A GEOTABLE. It used to be the latter: two
 * columns of already-rendered text ("MMSI", "Name/Call") plus a marker list,
 * which is the right shape for a browser that only wants to mirror the 240x320
 * screen and the wrong one for a browser that wants to sort by speed or draw a
 * heading rose. The dedicated payload publishes the FIELDS instead — the same
 * twelve AISRecentEntry fields the device's own detail page (
 * AISRecentEntryDetailView::paint) draws, from the same entry — and lets the
 * client lay them out. Nothing about the decode changed; only the shape.
 *
 * ABSENT STAYS ABSENT, and on AIS that is most of the payload most of the time.
 * ITU-R M.1371 gives nearly every field a "not available" encoding and
 * transponders use them constantly: a vessel heard only through a position
 * report has broadcast no name, no call sign and no destination (those arrive
 * in message 5 or 24, which many units send once every six minutes), and one
 * lying at a berth commonly reports 1023 for speed and 511 for heading. A ship
 * whose position field carries the 91/181-degree sentinel is NOT at 0N 0E, one
 * that has not reported a heading is NOT pointing due north, and one that has
 * sent no position report at all has no navigational status rather than
 * "under way w/engine" (status 0). Each of those is an omitted key here. Not a
 * zero, not an empty string, not a placeholder.
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

#include <cstdint>
#include <string>
#include <vector>

namespace remote {
namespace {

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

/* One vessel, field for field the same AISRecentEntry the device's detail page
 * draws — and through the same ais::format:: helpers, so a change to either
 * formatter moves the browser and the screen together. */
AisVessel ais_vessel(const app::AISRecentEntry& e) {
    AisVessel v;

    /* Nine digits, zero padded. Never absent: an AISRecentEntry only exists
     * because a frame carrying that MMSI passed the length and FCS checks, and
     * the leading zeros are part of the identity rather than screen padding. */
    v.mmsi = ais::format::mmsi(e.mmsi);

    /* ais::format::text() strips the '@' padding AIS uses for unused six-bit
     * characters, so a message 5 with a blank name field formats to "" and the
     * key is dropped. Note this is NOT the list view's name-or-call-sign
     * fallback: that rule exists because the screen has one cell for both, and
     * a payload with two keys has no reason to hide one behind the other. A
     * transponder that padded its name field and sent a real call sign
     * publishes a callsign and no name, which is exactly what it broadcast. */
    v.name = ais::format::text(e.name);
    v.callsign = ais::format::text(e.call_sign);
    v.destination = ais::format::text(e.destination);

    const auto& pos = e.last_position;

    /* The app's own gate, not a re-invented one: Latitude/Longitude::is_valid()
     * is what ais::format::latlon() branches on before it will print
     * coordinates, and what AISRecentEntryDetailView::update_map_markers()
     * requires before it will place a marker. Both keys or neither. */
    v.pos_valid = pos.latitude.is_valid() && pos.longitude.is_valid();
    if (v.pos_valid) {
        /* The app's own conversion (ais::format::latlon_float), which is what
         * feeds the device's ui::GeoMarker. Re-deriving "1/10000 minute to
         * degrees" here would be a second implementation to keep in step. */
        v.lat = static_cast<double>(ais::format::latlon_float(pos.latitude.normalized()));
        v.lon = static_cast<double>(ais::format::latlon_float(pos.longitude.normalized()));
    }

    /* Speed over ground, tenths of a knot. 1023 is "not available" — the value
     * AISPosition is constructed with, so every vessel heard only through a
     * message 5 has it. 1022 means ">= 102.2 knots", which is a real reading
     * and needs no special case: it divides to 102.2 like any other raw. */
    if (pos.speed_over_ground != 1023)
        v.sog_kn = static_cast<double>(pos.speed_over_ground) / 10.0;

    /* Course over ground, tenths of a degree. 3600 is "not available"; above
     * that the field is out of range, and ais::format::course_over_ground()
     * prints "invalid" rather than a course. Publishing 400.0 degrees because
     * the arithmetic happens to work would be inventing a heading the app
     * itself refuses to show, so the whole >= 3600 range is absent. */
    if (pos.course_over_ground < 3600)
        v.cog_deg = static_cast<double>(pos.course_over_ground) / 10.0;

    /* True heading, whole degrees. 511 is ITU-R M.1371's "not available" and
     * the AISPosition default. The gate is >= 511 rather than the > 359 that
     * ais::format::true_heading() calls "invalid": >= 511 is what the map
     * markers this panel replaced already used, and narrowing it here would
     * silently drop headings the portal has been publishing all along. */
    if (pos.true_heading < 511) v.heading_deg = static_cast<double>(pos.true_heading);

    /* -1 is the app's "no position report has arrived yet". Status 0 is a real
     * status ("under way w/engine"), so the two must not collapse. */
    if (e.navigational_status >= 0)
        v.nav_status = static_cast<int32_t>(e.navigational_status);

    v.msgs = static_cast<uint32_t>(e.received_count);
    /* The app's own string, verbatim; empty until a frame carrying a position
     * arrives, and omitted while it is. */
    v.time = pos.timestamp;

    return v;
    /* The app's own string, verbatim; empty until a frame carrying a position
     * arrives, and omitted while it is. */
    v.time = pos.timestamp;

    return v;
}

}  // namespace

/* Exposed rather than left in the anonymous namespace above so that
 * tests/test_provider_ais_ble.cpp can drive it with entries a real
 * AISRecentEntry::update() produced from a real ais::Packet. The formatting and
 * the omission rules are the parts that have to keep agreeing with the app, and
 * that is only worth asserting against the app's own output.
 *
 * The list is emitted in container order, which ui::on_packet() keeps
 * newest-first: it moves the entry a frame just touched to the front. That is
 * the order the device's own RecentEntriesView shows, so the browser's first
 * row and the screen's first row are the same ship. */
std::vector<AisVessel> ais_vessels(const app::AISRecentEntries& entries, size_t max_vessels) {
    std::vector<AisVessel> vessels;
    size_t n = 0;
    for (const auto& e : entries) {
        if (n >= max_vessels) break;
        n++;
        vessels.push_back(ais_vessel(e));
    }
    return vessels;
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

    panel.kind = PanelKind::Ais;
    /* The same 200-entry ceiling the geotable this replaced applied (it was
     * table_data_from_entries()' max_rows default, and the marker list was
     * capped to match so a marker could never refer to a row the table did not
     * carry). Nothing reaches it today — ui::on_packet() truncates `recent_` to
     * its own default of 64 — so it is a bound on the wire, not a policy. */
    panel.ais.vessels = ais_vessels(app_view->entries(), 200);
    panel.ais.packets_valid = static_cast<uint32_t>(app_view->packets_valid());
    return panel;
}

const ProviderRegistrar reg_ais{"ais", PanelKind::Ais, ais_panel};

}  // namespace
}  // namespace remote
