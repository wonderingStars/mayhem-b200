/*
 * mayhem-b200 — tests for the web portal panel providers of AIS RX and BLE RX
 * (src/remote/provider_ais.cpp, src/remote/provider_ble_rx.cpp).
 *
 * Two things have to hold for each app. The browser and the 240x320 screen must
 * agree — the same values through the same formatters, or an operator reading
 * one and then the other is being told two different stories. And a field a
 * target has not sent must stay ABSENT rather than being invented: a vessel
 * that has not broadcast its name is not a blank-named ship, one that has not
 * reported a heading is not pointing due north, and an advertiser with no
 * AdvData has not sent a run of zero bytes.
 *
 * The AIS half publishes a dedicated `ais` payload (fields, not rendered table
 * cells — see src/remote/provider_ais.cpp); BLE still publishes a table. The
 * AIS tests below are therefore about which keys exist and what they carry.
 * Their sentinels are the ones ITU-R M.1371 defines and every one of them is
 * driven through real payload bits rather than by setting a struct field, so
 * "1023 means no speed" is asserted about the actual decode path.
 *
 * Every entry below is filled in the way the app fills it — the AIS ones by the
 * real AISRecentEntry::update() from a real ais::Packet that passed its own
 * length and FCS checks, the BLE ones from a real ble_rx::Packet the real
 * ble_rx::Decoder recovered from a synthesised GFSK burst. Nothing here
 * re-derives a decode.
 *
 * The end-to-end blocks at the bottom are the ones that catch a wrong app id:
 * everything above them calls the table adapter directly and would keep passing
 * while the portal served a placeholder card forever.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ais_app.hpp"
#include "app_context.hpp"
#include "app_registry.hpp"
#include "audio_out.hpp"
#include "demod_digital.hpp"
#include "receiver_model.hpp"
#include "remote/app_bridge.hpp"
#include "remote/app_data.hpp"
#include "ui.hpp"
#include "ui_ble_rx.hpp"
#include "ui_navigation.hpp"
#include "ui_recent_entries.hpp"
#include "usrp_radio.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using remote::AisVessel;
using remote::PanelKind;
using remote::TableData;

namespace remote {
/* Defined in src/remote/provider_ais.cpp and src/remote/provider_ble_rx.cpp;
 * see the comments there for why they are exposed rather than left
 * file-local. */
std::vector<AisVessel> ais_vessels(const app::AISRecentEntries& entries, size_t max_vessels);
TableData ble_table_data(const app::ble_rx::BleRecentEntries& entries);
}  // namespace remote

namespace {

/* Crude but sufficient: the panel bodies asserted on here are small and their
 * key order is fixed by AppBridge::panel_json(). */
bool panel_contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/* --- AIS payload construction -----------------------------------------------
 *
 * ITU-R M.1371 numbers payload bits MSB-first across the octets, which is what
 * ais::Packet::read() returns, so a payload is built here as an MSB-first bit
 * array and packed MSB-first into octets. Same shape as the builder in
 * tests/test_ais.cpp, which is file-static there. */
class AisPayloadBuilder {
   public:
    void put(uint64_t value, size_t nbits) {
        for (size_t i = nbits; i > 0; --i)
            bits_.push_back(static_cast<uint8_t>((value >> (i - 1)) & 1));
    }

    /* Six-bit ASCII, '@'-padded to `chars` characters — the inverse of
     * ais::six_bit_to_ascii, and the padding ais::format::text() strips. */
    void put_text(const std::string& s, size_t chars) {
        for (size_t i = 0; i < chars; i++) {
            const uint8_t c = (i < s.size()) ? static_cast<uint8_t>(s[i]) : uint8_t{'@'};
            put(static_cast<uint64_t>(((c - 32) ^ 32) & 0x3F), 6);
        }
    }

    size_t bit_count() const { return bits_.size(); }

    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out((bits_.size() + 7) / 8, 0);
        for (size_t i = 0; i < bits_.size(); i++)
            if (bits_[i]) out[i / 8] = static_cast<uint8_t>(out[i / 8] | (0x80u >> (i % 8)));
        return out;
    }

   private:
    std::vector<uint8_t> bits_{};
};

/* The fields of a message 1 that the panel publishes, so a test can broadcast
 * one sentinel at a time and watch the key it governs disappear.
 *
 * The defaults are a transponder that has reported NOTHING but its identity —
 * every field at the "not available" encoding ITU-R M.1371 defines for it. The
 * two position defaults are taken from the app's own types rather than written
 * as literals: ais::Latitude{} is constructed with the 91-degree sentinel and
 * ais::Longitude{} with the 181-degree one, which is the same constant the
 * validity gate under test compares against. */
struct Msg1Fields {
    uint32_t nav_status{0};
    uint32_t sog{1023};
    int32_t lon_raw{ais::Longitude{}.raw()};
    int32_t lat_raw{ais::Latitude{}.raw()};
    uint32_t cog{3600};
    uint32_t heading{511};
};

/* Message 1, position report: 168 bits. Carries no name and no call sign, which
 * is the whole point of it here. */
std::vector<uint8_t> ais_message_1(uint32_t mmsi, const Msg1Fields& f = {}) {
    AisPayloadBuilder b;
    b.put(1, 6);            /*   0 message id       */
    b.put(0, 2);            /*   6 repeat           */
    b.put(mmsi, 30);        /*   8 MMSI             */
    b.put(f.nav_status, 4); /*  38 nav status       */
    b.put(0, 8);            /*  42 rate of turn     */
    b.put(f.sog, 10);       /*  50 speed over grnd  */
    b.put(1, 1);            /*  60 position acc.    */
    /* Signed 1/10000-minute fields: the low 28 and 27 bits of the two's
     * complement are what goes on air, and LatLonBase::normalized() sign
     * extends them back. */
    b.put(static_cast<uint64_t>(static_cast<int64_t>(f.lon_raw)), 28); /*  61 longitude */
    b.put(static_cast<uint64_t>(static_cast<int64_t>(f.lat_raw)), 27); /*  89 latitude  */
    b.put(f.cog, 12);       /* 116 course over grnd */
    b.put(f.heading, 9);    /* 128 true heading     */
    b.put(30, 6);           /* 137 UTC second       */
    b.put(0, 2);            /* 143 manoeuvre        */
    b.put(0, 3);            /* 145 spare            */
    b.put(0, 1);            /* 148 RAIM             */
    b.put(0x7FFFF, 19);     /* 149 radio status     */
    CHECK_EQ(b.bit_count(), size_t{168});
    return b.bytes();
}

/* 1/10000 of a minute per unit, i.e. ais::format::latlon_float()'s divisor of
 * 600000 read backwards. Both values below are chosen so that the app's float
 * conversion lands on an exactly representable result, which is what lets the
 * tests pin the JSON text rather than a tolerance. */
constexpr int32_t ais_raw_degrees(double degrees) {
    return static_cast<int32_t>(degrees * 600000.0);
}

/* Message 5, static and voyage related data: 424 bits. */
std::vector<uint8_t> ais_message_5(uint32_t mmsi,
                                   const std::string& call_sign,
                                   const std::string& name,
                                   const std::string& destination = "") {
    AisPayloadBuilder b;
    b.put(5, 6);              /*   0 message id  */
    b.put(0, 2);              /*   6 repeat      */
    b.put(mmsi, 30);          /*   8 MMSI        */
    b.put(0, 2);              /*  38 AIS version */
    b.put(9134567, 30);       /*  40 IMO number  */
    b.put_text(call_sign, 7); /*  70 call sign   */
    b.put_text(name, 20);     /* 112 vessel name */
    b.put(70, 8);             /* 232 ship type   */
    b.put(90, 9);             /* 240 dim to bow  */
    b.put(30, 9);             /* 249 dim to stern*/
    b.put(10, 6);             /* 258 dim to port */
    b.put(10, 6);             /* 264 dim to stbd */
    b.put(1, 4);              /* 270 EPFD type   */
    b.put(5, 4);              /* 274 ETA month   */
    b.put(17, 5);             /* 278 ETA day     */
    b.put(9, 5);              /* 283 ETA hour    */
    b.put(30, 6);             /* 288 ETA minute  */
    b.put(64, 8);             /* 294 draught     */
    b.put_text(destination, 20); /* 302 destination */
    b.put(0, 1);              /* 422 DTE         */
    b.put(0, 1);              /* 423 spare       */
    CHECK_EQ(b.bit_count(), size_t{424});
    return b.bytes();
}

ais::Packet ais_decode(const std::vector<uint8_t>& payload) {
    return ais::Packet::from_bits(ais::build_packet_bits(payload));
}

/* The app's own path from a frame to an entry: ui::on_packet() finds or creates
 * by MMSI, then AISRecentEntry::update() applies the fields — the two calls
 * AISAppView::on_packet() makes (ais_app.cpp). */
void ais_apply(app::AISRecentEntries& entries, const std::vector<uint8_t>& payload) {
    const ais::Packet packet = ais_decode(payload);
    /* Only a frame the app would have accepted is allowed through here. */
    CHECK(packet.is_valid());
    auto& entry = ui::on_packet(entries, packet.source_id());
    entry.update(packet);
}

/* The provider's answer for a one-ship list, and the JSON it serializes to.
 * Most tests below want both: the struct says what the provider decided, the
 * text says which keys actually reach the browser, and only the second one can
 * catch an absence that was serialized as a zero. */
AisVessel one_vessel(const app::AISRecentEntries& entries) {
    const auto vessels = remote::ais_vessels(entries, 200);
    CHECK_EQ(vessels.size(), size_t{1});
    if (vessels.empty()) return AisVessel{};
    return vessels.front();
}

std::string one_vessel_json(const app::AISRecentEntries& entries) {
    return remote::to_json(one_vessel(entries)).dump();
}

/* Builds the single-ship list a message would produce. */
app::AISRecentEntries ais_entries_from(const std::vector<uint8_t>& payload) {
    app::AISRecentEntries entries;
    ais_apply(entries, payload);
    return entries;
}

bool has_key(const std::string& json, const std::string& key) {
    return json.find("\"" + key + "\":") != std::string::npos;
}

}  // namespace

/* --- AIS: identity and text fields ----------------------------------------- */

TEST(ais_panel_with_no_ships_heard_yields_no_vessels) {
    app::AISRecentEntries entries;
    CHECK_EQ(remote::ais_vessels(entries, 200).size(), size_t{0});
}

TEST(ais_panel_zero_pads_the_mmsi_the_way_the_app_does) {
    /* ais::format::mmsi() is always nine characters, and it is what both the
     * list and the detail page use. Not screen alignment: an MMSI is a nine
     * digit identity and its leading zeros are part of it. */
    const auto entries = ais_entries_from(ais_message_1(1234567u));
    CHECK_STR_EQ(one_vessel(entries).mmsi, "001234567");
    /* Pinned against the app's own formatter too, so a change to it moves the
     * browser and the screen together instead of splitting them. */
    CHECK_STR_EQ(one_vessel(entries).mmsi, ais::format::mmsi(1234567u));
}

TEST(ais_panel_publishes_the_name_and_call_sign_as_separate_fields) {
    /* The device's list has ONE cell for both and picks between them; the
     * payload has two keys and hides neither. Both come through
     * ais::format::text(), which strips the '@' padding of the unused six-bit
     * characters. */
    const auto entries =
        ais_entries_from(ais_message_5(244660320u, "PBRV", "EVER GIVEN", "ROTTERDAM"));

    const AisVessel v = one_vessel(entries);
    CHECK_STR_EQ(v.name, "EVER GIVEN");
    CHECK_STR_EQ(v.callsign, "PBRV");
    CHECK_STR_EQ(v.destination, "ROTTERDAM");

    const std::string json = one_vessel_json(entries);
    CHECK(json.find("\"name\":\"EVER GIVEN\"") != std::string::npos);
    CHECK(json.find("\"callsign\":\"PBRV\"") != std::string::npos);
    CHECK(json.find("\"destination\":\"ROTTERDAM\"") != std::string::npos);
}

TEST(ais_panel_omits_a_name_a_transponder_padded_out_and_never_sent) {
    /* A message 5 with an unused name field stores twenty '@' characters, which
     * ais::format::text() strips to nothing. The key is dropped rather than
     * emitted empty — and the call sign, which WAS sent, still appears beside
     * it. (The list view's name-or-call-sign fallback does not apply here: it
     * exists because the screen has a single cell for the pair.) */
    const auto entries = ais_entries_from(ais_message_5(211234560u, "DK1AB", ""));

    const AisVessel v = one_vessel(entries);
    CHECK_STR_EQ(v.name, "");
    CHECK_STR_EQ(v.callsign, "DK1AB");

    const std::string json = one_vessel_json(entries);
    CHECK(!has_key(json, "name"));
    CHECK(json.find("\"callsign\":\"DK1AB\"") != std::string::npos);
    /* And the destination field of that same frame, also all padding. */
    CHECK(!has_key(json, "destination"));
}

TEST(ais_panel_omits_the_name_and_call_sign_of_a_ship_that_has_sent_neither) {
    /* A position report carries neither. Many transponders send the message 5
     * that does only once every six minutes, so this is the ordinary state of a
     * freshly heard vessel — and it must read as "not sent yet", not as a ship
     * whose name is an empty string. */
    const auto entries = ais_entries_from(ais_message_1(232003812u));

    const std::string json = one_vessel_json(entries);
    CHECK(json.find("\"mmsi\":\"232003812\"") != std::string::npos);
    CHECK(!has_key(json, "name"));
    CHECK(!has_key(json, "callsign"));
    CHECK(!has_key(json, "destination"));
}

TEST(ais_panel_emits_only_the_key_for_an_entry_nothing_has_filled_in) {
    /* An entry that exists but has had no packet applied to it. The MMSI is the
     * entry's own key and so is real by construction; every other field is
     * absent, and the frame counter is honestly zero. */
    app::AISRecentEntries entries;
    entries.emplace_back(app::AISRecentEntry::Key{366123456u});

    CHECK_STR_EQ(one_vessel_json(entries), "{\"mmsi\":\"366123456\",\"msgs\":0}");
}

/* --- AIS: position --------------------------------------------------------- */

TEST(ais_panel_publishes_the_position_the_app_would_draw) {
    Msg1Fields f;
    f.lat_raw = ais_raw_degrees(51.5);
    f.lon_raw = ais_raw_degrees(-0.25);
    const auto entries = ais_entries_from(ais_message_1(244660320u, f));

    const AisVessel v = one_vessel(entries);
    CHECK(v.pos_valid);
    /* Against the app's own conversion, which is what feeds the device's
     * ui::GeoMarker — not a second implementation of "1/10000 minute". */
    CHECK_EQ(v.lat, static_cast<double>(ais::format::latlon_float(f.lat_raw)));
    CHECK_EQ(v.lon, static_cast<double>(ais::format::latlon_float(f.lon_raw)));
    CHECK(one_vessel_json(entries).find("\"lat\":51.5,\"lon\":-0.25") != std::string::npos);
}

TEST(ais_panel_omits_the_position_a_transponder_reported_as_not_available) {
    /* The 91/181-degree sentinels. This ship is NOT at 0N 0E — a real point in
     * the Gulf of Guinea the browser would happily draw and measure a range
     * to — and it is not at 91N either. Both keys go. */
    const auto entries = ais_entries_from(ais_message_1(244660320u));

    const AisVessel v = one_vessel(entries);
    CHECK(!v.pos_valid);

    const std::string json = one_vessel_json(entries);
    CHECK(!has_key(json, "lat"));
    CHECK(!has_key(json, "lon"));
    CHECK(json.find("91") == std::string::npos);
    CHECK(json.find("181") == std::string::npos);
}

TEST(ais_panel_publishes_a_zero_position_a_ship_really_broadcast) {
    /* The mirror of the rule above, and the reason "0,0 means unknown" cannot
     * be a convention: raw zero is a legal, in-range position field and the
     * app's own gate accepts it. A ship reporting it gets both keys. */
    Msg1Fields f;
    f.lat_raw = 0;
    f.lon_raw = 0;
    const auto entries = ais_entries_from(ais_message_1(244660320u, f));

    CHECK(one_vessel(entries).pos_valid);
    CHECK(one_vessel_json(entries).find("\"lat\":0,\"lon\":0") != std::string::npos);
}

/* --- AIS: the scalar sentinels --------------------------------------------- */

TEST(ais_panel_omits_speed_course_and_heading_at_their_sentinels) {
    /* 1023 / 3600 / 511 are ITU-R M.1371's "not available" encodings and are
     * also the values AISPosition is constructed with, so this is the state of
     * every vessel heard only through a static frame. A moored ship is not
     * doing 102.3 knots on course 360 heading 511, and it is not doing 0
     * either. */
    const auto entries = ais_entries_from(ais_message_1(244660320u));

    const AisVessel v = one_vessel(entries);
    CHECK(!v.sog_kn.has_value());
    CHECK(!v.cog_deg.has_value());
    CHECK(!v.heading_deg.has_value());

    const std::string json = one_vessel_json(entries);
    CHECK(!has_key(json, "sog_kn"));
    CHECK(!has_key(json, "cog_deg"));
    CHECK(!has_key(json, "heading_deg"));
}

TEST(ais_panel_publishes_speed_course_and_heading_when_reported) {
    Msg1Fields f;
    f.sog = 74;       /* tenths of a knot   */
    f.cog = 1234;     /* tenths of a degree */
    f.heading = 41;   /* whole degrees      */
    const auto entries = ais_entries_from(ais_message_1(244660320u, f));

    const std::string json = one_vessel_json(entries);
    CHECK(json.find("\"sog_kn\":7.4") != std::string::npos);
    CHECK(json.find("\"cog_deg\":123.4") != std::string::npos);
    CHECK(json.find("\"heading_deg\":41") != std::string::npos);
}

TEST(ais_panel_publishes_the_maximum_speed_encoding_as_the_reading_it_is) {
    /* 1022 is ">= 102.2 knots", which is a reading and not an absence — the one
     * neighbour of 1023 that must NOT vanish. (The app's own
     * ais::format::speed_over_ground() draws it as ">= 102.2 knots" for the
     * same reason.) */
    Msg1Fields f;
    f.sog = 1022;
    const auto entries = ais_entries_from(ais_message_1(244660320u, f));

    const AisVessel v = one_vessel(entries);
    CHECK(v.sog_kn.has_value());
    CHECK(one_vessel_json(entries).find("\"sog_kn\":102.2") != std::string::npos);
}

TEST(ais_panel_publishes_a_zero_speed_course_and_heading) {
    /* The mirror again: a ship stopped and pointing due north has reported
     * three real numbers, and dropping them as falsey would be the same lie in
     * the opposite direction. */
    Msg1Fields f;
    f.sog = 0;
    f.cog = 0;
    f.heading = 0;
    const auto entries = ais_entries_from(ais_message_1(244660320u, f));

    const std::string json = one_vessel_json(entries);
    CHECK(json.find("\"sog_kn\":0") != std::string::npos);
    CHECK(json.find("\"cog_deg\":0") != std::string::npos);
    CHECK(json.find("\"heading_deg\":0") != std::string::npos);
}

TEST(ais_panel_omits_a_course_over_ground_the_app_calls_invalid) {
    /* Above 3600 the field is out of range and
     * ais::format::course_over_ground() prints "invalid" rather than a course.
     * The arithmetic would happily yield 400 degrees; publishing it would put a
     * heading on the browser's compass that the device refuses to show. */
    Msg1Fields f;
    f.cog = 4000;
    const auto entries = ais_entries_from(ais_message_1(244660320u, f));

    CHECK(!one_vessel(entries).cog_deg.has_value());
    CHECK(!has_key(one_vessel_json(entries), "cog_deg"));
}

TEST(ais_panel_keeps_the_heading_gate_at_the_not_available_sentinel) {
    /* Deliberately NOT the > 359 gate ais::format::true_heading() calls
     * "invalid": the map markers this payload replaced published everything
     * below 511, and narrowing it here would silently drop headings the portal
     * has been serving all along. 510 is published; 511 is the sentinel and is
     * not. */
    Msg1Fields f;
    f.heading = 510;
    CHECK(one_vessel_json(ais_entries_from(ais_message_1(244660320u, f)))
              .find("\"heading_deg\":510") != std::string::npos);

    f.heading = 511;
    CHECK(!has_key(one_vessel_json(ais_entries_from(ais_message_1(244660320u, f))),
                   "heading_deg"));
}

TEST(ais_panel_omits_the_navigational_status_until_a_position_report_arrives) {
    /* The app holds -1 for "no message 1/2/3 has been decoded yet", and 0 is a
     * real status ("under way w/engine"). Collapsing the two would report every
     * statically-heard vessel as under way on engines. */
    app::AISRecentEntries entries;
    ais_apply(entries, ais_message_5(244660320u, "PBRV", "EVER GIVEN"));
    CHECK_EQ(entries.front().navigational_status, int8_t{-1});
    CHECK(!one_vessel(entries).nav_status.has_value());
    CHECK(!has_key(one_vessel_json(entries), "nav_status"));

    /* Status 0 arrives and must survive as a value. */
    ais_apply(entries, ais_message_1(244660320u));
    CHECK(one_vessel_json(entries).find("\"nav_status\":0") != std::string::npos);

    /* And a non-zero one, so the assertion above cannot pass on a stuck 0. */
    Msg1Fields moored;
    moored.nav_status = 5;
    ais_apply(entries, ais_message_1(244660320u, moored));
    CHECK(one_vessel_json(entries).find("\"nav_status\":5") != std::string::npos);
}

/* --- AIS: counters, timestamp, ordering and the cap ------------------------ */

TEST(ais_panel_counts_every_frame_and_carries_the_apps_own_timestamp) {
    app::AISRecentEntries entries;
    ais_apply(entries, ais_message_5(244660320u, "PBRV", "EVER GIVEN"));

    /* Message 5 carries no position, so the app has stamped no time yet: the
     * key is absent rather than an empty string or an epoch. */
    CHECK_EQ(one_vessel(entries).msgs, uint32_t{1});
    CHECK(!has_key(one_vessel_json(entries), "time"));

    ais_apply(entries, ais_message_1(244660320u));
    const AisVessel v = one_vessel(entries);
    CHECK_EQ(v.msgs, uint32_t{2});
    /* Verbatim, not reformatted: the browser and the detail page's "Last" field
     * must show the same string. */
    CHECK_STR_EQ(v.time, entries.front().last_position.timestamp);
    CHECK(!v.time.empty());
}

TEST(ais_panel_lists_the_most_recently_heard_ship_first) {
    app::AISRecentEntries entries;
    ais_apply(entries, ais_message_5(244660320u, "PBRV", "EVER GIVEN"));
    ais_apply(entries, ais_message_5(232003812u, "MDXY3", "QUEEN MARY 2"));

    const auto vessels = remote::ais_vessels(entries, 200);
    CHECK_EQ(vessels.size(), size_t{2});
    /* ui::on_packet() moves the entry a frame just touched to the front, which
     * is the order the device's own list shows. */
    CHECK_STR_EQ(vessels[0].mmsi, "232003812");
    CHECK_STR_EQ(vessels[1].mmsi, "244660320");
}

TEST(ais_panel_keeps_one_vessel_per_mmsi_across_repeat_frames) {
    /* The position report arrives first and names nothing; the static frame
     * then fills the same entry in rather than making a second one. */
    app::AISRecentEntries entries;
    ais_apply(entries, ais_message_1(244660320u));
    CHECK(!has_key(one_vessel_json(entries), "name"));

    ais_apply(entries, ais_message_5(244660320u, "PBRV", "EVER GIVEN"));
    const auto vessels = remote::ais_vessels(entries, 200);
    CHECK_EQ(vessels.size(), size_t{1});
    CHECK_STR_EQ(vessels[0].name, "EVER GIVEN");
    CHECK_EQ(vessels[0].msgs, uint32_t{2});
}

TEST(ais_panel_caps_the_vessel_list) {
    /* The provider passes 200, the same ceiling the geotable this replaced
     * applied. Nothing reaches it in practice (ui::on_packet truncates the
     * app's list to 64), so the cap is asserted here at a size the test can
     * build rather than left as an untested branch. */
    app::AISRecentEntries entries;
    for (uint32_t i = 0; i < 5; i++)
        entries.emplace_back(app::AISRecentEntry::Key{244660320u + i});

    CHECK_EQ(remote::ais_vessels(entries, 3).size(), size_t{3});
    CHECK_EQ(remote::ais_vessels(entries, 200).size(), size_t{5});
    /* The cap keeps the front of the list, which is the newest end of it. */
    CHECK_STR_EQ(remote::ais_vessels(entries, 3)[0].mmsi, "244660320");
}

/* --- BLE: columns and cells ------------------------------------------------ */

namespace {

namespace ble = app::ble_rx;

std::vector<uint8_t> ble_bytes_to_bits_lsb(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (uint8_t b : bytes)
        for (int k = 0; k < 8; k++) bits.push_back(static_cast<uint8_t>((b >> k) & 1));
    return bits;
}

/* A synthesised advertising burst on channel 37, built with the same CRC and
 * whitening the decoder verifies — the same construction tests/test_ble_rx.cpp
 * proves the decoder against, so the packet the decoder hands back here is a
 * genuine decode rather than a hand-set struct.
 *
 * `pdu` is the on-air PDU: header {type/flags, length}, then the six AdvA
 * octets least-significant first, then AdvData. */
std::vector<dsp::cfloat> ble_burst(const std::vector<uint8_t>& pdu) {
    const uint32_t crc = ble::crc24(pdu.data(), pdu.size(), ble::crc_init_reorder(ble::kCrcInit));

    std::vector<uint8_t> full = pdu;
    full.push_back(static_cast<uint8_t>(crc & 0xFF));
    full.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    full.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));

    const auto whitened = ble::whiten(ble_bytes_to_bits_lsb(full), 37);

    /* Preamble 0xAA then access address D6 BE 89 8E, on-air (LSB-first) order. */
    auto bits = ble_bytes_to_bits_lsb(std::vector<uint8_t>{0xAA, 0xD6, 0xBE, 0x89, 0x8E});
    bits.insert(bits.end(), whitened.begin(), whitened.end());
    for (int i = 0; i < 4; i++) bits.push_back(0); /* trailing readable symbols */

    return dsp::fsk_modulate(bits, 4'000'000.0f, 1'000'000.0f, 250'000.0f, 0.5f);
}

ble::Packet ble_decode(const std::vector<uint8_t>& pdu) {
    const auto iq = ble_burst(pdu);
    CHECK(!iq.empty());

    ble::Decoder decoder;
    decoder.configure(4, 37);

    ble::Packet got{};
    int hits = 0;
    decoder.set_on_packet([&](const ble::Packet& p) {
        got = p;
        hits++;
    });
    decoder.process(iq.data(), iq.size());

    /* Anything else and the fixture is wrong, not the provider. */
    CHECK_EQ(hits, 1);
    return got;
}

/* The app's own path from a decoded packet to an entry, as
 * BleRxView::on_packet() writes it (ui_ble_rx.cpp). That method is private and
 * is only ever driven by the sample tap, so this is the closest reachable
 * spelling of it; the four assignments are the whole of it. */
void ble_apply(ble::BleRecentEntries& entries, const ble::Packet& packet) {
    auto& entry = ui::on_packet(entries, packet.mac_key(), 32);
    entry.count++;
    entry.type = packet.type;
    entry.payload_len = packet.payload_len;
    entry.data_hex = packet.data_string();
}

/* ADV_IND from AA:BB:CC:DD:EE:FF carrying the three-octet "Flags: LE General
 * Discoverable" AD structure. */
std::vector<uint8_t> ble_pdu_with_data() {
    return {0x40, 0x09, 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x02, 0x01, 0x1A};
}

/* The same advertiser with an empty payload: length 6 is the six AdvA octets
 * and no AdvData at all. */
std::vector<uint8_t> ble_pdu_without_data() {
    return {0x42, 0x06, 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
}

}  // namespace

TEST(ble_panel_publishes_the_columns_the_app_shows) {
    /* Names and order are BleRxView::columns_ (src/apps/ui_ble_rx.hpp):
     * {"MAC", 13}, {"T", 2}, {"Data", 0}. The object is a private member the
     * table only holds a reference to, so it cannot be read back off a running
     * view; this is what holds the provider's copy to the app's. */
    ble::BleRecentEntries entries;
    const TableData t = remote::ble_table_data(entries);

    CHECK_EQ(t.columns.size(), size_t{3});
    CHECK_STR_EQ(t.columns[0], "MAC");
    CHECK_STR_EQ(t.columns[1], "T");
    CHECK_STR_EQ(t.columns[2], "Data");
}

TEST(ble_panel_with_no_advertisers_heard_yields_no_rows) {
    ble::BleRecentEntries entries;
    const TableData t = remote::ble_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{0});
    CHECK_EQ(t.columns.size(), size_t{3});
}

TEST(ble_panel_row_matches_the_line_the_app_draws) {
    const ble::Packet packet = ble_decode(ble_pdu_with_data());
    ble::BleRecentEntries entries;
    ble_apply(entries, packet);

    const TableData t = remote::ble_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_EQ(t.rows[0].size(), size_t{3});

    /* Pinned twice: against the literal the device paints, and against the
     * app's own Packet::mac_string()/data_string(), so a change to either
     * formatter shows up here rather than silently splitting the browser from
     * the screen. */
    CHECK_STR_EQ(t.rows[0][0], "AA:BB:CC:DD:EE:FF");
    CHECK_STR_EQ(t.rows[0][0], packet.mac_string());
    /* The numeric ADV_PDU_TYPE, which is what the app's on_draw shows. */
    CHECK_STR_EQ(t.rows[0][1], "0");
    CHECK_STR_EQ(t.rows[0][2], "02 01 1A");
    CHECK_STR_EQ(t.rows[0][2], packet.data_string());
}

TEST(ble_panel_leaves_an_advertiser_that_sent_no_payload_blank) {
    /* ADV_NONCONN_IND with a six octet payload: the AdvA and nothing else. The
     * Data cell must be empty, not a run of zero bytes. */
    const ble::Packet packet = ble_decode(ble_pdu_without_data());
    CHECK_EQ(packet.payload_len, uint8_t{6});
    CHECK_EQ(packet.data.size(), size_t{0});

    ble::BleRecentEntries entries;
    ble_apply(entries, packet);

    const TableData t = remote::ble_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][0], "AA:BB:CC:DD:EE:FF");
    CHECK_STR_EQ(t.rows[0][1], "2"); /* ADV_NONCONN_IND */
    CHECK_STR_EQ(t.rows[0][2], "");
}

TEST(ble_panel_emits_an_empty_data_cell_for_an_entry_nothing_has_filled_in) {
    /* An entry that exists but has had no packet applied to it. The MAC is the
     * entry's own key and so is real by construction; the payload hex is the
     * only thing that can be absent, and it comes out empty. */
    ble::BleRecentEntries entries;
    entries.emplace_back(ble::BleRecentEntry::Key{0xAABBCCDDEEFFULL});

    const TableData t = remote::ble_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][0], "AA:BB:CC:DD:EE:FF");
    CHECK_STR_EQ(t.rows[0][2], "");
}

TEST(ble_panel_keeps_one_row_per_advertiser_across_repeat_frames) {
    const ble::Packet with_data = ble_decode(ble_pdu_with_data());
    const ble::Packet without_data = ble_decode(ble_pdu_without_data());
    /* Same advertiser, so one row — and the newer frame's payload wins, which
     * is what the app's on_packet does. */
    CHECK_EQ(with_data.mac_key(), without_data.mac_key());

    ble::BleRecentEntries entries;
    ble_apply(entries, with_data);
    ble_apply(entries, without_data);

    const TableData t = remote::ble_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][1], "2");
    CHECK_STR_EQ(t.rows[0][2], "");
}

/* --- The providers themselves ------------------------------------------------
 *
 * Everything above drives the table adapters directly, which pins the columns
 * and the cell formatting but never runs ais_panel()/ble_panel() — so the part
 * of each provider that has to find the app in the first place, and the app id
 * it registers under, were unexercised. A wrong id is the single most likely
 * way this silently does nothing: the provider never fires, the portal keeps
 * serving its placeholder card, and every assertion above still passes.
 *
 * These go through AppBridge end to end — request_launch(), drain, service(),
 * refresh(), panel_json() — because that is the only path that sets the
 * bridge's current app id, and it is the path the HTTP handler reads. */

namespace {

/* Both views bind globals() in their constructors or on_show(), so a
 * ReceiverModel has to exist before the app registry's factory can build one.
 * Nothing here opens a device: on_show() calls receiver.start(), which fails at
 * start_rx() on a closed radio and returns false without spawning a DSP thread,
 * so each view comes up with an empty list — which is exactly the state under
 * test. Tears the globals back down so later tests see them as they were. */
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
         * believing one of these apps is open hands that state to every later
         * test in the binary — and to every -count= rerun. Clearing it needs
         * the nav still wired up, so it happens before the globals go back. */
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
};

}  // namespace

TEST(ais_panel_provider_publishes_a_table_when_the_app_is_open) {
    ProviderHarness h;
    /* The id has to be the one src/apps/ais_app.cpp registers ("ais"), or the
     * bridge never reaches the provider at all and the depth stays 1. */
    h.launch("ais");
    CHECK_EQ(h.nav.depth(), size_t{2});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"app_id\":\"ais\""));
    /* Its own kind now (provider_ais.cpp), not the "geotable" of rendered
     * table cells plus markers it published before. */
    CHECK(panel_contains(panel, "\"panel_kind\":\"ais\""));
    /* No device, so no ships: an empty vessels array, not a fabricated one. */
    CHECK(panel_contains(panel, "\"vessels\":[]"));
    /* The counter is real and honestly zero — the decoder has seen no frames. */
    CHECK(panel_contains(panel, "\"stats\":{\"packets_valid\":0}"));
    /* And nothing left over from the table shape. */
    CHECK(!panel_contains(panel, "\"columns\""));
    CHECK(!panel_contains(panel, "\"rows\""));
}

TEST(ais_panel_provider_survives_the_operator_drilling_into_a_sub_view) {
    ProviderHarness h;
    h.launch("ais");

    /* AISRecentEntryDetailView pushes a GeoMapView when a ship's "See on map"
     * button is pressed (ais_app.cpp). Any pushed view reproduces the
     * condition: the provider is handed that view, and AISAppView is only
     * reachable by walking down. */
    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{3});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    /* Still the ship list. Without NavigationView::at_depth() this would be a
     * PanelKind::Screen and the browser would go blank for as long as the
     * operator left the map open. */
    CHECK(panel_contains(panel, "\"panel_kind\":\"ais\""));
    CHECK(panel_contains(panel, "\"vessels\":["));
}

TEST(ais_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    ProviderHarness h;
    h.launch("ais");

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

    CHECK(panel_contains(panel, "\"panel_kind\":\"screen\""));
    CHECK(panel_contains(panel, "Home -- no app is open."));
    /* The stale id is what this change fixed; pin it, not just the text. */
    CHECK(panel_contains(panel, "\"app_id\":\"\""));
    CHECK(!panel_contains(panel, "\"vessels\""));
}

TEST(ble_panel_provider_publishes_a_table_when_the_app_is_open) {
    ProviderHarness h;
    /* The id has to be the one src/apps/ui_ble_rx.cpp registers ("blerx"). */
    h.launch("blerx");
    CHECK_EQ(h.nav.depth(), size_t{2});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"app_id\":\"blerx\""));
    CHECK(panel_contains(panel, "\"panel_kind\":\"table\""));
    CHECK(panel_contains(panel, "\"columns\":[\"MAC\",\"T\",\"Data\"]"));
    /* No device, so nothing heard: an empty rows array, not a fabricated row. */
    CHECK(panel_contains(panel, "\"rows\":[]"));
}

TEST(ble_panel_provider_survives_the_operator_drilling_into_a_sub_view) {
    ProviderHarness h;
    h.launch("blerx");

    /* BleRxView pushes nothing of its own today, but the provider must not
     * depend on that: anything on top of it has to leave the panel intact. */
    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{3});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"panel_kind\":\"table\""));
    CHECK(panel_contains(panel, "\"columns\":[\"MAC\",\"T\",\"Data\"]"));
}

TEST(ble_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    ProviderHarness h;
    h.launch("blerx");

    h.nav.pop_to_root();
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{1});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"panel_kind\":\"screen\""));
    CHECK(panel_contains(panel, "Home -- no app is open."));
    /* The stale id is what this change fixed; pin it, not just the text. */
    CHECK(panel_contains(panel, "\"app_id\":\"\""));
    CHECK(!panel_contains(panel, "\"columns\""));
}
