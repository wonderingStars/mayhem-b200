/*
 * mayhem-b200 — tests for the web portal's image and geo-table panels
 * (src/remote/app_data.*, provider_apt/wefax/sstv/wardrive/ais/aprs).
 *
 * Three properties are worth this much test code.
 *
 * ABSENT STAYS ABSENT. A ship that has broadcast no position must appear in the
 * table and NOT on the map; 0N 0E is a real point in the Gulf of Guinea and a
 * browser will happily draw it, measure a range to it and believe it. The same
 * rule covers a heading that was never reported (a marker on heading 000 is a
 * claim the vessel points due north) and a picture that has not been decoded
 * yet (a black rectangle is not "no image", it is a lie that looks like data).
 *
 * THE TABLE HALF DOES NOT MOVE. AIS and APRS were `table` panels and are now
 * `geotable` panels. The whole point of the geotable shape is that the table
 * inside it is byte-for-byte what the plain table panel emitted, so the browser
 * and the 240x320 screen keep telling the operator the same story. That is
 * asserted directly against to_json(TableData) rather than by eye.
 *
 * REV ONLY MOVES WHEN THE PIXELS DO. Every bump of an image panel's `rev` costs
 * the browser a fresh ~170 kB fetch, so a decoder that has produced nothing
 * since the last poll must not bump, and a client that already holds the
 * current rev must be sent no pixels at all.
 *
 * The end-to-end blocks at the bottom are the ones that catch a wrong app id:
 * everything above them calls a helper directly and would keep passing while
 * the portal served its placeholder card forever.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ais_app.hpp"
#include "app_context.hpp"
#include "app_registry.hpp"
#include "audio_out.hpp"
#include "receiver_model.hpp"
#include "remote/app_bridge.hpp"
#include "remote/app_data.hpp"
#include "ui.hpp"
#include "ui_aprs_rx.hpp"
#include "ui_navigation.hpp"
#include "ui_recent_entries.hpp"
#include "ui_wardrivemap.hpp"
#include "usrp_radio.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using remote::GeoTableMarker;
using remote::ImageData;
using remote::ImageRevCounter;
using remote::MapData;
using remote::PanelKind;
using remote::TableData;

namespace remote {
/* Defined in src/remote/provider_apt.cpp (shared by the three image providers),
 * provider_ais.cpp, provider_aprs.cpp and provider_wardrive.cpp; see the
 * comments there for why they are exposed rather than left file-local. */
ImageData image_data_from_pixels(const std::vector<ui::Color>& pixels,
                                 uint32_t width,
                                 uint32_t height,
                                 ImageRevCounter& rev_counter,
                                 std::string app_name,
                                 std::string resolution_note);
TableData ais_table_data(const app::AISRecentEntries& entries);
std::vector<GeoTableMarker> ais_geo_markers(const app::AISRecentEntries& entries,
                                            size_t max_markers);
TableData aprs_table_data(const app::AprsRecentEntries& entries);
std::vector<GeoTableMarker> aprs_geo_markers(const app::AprsRecentEntries& entries,
                                             size_t max_markers);
MapData wardrive_map_data(const std::vector<app::wardrive::Observation>& observations);
}  // namespace remote

namespace {

bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/* ===========================================================================
 * base64
 * ===========================================================================*/

TEST(base64_matches_the_rfc4648_test_vectors) {
    auto enc = [](const std::string& s) {
        return remote::base64_encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    };
    /* RFC 4648 section 10. The three tail lengths are the whole risk here: a
     * browser's atob() rejects wrong padding outright, so a picture would fail
     * to decode entirely rather than look slightly wrong. */
    CHECK_STR_EQ(enc(""), "");
    CHECK_STR_EQ(enc("f"), "Zg==");
    CHECK_STR_EQ(enc("fo"), "Zm8=");
    CHECK_STR_EQ(enc("foo"), "Zm9v");
    CHECK_STR_EQ(enc("foob"), "Zm9vYg==");
    CHECK_STR_EQ(enc("fooba"), "Zm9vYmE=");
    CHECK_STR_EQ(enc("foobar"), "Zm9vYmFy");
}

TEST(base64_covers_the_whole_alphabet_including_the_high_bytes) {
    /* 0xFB 0xEF 0xBE exercises the '+' and '/' code points, which a
     * URL-safe-by-accident table would render as '-' and '_' and silently
     * corrupt every image with a bright pixel in it. */
    const uint8_t bytes[] = {0xFB, 0xEF, 0xBE};
    CHECK_STR_EQ(remote::base64_encode(bytes, sizeof(bytes)), "++++");

    const uint8_t all_ff[] = {0xFF, 0xFF, 0xFF};
    CHECK_STR_EQ(remote::base64_encode(all_ff, sizeof(all_ff)), "////");

    /* A null pointer or an empty run is an empty string, not a crash. */
    CHECK_STR_EQ(remote::base64_encode(nullptr, 0), "");
}

/* ===========================================================================
 * RGB565 helpers
 * ===========================================================================*/

TEST(rgb565_blank_detection_distinguishes_black_from_nearly_black) {
    std::vector<uint16_t> px(64, 0);
    CHECK(remote::rgb565_is_blank(px.data(), px.size()));

    /* One pixel of the darkest possible non-black blue is enough: a canvas that
     * has had a single scan line written to it has an image on it. */
    px[37] = 0x0001;
    CHECK(!remote::rgb565_is_blank(px.data(), px.size()));

    CHECK(remote::rgb565_is_blank(nullptr, 0));
}

TEST(rgb565_content_hash_moves_with_the_content_and_not_otherwise) {
    std::vector<uint16_t> a(128, 0x1234);
    std::vector<uint16_t> b = a;

    const uint64_t ha = remote::rgb565_content_hash(a.data(), a.size());
    CHECK_EQ(remote::rgb565_content_hash(b.data(), b.size()), ha);

    /* A single changed pixel must move the hash, or a decoder that is producing
     * one new line a second would look idle to the browser. */
    b[100] = 0x1235;
    CHECK(remote::rgb565_content_hash(b.data(), b.size()) != ha);

    /* Length is part of the identity: a shorter buffer of the same prefix is a
     * different picture, not the same one. */
    std::vector<uint16_t> shorter(a.begin(), a.end() - 1);
    CHECK(remote::rgb565_content_hash(shorter.data(), shorter.size()) != ha);
}

TEST(rgb565_expansion_matches_the_displays_own_compositor) {
    /* Display::composite_bgra() replicates the low bits so a full-scale channel
     * reaches 0xFF. ui::Color::r()/g()/b() do NOT — they leave 0xF8/0xFC — so
     * taking that shortcut here would show the operator a browser picture
     * systematically darker than the device's own screen. */
    const std::vector<uint16_t> px = {
        0xFFFF, /* white  */
        0x0000, /* black  */
        0xF800, /* red    */
        0x07E0, /* green  */
        0x001F, /* blue   */
    };
    std::vector<uint8_t> out;
    remote::rgb565_to_rgb888(px.data(), px.size(), out);

    CHECK_EQ(out.size(), px.size() * 3);
    CHECK_EQ(out[0], 0xFF); CHECK_EQ(out[1], 0xFF); CHECK_EQ(out[2], 0xFF);
    CHECK_EQ(out[3], 0x00); CHECK_EQ(out[4], 0x00); CHECK_EQ(out[5], 0x00);
    CHECK_EQ(out[6], 0xFF); CHECK_EQ(out[7], 0x00); CHECK_EQ(out[8], 0x00);
    CHECK_EQ(out[9], 0x00); CHECK_EQ(out[10], 0xFF); CHECK_EQ(out[11], 0x00);
    CHECK_EQ(out[12], 0x00); CHECK_EQ(out[13], 0x00); CHECK_EQ(out[14], 0xFF);

    /* An empty run clears the output rather than leaving stale pixels behind. */
    remote::rgb565_to_rgb888(nullptr, 0, out);
    CHECK_EQ(out.size(), size_t{0});
}

/* ===========================================================================
 * ImageRevCounter
 * ===========================================================================*/

TEST(image_rev_counter_starts_at_one_so_zero_stays_reserved) {
    ImageRevCounter c;
    CHECK_EQ(c.rev(), 0u);
    /* rev 0 means "nothing decoded yet" on the wire, so the first real frame
     * has to be 1 — otherwise a decoded picture is indistinguishable from an
     * empty one. */
    CHECK_EQ(c.observe(0xABCDEF), 1u);
}

TEST(image_rev_counter_does_not_bump_for_an_unchanged_frame) {
    ImageRevCounter c;
    CHECK_EQ(c.observe(42), 1u);
    /* The UI thread calls the provider ~60 times a second. If every call bumped,
     * the browser would refetch ~170 kB 60 times a second for a picture that
     * gains two lines a second. */
    for (int i = 0; i < 100; i++) CHECK_EQ(c.observe(42), 1u);
}

TEST(image_rev_counter_is_monotonic_even_when_content_repeats) {
    ImageRevCounter c;
    CHECK_EQ(c.observe(1), 1u);
    CHECK_EQ(c.observe(2), 2u);
    /* Back to a picture the client may already have discarded. The rev must
     * still move forward: a rev that went backwards would leave a client that
     * cached rev 2 showing it forever. */
    CHECK_EQ(c.observe(1), 3u);
    CHECK_EQ(c.observe(1), 3u);
}

TEST(image_rev_counter_bumps_on_a_zero_hash_first_seen) {
    /* 0 is a perfectly ordinary hash value and must not read as "unseeded". */
    ImageRevCounter c;
    CHECK_EQ(c.observe(0), 1u);
    CHECK_EQ(c.observe(0), 1u);
    CHECK_EQ(c.observe(1), 2u);
}

/* ===========================================================================
 * ImageData serialization (contract 4)
 * ===========================================================================*/

TEST(image_payload_with_nothing_decoded_carries_no_pixels_and_says_so) {
    ImageData img;
    img.app_name = "NOAA APT";
    img.width = 240;
    img.height = 232;
    img.note = "No image decoded yet.";

    const std::string j = remote::to_json(img).dump();
    CHECK(has(j, "\"rev\":0"));
    CHECK(has(j, "\"format\":\"rgb888\""));
    CHECK(has(j, "\"note\":\"No image decoded yet.\""));
    /* The one thing that must never happen: 167 kB of black serialized as if a
     * satellite pass had been decoded. */
    CHECK(!has(j, "data_b64"));
}

TEST(image_payload_carries_pixels_when_the_client_has_a_different_rev) {
    ImageData img;
    img.width = 2;
    img.height = 1;
    img.rev = 7;
    img.rgb = {0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF};

    const std::string j = remote::to_json(img, 6).dump();
    CHECK(has(j, "\"width\":2"));
    CHECK(has(j, "\"height\":1"));
    CHECK(has(j, "\"rev\":7"));
    CHECK(has(j, "\"data_b64\":\"" + remote::base64_encode(img.rgb.data(), img.rgb.size()) + "\""));
}

TEST(image_payload_omits_the_pixels_when_the_client_already_holds_this_rev) {
    ImageData img;
    img.width = 2;
    img.height = 1;
    img.rev = 7;
    img.rgb = {0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF};

    /* The entire reason rev exists. Geometry and rev still go out so the client
     * can tell the panel is alive and its cached picture is current. */
    const std::string j = remote::to_json(img, 7).dump();
    CHECK(has(j, "\"rev\":7"));
    CHECK(has(j, "\"width\":2"));
    CHECK(!has(j, "data_b64"));
}

TEST(image_payload_refuses_a_payload_that_disagrees_with_its_geometry) {
    /* A short buffer rendered against the stated width tears every row by a
     * pixel and produces convincing-looking diagonal garbage. Better to send
     * nothing. */
    ImageData img;
    img.width = 4;
    img.height = 4;
    img.rev = 3;
    img.rgb.assign(4 * 4 * 3 - 1, 0x40);

    const std::string j = remote::to_json(img).dump();
    CHECK(has(j, "\"rev\":3"));
    CHECK(!has(j, "data_b64"));
}

TEST(image_payload_omits_the_optional_strings_when_they_are_empty) {
    ImageData img;
    img.width = 1;
    img.height = 1;
    img.rev = 1;
    img.rgb = {1, 2, 3};

    const std::string j = remote::to_json(img).dump();
    CHECK(!has(j, "app_name"));
    CHECK(!has(j, "note"));
    CHECK(has(j, "data_b64"));
}

/* ===========================================================================
 * image_data_from_pixels — the shared conversion the three image providers use
 * ===========================================================================*/

TEST(image_from_a_blank_canvas_is_rev_zero_with_no_pixels) {
    /* The state a ScanCanvas is in from its constructor and after clear(). */
    const std::vector<ui::Color> px(8 * 4, ui::Color::black());
    ImageRevCounter rev;

    const ImageData img =
        remote::image_data_from_pixels(px, 8, 4, rev, "NOAA APT", "resolution caveat");

    CHECK_EQ(img.rev, 0u);
    CHECK_EQ(img.rgb.size(), size_t{0});
    CHECK_STR_EQ(img.note, "No image decoded yet.");
    /* Geometry is still published, so the browser can size its canvas before
     * the first line arrives. */
    CHECK_EQ(img.width, 8u);
    CHECK_EQ(img.height, 4u);
    /* And the counter has not been touched: the first real frame is still 1. */
    CHECK_EQ(rev.rev(), 0u);
}

TEST(image_from_a_partly_decoded_canvas_publishes_every_row_it_has) {
    /* One scan line written and the rest still black is the ordinary state of a
     * pass in progress; the black below is real screen content, not padding. */
    std::vector<ui::Color> px(4 * 3, ui::Color::black());
    px[0] = ui::Color{255, 255, 255};
    px[3] = ui::Color{255, 255, 255};
    ImageRevCounter rev;

    const ImageData img =
        remote::image_data_from_pixels(px, 4, 3, rev, "WeFax", "resolution caveat");

    CHECK_EQ(img.rev, 1u);
    CHECK_EQ(img.rgb.size(), size_t{4 * 3 * 3});
    CHECK_STR_EQ(img.app_name, "WeFax");
    CHECK_STR_EQ(img.note, "resolution caveat");
    CHECK_EQ(img.rgb[0], 0xFF);
    /* Row 0 column 1 was never written and stays black. */
    CHECK_EQ(img.rgb[3], 0x00);
}

TEST(image_rev_holds_still_while_the_decoder_produces_nothing) {
    std::vector<ui::Color> px(4 * 3, ui::Color::black());
    px[5] = ui::Color{0, 255, 0};
    ImageRevCounter rev;

    const ImageData first =
        remote::image_data_from_pixels(px, 4, 3, rev, "SSTV RX", "note");
    CHECK_EQ(first.rev, 1u);

    /* Ten UI frames with no new scan line. */
    for (int i = 0; i < 10; i++) {
        const ImageData again =
            remote::image_data_from_pixels(px, 4, 3, rev, "SSTV RX", "note");
        CHECK_EQ(again.rev, 1u);
    }

    px[6] = ui::Color{0, 0, 255};
    const ImageData moved =
        remote::image_data_from_pixels(px, 4, 3, rev, "SSTV RX", "note");
    CHECK_EQ(moved.rev, 2u);
}

TEST(image_from_pixels_reports_a_geometry_it_cannot_trust_rather_than_guessing) {
    const std::vector<ui::Color> px(10, ui::Color{255, 255, 255});
    ImageRevCounter rev;

    /* 4x3 is 12 pixels; the buffer has 10. */
    const ImageData img = remote::image_data_from_pixels(px, 4, 3, rev, "NOAA APT", "note");
    CHECK_EQ(img.rev, 0u);
    CHECK_EQ(img.rgb.size(), size_t{0});
    CHECK_STR_EQ(img.note, "Preview geometry is unavailable.");

    const ImageData zero = remote::image_data_from_pixels(px, 0, 0, rev, "NOAA APT", "note");
    CHECK_EQ(zero.rgb.size(), size_t{0});
}

/* ===========================================================================
 * GeoTableData serialization (contract 5)
 * ===========================================================================*/

TEST(geotable_table_half_is_byte_identical_to_a_plain_table_panel) {
    /* The load-bearing property of the whole geotable upgrade: an app moving
     * from `table` to `geotable` must not move a single cell, or the browser
     * and the device screen start telling different stories. */
    TableData t;
    t.columns = {"Source", "Loc", "Hits", "Time"};
    t.rows = {{"N0CALL-9", "*", "3", "12:04:11"}, {"MB7U", "", "1", "12:04:19"}};

    remote::GeoTableData g;
    g.table = t;

    const std::string plain = remote::to_json(t).dump();
    const std::string geo = remote::to_json(g).dump();
    CHECK(has(geo, "\"table\":" + plain));
}

TEST(geotable_marker_omits_a_heading_that_was_never_reported) {
    remote::GeoTableData g;
    GeoTableMarker with_heading;
    with_heading.lat = 51.5;
    with_heading.lon = -0.12;
    with_heading.label = "A";
    with_heading.heading_deg = 41.0;
    with_heading.kind = "vessel";

    GeoTableMarker without;
    without.lat = 49.0;
    without.lon = -72.0;
    without.label = "B";

    g.markers = {with_heading, without};

    const std::string j = remote::to_json(g).dump();
    CHECK(has(j, "{\"lat\":51.5,\"lon\":-0.12,\"label\":\"A\",\"heading_deg\":41,\"kind\":\"vessel\"}"));
    /* No heading_deg and no kind on the second marker: a station with no
     * reported heading is not pointing due north. */
    CHECK(has(j, "{\"lat\":49,\"lon\":-72,\"label\":\"B\"}"));
}

TEST(geotable_with_no_positioned_entries_still_carries_an_empty_marker_list) {
    remote::GeoTableData g;
    g.table.columns = {"MMSI", "Name/Call"};
    g.table.rows = {{"244660320", "EVER GIVEN"}};

    const std::string j = remote::to_json(g).dump();
    /* An empty markers array, not a missing key: the renderer draws an empty
     * map rather than falling back to "this panel has no map". */
    CHECK(has(j, "\"map\":{\"markers\":[]}"));
    CHECK(has(j, "\"rows\":[[\"244660320\",\"EVER GIVEN\"]]"));
}

TEST(panel_payload_routes_the_new_kinds_and_names_them) {
    remote::PanelData p;
    p.kind = PanelKind::Image;
    p.image.width = 1;
    p.image.height = 1;
    CHECK_STR_EQ(remote::panel_kind_name(PanelKind::Image), "image");
    CHECK(has(remote::to_json(p).dump(), "\"kind\":\"image\",\"image\":{"));

    p = remote::PanelData{};
    p.kind = PanelKind::GeoTable;
    CHECK_STR_EQ(remote::panel_kind_name(PanelKind::GeoTable), "geotable");
    CHECK(has(remote::to_json(p).dump(), "\"kind\":\"geotable\",\"geotable\":{"));
}

TEST(panel_payload_threads_have_image_rev_only_to_the_image_kind) {
    remote::PanelData p;
    p.kind = PanelKind::Image;
    p.image.width = 1;
    p.image.height = 1;
    p.image.rev = 5;
    p.image.rgb = {9, 9, 9};

    CHECK(has(remote::panel_payload(p, 4).dump(), "data_b64"));
    CHECK(!has(remote::panel_payload(p, 5).dump(), "data_b64"));

    /* A geotable ignores it entirely rather than mangling anything. */
    remote::PanelData g;
    g.kind = PanelKind::GeoTable;
    CHECK_STR_EQ(remote::panel_payload(g, 0).dump(), remote::panel_payload(g, 99).dump());
}

/* ===========================================================================
 * AIS markers
 * ===========================================================================*/

/* ITU-R M.1371 numbers payload bits MSB-first across the octets, which is what
 * ais::Packet::read() returns. Same builder shape as tests/test_ais.cpp and
 * tests/test_provider_ais_ble.cpp, which keep their own file-static copies. */
class AisBits {
   public:
    void put(uint64_t value, size_t nbits) {
        for (size_t i = nbits; i > 0; --i)
            bits_.push_back(static_cast<uint8_t>((value >> (i - 1)) & 1));
    }

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

/* Message 1, position report, with the position and heading left to the caller
 * so the "reported" and "not reported" cases are the same code path.
 * `lat_raw`/`lon_raw` are the raw ITU fields in 1/10000 minute, two's
 * complement in 27 and 28 bits respectively. */
std::vector<uint8_t> ais_position_report(uint32_t mmsi,
                                         uint32_t lon_raw,
                                         uint32_t lat_raw,
                                         uint32_t true_heading) {
    AisBits b;
    b.put(1, 6);              /*   0 message id       */
    b.put(0, 2);              /*   6 repeat           */
    b.put(mmsi, 30);          /*   8 MMSI             */
    b.put(0, 4);              /*  38 nav status       */
    b.put(0, 8);              /*  42 rate of turn     */
    b.put(74, 10);            /*  50 speed over grnd  */
    b.put(1, 1);              /*  60 position acc.    */
    b.put(lon_raw, 28);       /*  61 longitude        */
    b.put(lat_raw, 27);       /*  89 latitude         */
    b.put(1234, 12);          /* 116 course over grnd */
    b.put(true_heading, 9);   /* 128 true heading     */
    b.put(30, 6);             /* 137 UTC second       */
    b.put(0, 2);              /* 143 manoeuvre        */
    b.put(0, 3);              /* 145 spare            */
    b.put(0, 1);              /* 148 RAIM             */
    b.put(0x7FFFF, 19);       /* 149 radio status     */
    CHECK_EQ(b.bit_count(), size_t{168});
    return b.bytes();
}

/* Message 5, static and voyage data: a name and a call sign, and no position at
 * all. This is the entry that must reach the table and not the map. */
std::vector<uint8_t> ais_static_report(uint32_t mmsi,
                                       const std::string& call_sign,
                                       const std::string& name) {
    AisBits b;
    b.put(5, 6);
    b.put(0, 2);
    b.put(mmsi, 30);
    b.put(0, 2);
    b.put(9134567, 30);
    b.put_text(call_sign, 7);
    b.put_text(name, 20);
    b.put(70, 8);
    b.put(90, 9);
    b.put(30, 9);
    b.put(10, 6);
    b.put(10, 6);
    b.put(1, 4);
    b.put(5, 4);
    b.put(17, 5);
    b.put(9, 5);
    b.put(30, 6);
    b.put(64, 8);
    b.put_text("", 20);
    b.put(0, 1);
    b.put(0, 1);
    CHECK_EQ(b.bit_count(), size_t{424});
    return b.bytes();
}

/* The app's own path from a frame to an entry: ui::on_packet() finds or creates
 * by MMSI, then AISRecentEntry::update() applies the fields — the two calls
 * AISAppView::on_packet() makes (ais_app.cpp). */
void ais_apply(app::AISRecentEntries& entries, const std::vector<uint8_t>& payload) {
    const ais::Packet packet = ais::Packet::from_bits(ais::build_packet_bits(payload));
    CHECK(packet.is_valid());
    auto& entry = ui::on_packet(entries, packet.source_id());
    entry.update(packet);
}

/* 51.5 N / 0.12 W in 1/10000 minute: degrees * 600000. The longitude is
 * negative, so it goes on the wire as a 28-bit two's complement field. */
constexpr uint32_t kLatRaw51_5N = 30900000u;
constexpr uint32_t kLonRaw0_12W = 268435456u - 72000u;

}  // namespace

TEST(ais_marker_uses_the_apps_own_degrees_conversion) {
    app::AISRecentEntries entries;
    ais_apply(entries, ais_position_report(232003812u, kLonRaw0_12W, kLatRaw51_5N, 41u));

    const auto markers = remote::ais_geo_markers(entries, 200);
    CHECK_EQ(markers.size(), size_t{1});
    /* ais::format::latlon_float()'s (v*5/3)/1e6, which is what the app feeds its
     * own ui::GeoMarker. A second conversion written here would be a second
     * thing to keep in step. */
    CHECK_NEAR(markers[0].lat, 51.5, 1e-4);
    CHECK_NEAR(markers[0].lon, -0.12, 1e-4);
    CHECK(markers[0].heading_deg.has_value());
    CHECK_NEAR(*markers[0].heading_deg, 41.0, 1e-9);
    CHECK_STR_EQ(markers[0].kind, "vessel");
}

TEST(ais_ship_that_has_sent_no_position_is_in_the_table_and_not_on_the_map) {
    /* A message 5 carries a name and a call sign and no position whatsoever.
     * AISPosition's default latitude/longitude are the 91/181-degree "not
     * available" sentinels, which is emphatically not 0N 0E. */
    app::AISRecentEntries entries;
    ais_apply(entries, ais_static_report(244660320u, "PBRV", "EVER GIVEN"));

    const TableData t = remote::ais_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][1], "EVER GIVEN");

    CHECK_EQ(remote::ais_geo_markers(entries, 200).size(), size_t{0});
}

TEST(ais_marker_omits_a_heading_the_ship_reported_as_not_available) {
    /* 511 is ITU-R M.1371's "not available", and the value AISPosition is
     * constructed with. */
    app::AISRecentEntries entries;
    ais_apply(entries, ais_position_report(232003812u, kLonRaw0_12W, kLatRaw51_5N, 511u));

    const auto markers = remote::ais_geo_markers(entries, 200);
    CHECK_EQ(markers.size(), size_t{1});
    CHECK(!markers[0].heading_deg.has_value());
}

TEST(ais_marker_label_is_the_rows_own_name_cell_then_the_mmsi) {
    app::AISRecentEntries entries;
    /* Position first, then the static report that names her: the marker and the
     * table row must name the same ship. */
    ais_apply(entries, ais_position_report(244660320u, kLonRaw0_12W, kLatRaw51_5N, 41u));
    ais_apply(entries, ais_static_report(244660320u, "PBRV", "EVER GIVEN"));

    const TableData t = remote::ais_table_data(entries);
    const auto markers = remote::ais_geo_markers(entries, 200);
    CHECK_EQ(markers.size(), size_t{1});
    CHECK_STR_EQ(markers[0].label, "EVER GIVEN");
    CHECK_STR_EQ(markers[0].label, t.rows[0][1]);
}

TEST(ais_marker_falls_back_to_the_padded_mmsi_for_an_unnamed_ship) {
    /* A vessel heard only through a position report has broadcast no name and
     * no call sign; the MMSI is the entry's own key and can never be absent. */
    app::AISRecentEntries entries;
    ais_apply(entries, ais_position_report(1234567u, kLonRaw0_12W, kLatRaw51_5N, 41u));

    const auto markers = remote::ais_geo_markers(entries, 200);
    CHECK_EQ(markers.size(), size_t{1});
    CHECK_STR_EQ(markers[0].label, "001234567");
}

TEST(ais_markers_respect_the_same_ceiling_the_table_does) {
    app::AISRecentEntries entries;
    for (uint32_t i = 0; i < 5; i++)
        ais_apply(entries, ais_position_report(200000000u + i, kLonRaw0_12W, kLatRaw51_5N, 41u));

    /* A marker referring to a row the table never sent would be unreachable
     * from the browser's table half. */
    CHECK_EQ(remote::ais_geo_markers(entries, 3).size(), size_t{3});
}

/* ===========================================================================
 * APRS markers
 * ===========================================================================*/

namespace {

/* One shifted AX.25 address octet group: (0x60 | ssid<<1 | last), preceded by
 * the six callsign characters shifted left one bit. Same construction as
 * tests/test_aprs_rx.cpp, which keeps its own file-static copy. */
std::vector<uint8_t> ax25_address(const std::string& call, uint8_t ssid, bool last) {
    std::vector<uint8_t> out;
    std::string c = call;
    c.resize(6, ' ');
    for (size_t i = 0; i < 6; i++) out.push_back(static_cast<uint8_t>(c[i] << 1));
    out.push_back(static_cast<uint8_t>(0x60 | ((ssid & 0x0F) << 1) | (last ? 1 : 0)));
    return out;
}

/* A complete UI frame plus the FCS the receiver would have checked, built with
 * the app's own app::ax25_fcs(). */
app::AprsPacket aprs_frame(const std::string& source, const std::string& information) {
    std::vector<uint8_t> f;
    const auto dest = ax25_address("APRS", 0, false);
    f.insert(f.end(), dest.begin(), dest.end());
    const auto src = ax25_address(source, 0, true);
    f.insert(f.end(), src.begin(), src.end());
    f.push_back(0x03); /* UI control */
    f.push_back(0xF0); /* PID: no layer 3 */
    for (char c : information) f.push_back(static_cast<uint8_t>(c));

    const uint16_t fcs = app::ax25_fcs(f.data(), f.size());
    f.push_back(static_cast<uint8_t>(fcs & 0xFF));
    f.push_back(static_cast<uint8_t>((fcs >> 8) & 0xFF));

    app::AprsPacket p;
    p.set_bytes(f.data(), f.size());
    p.set_valid_checksum(app::ax25_frame_valid(f.data(), f.size()));
    return p;
}

/* AprsTableView::on_packet()'s body (ui_aprs_rx.cpp), minus the two lines that
 * poke at widgets. The position gate is the part under test, so it is the app's
 * own code path that has to fill the entry. */
void aprs_apply(app::AprsRecentEntries& entries,
                const app::AprsPacket& packet,
                const std::string& timestamp) {
    auto& entry = ui::on_packet(entries, packet.get_source());
    entry.inc_hit();
    entry.source_formatted = packet.get_source_formatted();
    entry.time_string = timestamp;
    entry.info_string = packet.get_stream_text();
    if (packet.has_position()) {
        entry.has_position = true;
        entry.pos = packet.get_position();
    }
}

}  // namespace

TEST(aprs_marker_carries_the_position_the_app_decoded) {
    app::AprsRecentEntries entries;
    aprs_apply(entries, aprs_frame("N0CALL", "!4903.50N/07201.75W-Test"), "12:04:11");

    const auto markers = remote::aprs_geo_markers(entries, 200);
    CHECK_EQ(markers.size(), size_t{1});
    /* 49 deg 03.50' N, 72 deg 01.75' W, already in decimal degrees on
     * AprsPosition — nothing is converted here. */
    CHECK_NEAR(markers[0].lat, 49.0 + 3.50 / 60.0, 1e-5);
    CHECK_NEAR(markers[0].lon, -(72.0 + 1.75 / 60.0), 1e-5);
    CHECK_STR_EQ(markers[0].label, "N0CALL");
    CHECK_STR_EQ(markers[0].kind, "station");
    /* An APRS entry has no heading field at all. */
    CHECK(!markers[0].heading_deg.has_value());
}

TEST(aprs_station_heard_only_through_a_status_packet_gets_no_marker) {
    app::AprsRecentEntries entries;
    aprs_apply(entries, aprs_frame("MB7U", ">Net control, back at 1900"), "12:05:02");

    const TableData t = remote::aprs_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][0], "MB7U");
    /* The Loc column is blank for exactly the same reason the marker is
     * missing: the app never set has_position. */
    CHECK_STR_EQ(t.rows[0][1], "");

    CHECK_EQ(remote::aprs_geo_markers(entries, 200).size(), size_t{0});
}

TEST(aprs_markers_cover_only_the_positioned_subset_of_the_table) {
    app::AprsRecentEntries entries;
    aprs_apply(entries, aprs_frame("N0CALL", "!4903.50N/07201.75W-Test"), "12:04:11");
    aprs_apply(entries, aprs_frame("MB7U", ">status only"), "12:05:02");

    const TableData t = remote::aprs_table_data(entries);
    const auto markers = remote::aprs_geo_markers(entries, 200);
    CHECK_EQ(t.rows.size(), size_t{2});
    CHECK_EQ(markers.size(), size_t{1});
    CHECK_STR_EQ(markers[0].label, "N0CALL");
}

/* ===========================================================================
 * WardriveMap markers
 * ===========================================================================*/

TEST(wardrive_markers_carry_every_observation_with_no_invented_heading) {
    using app::wardrive::Observation;
    const std::vector<Observation> obs{
        {"2026-08-13 09:00:00", 51.5074f, -0.1278f, 2412000000ull, "home-ap"},
        {"2026-08-13 09:04:00", -33.8688f, 151.2093f, 868000000ull, "sydney"},
    };

    const MapData md = remote::wardrive_map_data(obs);
    CHECK_EQ(md.markers.size(), size_t{2});
    CHECK_NEAR(md.markers[0].lat, 51.5074, 1e-4);
    CHECK_NEAR(md.markers[0].lon, -0.1278, 1e-4);
    CHECK_STR_EQ(md.markers[0].label, "home-ap");
    CHECK_STR_EQ(md.markers[1].label, "sydney");

    /* Upstream builds these markers with ui::invalid_angle: a wardrive capture
     * is a place, not a moving target. heading_deg must be absent on the wire,
     * not 0. */
    CHECK(!md.markers[0].heading_deg.has_value());
    const std::string j = remote::to_json(md).dump();
    CHECK(!has(j, "heading_deg"));
    CHECK(has(j, "\"label\":\"home-ap\""));
}

TEST(wardrive_marker_label_is_left_empty_when_the_log_had_no_name) {
    using app::wardrive::Observation;
    const std::vector<Observation> obs{{"", 51.5f, -0.1f, 0ull, ""}};

    const MapData md = remote::wardrive_map_data(obs);
    CHECK_EQ(md.markers.size(), size_t{1});
    /* Not substituted with the frequency: a label that reads like a name and is
     * actually something else is worse than no label. */
    CHECK_STR_EQ(md.markers[0].label, "");
}

/* ===========================================================================
 * End to end through AppBridge
 *
 * Everything above drives a helper directly, which pins the data rules but
 * never runs a provider — so the part that finds the app in the first place,
 * and the app id it registers under, were unexercised. A wrong id is the single
 * most likely way this silently does nothing: the provider never fires, the
 * portal keeps serving its placeholder card, and every assertion above still
 * passes.
 * ===========================================================================*/

namespace {

/* Same harness shape as tests/test_provider_ais_ble.cpp's, kept here rather
 * than shared because appending to another agent's test file is how two
 * change-sets collide. Nothing here opens a device: on_show() calls
 * receiver.start(), which fails at start_rx() on a closed radio and returns
 * false without spawning a DSP thread. */
struct BridgeHarness {
    radio::UsrpRadio radio{};
    audio::AudioOut audio{};
    radio::ReceiverModel receiver{radio, audio};
    ui::NavigationView nav{{0, 0, 240, 304}};

    radio::RadioDevice* saved_radio{app::globals().radio};
    radio::ReceiverModel* saved_receiver{app::globals().receiver};
    ui::NavigationView* saved_nav{app::globals().nav};

    BridgeHarness() {
        app::globals().radio = &radio;
        app::globals().receiver = &receiver;
        app::globals().nav = &nav;

        nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304})); /* root */
        nav.service();
    }

    ~BridgeHarness() {
        /* AppBridge is a process-global singleton, so a test that leaves it
         * believing one of these apps is open hands that state to every later
         * test in the binary — and to every -count= rerun. */
        remote::AppBridge::instance().request_home();
        remote::AppBridge::instance().drain_launch_queue();
        nav.service();

        app::globals().radio = saved_radio;
        app::globals().receiver = saved_receiver;
        app::globals().nav = saved_nav;
    }

    void launch(const std::string& app_id) {
        remote::AppBridge::instance().request_launch(app_id);
        remote::AppBridge::instance().drain_launch_queue();
        nav.service();
    }

    std::string panel() {
        remote::AppBridge::instance().refresh();
        return remote::AppBridge::instance().panel_json();
    }
};

}  // namespace

TEST(apt_provider_publishes_an_image_panel_when_the_app_is_open) {
    BridgeHarness h;
    /* The id has to be the one src/apps/ui_noaaapt_rx.cpp registers. */
    h.launch("noaaapt_rx");
    CHECK_EQ(h.nav.depth(), size_t{2});

    const std::string p = h.panel();
    CHECK(has(p, "\"app_id\":\"noaaapt_rx\""));
    CHECK(has(p, "\"panel_kind\":\"image\""));
    /* No device, so no pass decoded: rev 0, no pixels, and an honest note —
     * never a black 240x232 rectangle dressed up as a satellite image. */
    CHECK(has(p, "\"rev\":0"));
    CHECK(has(p, "\"note\":\"No image decoded yet.\""));
    CHECK(!has(p, "data_b64"));
    /* The geometry the app really keeps: 240 wide, and the canvas rect's rows. */
    CHECK(has(p, "\"width\":240"));
    CHECK(has(p, "\"height\":232"));
}

TEST(wefax_provider_publishes_an_image_panel_when_the_app_is_open) {
    BridgeHarness h;
    h.launch("wefax_rx");
    CHECK_EQ(h.nav.depth(), size_t{2});

    const std::string p = h.panel();
    CHECK(has(p, "\"app_id\":\"wefax_rx\""));
    CHECK(has(p, "\"panel_kind\":\"image\""));
    CHECK(has(p, "\"rev\":0"));
    CHECK(has(p, "\"note\":\"No image decoded yet.\""));
    CHECK(!has(p, "data_b64"));
    CHECK(has(p, "\"width\":240"));
    CHECK(has(p, "\"height\":214"));
}

TEST(sstv_provider_publishes_an_image_panel_when_the_app_is_open) {
    BridgeHarness h;
    h.launch("sstvrx");
    CHECK_EQ(h.nav.depth(), size_t{2});

    const std::string p = h.panel();
    CHECK(has(p, "\"app_id\":\"sstvrx\""));
    CHECK(has(p, "\"panel_kind\":\"image\""));
    CHECK(has(p, "\"rev\":0"));
    CHECK(has(p, "\"note\":\"No image decoded yet.\""));
    CHECK(!has(p, "data_b64"));
    CHECK(has(p, "\"width\":240"));
    CHECK(has(p, "\"height\":202"));
}

TEST(image_providers_say_so_honestly_when_the_app_is_not_on_the_stack) {
    BridgeHarness h;
    h.launch("noaaapt_rx");

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

    const std::string p = h.panel();
    CHECK(has(p, "\"panel_kind\":\"screen\""));
    CHECK(has(p, "Home -- no app is open."));
    /* The stale id is what this change fixed; pin it, not just the text. */
    CHECK(has(p, "\"app_id\":\"\""));
    CHECK(!has(p, "\"format\""));
}

TEST(image_provider_survives_the_operator_drilling_into_a_sub_view) {
    BridgeHarness h;
    h.launch("sstvrx");
    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{3});

    /* Without the at_depth() walk the browser would go blank for as long as the
     * operator left the sub-view open. */
    CHECK(has(h.panel(), "\"panel_kind\":\"image\""));
}

TEST(wardrive_provider_publishes_a_map_panel_of_the_observations_it_holds) {
    BridgeHarness h;
    /* The id has to be the one src/apps/ui_wardrivemap.cpp registers. */
    h.launch("wardrivemap");
    CHECK_EQ(h.nav.depth(), size_t{2});

    /* The view loads the operator's own log in its constructor, so the count on
     * this machine is not knowable; seed a known list through the app's own
     * public setter (which applies upstream's geotag filter) instead of
     * asserting against whatever happens to be on disk. */
    auto* view = dynamic_cast<app::WardriveMapView*>(h.nav.top());
    CHECK(view != nullptr);
    if (view != nullptr) {
        view->set_observations({
            {"2026-08-13 09:00:00", 51.5074f, -0.1278f, 2412000000ull, "home-ap"},
            {"", 0.0f, 0.0f, 0ull, "no-fix"}, /* dropped by the app's own filter */
        });
    }

    const std::string p = h.panel();
    CHECK(has(p, "\"app_id\":\"wardrivemap\""));
    CHECK(has(p, "\"panel_kind\":\"map\""));
    CHECK(has(p, "\"label\":\"home-ap\""));
    /* The observation with no fix reached neither the app's list nor the map. */
    CHECK(!has(p, "no-fix"));
    CHECK(!has(p, "heading_deg"));
}

TEST(wardrive_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    BridgeHarness h;
    h.launch("wardrivemap");
    h.nav.pop_to_root();
    h.nav.service();

    const std::string p = h.panel();
    CHECK(has(p, "\"panel_kind\":\"screen\""));
    CHECK(has(p, "Home -- no app is open."));
    /* The stale id is what this change fixed; pin it, not just the text. */
    CHECK(has(p, "\"app_id\":\"\""));
    CHECK(!has(p, "\"markers\""));
}

TEST(ais_provider_publishes_a_geotable_with_the_table_it_always_published) {
    BridgeHarness h;
    h.launch("ais");
    CHECK_EQ(h.nav.depth(), size_t{2});

    const std::string p = h.panel();
    CHECK(has(p, "\"app_id\":\"ais\""));
    /* Upgraded from "table"; the columns inside are unchanged. */
    CHECK(has(p, "\"panel_kind\":\"geotable\""));
    CHECK(has(p, "\"table\":{\"columns\":[\"MMSI\",\"Name/Call\"],\"rows\":[]}"));
    CHECK(has(p, "\"map\":{\"markers\":[]}"));
}

TEST(aprs_provider_publishes_a_geotable_with_the_table_it_always_published) {
    BridgeHarness h;
    h.launch("aprsrx");
    CHECK_EQ(h.nav.depth(), size_t{2});

    const std::string p = h.panel();
    CHECK(has(p, "\"app_id\":\"aprsrx\""));
    CHECK(has(p, "\"panel_kind\":\"geotable\""));
    CHECK(has(p,
              "\"table\":{\"columns\":[\"Source\",\"Loc\",\"Hits\",\"Time\"],\"rows\":[]}"));
    CHECK(has(p, "\"map\":{\"markers\":[]}"));
}
