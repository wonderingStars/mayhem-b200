/*
 * mayhem-b200 — geographic map widget.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_geomap.hpp"

#include "../apps/app_context.hpp"
#include "../apps/ui_navigation.hpp"
#include "../core/file_path.hpp"
#include "../core/string_format.hpp"
#include "display.hpp"
#include "theme.hpp"
#include "ui_font_fixed_5x8.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ui {

namespace {

/* Colours for the no-image case. Deliberately not the theme background: the
 * graticule has to read as "a map with no data", not as an unpainted widget. */
constexpr Color color_backdrop{0, 0, 32};
constexpr Color color_grid{0, 64, 96};
constexpr Color color_axis{0, 128, 128};
constexpr Color color_grid_label{128, 160, 160};
constexpr Color color_off_map{16, 16, 16};

/* Longitude/latitude spacings the graticule may use, coarse to fine. */
constexpr double graticule_steps[] = {
    90.0, 45.0, 30.0, 20.0, 10.0, 5.0, 2.0, 1.0,
    0.5, 0.2, 0.1, 0.05, 0.02, 0.01, 0.005, 0.002, 0.001};

/* Minimum pixel spacing between grid lines; below this the labels collide. */
constexpr double graticule_min_spacing_px = 45.0;

int decimals_for_step(double step) {
    if (step >= 1.0) return 0;
    if (step >= 0.1) return 1;
    if (step >= 0.01) return 2;
    return 3;
}

std::string format_degrees(double value, int decimals, char positive, char negative) {
    const char hemisphere = (value < 0.0) ? negative : positive;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f%c", decimals, std::fabs(value), hemisphere);
    return std::string{buf};
}

/* Rounds `meters` down to the nearest 1/2/5 x 10^n, for the scale bar. */
double nice_distance(double meters) {
    if (!(meters > 0.0)) return 1.0;
    const double magnitude = std::pow(10.0, std::floor(std::log10(meters)));
    const double normalized = meters / magnitude;
    if (normalized >= 5.0) return 5.0 * magnitude;
    if (normalized >= 2.0) return 2.0 * magnitude;
    return magnitude;
}

std::string format_distance(double meters) {
    char buf[32];
    if (meters < 1000.0) {
        std::snprintf(buf, sizeof(buf), "%d m", static_cast<int>(std::lround(meters)));
    } else {
        const double km = meters / 1000.0;
        if (km >= 10.0)
            std::snprintf(buf, sizeof(buf), "%d km", static_cast<int>(std::lround(km)));
        else
            std::snprintf(buf, sizeof(buf), "%.1f km", km);
    }
    return std::string{buf};
}

bool seek_absolute(std::FILE* f, uint64_t offset) {
#if defined(_WIN32)
    return _fseeki64(f, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

}  // namespace

/* --- mercator -------------------------------------------------------------- */

namespace mercator {

int clamp_zoom(int zoom) {
    if (zoom < min_zoom) return min_zoom;
    if (zoom > max_zoom) return max_zoom;
    return zoom;
}

double world_size(int zoom) {
    return tile_size * std::pow(2.0, static_cast<double>(clamp_zoom(zoom)));
}

double clamp_latitude(double lat) {
    if (lat > latitude_limit) return latitude_limit;
    if (lat < -latitude_limit) return -latitude_limit;
    return lat;
}

double normalize_longitude(double lon) {
    double v = std::fmod(lon + 180.0, 360.0);
    if (v < 0.0) v += 360.0;
    return v - 180.0;
}

double lon_to_world_x(double lon, int zoom) {
    return ((normalize_longitude(lon) + 180.0) / 360.0) * world_size(zoom);
}

double lat_to_world_y(double lat, int zoom) {
    const double s = std::sin(clamp_latitude(lat) * M_PI / 180.0);
    return (0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI)) * world_size(zoom);
}

double world_x_to_lon(double x, int zoom) {
    return (x / world_size(zoom)) * 360.0 - 180.0;
}

double world_y_to_lat(double y, int zoom) {
    const double n = M_PI * (1.0 - 2.0 * y / world_size(zoom));
    return std::atan(std::sinh(n)) * 180.0 / M_PI;
}

double pixels_per_degree_lon(int zoom) {
    return world_size(zoom) / 360.0;
}

double meters_per_pixel(double lat, int zoom) {
    return earth_circumference_m * std::cos(clamp_latitude(lat) * M_PI / 180.0) /
           world_size(zoom);
}

}  // namespace mercator

/* --- worldmap -------------------------------------------------------------- */

namespace worldmap {

namespace {

/* Mercator y of `lat` in image pixels, measured up from the equator. This is
 * upstream's `map_world_lon / 2 * log((1 + sin) / (1 - sin))`. */
double mercator_offset(double lat, uint32_t map_width) {
    const double map_world_lon = static_cast<double>(map_width) / (2.0 * M_PI);
    const double s = std::sin(mercator::clamp_latitude(lat) * M_PI / 180.0);
    return (map_world_lon / 2.0) * std::log((1.0 + s) / (1.0 - s));
}

}  // namespace

double file_x(double lon, uint32_t map_width) {
    return static_cast<double>(map_width) *
           (mercator::normalize_longitude(lon) + 180.0) / 360.0;
}

double file_y(double lat, uint32_t map_width, uint32_t map_height) {
    const double bottom = mercator_offset(bottom_latitude, map_width);
    return static_cast<double>(map_height) - (mercator_offset(lat, map_width) - bottom);
}

}  // namespace worldmap

/* --- WorldMapImage --------------------------------------------------------- */

WorldMapImage::~WorldMapImage() {
    close();
}

std::vector<std::string> WorldMapImage::search_paths() {
    const std::filesystem::path base{core::data_directory()};
    return {
        (base / "ADSB" / "world_map.bin").string(),
        (base / "MAPS" / "world_map.bin").string(),
        (base / "world_map.bin").string(),
    };
}

bool WorldMapImage::open_default() {
    const auto candidates = search_paths();
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) continue;
        if (open(candidate)) return true;
    }

    close();
    path_ = candidates.empty() ? std::string{} : candidates.front();
    status_ = "no world_map.bin under " + core::data_directory();
    return false;
}

bool WorldMapImage::open(const std::string& path) {
    close();
    path_ = path;

    file_ = std::fopen(path.c_str(), "rb");
    if (file_ == nullptr) {
        status_ = "cannot open " + path;
        return false;
    }

    uint16_t header[2] = {0, 0};
    if (std::fread(header, sizeof(uint16_t), 2, file_) != 2) {
        status_ = "truncated header in " + path;
        close();
        path_ = path;
        return false;
    }

    width_ = header[0];
    height_ = header[1];

    if (width_ == 0 || height_ == 0) {
        status_ = "bad size in " + path;
        close();
        path_ = path;
        return false;
    }

    /* The header must agree with the file length, otherwise a row read walks
     * off the end and the map silently shears. */
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    const uint64_t needed =
        4ull + static_cast<uint64_t>(width_) * static_cast<uint64_t>(height_) * 2ull;
    if (ec || static_cast<uint64_t>(bytes) < needed) {
        status_ = "short image data in " + path;
        close();
        path_ = path;
        return false;
    }

    status_ = "map " + to_string_dec_uint(width_) + "x" + to_string_dec_uint(height_);
    return true;
}

void WorldMapImage::close() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
}

bool WorldMapImage::read_span(uint32_t x, uint32_t y, uint32_t count, Color* out) {
    if (file_ == nullptr || out == nullptr) return false;
    if (width_ == 0 || height_ == 0) return false;
    if (y >= height_ || count == 0 || count > width_) return false;

    x %= width_;

    /* At most two chunks: the tail of the row, then the head after wrapping. */
    uint32_t written = 0;
    while (written < count) {
        const uint32_t chunk = std::min(count - written, width_ - x);
        const uint64_t offset =
            4ull + (static_cast<uint64_t>(y) * width_ + x) * sizeof(uint16_t);
        if (!seek_absolute(file_, offset)) return false;
        if (std::fread(out + written, sizeof(uint16_t), chunk, file_) != chunk) return false;
        written += chunk;
        x = 0;
    }
    return true;
}

/* --- GeoMap ---------------------------------------------------------------- */

GeoMap::GeoMap(Rect parent_rect)
    : Widget{parent_rect} {
}

bool GeoMap::init() {
    const bool ok = image_.open_default();
    set_dirty();
    return ok;
}

bool GeoMap::init(const std::string& path) {
    const bool ok = image_.open(path);
    set_dirty();
    return ok;
}

void GeoMap::set_lat_lon(float lat, float lon) {
    const float clamped_lat = static_cast<float>(mercator::clamp_latitude(lat));
    const float wrapped_lon = static_cast<float>(mercator::normalize_longitude(lon));
    if (clamped_lat == lat_ && wrapped_lon == lon_) return;
    lat_ = clamped_lat;
    lon_ = wrapped_lon;
    set_dirty();
}

void GeoMap::set_zoom(int zoom) {
    const int z = mercator::clamp_zoom(zoom);
    if (z == zoom_) return;
    zoom_ = z;
    set_dirty();
}

bool GeoMap::nudge_zoom(int delta) {
    const int z = mercator::clamp_zoom(zoom_ + delta);
    if (z == zoom_) return false;
    zoom_ = z;
    set_dirty();
    return true;
}

void GeoMap::set_mode(GeoMapMode mode) {
    if (mode == mode_) return;
    mode_ = mode;
    set_dirty();
}

void GeoMap::set_manual_panning(bool v) {
    if (v == manual_panning_) return;
    manual_panning_ = v;
    set_dirty();
}

void GeoMap::set_tag(std::string new_tag) {
    if (new_tag == tag_) return;
    tag_ = std::move(new_tag);
    set_dirty();
}

void GeoMap::set_angle(uint16_t new_angle) {
    if (new_angle == angle_) return;
    angle_ = new_angle;
    set_dirty();
}

void GeoMap::set_hide_center_marker(bool hide) {
    if (hide == hide_center_marker_) return;
    hide_center_marker_ = hide;
    set_dirty();
}

void GeoMap::set_show_graticule(bool v) {
    if (v == show_graticule_) return;
    show_graticule_ = v;
    set_dirty();
}

void GeoMap::clear_markers() {
    if (markers_.empty()) return;
    markers_.clear();
    set_dirty();
}

MapMarkerStored GeoMap::store_marker(const GeoMarker& marker) {
    /* Upstream drops markers that cannot be on screen before checking capacity,
     * so a distant aircraft never displaces a nearby one. */
    if (!is_visible(marker.lat, marker.lon)) return MARKER_NOT_STORED;
    if (markers_.size() >= static_cast<size_t>(NumMarkerListElements)) return MARKER_LIST_FULL;

    markers_.push_back(marker);
    set_dirty();
    return MARKER_STORED;
}

void GeoMap::update_my_position(float lat, float lon, int32_t altitude) {
    const bool changed = (my_pos_.lat != lat) || (my_pos_.lon != lon);
    my_pos_.lat = lat;
    my_pos_.lon = lon;
    my_altitude_ = altitude;
    if (changed) set_dirty();
}

void GeoMap::update_my_orientation(uint16_t angle, bool refresh) {
    const bool changed = (my_pos_.angle != angle);
    my_pos_.angle = angle;
    if (refresh && changed) set_dirty();
}

bool GeoMap::my_position_valid() const {
    return (my_pos_.lat < invalid_lat_lon) && (my_pos_.lon < invalid_lat_lon);
}

GeoPoint GeoMap::offset_from_center(double lat, double lon) const {
    /* Longitude difference is folded into [-180, 180) first: without that a
     * marker at -179 seen from +179 projects most of a world to the west. */
    const double dlon = mercator::normalize_longitude(lon - static_cast<double>(lon_));
    const double x = dlon * mercator::pixels_per_degree_lon(zoom_);
    const double y = mercator::lat_to_world_y(lat, zoom_) -
                     mercator::lat_to_world_y(static_cast<double>(lat_), zoom_);
    return {x, y};
}

Point GeoMap::map_to_screen(double lat, double lon) const {
    const auto r = screen_rect();
    const auto o = offset_from_center(lat, lon);
    return {r.center().x() + static_cast<int>(std::lround(o.x)),
            r.center().y() + static_cast<int>(std::lround(o.y))};
}

void GeoMap::screen_to_map(Point p, double& out_lat, double& out_lon) const {
    const auto r = screen_rect();
    const double world = mercator::world_size(zoom_);

    const double wx = mercator::lon_to_world_x(static_cast<double>(lon_), zoom_) +
                      (p.x() - r.center().x());
    double wy = mercator::lat_to_world_y(static_cast<double>(lat_), zoom_) +
                (p.y() - r.center().y());
    wy = std::clamp(wy, 0.0, world);

    out_lon = mercator::normalize_longitude(mercator::world_x_to_lon(wx, zoom_));
    out_lat = mercator::world_y_to_lat(wy, zoom_);
}

bool GeoMap::is_visible(double lat, double lon, int margin) const {
    const auto r = screen_rect();
    if (r.is_empty()) return false;
    /* Rejects the INVALID_LAT_LON sentinel as well as nonsense input. */
    if (!(std::fabs(lat) <= 90.0) || !(std::fabs(lon) <= 180.0)) return false;

    const auto o = offset_from_center(lat, lon);
    return (std::fabs(o.x) < (r.width() / 2.0 + margin)) &&
           (std::fabs(o.y) < (r.height() / 2.0 + margin));
}

double GeoMap::meters_per_pixel() const {
    return mercator::meters_per_pixel(static_cast<double>(lat_), zoom_);
}

/* --- GeoMap: input --------------------------------------------------------- */

bool GeoMap::on_encoder(const EncoderEvent delta) {
    if (delta > 0) return zoom_in();
    if (delta < 0) return zoom_out();
    return false;
}

bool GeoMap::on_keyboard(const KeyboardEvent event) {
    if (event == '+' || event == ' ') return zoom_in();
    if (event == '-') return zoom_out();
    return false;
}

bool GeoMap::on_touch(const TouchEvent event) {
    if (event.type != TouchEvent::Type::Start) return false;
    if (mode_ != PROMPT || !on_move) return false;

    double lat = 0.0;
    double lon = 0.0;
    screen_to_map(event.point, lat, lon);
    set_highlighted(true);
    /* Absolute: the host projection is invertible, so there is no need for
     * upstream's delta-from-centre approximation. */
    on_move(static_cast<float>(lon), static_cast<float>(lat), true);
    return true;
}

/* --- GeoMap: painting ------------------------------------------------------ */

void GeoMap::paint(Painter& painter) {
    const auto r = screen_rect();
    if (r.is_empty()) return;

    if (image_.is_open()) {
        draw_map_image(r);
        if (show_graticule_) draw_graticule(painter, r);
    } else {
        painter.fill_rectangle(r, color_backdrop);
        draw_graticule(painter, r);
        draw_no_map_banner(painter, r);
    }

    draw_scale(painter, r);
    draw_markers(painter, r);

    if (my_position_valid() && is_visible(my_pos_.lat, my_pos_.lon)) {
        const auto p = map_to_screen(my_pos_.lat, my_pos_.lon);
        draw_marker(painter, p, my_pos_.angle, my_pos_.tag, Color::yellow(),
                    Color::yellow(), Color::black());
    }

    draw_center(painter, r);
}

void GeoMap::draw_map_image(const Rect& r) {
    const int w = r.width();
    const int h = r.height();
    const uint32_t fw = image_.width();
    const uint32_t fh = image_.height();
    if (w <= 0 || h <= 0 || fw == 0 || fh == 0) return;

    const double world = mercator::world_size(zoom_);
    const double cwx = mercator::lon_to_world_x(static_cast<double>(lon_), zoom_);
    const double cwy = mercator::lat_to_world_y(static_cast<double>(lat_), zoom_);

    /* The image and the world pixel grid are both linear in longitude, so one
     * scale factor converts between them. */
    const double sx = static_cast<double>(fw) / world;

    const double fx_left = (cwx - w / 2.0 + 0.5) * sx;
    const double fx_right = (cwx + w / 2.0 - 0.5) * sx;

    int64_t src_start = static_cast<int64_t>(std::floor(fx_left));
    const int64_t src_end = static_cast<int64_t>(std::floor(fx_right));
    int64_t span = src_end - src_start + 1;
    bool whole_row = false;
    if (span >= static_cast<int64_t>(fw)) {
        span = static_cast<int64_t>(fw);
        src_start = 0;
        whole_row = true;
    }
    if (span < 1) span = 1;

    /* Every row samples the same set of columns, so resolve them once. */
    std::vector<int32_t> columns(static_cast<size_t>(w));
    for (int i = 0; i < w; i++) {
        const double fx = (cwx - w / 2.0 + i + 0.5) * sx;
        int64_t ix = static_cast<int64_t>(std::floor(fx));
        if (whole_row) {
            ix = ((ix % static_cast<int64_t>(fw)) + static_cast<int64_t>(fw)) %
                 static_cast<int64_t>(fw);
        } else {
            ix -= src_start;
        }
        columns[static_cast<size_t>(i)] =
            static_cast<int32_t>(std::clamp<int64_t>(ix, 0, span - 1));
    }

    std::vector<Color> source(static_cast<size_t>(span));
    row_.assign(static_cast<size_t>(w), color_off_map);

    const uint32_t read_x = static_cast<uint32_t>(
        ((src_start % static_cast<int64_t>(fw)) + static_cast<int64_t>(fw)) %
        static_cast<int64_t>(fw));

    for (int j = 0; j < h; j++) {
        const double wy = cwy + (j - h / 2.0) + 0.5;
        bool have_row = (wy >= 0.0) && (wy < world);
        uint32_t src_y = 0;

        if (have_row) {
            const double lat = mercator::world_y_to_lat(wy, zoom_);
            const double fy = worldmap::file_y(lat, fw, fh);
            if (fy < 0.0 || fy >= static_cast<double>(fh))
                have_row = false;
            else
                src_y = static_cast<uint32_t>(fy);
        }

        if (have_row)
            have_row = image_.read_span(read_x, src_y, static_cast<uint32_t>(span),
                                        source.data());

        if (have_row) {
            for (int i = 0; i < w; i++)
                row_[static_cast<size_t>(i)] =
                    source[static_cast<size_t>(columns[static_cast<size_t>(i)])];
        } else {
            std::fill(row_.begin(), row_.end(), color_off_map);
        }

        host::display.draw_pixels({r.left(), r.top() + j, w, 1}, row_);
    }
}

void GeoMap::draw_graticule(Painter& painter, const Rect& r) {
    const double ppd = mercator::pixels_per_degree_lon(zoom_);
    const double world = mercator::world_size(zoom_);
    const double cwy = mercator::lat_to_world_y(static_cast<double>(lat_), zoom_);
    const auto center = r.center();

    double step = graticule_steps[0];
    for (double candidate : graticule_steps) {
        if (candidate * ppd >= graticule_min_spacing_px)
            step = candidate;
        else
            break;
    }
    const int decimals = decimals_for_step(step);
    const auto& font = font::fixed_5x8;

    /* Meridians. The loop runs over unwrapped longitudes so that a view
     * straddling the antimeridian gets one line per meridian, not two. */
    const double half_span_lon = (r.width() / 2.0) / ppd;
    const double lon_first = std::ceil((lon_ - half_span_lon) / step) * step;
    for (double lon = lon_first; lon <= lon_ + half_span_lon + 1e-9; lon += step) {
        const int x = center.x() + static_cast<int>(std::lround((lon - lon_) * ppd));
        if (x < r.left() || x >= r.right()) continue;

        const double shown = mercator::normalize_longitude(lon);
        const bool axis = std::fabs(shown) < (step / 2.0);
        painter.draw_vline({x, r.top()}, r.height(), axis ? color_axis : color_grid);

        const auto label = format_degrees(shown, decimals, 'E', 'W');
        const int label_w = static_cast<int>(label.size()) * font.char_width();
        const int label_x = std::clamp(x + 2, r.left(), r.right() - label_w);
        painter.draw_string({label_x, r.bottom() - font.line_height() - 1}, font,
                            color_grid_label, color_backdrop, label);
    }

    /* Parallels. Spacing grows away from the equator under Mercator, so the
     * positions come from the projection rather than a constant pitch. */
    const double top_lat =
        mercator::world_y_to_lat(std::max(0.0, cwy - r.height() / 2.0), zoom_);
    const double bottom_lat =
        mercator::world_y_to_lat(std::min(world, cwy + r.height() / 2.0), zoom_);
    const double lat_first = std::ceil(bottom_lat / step) * step;
    for (double lat = lat_first; lat <= top_lat + 1e-9; lat += step) {
        const double dy = mercator::lat_to_world_y(lat, zoom_) - cwy;
        const int y = center.y() + static_cast<int>(std::lround(dy));
        if (y < r.top() || y >= r.bottom()) continue;

        const bool axis = std::fabs(lat) < (step / 2.0);
        painter.draw_hline({r.left(), y}, r.width(), axis ? color_axis : color_grid);

        const auto label = format_degrees(lat, decimals, 'N', 'S');
        painter.draw_string({r.left() + 2, y + 1}, font, color_grid_label,
                            color_backdrop, label);
    }
}

void GeoMap::draw_no_map_banner(Painter& painter, const Rect& r) {
    const auto& font = font::fixed_5x8;
    const int line = font.line_height();
    const Rect banner{r.left(), r.top(), r.width(), line * 2 + 2};

    painter.fill_rectangle(banner, Color::black());
    painter.draw_string({r.left() + 2, r.top() + 1}, font, Color::yellow(),
                        Color::black(), "No world map image - graticule only");

    /* Say where it looked, so the fix is obvious. */
    std::string detail = image_.status();
    const size_t max_chars = static_cast<size_t>(r.width() / font.char_width());
    if (detail.size() > max_chars) detail = detail.substr(detail.size() - max_chars);
    painter.draw_string({r.left() + 2, r.top() + 1 + line}, font, Color::light_grey(),
                        Color::black(), detail);
}

void GeoMap::draw_scale(Painter& painter, const Rect& r) {
    const double mpp = meters_per_pixel();
    if (!(mpp > 0.0)) return;

    const double target_m = mpp * (r.width() / 3.0);
    const double bar_m = nice_distance(target_m);
    const int bar_px = static_cast<int>(std::lround(bar_m / mpp));
    if (bar_px < 4 || bar_px > r.width() - 10) return;

    const Color bar_color = image_.is_open() ? Color::black() : Color::white();
    const int y = r.bottom() - 4;
    const int x0 = r.right() - 5 - bar_px;

    painter.fill_rectangle({x0, y, bar_px, 2}, bar_color);
    painter.fill_rectangle({x0, y - 4, 2, 6}, bar_color);
    painter.fill_rectangle({r.right() - 5, y - 4, 2, 6}, bar_color);

    const auto label = format_distance(bar_m);
    const auto& font = font::fixed_5x8;
    const int label_w = static_cast<int>(label.size()) * font.char_width();
    painter.draw_string({x0 + (bar_px - label_w) / 2, y - 4 - font.line_height()}, font,
                        Color::white(), Color::black(), label);
}

void GeoMap::draw_markers(Painter& painter, const Rect& r) {
    for (const auto& item : markers_) {
        if (!is_visible(item.lat, item.lon)) continue;

        const auto p = map_to_screen(item.lat, item.lon);
        /* Upstream refuses to draw within one symbol height of the top edge,
         * where the arrowhead would be clipped into something unreadable. */
        if ((p.y() - r.top()) <= 10) continue;

        draw_marker(painter, p, item.angle, item.tag, item.color, item.color,
                    Color::black());
    }
}

void GeoMap::draw_bearing(Painter& painter, const Point origin, const uint16_t angle,
                          uint32_t size, const Color color) {
    /* Three nested outlines: the display has no filled-triangle primitive, and
     * this is what upstream does to get a solid-looking arrowhead. */
    for (uint32_t thickness = 0; thickness < 3 && size > 0; thickness++, size--) {
        const Point a = fast_polar_to_point(static_cast<int32_t>(angle), size) + origin;
        const Point b =
            fast_polar_to_point(static_cast<int32_t>(angle + 180 - 35), size) + origin;
        const Point c =
            fast_polar_to_point(static_cast<int32_t>(angle + 180 + 35), size) + origin;

        painter.draw_line(a, b, color);
        painter.draw_line(b, c, color);
        painter.draw_line(c, a, color);
    }

    /* One pixel marking the pivot, so the bearing's origin stays readable. */
    host::display.draw_pixel(origin, color);
}

void GeoMap::draw_marker(Painter& painter, const Point p, const uint16_t item_angle,
                         const std::string& item_tag, const Color color,
                         const Color font_color, const Color back_color) {
    const auto r = screen_rect();
    int tag_offset = 10;

    if (mode_ == PROMPT) {
        painter.fill_rectangle({p.x() - 16, p.y() - 1, 32, 2}, color);
        painter.fill_rectangle({p.x() - 1, p.y() - 16, 2, 32}, color);
        tag_offset = 16;
    } else if (item_angle < 360) {
        /* Deviation from upstream: it gates the arrowhead on the *widget's*
         * angle while drawing the item's, so markers with a known heading get
         * plain crosses whenever the centre has none. Each marker's own heading
         * decides here. */
        draw_bearing(painter, p, item_angle, 10, color);
        tag_offset = 10;
    } else {
        painter.fill_rectangle({p.x() - 8, p.y() - 1, 16, 2}, color);
        painter.fill_rectangle({p.x() - 1, p.y() - 8, 2, 16}, color);
        tag_offset = 8;
    }

    if (item_tag.find_first_not_of(' ') == std::string::npos) return;
    if ((p.y() - r.top()) < 32) return;  /* label would run off the top */

    const auto& font = style().font;
    const int label_w = static_cast<int>(item_tag.size()) * font.char_width();
    painter.draw_string({p.x() - label_w / 2, p.y() - 14 - tag_offset}, font, font_color,
                        back_color, item_tag);
}

void GeoMap::draw_center(Painter& painter, const Rect& r) {
    const auto c = r.center();

    /* While panning, the centre is a target reticle rather than a position. */
    if (manual_panning_ || mode_ == PROMPT) {
        painter.fill_rectangle({c.x() - 16, c.y() - 1, 32, 2}, Color::red());
        painter.fill_rectangle({c.x() - 1, c.y() - 16, 2, 32}, Color::red());
        return;
    }

    if (hide_center_marker_) return;

    /* A valid angle turns the crosshair into the bearing indicator. */
    draw_marker(painter, c, angle_, tag_, Color::red(), Color::white(), Color::black());
}

/* --- GeoPos ---------------------------------------------------------------- */

GeoPos::GeoPos(const Point pos, const alt_unit altitude_unit, const spd_unit speed_unit)
    : altitude_unit_(altitude_unit), speed_unit_(speed_unit) {
    set_parent_rect({pos, {screen_width, 3 * 16}});

    add_children({&labels_position,
                  &label_spd_position,
                  &field_altitude,
                  &field_speed,
                  &text_alt_unit,
                  &text_speed_unit,
                  &field_lat_degrees,
                  &field_lat_minutes,
                  &field_lat_seconds,
                  &text_lat_decimal,
                  &field_lon_degrees,
                  &field_lon_minutes,
                  &field_lon_seconds,
                  &text_lon_decimal});

    set_altitude(0);
    set_speed(0);
    set_lat(0);
    set_lon(0);

    const auto changed_fn = [this](int32_t) {
        const float lat_value = lat();
        const float lon_value = lon();

        text_lat_decimal.set(to_string_decimal(lat_value, 5));
        text_lon_decimal.set(to_string_decimal(lon_value, 5));

        if (on_change && report_change_)
            on_change(altitude(), lat_value, lon_value, speed());
    };

    field_altitude.on_change = changed_fn;
    field_speed.on_change = changed_fn;
    field_lat_degrees.on_change = changed_fn;
    field_lat_minutes.on_change = changed_fn;
    field_lat_seconds.on_change = changed_fn;
    field_lon_degrees.on_change = changed_fn;
    field_lon_minutes.on_change = changed_fn;
    field_lon_seconds.on_change = changed_fn;

    text_alt_unit.set((altitude_unit_ == METERS) ? "m" : "ft");
    if (speed_unit_ == KMPH) text_speed_unit.set("kmph");
    if (speed_unit_ == MPH) text_speed_unit.set("mph");
    if (speed_unit_ == KNOTS) text_speed_unit.set("knots");
    if (speed_unit_ == HIDDEN) {
        text_speed_unit.hidden(true);
        label_spd_position.hidden(true);
        field_speed.hidden(true);
    }
}

void GeoPos::set_read_only(bool v) {
    read_only_ = v;
    /* Only altitude and speed lock: panning by typing a lat/lon stays allowed,
     * which is how the operator moves the map without a touch screen. */
    field_altitude.set_focusable(!v);
    field_speed.set_focusable(!v);
}

void GeoPos::set_report_change(bool v) {
    report_change_ = v;
}

void GeoPos::focus() {
    if (field_altitude.focusable())
        field_altitude.focus();
    else
        field_lat_degrees.focus();
}

void GeoPos::hide_altandspeed() {
    /* Greyed out to show they are no longer being updated from the source. */
    field_altitude.set_style(Theme::getInstance()->fg_medium);
    field_speed.set_style(Theme::getInstance()->fg_medium);
}

void GeoPos::set_altitude(int32_t altitude) {
    field_altitude.set_value(altitude);
}

void GeoPos::set_speed(int32_t speed) {
    field_speed.set_value(speed);
}

void GeoPos::set_lat(float lat) {
    field_lat_degrees.set_value(static_cast<int32_t>(lat));
    field_lat_minutes.set_value(
        static_cast<int32_t>(std::fabs(lat * 60.0f)) % 60);
    field_lat_seconds.set_value(
        static_cast<int32_t>(std::fabs(lat * 3600.0f)) % 60);
}

void GeoPos::set_lon(float lon) {
    field_lon_degrees.set_value(static_cast<int32_t>(lon));
    field_lon_minutes.set_value(
        static_cast<int32_t>(std::fabs(lon * 60.0f)) % 60);
    field_lon_seconds.set_value(
        static_cast<int32_t>(std::fabs(lon * 3600.0f)) % 60);
}

float GeoPos::lat() const {
    const int32_t degrees = field_lat_degrees.value();
    const float fraction = static_cast<float>(field_lat_minutes.value()) / 60.0f +
                           static_cast<float>(field_lat_seconds.value()) / 3600.0f;
    return (degrees < 0) ? (degrees - fraction) : (degrees + fraction);
}

float GeoPos::lon() const {
    const int32_t degrees = field_lon_degrees.value();
    const float fraction = static_cast<float>(field_lon_minutes.value()) / 60.0f +
                           static_cast<float>(field_lon_seconds.value()) / 3600.0f;
    return (degrees < 0) ? (degrees - fraction) : (degrees + fraction);
}

int32_t GeoPos::altitude() const {
    return field_altitude.value();
}

int32_t GeoPos::speed() const {
    return field_speed.value();
}

/* --- GeoMapView ------------------------------------------------------------ */

GeoMapView::GeoMapView(const std::string& tag,
                       int32_t altitude,
                       GeoPos::alt_unit altitude_unit,
                       GeoPos::spd_unit speed_unit,
                       float lat,
                       float lon,
                       uint16_t angle,
                       const std::function<void(void)> on_close)
    : on_close_(on_close),
      mode_(DISPLAY),
      altitude_(altitude),
      altitude_unit_(altitude_unit),
      speed_unit_(speed_unit),
      lat_(lat),
      lon_(lon),
      angle_(angle) {
    add_child(&geopos);
    geomap.init();
    setup();

    geomap.set_mode(mode_);
    geomap.set_tag(tag);
    geomap.set_angle(angle);
    geomap.set_lat_lon(lat_, lon_);
    geomap.set_focusable(true);

    geopos.set_read_only(true);
}

GeoMapView::GeoMapView(int32_t altitude,
                       GeoPos::alt_unit altitude_unit,
                       GeoPos::spd_unit speed_unit,
                       float lat,
                       float lon,
                       const std::function<void(int32_t, float, float, int32_t)> on_done)
    : on_done_(on_done),
      mode_(PROMPT),
      altitude_(altitude),
      altitude_unit_(altitude_unit),
      speed_unit_(speed_unit),
      lat_(lat),
      lon_(lon) {
    add_child(&geopos);
    geomap.init();
    setup();
    add_child(&button_ok);

    geomap.set_mode(mode_);
    geomap.set_lat_lon(lat_, lon_);
    geomap.set_focusable(true);

    button_ok.on_select = [this](Button&) {
        if (on_done_) on_done_(altitude_, lat_, lon_, speed_);
        if (auto* nav = app::globals().nav) nav->pop();
    };
}

GeoMapView::~GeoMapView() {
    if (on_close_) on_close_();
}

void GeoMapView::setup() {
    add_child(&geomap);

    geopos.set_altitude(altitude_);
    geopos.set_lat(lat_);
    geopos.set_lon(lon_);

    geopos.on_change = [this](int32_t altitude, float lat, float lon, int32_t speed) {
        const bool changed =
            (altitude_ != altitude) || (lat_ != lat) || (lon_ != lon) || (speed_ != speed);
        altitude_ = altitude;
        lat_ = lat;
        lon_ = lon;
        speed_ = speed;
        geopos.hide_altandspeed();
        geomap.set_manual_panning(true);
        if (changed) geomap.set_lat_lon(lat_, lon_);
        geomap.set_dirty();
    };

    geomap.on_move = [this](float move_lon, float move_lat, bool absolute) {
        if (absolute) {
            lon_ = move_lon;
            lat_ = move_lat;
        } else {
            lon_ += move_lon;
            lat_ += move_lat;
        }

        /* Writing the fields would otherwise re-enter through on_change. */
        geopos.set_report_change(false);
        geopos.set_lon(lon_);
        geopos.set_lat(lat_);
        geopos.set_report_change(true);

        geomap.set_lat_lon(lat_, lon_);
        geomap.set_dirty();
    };
}

void GeoMapView::focus() {
    geopos.focus();
}

void GeoMapView::on_show() {
    View::on_show();
    /* Honesty rule: an empty-looking map has to explain itself. GeoMap draws the
     * banner, and this makes sure the first frame includes it. */
    geomap.set_dirty();
}

void GeoMapView::update_position(float lat, float lon, uint16_t angle, int32_t altitude,
                                 int32_t speed) {
    if (geomap.manual_panning()) {
        geomap.set_dirty();
        return;
    }

    const bool changed = (lat_ != lat) || (lon_ != lon) || (altitude_ != altitude) ||
                         (speed_ != speed) || (angle_ != angle);
    lat_ = lat;
    lon_ = lon;
    altitude_ = altitude;
    speed_ = speed;
    angle_ = angle;

    geopos.set_report_change(false);
    geopos.set_lat(lat_);
    geopos.set_lon(lon_);
    geopos.set_altitude(altitude_);
    geopos.set_speed(speed_);
    geopos.set_report_change(true);

    geomap.set_angle(angle);
    if (changed) {
        geomap.set_lat_lon(lat_, lon_);
        geomap.set_dirty();
    }
}

void GeoMapView::update_my_position(float lat, float lon, int32_t altitude) {
    geomap.update_my_position(lat, lon, altitude);
}

void GeoMapView::update_my_orientation(uint16_t angle, bool refresh) {
    geomap.update_my_orientation(angle, refresh);
}

void GeoMapView::update_tag(const std::string tag) {
    geomap.set_tag(tag);
}

void GeoMapView::clear_markers() {
    geomap.clear_markers();
}

MapMarkerStored GeoMapView::store_marker(const GeoMarker& marker) {
    return geomap.store_marker(marker);
}

}  // namespace ui
