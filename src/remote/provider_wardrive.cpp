/*
 * mayhem-b200 — web portal panel provider for WardriveMap.
 *
 * Publishes the geotagged observations WardriveMapView has already loaded, as
 * a map panel. Nothing here reads a file or validates a position: the CSV
 * parse, the capture-metadata parse and upstream's is_geotagged() filter all
 * ran in src/apps/ui_wardrivemap.cpp, and the view only ever keeps entries that
 * passed that filter (both load_from_disk() and set_observations() apply it).
 * Every observation in the list therefore has a real fix, and every one of them
 * becomes a marker — the "row without a position" case that AIS and APRS have
 * cannot arise here.
 *
 * NO HEADING. Upstream builds its own markers with ui::invalid_angle
 * (load_markers), because a wardrive observation is a capture location, not a
 * moving target. MapMarker::heading_deg is therefore left absent rather than
 * set to 0, which would draw every capture pointing due north.
 *
 * WHAT THE MAP SHAPE CANNOT CARRY. An Observation also has a timestamp and a
 * capture frequency. The `map` panel is markers only, so those two are not on
 * the wire; the marker label is obs.name, which is exactly the tag upstream
 * puts on its own GeoMarker.
 *
 * THE DEVICE'S PAGINATOR IS NOT APPLIED. WardriveMapView plots 30 markers at a
 * time because ui::GeoMap can only hold that many; a browser has no such limit,
 * so the whole list is published. The list is bounded by what the operator's
 * own log and captures directory contain, and truncating it silently is not an
 * option the map shape can report, so it is not done.
 *
 * THREADING: a provider is only ever called from AppBridge::refresh(), i.e. on
 * the UI thread — see app_bridge.hpp's file header.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/app_context.hpp"
#include "../apps/ui_navigation.hpp"
#include "../apps/ui_wardrivemap.hpp"

#include <string>
#include <vector>

namespace remote {

/* Exposed rather than left file-local so tests/test_provider_media_geo.cpp can
 * drive it with observations the app's own set_observations() accepted. */
MapData wardrive_map_data(const std::vector<app::wardrive::Observation>& observations) {
    MapData m;
    m.markers.reserve(observations.size());
    for (const auto& obs : observations) {
        MapMarker mk;
        mk.lat = static_cast<double>(obs.latitude);
        mk.lon = static_cast<double>(obs.longitude);
        /* Upstream's own marker tag, verbatim. An observation logged without a
         * name has an empty label here exactly as it does on the device; the
         * frequency is not substituted in, because a label that reads like a
         * name and is actually a frequency is worse than no label. */
        mk.label = obs.name;
        /* heading_deg deliberately left absent — see the file header. */
        m.markers.push_back(std::move(mk));
    }
    return m;
}

namespace {

/* WardriveMapView pushes nothing of its own today, but the provider must not
 * depend on that — see provider_aprs.cpp's find_aprs_view(). */
app::WardriveMapView* find_wardrive_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::WardriveMapView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::WardriveMapView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

PanelData wardrive_panel(ui::View& view) {
    PanelData panel;

    app::WardriveMapView* app_view = find_wardrive_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "WardriveMap is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::Map;
    panel.map = wardrive_map_data(app_view->observations());
    return panel;
}

const ProviderRegistrar reg_wardrivemap{"wardrivemap", PanelKind::Map, wardrive_panel};

}  // namespace
}  // namespace remote
