/*
 * mayhem-b200 — geographic map tests.
 *
 * The interesting part of a map widget is the projection: everything else is
 * pixels on a screen nobody can assert against. Expected values here come from
 * the standard Web Mercator pixel formulas
 *
 *     x = (lon + 180) / 360 * 256 * 2^z
 *     y = (1 - ln(tan(lat) + sec(lat)) / pi) / 2 * 256 * 2^z
 *
 * evaluated independently, and — for the world_map.bin layout — from upstream's
 * GeoMap::init / lat_lon_to_map_pixel, which define that file format.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "display.hpp"
#include "file_path.hpp"
#include "ui_geomap.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

/* The rect every widget test uses: full width, below the status bar, square so
 * that the horizontal and vertical culling limits are the same number. */
constexpr ui::Rect map_rect{0, 16, 240, 240};
constexpr int center_x = 120;
constexpr int center_y = 136;

ui::GeoMarker make_marker(float lat, float lon, uint16_t angle = ui::invalid_angle,
                          const char* tag = "") {
    ui::GeoMarker m{};
    m.lat = lat;
    m.lon = lon;
    m.angle = angle;
    m.tag = tag;
    return m;
}

std::filesystem::path temp_map_path(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

void write_map_file(const std::filesystem::path& path,
                    uint16_t width,
                    uint16_t height,
                    const std::vector<uint16_t>& pixels) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(&width), sizeof(width));
    out.write(reinterpret_cast<const char*>(&height), sizeof(height));
    if (!pixels.empty())
        out.write(reinterpret_cast<const char*>(pixels.data()),
                  static_cast<std::streamsize>(pixels.size() * sizeof(uint16_t)));
}

}  // namespace

/* --- Projection primitives ------------------------------------------------- */

TEST(mercator_world_size_doubles_each_zoom) {
    CHECK_NEAR(ui::mercator::world_size(0), 256.0, 1e-9);
    CHECK_NEAR(ui::mercator::world_size(1), 512.0, 1e-9);
    CHECK_NEAR(ui::mercator::world_size(8), 65536.0, 1e-9);

    for (int z = 0; z < 19; z++)
        CHECK_NEAR(ui::mercator::world_size(z + 1), 2.0 * ui::mercator::world_size(z), 1e-6);

    /* Out-of-range zooms clamp rather than producing a degenerate world. */
    CHECK_EQ(ui::mercator::clamp_zoom(-7), 0);
    CHECK_EQ(ui::mercator::clamp_zoom(99), 19);
    CHECK_NEAR(ui::mercator::world_size(-7), 256.0, 1e-9);
}

TEST(mercator_pixels_per_degree_of_longitude) {
    /* 65536 px of world / 360 degrees. */
    CHECK_NEAR(ui::mercator::pixels_per_degree_lon(8), 182.04444444444445, 1e-9);
    CHECK_NEAR(ui::mercator::pixels_per_degree_lon(5), 22.755555555555556, 1e-9);
    /* Longitude is linear: the scale is the same everywhere on the x axis. */
    const double ppd = ui::mercator::pixels_per_degree_lon(8);
    CHECK_NEAR(ui::mercator::lon_to_world_x(1.0, 8) - ui::mercator::lon_to_world_x(0.0, 8),
               ppd, 1e-6);
    CHECK_NEAR(
        ui::mercator::lon_to_world_x(-150.0, 8) - ui::mercator::lon_to_world_x(-151.0, 8),
        ppd, 1e-6);
}

TEST(mercator_longitude_maps_across_the_world) {
    const double world = ui::mercator::world_size(3);
    CHECK_NEAR(ui::mercator::lon_to_world_x(-180.0, 3), 0.0, 1e-9);
    CHECK_NEAR(ui::mercator::lon_to_world_x(0.0, 3), world / 2.0, 1e-9);
    CHECK_NEAR(ui::mercator::lon_to_world_x(90.0, 3), world * 0.75, 1e-9);
    /* +180 is the same meridian as -180 and folds onto column zero. */
    CHECK_NEAR(ui::mercator::lon_to_world_x(180.0, 3), 0.0, 1e-9);
}

TEST(mercator_latitude_scaling_matches_the_formula) {
    /* Reference values from y = (1 - ln(tan(lat) + sec(lat)) / pi) / 2 * 256. */
    CHECK_NEAR(ui::mercator::lat_to_world_y(0.0, 0), 128.0, 1e-9);
    CHECK_NEAR(ui::mercator::lat_to_world_y(45.0, 0), 92.08960945029247, 1e-9);
    CHECK_NEAR(ui::mercator::lat_to_world_y(-45.0, 0), 163.91039054970753, 1e-9);
    CHECK_NEAR(ui::mercator::lat_to_world_y(60.0, 0), 74.34230806029022, 1e-9);
}

TEST(mercator_latitude_is_symmetric_about_the_equator) {
    for (int z : {0, 5, 12}) {
        const double equator = ui::mercator::lat_to_world_y(0.0, z);
        CHECK_NEAR(equator, ui::mercator::world_size(z) / 2.0, 1e-9);

        for (double lat : {1.0, 12.5, 45.0, 70.0, 85.0}) {
            const double north = ui::mercator::lat_to_world_y(lat, z) - equator;
            const double south = ui::mercator::lat_to_world_y(-lat, z) - equator;
            /* Same distance, opposite sign; north is up, so its offset is
             * negative in screen coordinates. */
            CHECK(north < 0.0);
            CHECK(south > 0.0);
            CHECK_NEAR(north, -south, 1e-9);
        }
    }
}

TEST(mercator_latitude_stretches_away_from_the_equator) {
    /* The whole point of Mercator: a degree of latitude covers more pixels the
     * further from the equator you are. A linear projection would fail this. */
    const int z = 8;
    const double equator = ui::mercator::lat_to_world_y(0.0, z);
    const double at_45 = equator - ui::mercator::lat_to_world_y(45.0, z);
    const double at_60 = equator - ui::mercator::lat_to_world_y(60.0, z);

    CHECK_NEAR(at_45, 9193.059980725127, 1e-6);
    CHECK(at_60 > (60.0 / 45.0) * at_45);

    /* At the equator, though, the latitude scale equals the longitude scale. */
    const double per_hundredth = equator - ui::mercator::lat_to_world_y(0.01, z);
    CHECK_NEAR(per_hundredth, 0.01 * ui::mercator::pixels_per_degree_lon(z), 1e-6);
}

TEST(mercator_clamps_beyond_the_projection_limit) {
    /* atan(sinh(pi)) in degrees — where the square world runs out. */
    CHECK_NEAR(ui::mercator::latitude_limit, 85.0511287798066, 1e-9);

    CHECK_NEAR(ui::mercator::clamp_latitude(90.0), ui::mercator::latitude_limit, 1e-12);
    CHECK_NEAR(ui::mercator::clamp_latitude(-90.0), -ui::mercator::latitude_limit, 1e-12);
    CHECK_NEAR(ui::mercator::clamp_latitude(12.0), 12.0, 1e-12);

    /* The poles therefore land on the edges of the world, not outside it. */
    CHECK_NEAR(ui::mercator::lat_to_world_y(90.0, 0), 0.0, 1e-6);
    CHECK_NEAR(ui::mercator::lat_to_world_y(-90.0, 0), 256.0, 1e-6);
}

TEST(mercator_normalize_longitude_folds_into_half_open_range) {
    CHECK_NEAR(ui::mercator::normalize_longitude(0.0), 0.0, 1e-12);
    CHECK_NEAR(ui::mercator::normalize_longitude(179.0), 179.0, 1e-12);
    CHECK_NEAR(ui::mercator::normalize_longitude(190.0), -170.0, 1e-12);
    CHECK_NEAR(ui::mercator::normalize_longitude(-190.0), 170.0, 1e-12);
    CHECK_NEAR(ui::mercator::normalize_longitude(360.0), 0.0, 1e-12);
    CHECK_NEAR(ui::mercator::normalize_longitude(-360.0), 0.0, 1e-12);
    /* Both antimeridian spellings resolve to the same value. */
    CHECK_NEAR(ui::mercator::normalize_longitude(180.0), -180.0, 1e-12);
    CHECK_NEAR(ui::mercator::normalize_longitude(-180.0), -180.0, 1e-12);
    /* The difference across the antimeridian is small, not nearly a whole world. */
    CHECK_NEAR(ui::mercator::normalize_longitude(-179.0 - 179.0), 2.0, 1e-12);
    CHECK_NEAR(ui::mercator::normalize_longitude(179.0 - -179.0), -2.0, 1e-12);
}

TEST(mercator_round_trips) {
    for (int z : {0, 5, 14}) {
        for (double lon : {-180.0, -73.5, 0.0, 12.25, 179.9}) {
            const double x = ui::mercator::lon_to_world_x(lon, z);
            CHECK_NEAR(ui::mercator::world_x_to_lon(x, z), lon, 1e-6);
        }
        for (double lat : {-84.0, -33.87, 0.0, 51.5, 84.0}) {
            const double y = ui::mercator::lat_to_world_y(lat, z);
            CHECK_NEAR(ui::mercator::world_y_to_lat(y, z), lat, 1e-6);
        }
    }
}

TEST(mercator_ground_resolution) {
    /* 40075016.686 m of equator over 256 px at zoom 0. */
    CHECK_NEAR(ui::mercator::meters_per_pixel(0.0, 0), 156543.03392804097, 1e-6);
    /* Halves with every zoom level. */
    CHECK_NEAR(ui::mercator::meters_per_pixel(0.0, 1),
               ui::mercator::meters_per_pixel(0.0, 0) / 2.0, 1e-9);
    /* And shrinks with cos(latitude): 60 degrees north is exactly half. */
    CHECK_NEAR(ui::mercator::meters_per_pixel(60.0, 4),
               ui::mercator::meters_per_pixel(0.0, 4) / 2.0, 1e-6);
}

/* --- Widget projection ----------------------------------------------------- */

TEST(geomap_origin_maps_to_the_centre_of_the_view) {
    ui::GeoMap map{map_rect};
    map.set_lat_lon(0.0f, 0.0f);

    const auto offset = map.offset_from_center(0.0, 0.0);
    CHECK_NEAR(offset.x, 0.0, 1e-9);
    CHECK_NEAR(offset.y, 0.0, 1e-9);

    const auto p = map.map_to_screen(0.0, 0.0);
    CHECK_EQ(p.x(), center_x);
    CHECK_EQ(p.y(), center_y);

    /* Centring somewhere else moves the origin off the centre by the projected
     * distance, rather than leaving it pinned there. */
    map.set_zoom(8);
    map.set_lat_lon(0.0f, 1.0f);
    const auto moved = map.map_to_screen(0.0, 0.0);
    CHECK_EQ(moved.x(), center_x - 182);
    CHECK_EQ(moved.y(), center_y);
}

TEST(geomap_one_degree_east_is_the_expected_pixel_count) {
    ui::GeoMap map{map_rect};
    map.set_lat_lon(0.0f, 0.0f);

    map.set_zoom(8);
    CHECK_NEAR(map.offset_from_center(0.0, 1.0).x, 182.04444444444445, 1e-9);
    CHECK_NEAR(map.offset_from_center(0.0, 1.0).y, 0.0, 1e-9);
    CHECK_EQ(map.map_to_screen(0.0, 1.0).x(), center_x + 182);

    /* One degree west is the mirror image. */
    CHECK_NEAR(map.offset_from_center(0.0, -1.0).x, -182.04444444444445, 1e-9);

    /* Zooming out one level halves it. */
    map.set_zoom(7);
    CHECK_NEAR(map.offset_from_center(0.0, 1.0).x, 91.02222222222223, 1e-9);

    map.set_zoom(5);
    CHECK_NEAR(map.offset_from_center(0.0, 1.0).x, 22.755555555555556, 1e-9);
    CHECK_EQ(map.map_to_screen(0.0, 1.0).x(), center_x + 23);
}

TEST(geomap_one_degree_north_uses_the_mercator_scale) {
    ui::GeoMap map{map_rect};
    map.set_zoom(8);
    map.set_lat_lon(0.0f, 0.0f);

    /* 5 degrees north of the equator, from the reference formula. Negative
     * because north is up the screen. */
    CHECK_NEAR(map.offset_from_center(5.0, 0.0).y, -911.3797163780146, 1e-6);
    CHECK_NEAR(map.offset_from_center(-5.0, 0.0).y, 911.3797163780146, 1e-6);

    /* Same 5 degrees measured from 60 north covers appreciably more pixels. */
    map.set_lat_lon(60.0f, 0.0f);
    const double from_60 = std::fabs(map.offset_from_center(65.0, 0.0).y);
    CHECK(from_60 > 911.3797163780146 * 1.5);
}

TEST(geomap_screen_to_map_inverts_map_to_screen) {
    ui::GeoMap map{map_rect};
    map.set_zoom(10);
    map.set_lat_lon(51.5f, -0.12f);

    for (const auto& probe : {ui::Point{10, 30}, ui::Point{120, 136}, ui::Point{230, 250}}) {
        double lat = 0.0;
        double lon = 0.0;
        map.screen_to_map(probe, lat, lon);
        const auto back = map.map_to_screen(lat, lon);
        CHECK_EQ(back.x(), probe.x());
        CHECK_EQ(back.y(), probe.y());
    }
}

/* --- Marker culling -------------------------------------------------------- */

TEST(geomap_culls_markers_outside_the_viewport) {
    ui::GeoMap map{map_rect};
    map.set_zoom(8);  /* 182.04 px per degree; half the view is 120 px */
    map.set_lat_lon(0.0f, 0.0f);

    /* 0.65 deg -> 118.3 px: inside. */
    CHECK(map.is_visible(0.0, 0.65));
    CHECK_EQ(map.store_marker(make_marker(0.0f, 0.65f)), ui::MARKER_STORED);

    /* 0.66 deg -> 120.1 px: just outside, and refused rather than drawn on the
     * edge of a neighbouring view. */
    CHECK(!map.is_visible(0.0, 0.66));
    CHECK_EQ(map.store_marker(make_marker(0.0f, 0.66f)), ui::MARKER_NOT_STORED);

    /* Far away in both axes. */
    CHECK(!map.is_visible(0.0, 5.0));
    CHECK(!map.is_visible(5.0, 0.0));
    CHECK_EQ(map.store_marker(make_marker(0.0f, 5.0f)), ui::MARKER_NOT_STORED);
    CHECK_EQ(map.store_marker(make_marker(5.0f, 0.0f)), ui::MARKER_NOT_STORED);

    /* Only the one that fitted was kept. */
    CHECK_EQ(map.marker_count(), size_t{1});

    /* A margin widens the test without changing what gets stored. */
    CHECK(map.is_visible(0.0, 0.66, 10));

    /* The invalid-position sentinel is never visible. */
    CHECK(!map.is_visible(ui::invalid_lat_lon, ui::invalid_lat_lon));
    CHECK_EQ(map.store_marker(make_marker(ui::invalid_lat_lon, ui::invalid_lat_lon)),
             ui::MARKER_NOT_STORED);
}

TEST(geomap_culling_follows_the_zoom) {
    ui::GeoMap map{map_rect};
    map.set_lat_lon(0.0f, 0.0f);

    /* At zoom 8 a marker two degrees away is 364 px off screen. */
    map.set_zoom(8);
    CHECK(!map.is_visible(0.0, 2.0));

    /* Zoom out far enough and the same marker comes into view. */
    map.set_zoom(5);
    CHECK(map.is_visible(0.0, 2.0));
    CHECK_EQ(map.store_marker(make_marker(0.0f, 2.0f)), ui::MARKER_STORED);
}

TEST(geomap_marker_list_is_bounded) {
    ui::GeoMap map{map_rect};
    map.set_zoom(5);
    map.set_lat_lon(0.0f, 0.0f);

    for (int i = 0; i < ui::GeoMap::NumMarkerListElements; i++) {
        const float lon = 0.01f * static_cast<float>(i);
        CHECK_EQ(map.store_marker(make_marker(0.0f, lon)), ui::MARKER_STORED);
    }
    CHECK_EQ(map.marker_count(), static_cast<size_t>(ui::GeoMap::NumMarkerListElements));

    /* Full, but the marker is on screen — a different answer from culled, so
     * the caller can tell "not shown" from "no room". */
    CHECK_EQ(map.store_marker(make_marker(0.0f, 0.5f)), ui::MARKER_LIST_FULL);
    CHECK_EQ(map.marker_count(), static_cast<size_t>(ui::GeoMap::NumMarkerListElements));

    map.clear_markers();
    CHECK_EQ(map.marker_count(), size_t{0});
    CHECK_EQ(map.store_marker(make_marker(0.0f, 0.5f)), ui::MARKER_STORED);
}

TEST(geomap_marker_keeps_heading_and_label) {
    ui::GeoMap map{map_rect};
    map.set_zoom(5);
    map.set_lat_lon(0.0f, 0.0f);

    CHECK_EQ(map.store_marker(make_marker(0.1f, 0.2f, 270, "BAW117")), ui::MARKER_STORED);
    CHECK_EQ(map.marker(0).angle, uint16_t{270});
    CHECK_STR_EQ(map.marker(0).tag, "BAW117");
    CHECK_NEAR(map.marker(0).lat, 0.1, 1e-6);
}

/* --- Longitude wrapping ---------------------------------------------------- */

TEST(geomap_marker_across_the_antimeridian_stays_on_the_right_side) {
    ui::GeoMap map{map_rect};
    map.set_zoom(5);  /* 22.7556 px per degree */
    map.set_lat_lon(0.0f, 179.0f);

    /* -179 is two degrees EAST of +179, so it must plot to the right of centre
     * by 2 * 22.7556 px — not 358 degrees to the left. */
    const auto offset = map.offset_from_center(0.0, -179.0);
    CHECK_NEAR(offset.x, 45.51111111111111, 1e-9);

    const auto p = map.map_to_screen(0.0, -179.0);
    CHECK_EQ(p.x(), center_x + 46);
    CHECK(p.x() > center_x);
    CHECK(p.x() < map_rect.right());

    /* And it is close enough to be kept, not culled as half a world away. */
    CHECK(map.is_visible(0.0, -179.0));
    CHECK_EQ(map.store_marker(make_marker(0.0f, -179.0f)), ui::MARKER_STORED);

    /* Two degrees west of the centre still goes left. */
    CHECK_EQ(map.map_to_screen(0.0, 177.0).x(), center_x - 46);
}

TEST(geomap_view_west_of_the_antimeridian_mirrors) {
    ui::GeoMap map{map_rect};
    map.set_zoom(5);
    map.set_lat_lon(0.0f, -179.0f);

    /* +179 is two degrees WEST of -179. */
    CHECK_NEAR(map.offset_from_center(0.0, 179.0).x, -45.51111111111111, 1e-9);
    CHECK_EQ(map.map_to_screen(0.0, 179.0).x(), center_x - 46);
    CHECK(map.is_visible(0.0, 179.0));
}

TEST(geomap_centre_longitude_is_normalized) {
    ui::GeoMap map{map_rect};
    map.set_zoom(5);

    /* A caller handing over an unwrapped longitude must not shift the view. */
    map.set_lat_lon(0.0f, 181.0f);
    CHECK_NEAR(map.lon(), -179.0, 1e-4);
    CHECK_EQ(map.map_to_screen(0.0, -179.0).x(), center_x);

    /* Latitude is clamped to the projection limit instead of wrapping. */
    map.set_lat_lon(95.0f, 0.0f);
    CHECK_NEAR(map.lat(), ui::mercator::latitude_limit, 1e-4);
}

/* --- Zoom ------------------------------------------------------------------ */

TEST(geomap_zoom_clamps_at_both_ends) {
    ui::GeoMap map{map_rect};

    map.set_zoom(-5);
    CHECK_EQ(map.zoom(), 0);
    CHECK(!map.zoom_out());
    CHECK_EQ(map.zoom(), 0);

    CHECK(map.zoom_in());
    CHECK_EQ(map.zoom(), 1);

    map.set_zoom(99);
    CHECK_EQ(map.zoom(), ui::mercator::max_zoom);
    CHECK(!map.zoom_in());
    CHECK_EQ(map.zoom(), ui::mercator::max_zoom);

    CHECK(map.zoom_out());
    CHECK_EQ(map.zoom(), ui::mercator::max_zoom - 1);
}

TEST(geomap_encoder_and_keyboard_change_zoom) {
    ui::GeoMap map{map_rect};
    map.set_zoom(5);

    CHECK(map.on_encoder(+1));
    CHECK_EQ(map.zoom(), 6);
    CHECK(map.on_encoder(-1));
    CHECK_EQ(map.zoom(), 5);
    /* A zero delta is not ours to consume. */
    CHECK(!map.on_encoder(0));

    CHECK(map.on_keyboard('+'));
    CHECK_EQ(map.zoom(), 6);
    CHECK(map.on_keyboard('-'));
    CHECK_EQ(map.zoom(), 5);
    CHECK(!map.on_keyboard('q'));
    CHECK_EQ(map.zoom(), 5);
}

TEST(geomap_scale_bar_resolution_tracks_zoom_and_latitude) {
    ui::GeoMap map{map_rect};

    map.set_zoom(0);
    map.set_lat_lon(0.0f, 0.0f);
    CHECK_NEAR(map.meters_per_pixel(), 156543.03392804097, 1e-6);

    map.set_zoom(10);
    CHECK_NEAR(map.meters_per_pixel(), 156543.03392804097 / 1024.0, 1e-9);

    map.set_lat_lon(60.0f, 0.0f);
    CHECK_NEAR(map.meters_per_pixel(), 156543.03392804097 / 1024.0 / 2.0, 1e-6);
}

/* --- State ----------------------------------------------------------------- */

TEST(geomap_presentation_state) {
    ui::GeoMap map{map_rect};

    CHECK_EQ(map.mode(), ui::DISPLAY);
    CHECK(!map.manual_panning());
    CHECK(!map.hide_center_marker());
    /* No heading until one is supplied: the centre draws as a crosshair. */
    CHECK_EQ(map.angle(), ui::invalid_angle);

    map.set_mode(ui::PROMPT);
    CHECK_EQ(map.mode(), ui::PROMPT);

    map.set_manual_panning(true);
    CHECK(map.manual_panning());

    map.set_angle(45);
    CHECK_EQ(map.angle(), uint16_t{45});

    map.set_tag("G-ABCD");
    CHECK_STR_EQ(map.tag(), "G-ABCD");

    map.set_hide_center_marker(true);
    CHECK(map.hide_center_marker());

    /* No OSM tile reader on the host, so the source is always the .bin format
     * (or the graticule that stands in for it). */
    CHECK_EQ(map.get_map_type(), ui::MAP_TYPE_BIN);

    /* No position has been supplied, so there is nothing to draw for "me". */
    CHECK(!map.my_position_valid());
    map.update_my_position(51.5f, -0.12f, 30);
    CHECK(map.my_position_valid());
}

TEST(geomap_prompt_touch_reports_the_touched_position) {
    ui::GeoMap map{map_rect};
    map.set_zoom(8);
    map.set_lat_lon(0.0f, 0.0f);

    int calls = 0;
    float got_lat = 0.0f;
    float got_lon = 0.0f;
    bool got_absolute = false;
    map.on_move = [&](float lon, float lat, bool absolute) {
        calls++;
        got_lon = lon;
        got_lat = lat;
        got_absolute = absolute;
    };

    /* In DISPLAY mode a touch is not a position pick. */
    CHECK(!map.on_touch({{center_x + 182, center_y}, ui::TouchEvent::Type::Start}));
    CHECK_EQ(calls, 0);

    map.set_mode(ui::PROMPT);
    CHECK(map.on_touch({{center_x + 182, center_y}, ui::TouchEvent::Type::Start}));
    CHECK_EQ(calls, 1);
    CHECK(got_absolute);
    /* 182 px east of centre at zoom 8 is one degree of longitude. */
    CHECK_NEAR(got_lon, 1.0, 0.005);
    CHECK_NEAR(got_lat, 0.0, 0.005);

    /* Only the press, not the release, picks a position. */
    CHECK(!map.on_touch({{center_x, center_y}, ui::TouchEvent::Type::End}));
    CHECK_EQ(calls, 1);
}

/* --- world_map.bin --------------------------------------------------------- */

TEST(worldmap_file_projection_matches_upstream_layout) {
    constexpr uint32_t w = 8192;

    /* Longitude spans the full image width. */
    CHECK_NEAR(ui::worldmap::file_x(-180.0, w), 0.0, 1e-9);
    CHECK_NEAR(ui::worldmap::file_x(0.0, w), 4096.0, 1e-9);
    CHECK_NEAR(ui::worldmap::file_x(90.0, w), 6144.0, 1e-9);

    /* The bottom row of the image is upstream's -85.05 degrees, exactly. */
    CHECK_NEAR(ui::worldmap::file_y(ui::worldmap::bottom_latitude, w, w), 8192.0, 1e-6);
    /* A square image therefore reaches +85.05 within a pixel of its top row. */
    CHECK(std::fabs(ui::worldmap::file_y(85.05, w, w)) < 1.0);
    /* And the equator sits mid-image. */
    CHECK_NEAR(ui::worldmap::file_y(0.0, w, w), 4096.0, 1.0);

    /* Mercator symmetry about the equator. */
    const double equator = ui::worldmap::file_y(0.0, w, w);
    for (double lat : {5.0, 33.0, 70.0}) {
        CHECK_NEAR(ui::worldmap::file_y(lat, w, w) - equator,
                   -(ui::worldmap::file_y(-lat, w, w) - equator), 1e-6);
    }

    /* A non-square image keeps its bottom row anchored, as upstream does. */
    CHECK_NEAR(ui::worldmap::file_y(ui::worldmap::bottom_latitude, w, 4096), 4096.0, 1e-6);
}

TEST(worldmap_file_projection_shares_the_web_mercator_scale) {
    /* An 8192 px wide image is the world at zoom 5 (256 * 2^5). Its pixel
     * coordinates must therefore agree with the widget's projection: x exactly,
     * y up to the constant that comes from anchoring at -85.05 rather than at
     * the true Mercator limit. */
    constexpr uint32_t w = 8192;
    constexpr int z = 5;

    for (double lon : {-180.0, -60.0, 0.0, 44.5, 179.0})
        CHECK_NEAR(ui::worldmap::file_x(lon, w), ui::mercator::lon_to_world_x(lon, z), 1e-9);

    const double bias =
        ui::worldmap::file_y(0.0, w, w) - ui::mercator::lat_to_world_y(0.0, z);
    CHECK(std::fabs(bias) < 1.0);
    for (double lat : {-70.0, -12.0, 0.0, 33.9, 80.0}) {
        CHECK_NEAR(ui::worldmap::file_y(lat, w, w) - ui::mercator::lat_to_world_y(lat, z),
                   bias, 1e-6);
    }
}

TEST(worldmap_image_reports_a_missing_file) {
    ui::WorldMapImage image;

    const auto paths = ui::WorldMapImage::search_paths();
    CHECK(!paths.empty());
    for (const auto& p : paths) {
        CHECK(p.find("world_map.bin") != std::string::npos);
        CHECK(p.find(core::data_directory()) == size_t{0});
    }

    CHECK(!image.open("this-path-does-not-exist-mb200.bin"));
    CHECK(!image.is_open());
    CHECK(!image.status().empty());
    CHECK(image.status().find("cannot open") != std::string::npos);

    /* open_default() must agree with itself: the status is set either way, so a
     * missing map can always be explained on screen. */
    const bool opened = image.open_default();
    CHECK_EQ(opened, image.is_open());
    CHECK(!image.status().empty());
    if (!opened) CHECK(image.status().find("world_map.bin") != std::string::npos);
}

TEST(worldmap_image_reads_rows_and_wraps) {
    const auto path = temp_map_path("mb200_geomap_test_map.bin");
    /* 4 x 2 pixels, values chosen so a wrong offset is obvious. */
    write_map_file(path, 4, 2, {0x0001, 0x0002, 0x0003, 0x0004,
                                0x0011, 0x0012, 0x0013, 0x0014});

    ui::WorldMapImage image;
    CHECK(image.open(path.string()));
    CHECK(image.is_open());
    CHECK_EQ(image.width(), uint32_t{4});
    CHECK_EQ(image.height(), uint32_t{2});
    CHECK(image.status().find("4x2") != std::string::npos);

    ui::Color buffer[8]{};

    CHECK(image.read_span(0, 0, 4, buffer));
    CHECK_EQ(buffer[0].v, uint16_t{0x0001});
    CHECK_EQ(buffer[3].v, uint16_t{0x0004});

    CHECK(image.read_span(1, 1, 2, buffer));
    CHECK_EQ(buffer[0].v, uint16_t{0x0012});
    CHECK_EQ(buffer[1].v, uint16_t{0x0013});

    /* Starting near the end of a row wraps around the antimeridian. */
    CHECK(image.read_span(3, 0, 4, buffer));
    CHECK_EQ(buffer[0].v, uint16_t{0x0004});
    CHECK_EQ(buffer[1].v, uint16_t{0x0001});
    CHECK_EQ(buffer[2].v, uint16_t{0x0002});
    CHECK_EQ(buffer[3].v, uint16_t{0x0003});

    /* A column past the right edge is the same as wrapping to the left one. */
    CHECK(image.read_span(4, 0, 1, buffer));
    CHECK_EQ(buffer[0].v, uint16_t{0x0001});

    /* Out of range requests are refused, not clamped into a wrong picture. */
    CHECK(!image.read_span(0, 2, 1, buffer));
    CHECK(!image.read_span(0, 0, 5, buffer));
    CHECK(!image.read_span(0, 0, 0, buffer));
    CHECK(!image.read_span(0, 0, 1, nullptr));

    image.close();
    CHECK(!image.is_open());
    CHECK(!image.read_span(0, 0, 1, buffer));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(worldmap_image_rejects_a_short_file) {
    const auto path = temp_map_path("mb200_geomap_short_map.bin");
    /* Header claims 4 x 2 but only four pixels follow. */
    write_map_file(path, 4, 2, {0x0001, 0x0002, 0x0003, 0x0004});

    ui::WorldMapImage image;
    CHECK(!image.open(path.string()));
    CHECK(!image.is_open());
    CHECK(image.status().find("short image data") != std::string::npos);

    /* A zero-sized header is refused too. */
    const auto empty = temp_map_path("mb200_geomap_zero_map.bin");
    write_map_file(empty, 0, 0, {});
    CHECK(!image.open(empty.string()));
    CHECK(image.status().find("bad size") != std::string::npos);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(empty, ec);
}

/* --- Rendering a world image ----------------------------------------------- */

TEST(geomap_draws_the_world_image_in_the_right_place) {
    /* A 256 px wide image is the whole world at zoom 0, so one image pixel is
     * one screen pixel and the mapping can be checked exactly. Four quadrants
     * of flat colour, split at image pixel 128 — which is longitude 0 and,
     * within a hundredth of a pixel, the equator. */
    constexpr uint16_t side = 256;
    std::vector<uint16_t> pixels(static_cast<size_t>(side) * side);
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            const ui::Color c = (y < 128) ? ((x < 128) ? ui::Color::red() : ui::Color::green())
                                          : ((x < 128) ? ui::Color::blue() : ui::Color::white());
            pixels[static_cast<size_t>(y) * side + x] = c.v;
        }
    }

    const auto path = temp_map_path("mb200_geomap_quadrants.bin");
    write_map_file(path, side, side, pixels);

    host::display.init();
    host::display.scroll_disable();
    host::display.fill_rectangle({0, 0, 240, 320}, ui::Color::magenta());

    ui::GeoMap map{map_rect};
    CHECK(map.init(path.string()));
    CHECK(map.map_file_opened());
    CHECK(map.map_status().find("256x256") != std::string::npos);

    map.set_zoom(0);
    map.set_lat_lon(0.0f, 0.0f);
    /* The centre marker would paint over the pixels being probed. */
    map.set_hide_center_marker(true);

    ui::Painter painter;
    map.paint(painter);

    /* Screen (i, j) shows image pixel (8 + i, 8 + j) for this rect and zoom, so
     * the quadrant corner at image (128, 128) lands at widget (120, 120), which
     * is display (120, 136). Probing the four pixels around it catches a flip
     * or a one-pixel slip in either axis. */
    struct Probe {
        int x;
        int y;
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };
    const Probe probes[] = {
        {119, 135, 0xF8, 0x00, 0x00},  /* NW image quadrant: red   */
        {120, 135, 0x00, 0xFC, 0x00},  /* NE: green */
        {119, 136, 0x00, 0x00, 0xF8},  /* SW: blue  */
        {120, 136, 0xF8, 0xFC, 0xF8},  /* SE: white */
    };

    for (const auto& probe : probes) {
        std::vector<ui::ColorRGB888> pixel(1);
        host::display.read_pixels({probe.x, probe.y, 1, 1}, pixel);
        CHECK_EQ(static_cast<int>(pixel[0].r), static_cast<int>(probe.r));
        CHECK_EQ(static_cast<int>(pixel[0].g), static_cast<int>(probe.g));
        CHECK_EQ(static_cast<int>(pixel[0].b), static_cast<int>(probe.b));
    }

    /* The area above the widget must be untouched — the map does not paint
     * outside its own rectangle. */
    std::vector<ui::ColorRGB888> above(1);
    host::display.read_pixels({120, 15, 1, 1}, above);
    CHECK_EQ(static_cast<int>(above[0].r), 0xF8);
    CHECK_EQ(static_cast<int>(above[0].b), 0xF8);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(geomap_image_pans_with_the_centre) {
    /* Same image, but centred one degree east: everything shifts left by
     * 256/360 = 0.711 px, which at zoom 3 (world 2048) is 5.69 px. */
    constexpr uint16_t side = 256;
    std::vector<uint16_t> pixels(static_cast<size_t>(side) * side, ui::Color::blue().v);
    /* A single white column at image x = 128 (longitude 0). */
    for (int y = 0; y < side; y++)
        pixels[static_cast<size_t>(y) * side + 128] = ui::Color::white().v;

    const auto path = temp_map_path("mb200_geomap_meridian.bin");
    write_map_file(path, side, side, pixels);

    host::display.init();
    host::display.scroll_disable();

    ui::GeoMap map{map_rect};
    CHECK(map.init(path.string()));
    map.set_zoom(0);
    map.set_hide_center_marker(true);
    ui::Painter painter;

    /* Centred on the prime meridian, the white column is at the widget centre. */
    map.set_lat_lon(0.0f, 0.0f);
    map.paint(painter);
    std::vector<ui::ColorRGB888> pixel(1);
    host::display.read_pixels({120, 100, 1, 1}, pixel);
    CHECK_EQ(static_cast<int>(pixel[0].g), 0xFC);  /* white */

    /* Panning east moves it to the left of centre and off the middle column. */
    map.set_lat_lon(0.0f, 20.0f);
    map.paint(painter);
    host::display.read_pixels({120, 100, 1, 1}, pixel);
    CHECK_EQ(static_cast<int>(pixel[0].g), 0x00);  /* blue */
    /* 20 degrees is 14.2 px at zoom 0, so the meridian is now near x = 106. */
    int found = -1;
    for (int x = 100; x < 112; x++) {
        host::display.read_pixels({x, 100, 1, 1}, pixel);
        if (pixel[0].g == 0xFC) found = x;
    }
    CHECK(found >= 105 && found <= 107);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

/* --- Painting smoke test ---------------------------------------------------
 * Cannot assert what the map looks like, but it can assert that every drawing
 * path runs and stays inside the framebuffer. */

TEST(geomap_paints_every_mode_without_running_off_screen) {
    host::display.init();
    host::display.scroll_disable();

    ui::GeoMap map{map_rect};
    map.init();  /* no image expected on a dev machine; graticule path */

    ui::Painter painter;

    map.set_lat_lon(51.5f, -0.12f);
    map.store_marker(make_marker(51.5f, -0.11f, 90, "ONE"));
    map.store_marker(make_marker(51.49f, -0.13f, ui::invalid_angle, "TWO"));
    map.update_my_position(51.51f, -0.12f, 100);
    map.update_my_orientation(180, true);

    for (int z : {0, 1, 5, 12, 19}) {
        map.set_zoom(z);
        map.paint(painter);
    }

    /* Angle indicator on the centre marker. */
    map.set_angle(45);
    map.paint(painter);

    /* Graticule drawn over whatever the background is. */
    map.set_show_graticule(true);
    map.paint(painter);

    /* Panning reticle and prompt crosshair. */
    map.set_manual_panning(true);
    map.paint(painter);
    map.set_manual_panning(false);
    map.set_mode(ui::PROMPT);
    map.paint(painter);

    map.set_hide_center_marker(true);
    map.set_mode(ui::DISPLAY);
    map.paint(painter);

    /* Near the poles and the antimeridian, where the row mapping runs out. */
    map.set_lat_lon(84.9f, 179.99f);
    map.set_zoom(3);
    map.paint(painter);
    map.set_lat_lon(-84.9f, -179.99f);
    map.paint(painter);
}
