/*
 * mayhem-b200 — web portal panel provider for Radiosonde RX.
 *
 * Publishes the one thing this app tracks — the sonde it is currently hearing —
 * as a geo-table: a single row of the telemetry SondeView draws on the 240x320
 * screen, and a marker for it once the sonde has a fix.
 *
 * Nothing here decodes, converts or re-formats anything. The RS41 descramble,
 * the block CRCs, the ECEF->WGS84 position and every field extraction happened
 * in sonde::Packet long before this file sees the view, and each cell below is
 * the exact string src/apps/ui_sonde.cpp already wrote into the ui::Text widget
 * the operator is looking at — including the "..." placeholder each field is
 * constructed with, which is this app's own way of saying a value has not
 * arrived. Re-deriving "2.7V" or "-56.3\xB0C" here would be a second copy of the
 * app's formatting to keep in step with the first.
 *
 * The edits to the app are the const accessors on SondeView (ui_sonde.hpp), in
 * the same style as AprsTableView::entries(), plus the two pieces of state they
 * expose that the app was not keeping: a count of the packets it has actually
 * displayed, and the last GPS fix it ACCEPTED. It stays out of the app's own
 * .cpp so portal concerns stay out of the ported app; ProviderRegistrar
 * (app_bridge.hpp) self-registers at static-init time, so nothing else has to
 * know this file exists.
 *
 * THE MARKER GATE. This app's screen is a ui::GeoPos, and a ui::GeoPos with no
 * fix reads 0 deg 0' 0" on both axes — indistinguishable from a sonde over the
 * Gulf of Guinea, and exactly the fabricated position a map must never show. So
 * the position published here is not read off that widget at all: it is
 * SondeView::fix(), which on_packet() writes ONLY inside its own
 * gps_info_.is_valid() branch — the same branch that moves the device's own map
 * marker — and which is default-constructed until then. sonde::GPS_data::
 * is_valid() is upstream's test, and it reads the origin as "no fix" in as many
 * words: "a position at the origin is what an un-acquired receiver reports".
 * Applying it again here, to the value actually being published, is what makes
 * 0N 0E impossible rather than merely unlikely.
 *
 * A sonde with no fix is in the table and NOT on the map: its Loc cell is blank
 * and its Alt cell is empty, in the same way an APRS station heard through a
 * status packet has a blank Loc column (provider_aprs.cpp).
 *
 * THREADING: a provider is only ever called from AppBridge::refresh(), i.e. on
 * the UI thread, so walking the view stack and reading the view's widgets here
 * races nothing — see app_bridge.hpp's file header.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/app_context.hpp"
#include "../apps/ui_navigation.hpp"
#include "../apps/ui_sonde.hpp"
#include "../core/string_format.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace remote {
namespace {

/* SondeView has no RecentEntriesTable and therefore no columns_ initializer to
 * repeat: it tracks one sonde, not a list, and draws it as a column of labelled
 * fields. The column names and their order are that label column read top to
 * bottom (SondeView::labels_, src/apps/ui_sonde.hpp) — Type, ID, Time, Vbatt,
 * Frame, Temp, Humidity, Pressure, VSpeed — followed by the two the ui::GeoPos
 * block underneath contributes.
 *
 * The widths are declared for symmetry with the other providers and are dropped
 * by table_data_from_entries(); a browser lays its own cells out. The list is
 * pinned by a test rather than being free to drift. */
ui::RecentEntriesColumns sonde_columns() {
    return ui::RecentEntriesColumns{{"Type", 0},   {"ID", 9},       {"Time", 10},
                                    {"Vbatt", 6},  {"Frame", 6},    {"Temp", 7},
                                    {"Humidity", 8}, {"Pressure", 9}, {"VSpeed", 8},
                                    {"Alt", 8},    {"Loc", 4}};
}

/* The altitude cell, and the one value on the row that is NOT copied verbatim
 * off a ui::Text: the screen shows it in the GeoPos block's NumberField with
 * the unit in a separate widget beside it, and a browser table has nowhere to
 * put a separate unit. So the number is joined to the unit that widget is
 * configured with (ui::GeoPos::alt_unit::METERS, ui_sonde.hpp).
 *
 * It is empty, never "0", when there is no fix. The altitude arrives in the
 * same GPS block as the position and is only accepted with it, so publishing
 * the zero a fresh GeoPos holds would read as a sonde on the ground.
 *
 * The cast is the one SondeView itself makes when it hands the altitude to
 * geopos_.set_altitude(): GPS_data::alt is unsigned, and a sonde below the
 * ellipsoid decodes as a negative altitude that has already wrapped. Sending
 * the wrapped unsigned value would put a balloon 4294 km up. (The device then
 * clamps it again to the NumberField's own -1000..50000 range, which is that
 * widget's limit and not a property of the value.) */
std::string sonde_altitude_cell(const app::sonde::GPS_data& fix) {
    if (!fix.is_valid()) return {};
    return to_string_dec_int(static_cast<int32_t>(fix.alt)) + " m";
}

}  // namespace

/* Exposed rather than left in the anonymous namespace so that
 * tests/test_provider_sonde.cpp can drive it with a view a real sonde::Packet
 * has been fed through SondeView::on_packet(). The cells are what has to keep
 * agreeing with the device screen, and that is only worth asserting against the
 * app's own output rather than against a hand-written copy of it. */
TableData sonde_table_data(const app::SondeView& view) {
    TableData t;
    for (const auto& c : sonde_columns()) t.columns.push_back(c.first);

    /* No packet has been displayed, so every field on screen is still its "..."
     * placeholder. An empty table says "nothing heard"; a row of placeholders
     * would say "a sonde, all of whose fields are unknown", which is a
     * different and untrue claim. */
    if (view.packets_shown() == 0) return t;

    const auto& fix = view.fix();
    t.rows.push_back({
        view.type_text(),
        view.id_text(),
        view.time_text(),
        view.voltage_text(),
        view.frame_text(),
        view.temperature_text(),
        view.humidity_text(),
        view.pressure_text(),
        view.vspeed_text(),
        sonde_altitude_cell(fix),
        /* Upstream draws no such column; this is the APRS table's own
         * convention (provider_aprs.cpp), a '*' when the row has a position, so
         * the table half can say on its own which of its rows is the one on the
         * map. */
        fix.is_valid() ? std::string{"*"} : std::string{},
    });
    return t;
}

/* The map half. Exposed for the same reason as sonde_table_data(): the rule
 * about when a marker exists at all is the part worth asserting.
 *
 * At most one marker — the app tracks one sonde, overwriting its fields as
 * packets arrive — so there is no cap to apply and no way for a marker to refer
 * to a row the table does not carry. */
std::vector<GeoTableMarker> sonde_geo_markers(const app::SondeView& view) {
    std::vector<GeoTableMarker> markers;

    const auto& fix = view.fix();
    /* The app's own gate, applied to the value being published. See the file
     * header: this is what makes a marker at 0N 0E impossible. */
    if (!fix.is_valid()) return markers;

    GeoTableMarker mk;
    mk.lat = static_cast<double>(fix.lat);
    mk.lon = static_cast<double>(fix.lon);
    /* The row's ID cell, so a marker and its row name the same sonde, falling
     * back to the Type cell for a frame whose serial did not decode — the
     * device's own map marker is tagged with that same id (button_map_'s
     * handler passes sonde_id_). */
    mk.label = view.id_text().empty() ? view.type_text() : view.id_text();
    /* No heading: a sonde frame carries a position and a vertical speed, never
     * a course, and the app's own "See on map" button passes an out-of-range
     * angle for exactly that reason (ui_sonde.cpp) so the device draws a cross
     * rather than a bearing arrow. Absent, not zero: a 0 would read as "drifting
     * due north". */
    mk.kind = "sonde";
    markers.push_back(std::move(mk));
    return markers;
}

namespace {

/* The provider is handed nav->top(), which is the GeoMapView whenever the
 * operator has pressed "See on map" — SondeView pushes one and keeps a pointer
 * to it (ui_sonde.cpp). SondeView stays on the stack underneath, so walk down
 * for it rather than letting the browser's view go blank for as long as the map
 * is open on the device. */
app::SondeView* find_sonde_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::SondeView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::SondeView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

PanelData sonde_panel(ui::View& view) {
    PanelData panel;

    app::SondeView* app_view = find_sonde_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "Radiosonde RX is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::GeoTable;
    panel.geotable.table = sonde_table_data(*app_view);
    panel.geotable.markers = sonde_geo_markers(*app_view);
    return panel;
}

const ProviderRegistrar reg_radiosonde{"radiosonde", PanelKind::GeoTable, sonde_panel};

}  // namespace
}  // namespace remote
