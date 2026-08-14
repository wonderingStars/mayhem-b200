/*
 * mayhem-b200 — tests for the Radiosonde RX web portal panel provider
 * (src/remote/provider_sonde.cpp).
 *
 * Every assertion below runs through the app's REAL update path. A complete,
 * CRC-valid RS41 frame is built here, scrambled the way the sonde scrambles it,
 * decoded by sonde::Packet and handed to SondeView::on_packet() — the same
 * function the decoder's own packet handler calls on the device — and only then
 * is the panel asked what it publishes. Nothing pokes the view's state directly,
 * because the point of the provider is that it agrees with what the operator's
 * screen says, and a test that set the state itself could not tell.
 *
 * Three things are under test and they fail in different places:
 *
 *   1. The cells. Each is the string the app already wrote into a ui::Text, so
 *      what the browser gets is checked against the app's own formatting rather
 *      than against a hand-written idea of it. That includes the "..." every
 *      field is constructed with, and one upstream scaling quirk that this file
 *      pins deliberately (see the voltage cell).
 *   2. The marker gate. A ui::GeoPos with no fix reads 0/0, so "no position
 *      yet" and "a sonde over the Gulf of Guinea" are the same thing on screen;
 *      the tests that matter here are the ones where NO marker may appear.
 *   3. That the provider fires at all — the app id it registers under, and the
 *      nav-stack walk that keeps the panel alive while the operator has the
 *      device's own map open on top of the view. A wrong id fails silently: the
 *      portal just keeps showing the placeholder card while every assertion
 *      about cells still passes.
 *
 * NOT covered, and not claimable: reception. No USRP is attached, so nothing
 * here proves the app ever hears a radiosonde — only that what it decoded is
 * what the portal publishes.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_context.hpp"
#include "audio_out.hpp"
#include "receiver_model.hpp"
#include "remote/app_bridge.hpp"
#include "remote/app_data.hpp"
#include "sonde_packet.hpp"
#include "string_format.hpp"
#include "ui_navigation.hpp"
#include "ui_sonde.hpp"
#include "usrp_radio.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using app::sonde::Packet;
using remote::GeoTableMarker;
using remote::TableData;

/* Defined in src/remote/provider_sonde.cpp; see the comments there for why they
 * are not in an anonymous namespace. */
namespace remote {
TableData sonde_table_data(const app::SondeView& view);
std::vector<GeoTableMarker> sonde_geo_markers(const app::SondeView& view);
}  // namespace remote

namespace {

/* Crude but sufficient: the panel bodies asserted on here are small and their
 * key order is fixed by AppBridge::panel_json(). */
bool json_has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/* --- RS41 frame construction ------------------------------------------------
 *
 * The same builders tests/test_sonde.cpp uses, rebuilt here because that file's
 * helpers are static to its own translation unit. They produce a real frame:
 * real block CRCs, the real 64-byte scrambler, and a position written as ECEF
 * centimetres by an independent closed-form WGS84 forward transform, so the
 * decoder has to invert it rather than read back what a test wrote. */

constexpr double kPi = 3.14159265358979323846;

dsp::Packet packet_from_raw_lsb_first(const std::vector<uint8_t>& raw) {
    dsp::Packet p;
    for (const uint8_t v : raw)
        for (int b = 0; b < 8; b++) p.add(((v >> b) & 1) != 0);
    return p;
}

dsp::Packet make_rs41_packet(const std::vector<uint8_t>& clear) {
    std::vector<uint8_t> raw(clear.size());
    for (size_t pos = 0; pos < clear.size(); pos++)
        raw[pos] = static_cast<uint8_t>(clear[pos] ^ Packet::vaisala_mask[(pos + 4) % 64]);
    return packet_from_raw_lsb_first(raw);
}

void put_block(std::vector<uint8_t>& frame, uint32_t start, uint8_t id,
               const std::vector<uint8_t>& data) {
    frame[start] = id;
    frame[start + 1] = static_cast<uint8_t>(data.size());
    for (size_t i = 0; i < data.size(); i++) frame[start + 2 + i] = data[i];
    const uint16_t crc = dsp::crc16_ccitt(data.data(), data.size());
    frame[start + 2 + data.size()] = static_cast<uint8_t>(crc & 0xFF);
    frame[start + 3 + data.size()] = static_cast<uint8_t>((crc >> 8) & 0xFF);
}

void put_le32(std::vector<uint8_t>& v, size_t at, int32_t value) {
    const uint32_t u = static_cast<uint32_t>(value);
    v[at + 0] = static_cast<uint8_t>(u & 0xFF);
    v[at + 1] = static_cast<uint8_t>((u >> 8) & 0xFF);
    v[at + 2] = static_cast<uint8_t>((u >> 16) & 0xFF);
    v[at + 3] = static_cast<uint8_t>((u >> 24) & 0xFF);
}

struct Ecef {
    double x, y, z;
};

Ecef geodetic_to_ecef(double lat_deg, double lon_deg, double height_m) {
    const double a = 6378137.0;
    const double b = 6356752.31424518;
    const double e2 = (a * a - b * b) / (a * a);
    const double phi = lat_deg * kPi / 180.0;
    const double lam = lon_deg * kPi / 180.0;
    const double N = a / std::sqrt(1.0 - e2 * std::sin(phi) * std::sin(phi));
    return {(N + height_m) * std::cos(phi) * std::cos(lam),
            (N + height_m) * std::cos(phi) * std::sin(lam),
            (N * (1.0 - e2) + height_m) * std::sin(phi)};
}

std::vector<uint8_t> build_rs41_frame(const std::string& serial,
                                      uint16_t frame_number,
                                      uint8_t voltage_tenths,
                                      double lat_deg, double lon_deg, double alt_m) {
    std::vector<uint8_t> frame(320, 0);

    std::vector<uint8_t> status(40, 0);
    status[0] = static_cast<uint8_t>(frame_number & 0xFF);
    status[1] = static_cast<uint8_t>((frame_number >> 8) & 0xFF);
    for (size_t i = 0; i < 8; i++)
        status[2 + i] = (i < serial.size()) ? static_cast<uint8_t>(serial[i]) : uint8_t{0x20};
    status[10] = voltage_tenths;
    status[23] = 0x03;
    put_block(frame, Packet::block_status, 0x79, status);

    const std::vector<uint8_t> meas(42, 0);
    put_block(frame, Packet::block_meas, 0x7A, meas);

    std::vector<uint8_t> gps(21, 0);
    const Ecef e = geodetic_to_ecef(lat_deg, lon_deg, alt_m);
    put_le32(gps, 0, static_cast<int32_t>(std::llround(e.x * 100.0)));
    put_le32(gps, 4, static_cast<int32_t>(std::llround(e.y * 100.0)));
    put_le32(gps, 8, static_cast<int32_t>(std::llround(e.z * 100.0)));
    put_block(frame, Packet::block_gpspos, 0x7B, gps);

    return frame;
}

Packet rs41_packet(const std::vector<uint8_t>& clear) {
    return Packet{make_rs41_packet(clear), Packet::Type::Vaisala_RS41_SG};
}

/* --- Harness ----------------------------------------------------------------
 *
 * SondeView's constructor dereferences globals().receiver, so a ReceiverModel
 * has to exist before the app registry's factory can build one. Nothing here
 * opens a device: on_show() calls receiver.start(), which fails at start_rx()
 * on a closed radio and returns false without spawning a DSP thread, so the
 * view comes up having decoded nothing — which is exactly the state the
 * "nothing heard" tests are about, and the state every other test then feeds a
 * packet into by hand. Tears the globals back down so later tests see them as
 * they were. */
struct ProviderHarness {
    radio::UsrpRadio radio{};
    audio::AudioOut audio{};
    radio::ReceiverModel receiver{radio, audio};
    ui::NavigationView nav{{0, 0, 240, 304}};

    radio::RadioDevice* saved_radio{app::globals().radio};
    radio::ReceiverModel* saved_receiver{app::globals().receiver};
    ui::NavigationView* saved_nav{app::globals().nav};

    ProviderHarness() {
        app::globals().radio = &radio;
        app::globals().receiver = &receiver;
        app::globals().nav = &nav;

        nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304})); /* root */
        nav.service();
    }

    ~ProviderHarness() {
        /* AppBridge is a process-global singleton, so a test that leaves it
         * believing radiosonde is open hands that state to every later test in
         * the binary — and to every -count= rerun. Clearing it needs the nav
         * still wired up, so it happens before the globals go back. */
        remote::AppBridge::instance().request_home();
        remote::AppBridge::instance().drain_launch_queue();
        nav.service();

        app::globals().radio = saved_radio;
        app::globals().receiver = saved_receiver;
        app::globals().nav = saved_nav;
    }

    /* Puts an app on the stack the way the portal does. */
    void launch(const std::string& app_id) {
        remote::AppBridge::instance().request_launch(app_id);
        remote::AppBridge::instance().drain_launch_queue();
        nav.service();
    }

    /* The live SondeView the registry built, or null. Every test that feeds a
     * packet goes through this: it is the same object the provider will find. */
    app::SondeView* sonde_view() {
        for (size_t i = 0; i < nav.depth(); i++)
            if (auto* v = dynamic_cast<app::SondeView*>(nav.at_depth(i))) return v;
        return nullptr;
    }
};

/* Launches the app and returns its view, or null — hoisted because every test
 * below starts this way. */
app::SondeView* open_sonde(ProviderHarness& h) {
    h.launch("radiosonde");
    return h.sonde_view();
}

}  // namespace

/* --- Columns and the empty state -------------------------------------------- */

TEST(sonde_panel_publishes_the_fields_the_screen_labels) {
    ProviderHarness h;
    app::SondeView* view = open_sonde(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;

    /* SondeView::labels_ read top to bottom (src/apps/ui_sonde.hpp), then the
     * two the ui::GeoPos block underneath contributes. */
    const TableData t = remote::sonde_table_data(*view);
    CHECK_EQ(t.columns.size(), size_t{11});
    CHECK_STR_EQ(t.columns[0], "Type");
    CHECK_STR_EQ(t.columns[1], "ID");
    CHECK_STR_EQ(t.columns[2], "Time");
    CHECK_STR_EQ(t.columns[3], "Vbatt");
    CHECK_STR_EQ(t.columns[4], "Frame");
    CHECK_STR_EQ(t.columns[5], "Temp");
    CHECK_STR_EQ(t.columns[6], "Humidity");
    CHECK_STR_EQ(t.columns[7], "Pressure");
    CHECK_STR_EQ(t.columns[8], "VSpeed");
    CHECK_STR_EQ(t.columns[9], "Alt");
    CHECK_STR_EQ(t.columns[10], "Loc");
}

TEST(sonde_panel_with_nothing_heard_yields_no_rows_and_no_markers) {
    ProviderHarness h;
    app::SondeView* view = open_sonde(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;

    CHECK_EQ(view->packets_shown(), static_cast<uint32_t>(0));

    /* A row of "..." placeholders would claim a sonde whose every field is
     * unknown, which is a different and untrue thing from having heard none. */
    const TableData t = remote::sonde_table_data(*view);
    CHECK_EQ(t.rows.size(), size_t{0});
    /* The columns still go out, so the browser draws an empty table rather than
     * nothing at all. */
    CHECK_EQ(t.columns.size(), size_t{11});

    CHECK_EQ(remote::sonde_geo_markers(*view).size(), size_t{0});
}

/* --- The row ----------------------------------------------------------------- */

TEST(sonde_panel_row_is_the_telemetry_the_app_put_on_screen) {
    ProviderHarness h;
    app::SondeView* view = open_sonde(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;

    const auto clear = build_rs41_frame("R3140571", 0x0ABC, 27, 48.858370, 2.294481, 300.0);
    const Packet p = rs41_packet(clear);
    /* The packet has to decode the way the app reads it, or the row below is
     * asserting against this test's own arithmetic. */
    CHECK(p.crc_ok());
    CHECK_STR_EQ(p.serial_number(), "R3140571");

    view->on_packet(p);
    CHECK_EQ(view->packets_shown(), static_cast<uint32_t>(1));

    const TableData t = remote::sonde_table_data(*view);
    CHECK_EQ(t.rows.size(), size_t{1});
    if (t.rows.empty()) return;
    const auto& row = t.rows[0];
    CHECK_EQ(row.size(), size_t{11});

    CHECK_STR_EQ(row[0], "Vaisala RS41-SG");
    CHECK_STR_EQ(row[1], "R3140571");
    /* The timestamp is "now" formatted in local time, so its value cannot be
     * pinned — but it must be a real one and not the placeholder. */
    CHECK(!row[2].empty());
    CHECK(row[2] != "...");
    /* 2.7 V, rendered by the app as unit_auto_scale(battery_voltage(), 2, 2).
     * battery_voltage() is in millivolts and base_unit 2 is micro, so upstream's
     * own screen reads "2.70mV" for a 2.7 V battery. That is what the operator
     * sees, so that is what goes on the wire: a provider that "corrected" it
     * would be showing the browser something the device does not. */
    CHECK_STR_EQ(row[3], "2.70mV");
    CHECK_STR_EQ(row[4], "2748"); /* 0x0ABC */
    /* No RS41 calibration subframes have arrived, so temperature, humidity and
     * pressure are genuinely unknown and the app leaves its own "..." in place.
     * VSpeed needs two packets ten seconds apart, so it is unknown too. */
    CHECK_STR_EQ(row[5], "...");
    CHECK_STR_EQ(row[6], "...");
    CHECK_STR_EQ(row[7], "...");
    CHECK_STR_EQ(row[8], "...");

    /* The altitude decoded independently of this provider... */
    CHECK_NEAR(static_cast<double>(view->fix().alt), 300.0, 2.0);
    /* ...and the cell carries it, with the unit the GeoPos block is configured
     * with, because a browser table has nowhere else to put one. */
    CHECK_STR_EQ(row[9], to_string_dec_int(static_cast<int32_t>(view->fix().alt)) + " m");
    /* The APRS table's convention: this row is the one on the map. */
    CHECK_STR_EQ(row[10], "*");
}

/* --- The marker gate --------------------------------------------------------- */

TEST(sonde_panel_plots_the_fix_the_app_accepted) {
    ProviderHarness h;
    app::SondeView* view = open_sonde(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;

    const auto clear = build_rs41_frame("R3140571", 1, 27, 48.858370, 2.294481, 300.0);
    view->on_packet(rs41_packet(clear));

    const auto markers = remote::sonde_geo_markers(*view);
    CHECK_EQ(markers.size(), size_t{1});
    if (markers.empty()) return;

    /* The position the frame's ECEF centimetres invert to, which the app
     * accepted and drew. */
    CHECK_NEAR(markers[0].lat, 48.858370, 1e-4);
    CHECK_NEAR(markers[0].lon, 2.294481, 1e-4);
    /* The row's ID cell, so a marker and its row name the same sonde — and the
     * same tag the device's own "See on map" marker carries. */
    CHECK_STR_EQ(markers[0].label, "R3140571");
    CHECK_STR_EQ(markers[0].kind, "sonde");
    /* A sonde frame carries no course. Absent, not 0 — which map.js would draw
     * as an arrow pointing due north. */
    CHECK(!markers[0].heading_deg.has_value());
}

TEST(sonde_panel_never_plots_a_sonde_at_the_origin) {
    /* THE test. A frame whose ECEF really does invert to 0N 0E — the app's own
     * gate, sonde::GPS_data::is_valid(), calls that "no fix" in as many words
     * ("a position at the origin is what an un-acquired receiver reports"), and
     * a ui::GeoPos with no fix reads exactly the same 0/0, so nothing
     * downstream could tell the two apart. Neither may reach the map. */
    ProviderHarness h;
    app::SondeView* view = open_sonde(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;

    const auto clear = build_rs41_frame("R3140571", 7, 27, 0.0, 0.0, 1000.0);
    const Packet p = rs41_packet(clear);
    /* The decode really did produce the origin — this is not a frame that
     * failed to decode. */
    const auto gps = p.get_GPS_data();
    CHECK(p.crc_ok());
    CHECK_NEAR(gps.lat, 0.0, 1e-4);
    CHECK_NEAR(gps.lon, 0.0, 1e-4);
    CHECK(!gps.is_valid());

    view->on_packet(p);

    /* The sonde is in the table — the operator is told it was heard... */
    const TableData t = remote::sonde_table_data(*view);
    CHECK_EQ(t.rows.size(), size_t{1});
    if (t.rows.empty()) return;
    CHECK_STR_EQ(t.rows[0][1], "R3140571");
    /* ...with no position claimed anywhere on the row: no '*', and an empty
     * altitude rather than the 0 a fresh GeoPos holds, which would read as a
     * sonde on the ground. */
    CHECK_STR_EQ(t.rows[0][10], "");
    CHECK_STR_EQ(t.rows[0][9], "");
    /* ...and nothing on the map. */
    CHECK_EQ(remote::sonde_geo_markers(*view).size(), size_t{0});
}

TEST(sonde_panel_publishes_no_marker_when_the_gps_block_crc_fails) {
    /* The identity and the position ride in different blocks with different
     * CRCs, so a corrupt GPS block leaves a perfectly good sonde with no
     * position at all. sonde::Packet returns a default GPS_data for it, which
     * is 0/0/0 — the same shape as "never acquired", and just as unplottable. */
    ProviderHarness h;
    app::SondeView* view = open_sonde(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;

    auto clear = build_rs41_frame("R3140571", 3, 27, 48.858370, 2.294481, 300.0);
    clear[Packet::pos_GPSecefX] ^= 0x40; /* corrupt X, leave its CRC alone */

    const Packet p = rs41_packet(clear);
    CHECK(!p.get_GPS_data().is_valid());
    view->on_packet(p);

    const TableData t = remote::sonde_table_data(*view);
    CHECK_EQ(t.rows.size(), size_t{1});
    if (t.rows.empty()) return;
    CHECK_STR_EQ(t.rows[0][1], "R3140571"); /* the identity block still decoded */
    CHECK_STR_EQ(t.rows[0][10], "");
    CHECK_EQ(remote::sonde_geo_markers(*view).size(), size_t{0});
}

TEST(sonde_panel_keeps_the_last_accepted_fix_when_a_later_packet_carries_none) {
    /* The app updates its screen position "only when valid, to prevent
     * flashing" (ui_sonde.cpp), so the device goes on showing the last good
     * fix. The panel has to agree: dropping the marker on the next packet with
     * a corrupt GPS block would have the browser and the screen disagreeing
     * about where the balloon is. */
    ProviderHarness h;
    app::SondeView* view = open_sonde(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;

    view->on_packet(rs41_packet(
        build_rs41_frame("R3140571", 1, 27, 48.858370, 2.294481, 300.0)));
    CHECK_EQ(remote::sonde_geo_markers(*view).size(), size_t{1});

    auto broken = build_rs41_frame("R3140571", 2, 27, 48.858370, 2.294481, 300.0);
    broken[Packet::pos_GPSecefX] ^= 0x40;
    view->on_packet(rs41_packet(broken));

    /* Still one row (one sonde, its fields overwritten)... */
    const TableData t = remote::sonde_table_data(*view);
    CHECK_EQ(t.rows.size(), size_t{1});
    if (t.rows.empty()) return;
    CHECK_STR_EQ(t.rows[0][4], "2"); /* the newer frame number is on screen */
    /* ...and the position the app is still showing is still on the map. */
    const auto markers = remote::sonde_geo_markers(*view);
    CHECK_EQ(markers.size(), size_t{1});
    if (markers.empty()) return;
    CHECK_NEAR(markers[0].lat, 48.858370, 1e-4);
    CHECK_STR_EQ(t.rows[0][10], "*");
}

TEST(sonde_panel_marks_a_southern_western_fix_with_the_right_signs) {
    /* A sign dropped anywhere between the ECEF inversion and the marker would
     * put a Rio sonde in the Mediterranean, and no assertion on a single
     * northern/eastern position could see it. */
    ProviderHarness h;
    app::SondeView* view = open_sonde(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;

    view->on_packet(rs41_packet(
        build_rs41_frame("R0000002", 1, 27, -22.906800, -43.172900, 50.0)));

    const auto markers = remote::sonde_geo_markers(*view);
    CHECK_EQ(markers.size(), size_t{1});
    if (markers.empty()) return;
    CHECK_NEAR(markers[0].lat, -22.906800, 1e-4);
    CHECK_NEAR(markers[0].lon, -43.172900, 1e-4);
}

/* --- The provider itself, end to end ------------------------------------------
 *
 * Everything above drives the table and marker adapters directly, which pins
 * the cells and the gate but never runs sonde_panel() — so the part that has to
 * find the app in the first place, and the app id it registers under, would be
 * unexercised. A wrong id is the single most likely way this silently does
 * nothing. These go through AppBridge end to end, because that is the only path
 * that sets the bridge's current app id and it is the path the HTTP handler
 * reads. */

TEST(sonde_panel_provider_publishes_a_geotable_when_the_app_is_open) {
    ProviderHarness h;
    /* The id has to be the one src/apps/ui_sonde.cpp registers. */
    h.launch("radiosonde");
    CHECK_EQ(h.nav.depth(), size_t{2});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(json_has(panel, "\"app_id\":\"radiosonde\""));
    CHECK(json_has(panel, "\"panel_kind\":\"geotable\""));
    CHECK(json_has(panel, "\"columns\":[\"Type\",\"ID\",\"Time\",\"Vbatt\",\"Frame\","
                          "\"Temp\",\"Humidity\",\"Pressure\",\"VSpeed\",\"Alt\",\"Loc\"]"));
    /* No device, so nothing heard: an empty rows array and an empty markers
     * array, not a fabricated row and not a marker at 0N 0E. */
    CHECK(json_has(panel, "\"rows\":[]"));
    CHECK(json_has(panel, "\"map\":{\"markers\":[]}"));
}

TEST(sonde_panel_provider_publishes_a_decoded_sonde_through_the_bridge) {
    ProviderHarness h;
    app::SondeView* view = open_sonde(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;

    view->on_packet(rs41_packet(
        build_rs41_frame("R3140571", 1, 27, 48.858370, 2.294481, 300.0)));

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    /* The whole hop, not just the adapter: serialized under the keys
     * PANELS.md's geotable contract names, and reachable at GET /api/panel. */
    CHECK(json_has(panel, "\"panel_kind\":\"geotable\""));
    CHECK(json_has(panel, "\"R3140571\""));
    CHECK(json_has(panel, "\"kind\":\"sonde\""));
    CHECK(json_has(panel, "\"lat\":48.85837"));
    CHECK(!json_has(panel, "\"markers\":[]"));
}

TEST(sonde_panel_provider_survives_the_operator_drilling_into_a_sub_view) {
    ProviderHarness h;
    h.launch("radiosonde");

    /* SondeView's "See on map" button pushes a ui::GeoMapView and keeps a
     * pointer to it, so this is not a hypothetical: the provider is handed that
     * map view, and SondeView is only reachable by walking the stack down.
     * Without at_depth() the browser would go blank for as long as the operator
     * left the device's own map open. */
    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{3});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(json_has(panel, "\"panel_kind\":\"geotable\""));
    CHECK(json_has(panel, "\"columns\":[\"Type\",\"ID\","));
}

TEST(sonde_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    ProviderHarness h;
    h.launch("radiosonde");

    /* Popped on the device rather than through request_home() — the path a
     * remote key press takes, which never goes near the launch queue. */
    h.nav.pop_to_root();
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{1});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(json_has(panel, "\"panel_kind\":\"screen\""));
    CHECK(json_has(panel, "Home -- no app is open."));
    CHECK(json_has(panel, "\"app_id\":\"\""));
    /* Emphatically not an empty geotable, which would be indistinguishable from
     * a running receiver that has heard nothing. */
    CHECK(!json_has(panel, "\"columns\""));
}
