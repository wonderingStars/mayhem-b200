/*
 * mayhem-b200 — tests for the web portal's JSON serializer, generic table
 * adapter and thread-safe launch queue (src/remote/).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_context.hpp"
#include "app_registry.hpp"
#include "remote/app_bridge.hpp"
#include "remote/app_data.hpp"
#include "ui.hpp"
#include "ui_navigation.hpp"
#include "ui_recent_entries.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using remote::JsonValue;
using remote::PanelKind;
using remote::TableData;

/* --- JsonValue: scalars, numbers, escaping -------------------------------- */

TEST(json_scalars_serialize) {
    CHECK_STR_EQ(JsonValue::null().dump(), "null");
    CHECK_STR_EQ(JsonValue::boolean(true).dump(), "true");
    CHECK_STR_EQ(JsonValue::boolean(false).dump(), "false");
    CHECK_STR_EQ(JsonValue::integer(42).dump(), "42");
    CHECK_STR_EQ(JsonValue::integer(-7).dump(), "-7");
    CHECK_STR_EQ(JsonValue::string("hi").dump(), "\"hi\"");
}

TEST(json_numbers_format_without_scientific_notation) {
    CHECK_STR_EQ(remote::format_number(0.0), "0");
    /* 1e8 would print as "1e+08" under a naive %g; frequencies must not. */
    CHECK_STR_EQ(remote::format_number(100000000.0), "100000000");
    /* A realistic B200 tuning frequency. */
    CHECK_STR_EQ(remote::format_number(2412000000.0), "2412000000");
    CHECK_STR_EQ(remote::format_number(-3.5), "-3.5");
    CHECK_STR_EQ(remote::format_number(1.100000), "1.1");
    CHECK_STR_EQ(remote::format_number(1.0), "1");
}

TEST(json_string_escaping) {
    CHECK_STR_EQ(JsonValue::string("a\"b").dump(), "\"a\\\"b\"");
    CHECK_STR_EQ(JsonValue::string("a\\b").dump(), "\"a\\\\b\"");
    CHECK_STR_EQ(JsonValue::string("a\nb").dump(), "\"a\\nb\"");
    CHECK_STR_EQ(JsonValue::string("a\tb").dump(), "\"a\\tb\"");

    /* A control character with no named escape falls back to \u00XX. */
    std::string ctrl;
    ctrl.push_back('a');
    ctrl.push_back('\x01');
    ctrl.push_back('b');
    CHECK_STR_EQ(JsonValue::string(ctrl).dump(), "\"a\\u0001b\"");
}

TEST(json_empty_containers) {
    CHECK_STR_EQ(JsonValue::array().dump(), "[]");
    CHECK_STR_EQ(JsonValue::object().dump(), "{}");
}

TEST(json_array_and_object_nesting) {
    JsonValue arr = JsonValue::array();
    arr.push_back(JsonValue::integer(1));
    arr.push_back(JsonValue::integer(2));
    CHECK_STR_EQ(arr.dump(), "[1,2]");

    JsonValue obj = JsonValue::object();
    obj.set("a", JsonValue::integer(1));
    obj.set("b", JsonValue::string("x"));
    CHECK_STR_EQ(obj.dump(), "{\"a\":1,\"b\":\"x\"}");

    JsonValue nested = JsonValue::object();
    JsonValue inner_arr = JsonValue::array();
    inner_arr.push_back(JsonValue::object());
    JsonValue inner_obj = JsonValue::object();
    inner_obj.set("k", JsonValue::array());
    inner_arr.push_back(std::move(inner_obj));
    nested.set("items", std::move(inner_arr));
    CHECK_STR_EQ(nested.dump(), "{\"items\":[{},{\"k\":[]}]}");
}

TEST(json_object_set_overwrites_existing_key_in_place) {
    JsonValue obj = JsonValue::object();
    obj.set("a", JsonValue::integer(1));
    obj.set("b", JsonValue::integer(2));
    obj.set("a", JsonValue::integer(99));
    /* Overwriting must not move the key to the end. */
    CHECK_STR_EQ(obj.dump(), "{\"a\":99,\"b\":2}");
}

TEST(json_push_back_and_set_on_the_wrong_kind_are_a_no_op) {
    JsonValue obj = JsonValue::object();
    obj.push_back(JsonValue::integer(1)); /* not an array: ignored */
    CHECK_STR_EQ(obj.dump(), "{}");

    JsonValue arr = JsonValue::array();
    arr.set("k", JsonValue::integer(1)); /* not an object: ignored */
    CHECK_STR_EQ(arr.dump(), "[]");
}

/* --- to_json() for the data model ------------------------------------------ */

TEST(panel_data_serializes_only_the_active_kind) {
    remote::PanelData p;
    p.kind = PanelKind::Receiver;
    p.receiver.mode = "NFM";
    p.receiver.frequency_hz = 446000000;
    p.receiver.running = true;

    const std::string json = remote::to_json(p).dump();
    CHECK(json.find("\"kind\":\"receiver\"") != std::string::npos);
    CHECK(json.find("\"receiver\":{") != std::string::npos);
    /* The unused members of the tagged union must not leak into the output. */
    CHECK(json.find("\"table\"") == std::string::npos);
    CHECK(json.find("\"spectrum\"") == std::string::npos);
    CHECK(json.find("\"screen\"") == std::string::npos);
}

TEST(screen_panel_carries_an_honest_message) {
    remote::PanelData p;
    p.kind = PanelKind::Screen;
    p.screen.message = "No structured data provider for 'Foo' yet.";

    const std::string json = remote::to_json(p).dump();
    CHECK(json.find("\"kind\":\"screen\"") != std::string::npos);
    CHECK(json.find("No structured data provider for 'Foo' yet.") != std::string::npos);
}

/* --- The /api/panel wire contract ------------------------------------------
 *
 * This is the seam where the C++ and Go halves of the portal meet, and it
 * silently broke once already: to_json(PanelData)'s self-describing
 * {kind, <kind>:{...}} form was being served directly, while
 * internal/portal/client decodes {app_id, panel_kind, title, data} and
 * client.Panel.HasData() tests panel_kind != "". The two key sets do not
 * intersect, so every app rendered as "no structured view yet" against real
 * hardware while every unit test on both sides still passed. Pin the shape. */

TEST(panel_payload_is_the_bare_kind_specific_object) {
    remote::PanelData p;
    p.kind = PanelKind::Map;
    p.map.markers.push_back({51.5, -0.12, "EIN17G", 268.0});

    const std::string payload = remote::panel_payload(p).dump();
    /* The renderer is handed {markers:[...]} — not the wrapper. */
    CHECK(payload.rfind("{\"markers\":", 0) == 0);
    CHECK(payload.find("\"kind\"") == std::string::npos);
    CHECK(payload.find("\"map\":") == std::string::npos);
    CHECK(payload.find("EIN17G") != std::string::npos);
}

TEST(panel_payload_matches_the_body_to_json_nests) {
    /* to_json(PanelData) must be built ON the payload, so the two can never
     * drift apart and start describing different data. */
    remote::PanelData p;
    p.kind = PanelKind::Adsb;
    remote::AdsbAircraft a;
    a.icao = "4CA2D5";
    a.pos_valid = true;
    a.lat = 51.5312;
    a.lon = -0.4021;
    p.adsb.aircraft.push_back(a);

    const std::string payload = remote::panel_payload(p).dump();
    const std::string wrapped = remote::to_json(p).dump();
    CHECK(wrapped == "{\"kind\":\"adsb\",\"adsb\":" + payload + "}");
}

TEST(panel_payload_covers_every_kind) {
    /* A kind added without a panel_payload case would silently serve {} to
     * the browser. Every kind must produce a non-empty object. */
    const PanelKind kinds[] = {
        PanelKind::Table, PanelKind::Spectrum, PanelKind::Receiver,
        PanelKind::Console, PanelKind::Map, PanelKind::Adsb,
        PanelKind::Form, PanelKind::Screen, PanelKind::Image,
        PanelKind::GeoTable, PanelKind::Ais,
    };
    for (const auto k : kinds) {
        remote::PanelData p;
        p.kind = k;
        const std::string payload = remote::panel_payload(p).dump();
        CHECK(payload != "{}");
        CHECK(payload.front() == '{');
    }
}

/* --- ADS-B panel -----------------------------------------------------------
 *
 * The load-bearing property is that "not heard yet" never serializes as a
 * value. An aircraft with no position fix must not appear at 0N 0E (a real
 * point in the Gulf of Guinea, and one the browser would happily draw and
 * measure a range to), and one whose velocity message has not arrived must not
 * read as stationary on a heading of due north. */

TEST(adsb_panel_omits_fields_that_were_never_received) {
    remote::PanelData p;
    p.kind = PanelKind::Adsb;

    remote::AdsbAircraft a;
    a.icao = "40631C";
    a.messages = 7;
    a.age_s = 44;
    a.amp = 14;
    /* Everything else left invalid: an all-call-replies-only contact. */
    p.adsb.aircraft.push_back(a);

    const std::string json = remote::to_json(p).dump();
    CHECK(json.find("\"kind\":\"adsb\"") != std::string::npos);
    CHECK(json.find("\"icao\":\"40631C\"") != std::string::npos);
    CHECK(json.find("\"has_pos\":false") != std::string::npos);
    CHECK(json.find("\"lat\"") == std::string::npos);
    CHECK(json.find("\"lon\"") == std::string::npos);
    CHECK(json.find("\"altitude_ft\"") == std::string::npos);
    CHECK(json.find("\"speed_kt\"") == std::string::npos);
    CHECK(json.find("\"heading_deg\"") == std::string::npos);
    CHECK(json.find("\"vertical_rate_fpm\"") == std::string::npos);
    /* A squawk of 0 is "no identity frame seen", not squawk 0000. */
    CHECK(json.find("\"squawk\"") == std::string::npos);
    /* Counters are always meaningful, including at zero. */
    CHECK(json.find("\"messages\":7") != std::string::npos);
    CHECK(json.find("\"age_s\":44") != std::string::npos);
}

TEST(adsb_panel_emits_every_field_once_received) {
    remote::PanelData p;
    p.kind = PanelKind::Adsb;

    remote::AdsbAircraft a;
    a.icao = "4CA2D5";
    a.callsign = "EIN17G";
    a.state = "current";
    a.pos_valid = true;
    a.lat = 51.5312;
    a.lon = -0.4021;
    a.altitude_valid = true;
    a.altitude_ft = 3175;
    a.velocity_valid = true;
    a.speed_kt = 189;
    a.heading_deg = 268;
    a.vertical_rate_fpm = -1216;
    a.squawk = 6041;
    a.messages = 412;
    a.age_s = 1;
    a.amp = 61;
    p.adsb.aircraft.push_back(a);

    p.adsb.frames_seen = 48213;
    p.adsb.frames_accepted = 9127;

    const std::string json = remote::to_json(p).dump();
    CHECK(json.find("\"callsign\":\"EIN17G\"") != std::string::npos);
    CHECK(json.find("\"has_pos\":true") != std::string::npos);
    CHECK(json.find("\"lat\":51.5312") != std::string::npos);
    CHECK(json.find("\"lon\":-0.4021") != std::string::npos);
    CHECK(json.find("\"altitude_ft\":3175") != std::string::npos);
    CHECK(json.find("\"speed_kt\":189") != std::string::npos);
    CHECK(json.find("\"heading_deg\":268") != std::string::npos);
    /* A descent must survive as a negative number, not an unsigned wrap. */
    CHECK(json.find("\"vertical_rate_fpm\":-1216") != std::string::npos);
    CHECK(json.find("\"squawk\":6041") != std::string::npos);
    CHECK(json.find("\"rssi\":61") != std::string::npos);
    CHECK(json.find("\"state\":\"current\"") != std::string::npos);
    CHECK(json.find("\"frames_seen\":48213") != std::string::npos);
    CHECK(json.find("\"frames_accepted\":9127") != std::string::npos);
}

TEST(adsb_panel_omits_home_until_the_operator_sets_one) {
    remote::PanelData p;
    p.kind = PanelKind::Adsb;

    /* No receiver position known: the browser must be told nothing rather
     * than being handed an origin it would draw range rings around. */
    const std::string without = remote::to_json(p).dump();
    CHECK(without.find("\"home\"") == std::string::npos);
    CHECK(without.find("\"aircraft\":[]") != std::string::npos);

    p.adsb.home_valid = true;
    p.adsb.home_lat = 51.4775;
    p.adsb.home_lon = -0.4614;
    const std::string with = remote::to_json(p).dump();
    CHECK(with.find("\"home\":{\"lat\":51.4775,\"lon\":-0.4614}") != std::string::npos);
}

TEST(adsb_is_the_only_kind_serialized_when_active) {
    remote::PanelData p;
    p.kind = PanelKind::Adsb;
    p.map.markers.push_back({51.0, -0.1, "should not appear", 0.0});
    p.table.columns.push_back("neither should this");

    const std::string json = remote::to_json(p).dump();
    CHECK(json.find("\"adsb\":{") != std::string::npos);
    CHECK(json.find("should not appear") == std::string::npos);
    CHECK(json.find("neither should this") == std::string::npos);
}

/* --- AIS panel -------------------------------------------------------------
 *
 * Same load-bearing property as the ADS-B payload above, and a harder one to
 * hold: ITU-R M.1371 gives nearly every AIS field a "not available" encoding
 * and transponders use them constantly, so on a real channel most of this
 * payload is absent most of the time. A vessel with no position fix must not
 * appear at 0N 0E, one that has not reported a heading must not read as
 * pointing due north, and one that has sent no position report at all must not
 * read as "under way w/engine" (navigational status 0).
 *
 * The mirror rule matters exactly as much and is tested alongside: a zero a
 * vessel really did broadcast is a value, not an absence. A ship at anchor
 * reporting 0.0 knots on course 000 must publish those numbers.
 *
 * These drive to_json() directly, so they pin the serializer's omission rules
 * alone. Which raw sentinel maps to which absence is the provider's half and
 * is pinned in test_provider_ais_ble.cpp against real ais::Packet bytes. */

TEST(ais_panel_omits_every_field_a_vessel_has_not_broadcast) {
    remote::PanelData p;
    p.kind = PanelKind::Ais;

    remote::AisVessel v;
    v.mmsi = "232003812";
    v.msgs = 3;
    /* Everything else left as constructed: a vessel heard only through a
     * position report whose every optional field carried its sentinel. */
    p.ais.vessels.push_back(v);

    const std::string json = remote::to_json(p).dump();
    CHECK(json.find("\"kind\":\"ais\"") != std::string::npos);
    CHECK(json.find("\"mmsi\":\"232003812\"") != std::string::npos);
    /* The MMSI and the frame count are the only two keys that are always
     * meaningful, so the whole vessel object is exactly those two. */
    CHECK(json.find("{\"mmsi\":\"232003812\",\"msgs\":3}") != std::string::npos);
    CHECK(json.find("\"name\"") == std::string::npos);
    CHECK(json.find("\"callsign\"") == std::string::npos);
    CHECK(json.find("\"destination\"") == std::string::npos);
    CHECK(json.find("\"lat\"") == std::string::npos);
    CHECK(json.find("\"lon\"") == std::string::npos);
    CHECK(json.find("\"sog_kn\"") == std::string::npos);
    CHECK(json.find("\"cog_deg\"") == std::string::npos);
    CHECK(json.find("\"heading_deg\"") == std::string::npos);
    CHECK(json.find("\"nav_status\"") == std::string::npos);
    CHECK(json.find("\"time\"") == std::string::npos);
    /* No has_pos companion either: on this payload the key's presence IS the
     * answer, and a false flag beside an absent position would be a second
     * spelling of the same fact for the browser to disagree with. */
    CHECK(json.find("\"has_pos\"") == std::string::npos);
    /* The counter is published even with nothing decoded. */
    CHECK(json.find("\"stats\":{\"packets_valid\":0}") != std::string::npos);
}

TEST(ais_panel_emits_every_field_once_received) {
    remote::PanelData p;
    p.kind = PanelKind::Ais;

    remote::AisVessel v;
    v.mmsi = "244660320";
    v.name = "EVER GIVEN";
    v.callsign = "PBRV";
    v.destination = "ROTTERDAM";
    v.pos_valid = true;
    v.lat = 51.5;
    v.lon = -0.25;
    v.sog_kn = 7.4;
    v.cog_deg = 123.4;
    v.heading_deg = 41.0;
    v.nav_status = 5;
    v.msgs = 12;
    v.time = "2026-08-14 09:31:07";
    p.ais.vessels.push_back(v);
    p.ais.packets_valid = 918;

    /* The whole object, in order, rather than a key at a time: the key set and
     * the ordering are the wire contract the browser is written against. */
    CHECK(remote::panel_payload(p).dump() ==
          "{\"vessels\":[{\"mmsi\":\"244660320\",\"name\":\"EVER GIVEN\","
          "\"callsign\":\"PBRV\",\"destination\":\"ROTTERDAM\","
          "\"lat\":51.5,\"lon\":-0.25,\"sog_kn\":7.4,\"cog_deg\":123.4,"
          "\"heading_deg\":41,\"nav_status\":5,\"msgs\":12,"
          "\"time\":\"2026-08-14 09:31:07\"}],"
          "\"stats\":{\"packets_valid\":918}}");
}

TEST(ais_panel_publishes_both_coordinates_or_neither) {
    /* A half-published position is never a legitimate answer, so one flag
     * gates both keys. Coordinates left in the struct while the app's own
     * validity gate said no must not reach the wire at all. */
    remote::AisVessel v;
    v.mmsi = "244660320";
    v.lat = 51.5;
    v.lon = -0.25;

    const std::string without = remote::to_json(v).dump();
    CHECK(without.find("\"lat\"") == std::string::npos);
    CHECK(without.find("\"lon\"") == std::string::npos);
    CHECK(without.find("51.5") == std::string::npos);

    v.pos_valid = true;
    CHECK(remote::to_json(v).dump() ==
          "{\"mmsi\":\"244660320\",\"lat\":51.5,\"lon\":-0.25,\"msgs\":0}");
}

TEST(ais_panel_keeps_a_zero_a_vessel_really_broadcast) {
    /* The other half of the rule. A ship moored at 0.0 knots on course 000
     * with navigational status 0 ("under way w/engine") has reported three
     * real values, and dropping them as falsey would be the same lie in the
     * opposite direction. 0N 0E is a real point in the Gulf of Guinea and a
     * transponder can legitimately report it. */
    remote::AisVessel v;
    v.mmsi = "000000001";
    v.pos_valid = true;
    v.lat = 0.0;
    v.lon = 0.0;
    v.sog_kn = 0.0;
    v.cog_deg = 0.0;
    v.heading_deg = 0.0;
    v.nav_status = 0;

    CHECK(remote::to_json(v).dump() ==
          "{\"mmsi\":\"000000001\",\"lat\":0,\"lon\":0,\"sog_kn\":0,"
          "\"cog_deg\":0,\"heading_deg\":0,\"nav_status\":0,\"msgs\":0}");
}

TEST(ais_panel_payload_is_the_bare_vessels_and_stats_object) {
    remote::PanelData p;
    p.kind = PanelKind::Ais;
    remote::AisVessel v;
    v.mmsi = "244660320";
    p.ais.vessels.push_back(v);

    const std::string payload = remote::panel_payload(p).dump();
    /* What GET /api/panel's `data` carries: no wrapper, no kind tag. */
    CHECK(payload.rfind("{\"vessels\":[", 0) == 0);
    CHECK(payload.find("\"kind\"") == std::string::npos);
    CHECK(payload.find("\"ais\":") == std::string::npos);

    /* And to_json(PanelData) is built ON that payload, so the two can never
     * drift apart and start describing different data. */
    CHECK(remote::to_json(p).dump() == "{\"kind\":\"ais\",\"ais\":" + payload + "}");
}

/* --- Table adapter: generic ui::RecentEntriesTable -> TableData ------------
 *
 * Same minimal Entry shape as test_recent_entries.cpp's TestEntry: a Key, an
 * invalid_key sentinel, a construct-from-key constructor and key(). The point
 * of this adapter is that it never needs to know that shape ahead of time —
 * it is exercised here with a type app_bridge.hpp has never heard of. */
namespace {

struct ProbeEntry {
    using Key = uint32_t;
    static constexpr Key invalid_key = 0xffffffffu;

    uint32_t id{0};
    size_t hits{0};

    ProbeEntry() = default;
    explicit ProbeEntry(Key k) : id{k} {}
    Key key() const { return id; }
};

using ProbeEntries = ui::RecentEntries<ProbeEntry>;

std::vector<std::string> probe_row(const ProbeEntry& e) {
    return {std::to_string(e.id), std::to_string(e.hits)};
}

}  // namespace

TEST(table_adapter_walks_entries_into_rows_generically) {
    ui::RecentEntriesColumns columns{{"ID", 6}, {"Hits", 4}};
    ProbeEntries entries;

    auto& a = ui::on_packet(entries, 1u);
    a.hits = 5;
    auto& b = ui::on_packet(entries, 2u);
    b.hits = 9;
    /* Newest-first: 2, then 1. */

    const TableData t = remote::table_data_from_entries(columns, entries, probe_row);

    CHECK_EQ(t.columns.size(), size_t{2});
    CHECK_STR_EQ(t.columns[0], "ID");
    CHECK_STR_EQ(t.columns[1], "Hits");

    CHECK_EQ(t.rows.size(), size_t{2});
    CHECK_STR_EQ(t.rows[0][0], "2");
    CHECK_STR_EQ(t.rows[0][1], "9");
    CHECK_STR_EQ(t.rows[1][0], "1");
    CHECK_STR_EQ(t.rows[1][1], "5");
}

TEST(table_adapter_respects_max_rows) {
    ui::RecentEntriesColumns columns{{"ID", 6}};
    ProbeEntries entries;
    for (uint32_t i = 1; i <= 10; i++) ui::on_packet(entries, i);

    const TableData t = remote::table_data_from_entries(columns, entries, probe_row, /*max_rows=*/3);
    CHECK_EQ(t.rows.size(), size_t{3});
    /* Newest-first, so the first three are 10, 9, 8. */
    CHECK_STR_EQ(t.rows[0][0], "10");
    CHECK_STR_EQ(t.rows[2][0], "8");
}

TEST(table_adapter_handles_an_empty_container) {
    ui::RecentEntriesColumns columns{{"ID", 6}};
    ProbeEntries entries;
    const TableData t = remote::table_data_from_entries(columns, entries, probe_row);
    CHECK_EQ(t.columns.size(), size_t{1});
    CHECK_EQ(t.rows.size(), size_t{0});
}

/* --- Launch queue -----------------------------------------------------------
 *
 * app::globals().nav is null by default in the test binary (see
 * test_recent_entries.cpp's equivalent note); the second test below points it
 * at a local NavigationView for its duration and restores it to null
 * afterward so later tests keep seeing the ambient default. */

TEST(launch_queue_draining_with_nothing_queued_is_a_no_op) {
    CHECK(!remote::AppBridge::instance().drain_launch_queue());
}

TEST(launch_queue_applies_a_request_from_another_thread_exactly_once) {
    /* A self-contained probe app: its factory touches no globals, so it is
     * safe to construct outside of main()'s fully wired-up app::globals(). */
    app::AppRegistry::instance().add(app::AppEntry{
        "remote_test_probe_app", "Remote Test Probe", app::Category::Utilities,
        ui::Color{}, nullptr,
        [] { return std::make_unique<ui::View>(ui::Rect{0, 0, 10, 10}); }});

    ui::NavigationView nav{{0, 0, 240, 304}};
    nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304})); /* root */
    nav.service();
    CHECK_EQ(nav.depth(), size_t{1});

    app::globals().nav = &nav;

    /* The request itself comes from another thread, same as an HTTP
     * connection thread would send it. */
    std::thread t([] { remote::AppBridge::instance().request_launch("remote_test_probe_app"); });
    t.join();

    CHECK(remote::AppBridge::instance().drain_launch_queue());
    CHECK(nav.service()); /* drain_launch_queue() only enqueues; service() applies it */
    CHECK_EQ(nav.depth(), size_t{2});

    /* Applied exactly once: nothing left to apply on a second drain. */
    CHECK(!remote::AppBridge::instance().drain_launch_queue());
    CHECK(!nav.service());
    CHECK_EQ(nav.depth(), size_t{2});

    app::globals().nav = nullptr;
}

TEST(launch_queue_ignores_an_unknown_app_id_without_crashing) {
    ui::NavigationView nav{{0, 0, 240, 304}};
    nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    nav.service();
    app::globals().nav = &nav;

    remote::AppBridge::instance().request_launch("this_app_id_does_not_exist");
    /* Nothing to apply: the id is unknown, so drain_launch_queue() finds no
     * matching AppEntry and the request is dropped defensively. */
    CHECK(!remote::AppBridge::instance().drain_launch_queue());
    CHECK_EQ(nav.depth(), size_t{1});

    app::globals().nav = nullptr;
}

TEST(launch_queue_home_request_pops_to_root) {
    app::AppRegistry::instance().add(app::AppEntry{
        "remote_test_probe_app_2", "Remote Test Probe 2", app::Category::Utilities,
        ui::Color{}, nullptr,
        [] { return std::make_unique<ui::View>(ui::Rect{0, 0, 10, 10}); }});

    ui::NavigationView nav{{0, 0, 240, 304}};
    nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    nav.service();
    app::globals().nav = &nav;

    remote::AppBridge::instance().request_launch("remote_test_probe_app_2");
    CHECK(remote::AppBridge::instance().drain_launch_queue());
    nav.service();
    CHECK_EQ(nav.depth(), size_t{2});

    remote::AppBridge::instance().request_home();
    CHECK(remote::AppBridge::instance().drain_launch_queue());
    nav.service();
    CHECK_EQ(nav.depth(), size_t{1});

    app::globals().nav = nullptr;
}

/* --- APRS RX panel ----------------------------------------------------------
 *
 * src/remote/provider_aprs.cpp publishes AprsTableView's Stations tab as a
 * generic table panel. Two things have to hold. The browser and the 240x320
 * screen must show the same four columns with the same cell text, or an
 * operator reading one and then the other is being told two different stories.
 * And a field a station has not sent must stay empty rather than being
 * invented: a station heard only through a status packet has no position and
 * must never turn up at 0N 0E.
 *
 * Every entry below is produced by the real AprsTableView::on_packet(), so the
 * app's own address and position parsing is what fills it in — nothing here
 * re-derives either. */

#include "ui_aprs_rx.hpp"

namespace remote {
/* Defined in src/remote/provider_aprs.cpp; see the comment there for why it is
 * exposed rather than left file-local. */
TableData aprs_table_data(const app::AprsRecentEntries& entries);
}  // namespace remote

namespace {

/* One shifted AX.25 address group, in the form ui_aprs_rx.cpp's parse_address()
 * reads back: six characters left-shifted by one, then a byte carrying the SSID
 * and the "another address follows" extension bit. */
void append_aprs_address(std::vector<uint8_t>& out,
                         const std::string& call,
                         uint8_t ssid,
                         bool last) {
    std::string padded = call;
    padded.resize(6, ' ');
    for (size_t i = 0; i < 6; i++)
        out.push_back(static_cast<uint8_t>(static_cast<uint8_t>(padded[i]) << 1));
    out.push_back(static_cast<uint8_t>(0x60 | ((ssid & 0x0F) << 1) | (last ? 1 : 0)));
}

/* "<call>-<ssid>>APRS,WIDE1-1:<information>", carrying its FCS: AprsPacket
 * bounds every field walk at payload_size - 2 because the buffer the framer
 * hands over always ends with those two bytes. */
app::AprsPacket make_aprs_packet(const std::string& call,
                                 uint8_t ssid,
                                 const std::string& information) {
    std::vector<uint8_t> frame;
    append_aprs_address(frame, "APRS", 0, false);
    append_aprs_address(frame, call, ssid, false);
    append_aprs_address(frame, "WIDE1", 1, true);
    frame.push_back(0x03); /* UI control */
    frame.push_back(0xF0); /* PID: no layer 3 */
    for (char c : information) frame.push_back(static_cast<uint8_t>(c));

    const uint16_t fcs = app::ax25_fcs(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(fcs & 0xFF));
    frame.push_back(static_cast<uint8_t>((fcs >> 8) & 0xFF));

    app::AprsPacket packet;
    packet.set_bytes(frame.data(), frame.size());
    packet.set_valid_checksum(true);
    return packet;
}

/* Upstream's reference position report: 49 deg 03.50' N, 72 deg 01.75' W. */
const char kAprsPositionInfo[] = "!4903.50N/07201.75W-Test";

}  // namespace

TEST(aprs_panel_publishes_the_columns_the_app_shows) {
    /* Names and order are AprsTableView::columns_ (src/apps/ui_aprs_rx.hpp):
     * {"Source", 0}, {"Loc", 6}, {"Hits", 4}, {"Time", 8}. They cannot be read
     * back off a running view — every RecentEntriesColumns reference inside the
     * table and its header is private — so this is what holds the provider's
     * copy to the app's. */
    app::AprsRecentEntries entries;
    const TableData t = remote::aprs_table_data(entries);

    CHECK_EQ(t.columns.size(), size_t{4});
    CHECK_STR_EQ(t.columns[0], "Source");
    CHECK_STR_EQ(t.columns[1], "Loc");
    CHECK_STR_EQ(t.columns[2], "Hits");
    CHECK_STR_EQ(t.columns[3], "Time");
}

TEST(aprs_panel_with_no_stations_heard_yields_no_rows) {
    app::AprsRecentEntries entries;
    const TableData t = remote::aprs_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{0});
    /* An empty table still describes itself, so the browser draws the header
     * row rather than an empty box. */
    CHECK_EQ(t.columns.size(), size_t{4});
}

TEST(aprs_panel_row_matches_the_line_the_app_draws) {
    app::AprsTableView table{{0, 0, 240, 280}};
    table.on_packet(make_aprs_packet("N0CALL", 9, kAprsPositionInfo), "12:34:56");

    const TableData t = remote::aprs_table_data(table.entries());
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_EQ(t.rows[0].size(), size_t{4});
    CHECK_STR_EQ(t.rows[0][0], "N0CALL-9");
    /* The device paints a green '*' in the Loc column for a station that has
     * reported a position, so '*' is what goes on the wire. */
    CHECK_STR_EQ(t.rows[0][1], "*");
    CHECK_STR_EQ(t.rows[0][2], "1");
    CHECK_STR_EQ(t.rows[0][3], "12:34:56");
}

TEST(aprs_panel_leaves_a_station_with_no_position_blank) {
    app::AprsTableView table{{0, 0, 240, 280}};
    /* '>' is a status report: it carries no position field, and the app's
     * has_position() says so. */
    table.on_packet(make_aprs_packet("M7ABC", 0, ">on air, no position"), "09:00:01");

    const TableData t = remote::aprs_table_data(table.entries());
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][0], "M7ABC");
    /* Empty. Not a marker, and emphatically not a 0,0 coordinate. */
    CHECK_STR_EQ(t.rows[0][1], "");
    CHECK_STR_EQ(t.rows[0][3], "09:00:01");
}

TEST(aprs_panel_emits_empty_cells_for_an_entry_nothing_has_filled_in) {
    /* An entry that exists but has had no packet applied to it: every string
     * field has to come out empty rather than carrying a stand-in. The hit
     * count is a counter, so 0 is a real value and is published as one. */
    app::AprsRecentEntries entries;
    entries.emplace_back(app::AprsRecentEntry::Key{0x0102030405060708ULL});

    const TableData t = remote::aprs_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][0], "");
    CHECK_STR_EQ(t.rows[0][1], "");
    CHECK_STR_EQ(t.rows[0][2], "0");
    CHECK_STR_EQ(t.rows[0][3], "");
}

TEST(aprs_panel_keeps_the_apps_own_hit_count_ceiling) {
    /* AprsTableView's on_draw shows "999+" past 999 rather than a wider number,
     * because the column is four characters. That ceiling is a real limit on
     * what the app reports and is kept; the four-character right-justification
     * around it is screen layout and is dropped. */
    app::AprsRecentEntries entries;
    auto& e = entries.emplace_back(app::AprsRecentEntry::Key{1});
    e.source_formatted = "G0ABC";
    e.time_string = "23:59:59";

    e.hits = 999;
    CHECK_STR_EQ(remote::aprs_table_data(entries).rows[0][2], "999");

    e.hits = 1000;
    CHECK_STR_EQ(remote::aprs_table_data(entries).rows[0][2], "999+");
}

TEST(aprs_panel_tracks_the_apps_counters_across_repeat_packets) {
    app::AprsTableView table{{0, 0, 240, 280}};
    table.on_packet(make_aprs_packet("N0CALL", 9, kAprsPositionInfo), "12:34:56");
    table.on_packet(make_aprs_packet("N0CALL", 9, ">now only a status"), "12:35:10");

    const TableData t = remote::aprs_table_data(table.entries());
    /* Same source address, so one station rather than two. */
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][2], "2");
    CHECK_STR_EQ(t.rows[0][3], "12:35:10");
    /* The app keeps a position once reported, so the marker survives a status
     * packet that carries none; the web view must not blank it either. */
    CHECK_STR_EQ(t.rows[0][1], "*");
}

TEST(aprs_panel_lists_the_most_recently_heard_station_first) {
    app::AprsTableView table{{0, 0, 240, 280}};
    table.on_packet(make_aprs_packet("G0ABC", 0, kAprsPositionInfo), "10:00:00");
    table.on_packet(make_aprs_packet("M7XYZ", 7, ">status"), "10:00:05");

    const TableData t = remote::aprs_table_data(table.entries());
    CHECK_EQ(t.rows.size(), size_t{2});
    CHECK_STR_EQ(t.rows[0][0], "M7XYZ-7");
    CHECK_STR_EQ(t.rows[1][0], "G0ABC");
}

/* --- APRS RX panel: the provider itself -------------------------------------
 *
 * Everything above drives remote::aprs_table_data() directly, which pins the
 * columns and the cell formatting but never runs aprs_panel() — so the part of
 * the provider that has to find the app in the first place was unexercised.
 * That is the half that fails in the operator's hands rather than in a unit:
 * the provider is handed nav->top(), and the moment someone on the device
 * opens a station's map the top of the stack is a GeoMapView, not AprsRxView.
 *
 * These go through AppBridge end to end — request_launch(), drain, service(),
 * refresh(), panel_json() — because that is the only path that sets the
 * bridge's current app id, and it is the path the HTTP handler reads. */

#include "audio_out.hpp"
#include "receiver_model.hpp"
#include "usrp_radio.hpp"

namespace {

/* AprsRxView's constructor binds globals().receiver, so a ReceiverModel has to
 * exist before the app registry's factory can build one. Nothing here opens a
 * device: on_show() calls receiver.start(), which fails at start_rx() on a
 * closed radio and returns false without spawning a DSP thread, so the view
 * comes up with an empty stations list — which is exactly the state under
 * test. Tears the globals back down so later tests see them as they were. */
struct AprsPanelHarness {
    radio::UsrpRadio radio{};
    audio::AudioOut audio{};
    radio::ReceiverModel receiver{radio, audio};
    ui::NavigationView nav{{0, 0, 240, 304}};

    radio::RadioDevice* saved_radio{app::globals().radio};
    radio::ReceiverModel* saved_receiver{app::globals().receiver};
    ui::NavigationView* saved_nav{app::globals().nav};

    AprsPanelHarness() {
        app::globals().radio = &radio;
        app::globals().receiver = &receiver;
        app::globals().nav = &nav;

        nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304})); /* root */
        nav.service();
    }

    ~AprsPanelHarness() {
        /* AppBridge is a process-global singleton, so a test that leaves it
         * believing aprsrx is open hands that state to every later test in the
         * binary — and to every -count= rerun. Clearing it needs the nav still
         * wired up, so it happens before the globals are put back. */
        remote::AppBridge::instance().request_home();
        remote::AppBridge::instance().drain_launch_queue();
        nav.service();

        app::globals().radio = saved_radio;
        app::globals().receiver = saved_receiver;
        app::globals().nav = saved_nav;
    }

    /* Puts APRS RX on the stack the way the portal does. */
    void launch_aprs() {
        remote::AppBridge::instance().request_launch("aprsrx");
        remote::AppBridge::instance().drain_launch_queue();
        nav.service();
    }
};

/* Crude but sufficient: the panel bodies asserted on here are small and their
 * key order is fixed by AppBridge::panel_json(). */
bool json_contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST(aprs_panel_provider_publishes_a_table_when_the_app_is_open) {
    AprsPanelHarness h;
    h.launch_aprs();
    /* The id has to be the one src/apps/ui_aprs_rx.cpp registers, or the
     * bridge never reaches the provider at all. */
    CHECK_EQ(h.nav.depth(), size_t{2});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(json_contains(panel, "\"app_id\":\"aprsrx\""));
    /* Upgraded from "table" to "geotable" (provider_aprs.cpp): the table half is
     * unchanged and the station markers are a strict addition beside it, so the
     * columns and rows assertions below still hold verbatim. */
    CHECK(json_contains(panel, "\"panel_kind\":\"geotable\""));
    CHECK(json_contains(panel, "\"columns\":[\"Source\",\"Loc\",\"Hits\",\"Time\"]"));
    /* No device, so no stations: an empty rows array, not a fabricated row. */
    CHECK(json_contains(panel, "\"rows\":[]"));
}

TEST(aprs_panel_provider_survives_the_operator_drilling_into_a_sub_view) {
    AprsPanelHarness h;
    h.launch_aprs();

    /* AprsRxView pushes a GeoMapView when a station's Map button is pressed
     * (ui_aprs_rx.cpp). Any pushed view reproduces the condition: the provider
     * is handed that view, and AprsRxView is only reachable by walking down. */
    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{3});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    /* Still the stations table. Without NavigationView::at_depth() this would
     * be a PanelKind::Screen and the browser would go blank for as long as the
     * operator left the map open. (Kind upgraded from "table" to "geotable";
     * the columns are unchanged.) */
    CHECK(json_contains(panel, "\"panel_kind\":\"geotable\""));
    CHECK(json_contains(panel, "\"columns\":[\"Source\",\"Loc\",\"Hits\",\"Time\"]"));
}

TEST(aprs_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    AprsPanelHarness h;
    h.launch_aprs();

    /* Popped on the device rather than through request_home() -- the path a
     * remote key press takes, which never goes near the launch queue.
     * AppBridge::refresh() derives the current app from the navigation stack,
     * so with the view gone the truthful answer is Home: not this app's data,
     * and not this app's name over an empty version of it. Before that
     * derivation the bridge went on believing the app was current and the
     * provider's own "... is not the open app." guard is what answered here.
     * That guard is still in the provider; the bridge simply no longer asks a
     * provider about an app that is not on the stack. */
    h.nav.pop_to_root();
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{1});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(json_contains(panel, "\"panel_kind\":\"screen\""));
    CHECK(json_contains(panel, "Home -- no app is open."));
    /* The stale id is what this change fixed; pin it, not just the text. */
    CHECK(json_contains(panel, "\"app_id\":\"\""));
    /* Emphatically not an empty table, which would be indistinguishable from
     * a running receiver that has heard nothing. */
    CHECK(!json_contains(panel, "\"columns\""));
}
