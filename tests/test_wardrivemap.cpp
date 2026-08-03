/*
 * mayhem-b200 — Wardrive Map tests.
 *
 * Two things are worth asserting without a screen:
 *
 *  1. The observation log round-trips: an Observation written to the CSV format
 *     and parsed back is the same Observation (to the 7-decimal precision the
 *     format keeps). Negative sub-degree longitudes are the interesting case —
 *     the firmware's to_string_decimal drops their sign, which is exactly why
 *     the writer here uses snprintf instead.
 *
 *  2. A logged coordinate projects to the marker pixel the Web-Mercator maths
 *     predicts. Expected pixel offsets are computed from the reference formula
 *     independently of the widget, the same way test_geomap.cpp does.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_geomap.hpp"
#include "ui_wardrivemap.hpp"

#include <cmath>
#include <string>
#include <vector>

using app::wardrive::Observation;

namespace {

/* Same rect the geomap tests use: full width, below the status bar, square so
 * the horizontal and vertical culling limits match. Centre is (120, 136). */
constexpr ui::Rect map_rect{0, 16, 240, 240};
constexpr int center_x = 120;
constexpr int center_y = 136;

/* Reference Web-Mercator world-pixel y, evaluated independently of the widget. */
double merc_y(double lat_deg, int zoom) {
    const double s = std::sin(lat_deg * M_PI / 180.0);
    return (0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI)) * 256.0 *
           std::pow(2.0, zoom);
}

double ppd_lon(int zoom) {
    return 256.0 * std::pow(2.0, zoom) / 360.0;
}

}  // namespace

/* --- geotag validity (upstream filter) ------------------------------------- */

TEST(wardrive_is_geotagged_matches_upstream_filter) {
    /* Real fixes pass. */
    CHECK(app::wardrive::is_geotagged(51.5074f, -0.1278f));   /* London */
    CHECK(app::wardrive::is_geotagged(-33.8688f, 151.2093f)); /* Sydney, S/E */

    /* The (0,0) "no fix" default is rejected, in either axis. */
    CHECK(!app::wardrive::is_geotagged(0.0f, 0.0f));
    CHECK(!app::wardrive::is_geotagged(0.0f, 12.3f));
    CHECK(!app::wardrive::is_geotagged(45.0f, 0.0f));

    /* The 400-degree invalid-position sentinel is rejected. */
    CHECK(!app::wardrive::is_geotagged(400.0f, 10.0f));
    CHECK(!app::wardrive::is_geotagged(10.0f, 400.0f));
}

/* --- CSV round trip -------------------------------------------------------- */

TEST(wardrive_csv_line_round_trip) {
    const Observation in{"2026-08-03 12:34:56", 51.5074f, -0.1278f, 433920000ull,
                         "front door"};

    const std::string line = app::wardrive::to_csv_line(in);
    /* The negative sub-degree longitude keeps its sign in the text. */
    CHECK(line.find(",-0.1278") != std::string::npos);

    Observation out{};
    CHECK(app::wardrive::parse_csv_line(line, out));
    CHECK_STR_EQ(out.timestamp, in.timestamp);
    CHECK_NEAR(out.latitude, in.latitude, 1e-6);
    CHECK_NEAR(out.longitude, in.longitude, 1e-6);
    CHECK_EQ(out.frequency, in.frequency);
    CHECK_STR_EQ(out.name, in.name);
}

TEST(wardrive_csv_name_may_contain_commas) {
    const Observation in{"", 1.0f, 2.0f, 0ull, "cafe, corner of 5th"};
    Observation out{};
    CHECK(app::wardrive::parse_csv_line(app::wardrive::to_csv_line(in), out));
    /* Only the first four commas are structural, so the name survives whole. */
    CHECK_STR_EQ(out.name, "cafe, corner of 5th");
    CHECK_NEAR(out.latitude, 1.0f, 1e-6);
    CHECK_NEAR(out.longitude, 2.0f, 1e-6);
}

TEST(wardrive_log_round_trip) {
    std::vector<Observation> in{
        {"2026-08-03 00:00:01", 51.5074f, -0.1278f, 433920000ull, "home"},
        {"2026-08-03 00:00:02", -33.8688f, 151.2093f, 868000000ull, "opera"},
        {"2026-08-03 00:00:03", 40.7128f, -74.0060f, 0ull, "ny"},
    };

    const std::string text = app::wardrive::write_log(in);
    const std::vector<Observation> out = app::wardrive::parse_log(text);

    CHECK_EQ(out.size(), in.size());
    for (size_t i = 0; i < out.size() && i < in.size(); i++) {
        CHECK_STR_EQ(out[i].timestamp, in[i].timestamp);
        CHECK_NEAR(out[i].latitude, in[i].latitude, 1e-5);
        CHECK_NEAR(out[i].longitude, in[i].longitude, 1e-5);
        CHECK_EQ(out[i].frequency, in[i].frequency);
        CHECK_STR_EQ(out[i].name, in[i].name);
    }
}

TEST(wardrive_parse_log_skips_comments_blanks_and_junk) {
    const std::string text =
        "# mayhem-b200 wardrive log v1\n"
        "# timestamp,latitude,longitude,frequency_hz,name\n"
        "\n"
        "   \n"
        "2026-08-03 00:00:01,51.5074000,-0.1278000,433920000,home\n"
        "this is not a valid row\n"          /* no commas -> skipped */
        "only,two\n"                          /* too few columns -> skipped */
        ",1.0,2.0,0,empty timestamp is ok\n"  /* blank ts, still valid */
        "2026-08-03 00:00:02,notanumber,2.0,0,bad lat\n"; /* lat unparseable */

    const std::vector<Observation> out = app::wardrive::parse_log(text);
    CHECK_EQ(out.size(), size_t{2});
    CHECK_STR_EQ(out[0].name, "home");
    CHECK_STR_EQ(out[1].name, "empty timestamp is ok");
    CHECK_STR_EQ(out[1].timestamp, "");
}

/* --- upstream capture-metadata (.TXT key=value) ---------------------------- */

TEST(wardrive_capture_metadata_parse) {
    const std::vector<std::string> good{
        "center_frequency=433920000",
        "sample_rate=500000",
        "latitude=51.5074000",
        "longitude=-0.1278000",
        "satinuse=7",
    };
    Observation obs{};
    CHECK(app::wardrive::parse_capture_metadata(good, obs));
    CHECK_EQ(obs.frequency, 433920000ull);
    CHECK_NEAR(obs.latitude, 51.5074f, 1e-5);
    CHECK_NEAR(obs.longitude, -0.1278f, 1e-5);

    /* Missing sample_rate -> upstream treats the file as a parse failure. */
    const std::vector<std::string> no_rate{
        "center_frequency=433920000",
        "latitude=51.5074000",
        "longitude=-0.1278000",
    };
    CHECK(!app::wardrive::parse_capture_metadata(no_rate, obs));

    /* center_frequency zero -> parse failure. */
    const std::vector<std::string> no_freq{
        "center_frequency=0",
        "sample_rate=500000",
        "latitude=51.5074000",
        "longitude=-0.1278000",
    };
    CHECK(!app::wardrive::parse_capture_metadata(no_freq, obs));

    /* Valid capture but no GPS fix -> not plotted (geotag filter). */
    const std::vector<std::string> no_fix{
        "center_frequency=433920000",
        "sample_rate=500000",
    };
    CHECK(!app::wardrive::parse_capture_metadata(no_fix, obs));
}

/* --- observation -> marker placement via the geomap projection ------------- */

TEST(wardrive_observation_projects_to_expected_marker_pixel) {
    ui::GeoMap map{map_rect};
    map.set_zoom(8);

    /* Centre the map on the first logged observation. It must land dead centre. */
    const Observation home{"2026-08-03 00:00:00", 0.0f, 0.0f, 0ull, "home"};
    map.set_lat_lon(home.latitude, home.longitude);

    const auto centre = map.map_to_screen(home.latitude, home.longitude);
    CHECK_EQ(centre.x(), center_x);
    CHECK_EQ(centre.y(), center_y);

    /* A second observation 0.5 deg east and 0.25 deg north of centre. */
    const Observation near{"", 0.25f, 0.5f, 0ull, "near"};

    const double expected_x = 0.5 * ppd_lon(8);                 /* linear in lon */
    const double expected_y = merc_y(0.25, 8) - merc_y(0.0, 8); /* north is up (-) */
    CHECK(expected_y < 0.0);

    const auto off = map.offset_from_center(near.latitude, near.longitude);
    CHECK_NEAR(off.x, expected_x, 1e-4);
    CHECK_NEAR(off.y, expected_y, 1e-4);

    /* 91 px east, 45 px north — inside the 120 px half-extent, so it is kept as a
     * marker rather than culled, and its label is preserved. */
    CHECK(map.is_visible(near.latitude, near.longitude));
    const ui::GeoMarker marker{near.latitude, near.longitude, ui::invalid_angle, near.name};
    CHECK_EQ(map.store_marker(marker), ui::MARKER_STORED);
    CHECK_EQ(map.marker_count(), size_t{1});
    CHECK_STR_EQ(map.marker(0).tag, "near");

    /* The screen pixel is centre + the rounded projected offset. */
    const auto scr = map.map_to_screen(near.latitude, near.longitude);
    CHECK_EQ(scr.x(), center_x + static_cast<int>(std::lround(expected_x)));
    CHECK_EQ(scr.y(), center_y + static_cast<int>(std::lround(expected_y)));
}

TEST(wardrive_known_coordinate_marker_is_centred_when_map_follows_it) {
    /* A concrete coordinate (London), centred, projects to the widget centre and
     * is a stored marker regardless of zoom. */
    ui::GeoMap map{map_rect};
    const Observation london{"", 51.5074f, -0.1278f, 0ull, "london"};

    for (int z : {3, 8, 14}) {
        map.set_zoom(z);
        map.set_lat_lon(london.latitude, london.longitude);
        map.clear_markers();

        const auto p = map.map_to_screen(london.latitude, london.longitude);
        CHECK_EQ(p.x(), center_x);
        CHECK_EQ(p.y(), center_y);

        const ui::GeoMarker marker{london.latitude, london.longitude, ui::invalid_angle,
                                   london.name};
        CHECK_EQ(map.store_marker(marker), ui::MARKER_STORED);
    }
}

/* --- view: the geotag filter is applied when seeding observations ---------- */

TEST(wardrive_view_filters_non_geotagged_observations) {
    app::WardriveMapView view;

    std::vector<Observation> seed{
        {"", 51.5074f, -0.1278f, 0ull, "keep-1"},
        {"", 0.0f, 0.0f, 0ull, "drop-no-fix"},
        {"", -33.8688f, 151.2093f, 0ull, "keep-2"},
        {"", 40.0f, 0.0f, 0ull, "drop-zero-lon"},
    };
    view.set_observations(seed);

    /* Only the two real fixes survive the upstream filter. */
    CHECK_EQ(view.observation_count(), size_t{2});
}
