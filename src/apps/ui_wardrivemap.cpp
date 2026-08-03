/*
 * mayhem-b200 — Wardrive Map (host port of Mayhem's wardrivemap).
 *
 * Upstream: application/external/wardrivemap/ui_wardrivemap.{hpp,cpp}
 *   Copyright (C) 2024 HTotoo
 * The capture-metadata format is application/metadata_file.cpp
 *   Copyright (C) 2023 Kyle Reed
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_wardrivemap.hpp"

#include "app_context.hpp"
#include "file_path.hpp"
#include "fs_utils.hpp"
#include "string_format.hpp"
#include "ui_navigation.hpp"

#include <cstdio>
#include <cstdlib>

namespace app {

namespace wardrive {

bool is_geotagged(float latitude, float longitude) {
    /* Transcribed verbatim from upstream ui_wardrivemap.cpp: reject the (0,0)
     * "no fix" default and the 400-degree invalid-position sentinel. */
    return latitude != 0 && longitude != 0 && latitude < 400 && longitude < 400;
}

std::string csv_header() {
    return "# mayhem-b200 wardrive log v1\n"
           "# timestamp,latitude,longitude,frequency_hz,name\n";
}

std::string to_csv_line(const Observation& obs) {
    /* snprintf, not to_string_decimal: the firmware helper drops the sign of a
     * value in (-1, 0), which would turn longitude -0.1278 into +0.1278. */
    char latbuf[32];
    char lonbuf[32];
    std::snprintf(latbuf, sizeof(latbuf), "%.7f", static_cast<double>(obs.latitude));
    std::snprintf(lonbuf, sizeof(lonbuf), "%.7f", static_cast<double>(obs.longitude));

    /* Only newlines have to go — a comma in the name is fine because it is the
     * last, unsplit column. */
    std::string name;
    name.reserve(obs.name.size());
    for (char c : obs.name)
        name += (c == '\n' || c == '\r') ? ' ' : c;

    return obs.timestamp + "," + latbuf + "," + lonbuf + "," +
           to_string_dec_uint(obs.frequency) + "," + name;
}

bool parse_csv_line(std::string_view line, Observation& out) {
    /* Strip a trailing CR/LF (read_lines already does, but be robust). */
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.remove_suffix(1);

    /* Skip leading whitespace, then blank lines and comments. */
    size_t s = 0;
    while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) s++;
    if (s >= line.size()) return false;
    if (line[s] == '#') return false;

    /* Split on the first four commas only; the fifth field (name) keeps any
     * commas it contains. */
    std::vector<std::string> f;
    size_t start = 0;
    int commas = 0;
    for (size_t i = 0; i < line.size() && commas < 4; i++) {
        if (line[i] == ',') {
            f.emplace_back(line.substr(start, i - start));
            start = i + 1;
            commas++;
        }
    }
    f.emplace_back(line.substr(start));
    if (f.size() < 4) return false;  /* need at least ts,lat,lon,freq */

    Observation obs{};
    obs.timestamp = f[0];

    char* endp = nullptr;
    const double lat = std::strtod(f[1].c_str(), &endp);
    if (endp == f[1].c_str()) return false;  /* no number where lat should be */
    const double lon = std::strtod(f[2].c_str(), &endp);
    if (endp == f[2].c_str()) return false;
    const uint64_t freq = std::strtoull(f[3].c_str(), &endp, 10);

    obs.latitude = static_cast<float>(lat);
    obs.longitude = static_cast<float>(lon);
    obs.frequency = freq;
    obs.name = (f.size() >= 5) ? f[4] : std::string{};

    out = std::move(obs);
    return true;
}

std::string write_log(const std::vector<Observation>& observations) {
    std::string out = csv_header();
    for (const auto& o : observations)
        out += to_csv_line(o) + "\n";
    return out;
}

std::vector<Observation> parse_log(std::string_view text) {
    std::vector<Observation> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string_view line =
            (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        Observation obs;
        if (parse_csv_line(line, obs)) out.push_back(std::move(obs));
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return out;
}

bool parse_capture_metadata(const std::vector<std::string>& lines, Observation& out) {
    /* Faithful to application/metadata_file.cpp read_metadata_file(): parse the
     * key=value lines, and treat the record as usable only when both
     * center_frequency and sample_rate are present and non-zero. */
    Observation obs{};
    uint64_t center_frequency = 0;
    uint64_t sample_rate = 0;

    for (const auto& line : lines) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;  /* bad line, as upstream skips */
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);

        if (key == "center_frequency")
            center_frequency = std::strtoull(val.c_str(), nullptr, 10);
        else if (key == "sample_rate")
            sample_rate = std::strtoull(val.c_str(), nullptr, 10);
        else if (key == "latitude")
            obs.latitude = static_cast<float>(std::strtod(val.c_str(), nullptr));
        else if (key == "longitude")
            obs.longitude = static_cast<float>(std::strtod(val.c_str(), nullptr));
    }

    if (center_frequency == 0 || sample_rate == 0) return false;  /* parse failed */
    if (!is_geotagged(obs.latitude, obs.longitude)) return false;

    obs.frequency = center_frequency;
    out = std::move(obs);
    return true;
}

std::string log_path() {
    return core::data_directory() + "/WARDRIVE/wardrive.csv";
}

}  // namespace wardrive

void WardriveMapView::focus() {
    geopos.focus();
}

void WardriveMapView::load_from_disk() {
    observations_.clear();

    /* 1) The host's wardrive CSV log. */
    std::string csv;
    if (core::read_file(wardrive::log_path(), csv)) {
        for (auto& o : wardrive::parse_log(csv)) {
            if (wardrive::is_geotagged(o.latitude, o.longitude))
                observations_.push_back(std::move(o));
        }
    }

    /* 2) Upstream's data source: geotagged capture-metadata .TXT files. */
    std::vector<core::DirEntry> entries;
    if (core::list_directory(core::captures_directory(), entries, ".TXT")) {
        for (const auto& e : entries) {
            if (e.is_directory) continue;
            std::vector<std::string> lines;
            const std::string path =
                core::path_join(core::captures_directory(), e.name);
            if (!core::read_lines(path, lines)) continue;

            wardrive::Observation obs;
            if (wardrive::parse_capture_metadata(lines, obs)) {
                if (obs.name.empty()) obs.name = e.name;
                observations_.push_back(std::move(obs));
            }
        }
    }
}

// Ported from upstream WardriveMapView::load_markers. Re-plots on every map
// change because store_marker() drops any marker outside the current view.
void WardriveMapView::load_markers() {
    uint16_t displayed_cnt = 0;
    uint16_t cnt = 0;
    geomap.clear_markers();

    for (const auto& obs : observations_) {
        /* skip the first marker_start geotagged entries (the paginator) */
        if (marker_start <= cnt) {
            if (first_init == false) {
                /* move the map there before adding, so this one displays */
                geopos.set_report_change(false);
                geopos.set_lat(obs.latitude);
                geopos.set_lon(obs.longitude);
                geopos.set_report_change(true);
                geomap.move(obs.longitude, obs.latitude);
                first_init = true;
            }
            ui::GeoMarker tmp{obs.latitude, obs.longitude, ui::invalid_angle, obs.name};
            if (geomap.store_marker(tmp) == ui::MARKER_STORED) displayed_cnt++;
        }
        cnt++;
    }

    marker_cntall = static_cast<uint16_t>(observations_.size());

    /* show / hide paginator buttons */
    btn_back.hidden((marker_start == 0) || (marker_cntall == 0));
    btn_next.hidden(((marker_start + ui::GeoMap::NumMarkerListElements) >= marker_cntall) ||
                    (marker_cntall == 0));

    /* update text */
    text_info.set(to_string_dec_uint(marker_start + 1) + " - " +
                  to_string_dec_uint(displayed_cnt + marker_start) + " / " +
                  to_string_dec_uint(marker_cntall));
    set_dirty();
}

void WardriveMapView::set_observations(std::vector<wardrive::Observation> observations) {
    observations_.clear();
    for (auto& o : observations) {
        if (wardrive::is_geotagged(o.latitude, o.longitude))
            observations_.push_back(std::move(o));
    }
    first_init = false;
    marker_start = 0;
    load_markers();
}

WardriveMapView::WardriveMapView() {
    add_children({&text_info,
                  &geomap,
                  &geopos,
                  &text_notfound,
                  &btn_back,
                  &btn_next});

    geomap.set_mode(ui::DISPLAY);
    geomap.set_manual_panning(false);
    geomap.set_focusable(true);
    geomap.set_hide_center_marker(true);
    geomap.clear_markers();

    geopos.set_report_change(false);
    geopos.set_lat(0);
    geopos.set_lon(0);
    geopos.set_altitude(0);
    geopos.set_speed(0);
    geopos.set_read_only(true);
    geopos.hide_altandspeed();
    geopos.set_report_change(true);

    geopos.on_change = [this](int32_t altitude, float lat, float lon, int32_t speed) {
        (void)altitude;
        (void)speed;
        geomap.set_manual_panning(true);
        geomap.move(lon, lat);
        load_markers();
        geomap.set_dirty();
    };
    geomap.init();

    load_from_disk();
    load_markers();

    if (marker_cntall > 0) {
        text_notfound.hidden(true);
        geomap.set_dirty();
    } else {
        geomap.hidden(true);
        geopos.hidden(true);
        text_notfound.hidden(false);
        text_info.hidden(true);
    }

    /* never move this before the first load() — it would re-enter load_markers */
    geomap.on_move = [this](float lon, float lat, bool absolute) {
        (void)lon;
        (void)lat;
        (void)absolute;
        load_markers();
    };
    btn_back.on_select = [this](ui::Button&) {
        /* marker_start is unsigned; upstream's signed guard is always true and
         * relies on the paginator being hidden. Do the subtraction safely. */
        if (marker_start >= ui::GeoMap::NumMarkerListElements)
            marker_start = marker_start - ui::GeoMap::NumMarkerListElements;
        else
            marker_start = 0;
        load_markers();
    };
    btn_next.on_select = [this](ui::Button&) {
        if (marker_start + ui::GeoMap::NumMarkerListElements <= marker_cntall)
            marker_start = marker_start + ui::GeoMap::NumMarkerListElements;
        else if (marker_cntall >= ui::GeoMap::NumMarkerListElements)
            marker_start = marker_cntall - ui::GeoMap::NumMarkerListElements;
        load_markers();
    };
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_wardrivemap{{"wardrivemap", "WardriveMap",
                                      app::Category::Utilities, ui::Color::yellow(),
                                      nullptr,
                                      [] { return std::make_unique<app::WardriveMapView>(); },
                                      false}};
}  // namespace
