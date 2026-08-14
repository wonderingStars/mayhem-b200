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
 * The edits to the app are EpirbRxView::entries(), a const accessor in the same
 * style as AprsTableView::entries() (src/apps/ui_aprs_rx.hpp), because the
 * container is otherwise private and a provider handed a ui::View& cannot reach
 * it; the position fields on EpirbRecentEntry that the map half needs (the
 * Beacon object the view parses into is reused by the next frame, so a provider
 * running later cannot see the one an entry came from); and
 * epirb::Location::is_valid(), which belongs with the decode rather than here —
 * the 127/255 sentinels are protocol knowledge, not portal knowledge. All of it
 * is read-only from this side and nothing here mutates the app.
 *
 * It lives here rather than in the app's own .cpp so that portal concerns stay
 * out of the ported app; ProviderRegistrar (app_bridge.hpp) is designed for
 * exactly this and self-registers at static-init time, so nothing else has to
 * know this file exists.
 *
 * THE PANEL IS A GEOTABLE, NOT A PLAIN TABLE. The table half is byte-for-byte
 * what this file published before — same single column, same cells, same order
 * — and the markers are a strict addition beside it. Putting a distress beacon
 * on a map is the point of the app; putting one at a position it never
 * transmitted would send a search to the wrong ocean, so the gate is deliberately
 * layered and every layer can only WITHHOLD a marker, never invent one:
 *
 *   1. EpirbRxView::on_frame_bits() sets EpirbRecentEntry::has_position only
 *      for a frame that passed its BCH checks AND whose epirb::Location passes
 *      Location::is_valid() — neither the 127 nor the 255 "not available"
 *      sentinel, and in range. Several protocols carry no position at all by
 *      design (every short frame; the user protocols that encode only an
 *      identity), and those never set it.
 *   2. This file refuses the exact origin as well (see epirb_geo_markers).
 *
 * A beacon with no decoded position is in the table and NOT on the map. Its row
 * still carries everything the device shows about it, Maidenhead locator
 * included, so nothing is hidden from the operator — only unplotted.
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

/* The map half. Exposed for the same reason as epirb_table_data(): the rule
 * about which beacons do and do not get a marker is the part worth asserting,
 * and on this app it is the part that can do harm.
 *
 * No conversion and no decode happens here. The degrees are the ones
 * EpirbRxView stored from epirb::Location's own fields, through the app's own
 * Angle::degrees_decimal(); re-deriving "degrees + minutes/60 + seconds/3600,
 * negated for S/W" here would be a second implementation to keep in step with
 * the one Location::to_string() and Location::maidenhead() already use.
 *
 * There is no heading: a 406 MHz frame carries a position, never a course, so
 * heading_deg is absent rather than zero — a marker drawn pointing due north
 * would be a claim the beacon is moving that way. */
std::vector<GeoTableMarker> epirb_geo_markers(const app::EpirbRecentEntries& entries,
                                              size_t max_markers) {
    std::vector<GeoTableMarker> markers;
    size_t n = 0;
    for (const auto& e : entries) {
        if (n >= max_markers) break;
        n++;

        /* The app's own gate — see the file header for what it covers. */
        if (!e.has_position) continue;

        /* And the exact origin, which is refused here rather than in the app.
         * C/S encodes "not available" as all-ones, never as zero, so a frame
         * whose latitude AND longitude both decode to exactly 0 deg 0' 0" is
         * far more likely a field this decode read as zeros than a beacon 700
         * km south of Accra. The app is left free to report it — the row still
         * shows whatever the frame said — but this is a distress position, and
         * the honest answer to an ambiguous one is to plot nothing. (The sonde
         * app's own upstream gate, sonde::GPS_data::is_valid(), makes the same
         * call in the same words: "a position at the origin is what an
         * un-acquired receiver reports".) */
        if (e.latitude == 0.0f && e.longitude == 0.0f) continue;

        GeoTableMarker mk;
        mk.lat = static_cast<double>(e.latitude);
        mk.lon = static_cast<double>(e.longitude);
        /* The entry's own key, which is the leading run of the table's Beacon
         * cell (Beacon::summary() starts with the first four hex-ID
         * characters), so a marker and its row name the same beacon. It can
         * never be absent: an entry exists only because a frame produced a
         * hex ID. */
        mk.label = e.hex_id;
        mk.kind = "beacon";
        markers.push_back(std::move(mk));
    }
    return markers;
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

    panel.kind = PanelKind::GeoTable;
    panel.geotable.table = epirb_table_data(app_view->entries());
    /* The same 200-row ceiling table_data_from_entries() applies, so a marker
     * can never refer to a row the table does not carry. */
    panel.geotable.markers = epirb_geo_markers(app_view->entries(), 200);
    return panel;
}

const ProviderRegistrar reg_epirb_rx{"epirb_rx", PanelKind::GeoTable, epirb_panel};

}  // namespace
}  // namespace remote
