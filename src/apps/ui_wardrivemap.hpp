/*
 * mayhem-b200 — Wardrive Map (host port of Mayhem's wardrivemap).
 *
 * Upstream: application/external/wardrivemap/ui_wardrivemap.{hpp,cpp}
 *   Copyright (C) 2024 HTotoo
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * What upstream does
 * ------------------
 * WardriveMap is a *viewer*: it walks the geotagged files on the SD card
 * (capture-metadata .TXT files and Flipper .sub files), pulls the latitude and
 * longitude out of each, and plots them as markers on the GeoMap widget with a
 * 30-marker-per-page paginator. It also follows a live GPS fix supplied by the
 * PortaPack's GPS peripheral through the firmware message bus.
 *
 * What changes on the host
 * ------------------------
 *  - There is no GPS peripheral and no firmware message bus, so the live-fix
 *    following (on_gps / on_orientation) is not ported. The map is driven by the
 *    operator instead, through the GeoPos lat/lon fields and touch panning —
 *    which is exactly upstream's manual-panning path. This is stated in the
 *    report; nothing on screen pretends a GPS fix exists.
 *  - The observation source is a CSV-style wardrive log kept under
 *    core::data_directory() (see log_path()), plus — faithful to upstream — the
 *    capture-metadata .TXT files under core::captures_directory(). Flipper .sub
 *    files are a Flipper Zero artifact that does not exist on a B200 host, so
 *    that reader is not ported.
 *
 * Everything here is genuinely functional: it reads a real log off disk, applies
 * upstream's geotag-validity filter, and plots real markers through the real
 * Web-Mercator projection. hardware_limited is therefore false.
 */

#ifndef __MB200_UI_WARDRIVEMAP_H__
#define __MB200_UI_WARDRIVEMAP_H__

#include "ui.hpp"
#include "ui_geomap.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace app {

/* Pure log/observation logic, split out from the View so it can be tested with
 * no display and no filesystem. */
namespace wardrive {

/* One plotted observation. Mirrors what upstream reads out of a geotagged file:
 * a position, the capture frequency, and a label (the source filename upstream,
 * a free-text name here). lat/lon are float to match upstream's capture_metadata
 * and GeoMarker types. */
struct Observation {
    std::string timestamp{};   /* "YYYY-MM-DD HH:MM:SS", may be empty */
    float latitude{0.0f};
    float longitude{0.0f};
    uint64_t frequency{0};     /* Hz, 0 = unknown */
    std::string name{};        /* marker label */
};

/* Upstream's geotag-validity predicate, transcribed exactly from
 * ui_wardrivemap.cpp: non-zero and inside the 400-degree sentinel guard. A
 * marker is plotted only when this is true. */
bool is_geotagged(float latitude, float longitude);

/* --- CSV log format (the host's observation log) -------------------------- *
 * One observation per line:
 *
 *     timestamp,latitude,longitude,frequency_hz,name
 *
 * latitude/longitude are written with 7 decimal places (~1 cm), frequency as an
 * integer, name as free text. Lines beginning with '#' are comments (the header
 * is two of them); blank and malformed lines are skipped on read. Only the first
 * four commas are structural, so a name may itself contain commas and still
 * round-trips exactly. */

std::string csv_header();
std::string to_csv_line(const Observation& obs);

/* Parses one line. Returns false for a comment, a blank line, or a line without
 * the four required numeric-ish columns; the position is not validated here, so
 * the round trip is lossless. */
bool parse_csv_line(std::string_view line, Observation& out);

/* Whole-log helpers, so the parse/write round trip can be tested in memory. */
std::string write_log(const std::vector<Observation>& observations);
std::vector<Observation> parse_log(std::string_view text);

/* Upstream's other data source: a capture-metadata .TXT file, which is
 * "key=value" lines (center_frequency=, sample_rate=, latitude=, longitude=,
 * satinuse=). Faithful to application/metadata_file.cpp: returns true only for a
 * valid capture (center_frequency and sample_rate both non-zero) that is also
 * geotagged. */
bool parse_capture_metadata(const std::vector<std::string>& lines, Observation& out);

/* Path of the wardrive CSV log under core::data_directory(). */
std::string log_path();

}  // namespace wardrive

class WardriveMapView : public ui::View {
   public:
    WardriveMapView();

    std::string title() const override { return "WardriveMap"; }

    void focus() override;

    /* For tests: seed the observation list directly, bypassing disk, then plot.
     * Mirrors what load_from_disk() feeds the plotter. */
    void set_observations(std::vector<wardrive::Observation> observations);
    size_t observation_count() const { return observations_.size(); }

   private:
    /* Reads the CSV log and the capture-metadata .TXT files into observations_,
     * keeping only geotagged entries (upstream's filter). */
    void load_from_disk();

    /* Re-plots the current page onto the map. Ported from upstream load_markers,
     * iterating the in-memory list instead of re-reading the disk on every pan. */
    void load_markers();

    std::vector<wardrive::Observation> observations_{};

    bool first_init = false;    /* centre the map on the first marker, once */
    uint16_t marker_start = 0;  /* paginator: index of the first shown marker */
    uint16_t marker_cntall = 0; /* total geotagged marker count */

    ui::Text text_info{
        {0, 0, 20 * 8, 16}, "0 / 0"};
    ui::Text text_notfound{
        {0, 0, ui::screen_width, 16}, "No GeoTagged captures found"};

    ui::GeoPos geopos{
        {0, 20},
        ui::GeoPos::alt_unit::METERS,
        ui::GeoPos::spd_unit::HIDDEN};
    ui::GeoMap geomap{
        {0, 75, ui::screen_width, ui::screen_height - 16 - 75}};

    /* Paginator, right-aligned: upstream's UI_POS_X_RIGHT(8) / (4). */
    ui::Button btn_back{{ui::screen_width - 8 * 8, 0, 3 * 8, 16}, "<-"};
    ui::Button btn_next{{ui::screen_width - 4 * 8, 0, 3 * 8, 16}, "->"};
};

}  // namespace app

#endif /*__MB200_UI_WARDRIVEMAP_H__*/
