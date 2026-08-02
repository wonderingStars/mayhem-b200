/*
 * mayhem-b200 — geographic map widget.
 *
 * Ported from firmware/application/ui/ui_geomap.* (Jared Boone, Furrtek,
 * Mark Thompson). The firmware plots markers over a Web-Mercator world image
 * held on the SD card as /ADSB/world_map.bin, plus optional OSM .bmp tiles.
 *
 * Two things change on the host:
 *
 *  1. There is no SD card, and no world image ships with this project. The
 *     widget therefore always draws a lat/lon graticule with degree labels,
 *     which makes it useful with no map data at all, and it says on screen when
 *     the image is missing rather than showing an empty rectangle. If a
 *     world_map.bin *is* placed under core::data_directory() it is drawn
 *     underneath the graticule, read with the same header and pixel layout the
 *     firmware uses.
 *
 *  2. The firmware's projection is expressed in "pixels of the map file", so it
 *     only makes sense once a file is open, and its zoom is a pixel-replication
 *     factor. Here the projection is the standard Web Mercator world-pixel
 *     scheme the firmware already uses for its OSM tile code
 *     (lon_to_pixel_x_tile / lat_to_pixel_y_tile), with an integer OSM zoom
 *     level. That is file-independent, so markers project correctly whether or
 *     not an image is present, and the maths is exactly checkable.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __HOST_UI_GEOMAP_H__
#define __HOST_UI_GEOMAP_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ui {

/* --- Projection -------------------------------------------------------------
 *
 * Spherical Web Mercator (EPSG:3857) on a square world of 256 * 2^zoom pixels,
 * which is what upstream's OSM tile helpers implement. All functions are pure
 * and free-standing so the arithmetic can be tested without a widget, a display
 * or a map file. */
namespace mercator {

constexpr double tile_size = 256.0;

/* The latitude at which the Mercator y coordinate reaches the edge of the
 * square world: atan(sinh(pi)) in degrees. Beyond it there is no map. */
constexpr double latitude_limit = 85.05112877980659;

/* WGS84 equatorial circumference, used for the ground-resolution scale bar. */
constexpr double earth_circumference_m = 40075016.685578488;

constexpr int min_zoom = 0;
constexpr int max_zoom = 19;

int clamp_zoom(int zoom);

/* Width and height of the whole world, in pixels, at `zoom`. */
double world_size(int zoom);

double clamp_latitude(double lat);

/* Folds a longitude (or a difference of two longitudes) into [-180, +180).
 * This is what keeps a marker just west of the antimeridian from being plotted
 * most of a world away from a viewpoint just east of it. */
double normalize_longitude(double lon);

double lon_to_world_x(double lon, int zoom);
double lat_to_world_y(double lat, int zoom);
double world_x_to_lon(double x, int zoom);
double world_y_to_lat(double y, int zoom);

/* Horizontal scale. Latitude scale equals this only at the equator — that is
 * the whole point of Mercator. */
double pixels_per_degree_lon(int zoom);

/* Ground resolution at a given latitude: metres covered by one screen pixel. */
double meters_per_pixel(double lat, int zoom);

}  // namespace mercator

/* --- world_map.bin ----------------------------------------------------------
 *
 * The firmware's image format: uint16 width, uint16 height, then width*height
 * RGB565 pixels, row-major, top row first. The projection baked into the file
 * is Mercator over the full 360 degrees of longitude, with the *bottom* row of
 * the image pinned to latitude -85.05 degrees (see GeoMap::init and
 * lat_lon_to_map_pixel upstream). */
namespace worldmap {

/* Latitude the bottom row of the image represents. Upstream's constant. */
constexpr double bottom_latitude = -85.05;

/* Column in the image for a longitude. */
double file_x(double lon, uint32_t map_width);

/* Row in the image for a latitude. Uses map_width for the Mercator scale and
 * anchors the bottom of the image at `bottom_latitude`, exactly as upstream
 * does, so a non-square image is handled the same way. */
double file_y(double lat, uint32_t map_width, uint32_t map_height);

}  // namespace worldmap

/* An opened world_map.bin. Reads on demand rather than loading the whole image:
 * a 8192x8192 world is 128 MB, and only one row at a time is ever needed. */
class WorldMapImage {
   public:
    WorldMapImage() = default;
    ~WorldMapImage();

    WorldMapImage(const WorldMapImage&) = delete;
    WorldMapImage& operator=(const WorldMapImage&) = delete;

    /* Tries each of search_paths() in order. */
    bool open_default();
    bool open(const std::string& path);
    void close();

    bool is_open() const { return file_ != nullptr; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    /* Path of the opened file, or the last one tried. */
    const std::string& path() const { return path_; }

    /* Human-readable outcome of the last open(), shown on screen when there is
     * no map so the blank area is never mistaken for "no data received". */
    const std::string& status() const { return status_; }

    /* Reads `count` pixels of row `y` starting at column `x`, wrapping around
     * the antimeridian. False on a short read or a bad coordinate. */
    bool read_span(uint32_t x, uint32_t y, uint32_t count, Color* out);

    /* Where a world image may live, most specific first. */
    static std::vector<std::string> search_paths();

   private:
    std::FILE* file_{nullptr};
    uint32_t width_{0};
    uint32_t height_{0};
    std::string path_{};
    std::string status_{"no map image loaded"};
};

/* --- Widget types (names kept from upstream) ------------------------------- */

/* Sentinels upstream uses in place of std::optional. */
constexpr float invalid_lat_lon = 200.0f;
constexpr uint16_t invalid_angle = 400;

#define INVALID_LAT_LON 200
#define INVALID_ANGLE 400
#define GEOMAP_BANNER_HEIGHT (3 * 16)

struct GeoPoint {
    double x{0};
    double y{0};
};

struct GeoMarker {
    float lat{0};
    float lon{0};
    /* Heading in compass degrees. >= 360 means "unknown", and the marker is
     * then drawn as a cross instead of a bearing arrow. */
    uint16_t angle{invalid_angle};
    std::string tag{};
    Color color{Color::blue()};
};

enum GeoMapMode {
    DISPLAY,
    PROMPT
};

enum MapMarkerStored {
    MARKER_NOT_STORED,
    MARKER_STORED,
    MARKER_LIST_FULL
};

/* Upstream can also render an /OSM tile tree of .bmp files, and apps ask which
 * source is live because tiles redraw far more slowly than the single .bin.
 * Nothing on the host reads OSM tiles, so the answer is always MAP_TYPE_BIN —
 * kept so that ported call sites still compile and still get a true answer. */
enum MapType {
    MAP_TYPE_OSM,
    MAP_TYPE_BIN
};

class GeoMap : public Widget {
   public:
    /* Upstream's cap, kept so ported apps page their marker lists the same. */
    static constexpr int NumMarkerListElements = 30;

    explicit GeoMap(Rect parent_rect);

    /* Opens the world image if one is present under core::data_directory().
     * Returns true when it is; the widget is fully functional either way. */
    bool init();
    /* Opens a specific image instead of searching. */
    bool init(const std::string& path);
    bool map_file_opened() const { return image_.is_open(); }
    const std::string& map_status() const { return image_.status(); }
    MapType get_map_type() const { return MAP_TYPE_BIN; }

    /* --- Centring --- */
    void set_lat_lon(float lat, float lon);
    /* Upstream argument order (lon first). Forwards to set_lat_lon. */
    void move(const float lon, const float lat) { set_lat_lon(lat, lon); }
    float lat() const { return lat_; }
    float lon() const { return lon_; }

    /* --- Zoom --- */
    void set_zoom(int zoom);
    int zoom() const { return zoom_; }
    bool zoom_in() { return nudge_zoom(+1); }
    bool zoom_out() { return nudge_zoom(-1); }

    /* --- Presentation --- */
    void set_mode(GeoMapMode mode);
    GeoMapMode mode() const { return mode_; }
    void set_manual_panning(bool v);
    bool manual_panning() const { return manual_panning_; }
    void set_tag(std::string new_tag);
    const std::string& tag() const { return tag_; }
    /* Heading of the centre marker; >= 360 disables the angle indicator. */
    void set_angle(uint16_t new_angle);
    uint16_t angle() const { return angle_; }
    void set_hide_center_marker(bool hide);
    bool hide_center_marker() const { return hide_center_marker_; }
    /* Draw the graticule over the world image too. Without an image the
     * graticule is drawn regardless — it is the only thing there is. */
    void set_show_graticule(bool v);
    bool show_graticule() const { return show_graticule_; }

    /* --- Markers --- */
    void clear_markers();
    MapMarkerStored store_marker(const GeoMarker& marker);
    size_t marker_count() const { return markers_.size(); }
    const GeoMarker& marker(size_t index) const { return markers_[index]; }

    void update_my_position(float lat, float lon, int32_t altitude);
    void update_my_orientation(uint16_t angle, bool refresh = false);
    bool my_position_valid() const;

    /* --- Projection, in widget terms ---
     * Offsets are in pixels from the centre of the widget: +x east, +y south.
     * Kept public because apps (and the tests) need to reason about what is on
     * screen without repainting. */
    GeoPoint offset_from_center(double lat, double lon) const;
    Point map_to_screen(double lat, double lon) const;
    /* Inverse of map_to_screen, for touch handling. */
    void screen_to_map(Point p, double& out_lat, double& out_lon) const;

    /* True when the point falls strictly inside the widget rectangle. `margin`
     * widens the test by that many pixels on every side. */
    bool is_visible(double lat, double lon, int margin = 0) const;

    /* Metres per screen pixel at the current centre latitude and zoom. */
    double meters_per_pixel() const;

    /* Fired by a touch in PROMPT mode. Arguments are (lon, lat, absolute),
     * matching upstream: absolute=false means the values are deltas. */
    std::function<void(float, float, bool)> on_move{};

    void paint(Painter& painter) override;
    bool on_encoder(const EncoderEvent delta) override;
    bool on_keyboard(const KeyboardEvent event) override;
    bool on_touch(const TouchEvent event) override;

    /* The angle indicator: a filled-outline arrowhead pointing along `angle`
     * compass degrees, with a pixel marking the pivot. Public because the
     * bearing symbol is useful outside the map too. */
    static void draw_bearing(Painter& painter, const Point origin, const uint16_t angle,
                             uint32_t size, const Color color);

   private:
    bool nudge_zoom(int delta);

    void draw_map_image(const Rect& r);
    void draw_graticule(Painter& painter, const Rect& r);
    void draw_no_map_banner(Painter& painter, const Rect& r);
    void draw_scale(Painter& painter, const Rect& r);
    void draw_markers(Painter& painter, const Rect& r);
    void draw_marker(Painter& painter, const Point p, const uint16_t item_angle,
                     const std::string& item_tag, const Color color,
                     const Color font_color, const Color back_color);
    void draw_center(Painter& painter, const Rect& r);

    WorldMapImage image_{};
    std::vector<Color> row_{};

    float lat_{0.0f};
    float lon_{0.0f};
    int zoom_{5};

    GeoMapMode mode_{DISPLAY};
    bool manual_panning_{false};
    bool hide_center_marker_{false};
    bool show_graticule_{false};

    uint16_t angle_{invalid_angle};
    std::string tag_{};

    std::vector<GeoMarker> markers_{};

    /* The host's own position, when something (GPS, a serial feed) supplies it. */
    GeoMarker my_pos_{invalid_lat_lon, invalid_lat_lon, invalid_angle, "", Color::yellow()};
    int32_t my_altitude_{0};
};

/* --- Position entry -------------------------------------------------------- */

/* Degrees/minutes/seconds entry for latitude and longitude, plus altitude and
 * speed. Same layout as upstream's GeoPos.
 *
 * One upstream behaviour is absent: the firmware's NumberField has an `on_wrap`
 * callback that carries a seconds field rolling past 59 into the minutes field.
 * The host NumberField has no such hook, so the fields here are independent and
 * a value has to be dialled into each. */
class GeoPos : public View {
   public:
    enum alt_unit {
        FEET = 0,
        METERS
    };
    enum spd_unit {
        NONE = 0,
        MPH,
        KMPH,
        KNOTS,
        HIDDEN = 255
    };

    /* (altitude, lat, lon, speed) */
    std::function<void(int32_t, float, float, int32_t)> on_change{};

    GeoPos(const Point pos, const alt_unit altitude_unit, const spd_unit speed_unit);

    void focus() override;

    void set_read_only(bool v);
    void set_altitude(int32_t altitude);
    void set_speed(int32_t speed);
    void set_lat(float lat);
    void set_lon(float lon);
    int32_t altitude() const;
    int32_t speed() const;
    void hide_altandspeed();
    float lat() const;
    float lon() const;

    /* Suppresses on_change while the fields are being written programmatically,
     * which would otherwise feed straight back into the map. */
    void set_report_change(bool v);

   private:
    bool read_only_{false};
    bool report_change_{true};
    alt_unit altitude_unit_{};
    spd_unit speed_unit_{};

    Labels labels_position{
        {{1 * 8, 0 * 16}, "Alt:", Color::light_grey()},
        /* 0xB0 is the degree sign in the 8x16 font. */
        {{1 * 8, 1 * 16}, "Lat:    \xB0  '  \"", Color::light_grey()},
        {{1 * 8, 2 * 16}, "Lon:    \xB0  '  \"", Color::light_grey()},
    };
    Labels label_spd_position{
        {{15 * 8, 0 * 16}, "Spd:", Color::light_grey()},
    };

    NumberField field_altitude{{6 * 8, 0 * 16}, 5, {-1000, 50000}, 250, ' '};
    NumberField field_speed{{19 * 8, 0 * 16}, 4, {0, 5000}, 1, ' '};
    Text text_alt_unit{{12 * 8, 0 * 16, 2 * 8, 16}, ""};
    Text text_speed_unit{{25 * 8, 0 * 16, 5 * 8, 16}, ""};

    NumberField field_lat_degrees{{5 * 8, 1 * 16}, 4, {-90, 90}, 1, ' '};
    NumberField field_lat_minutes{{10 * 8, 1 * 16}, 2, {0, 59}, 1, ' ', true};
    NumberField field_lat_seconds{{13 * 8, 1 * 16}, 2, {0, 59}, 1, ' ', true};
    Text text_lat_decimal{{17 * 8, 1 * 16, 13 * 8, 16}, ""};

    NumberField field_lon_degrees{{5 * 8, 2 * 16}, 4, {-180, 180}, 1, ' '};
    NumberField field_lon_minutes{{10 * 8, 2 * 16}, 2, {0, 59}, 1, ' ', true};
    NumberField field_lon_seconds{{13 * 8, 2 * 16}, 2, {0, 59}, 1, ' ', true};
    Text text_lon_decimal{{17 * 8, 2 * 16, 13 * 8, 16}, ""};
};

/* --- Full-screen map view -------------------------------------------------- */

class GeoMapView : public View {
   public:
    /* Display mode: shows a position supplied by the caller. */
    GeoMapView(const std::string& tag,
               int32_t altitude,
               GeoPos::alt_unit altitude_unit,
               GeoPos::spd_unit speed_unit,
               float lat,
               float lon,
               uint16_t angle,
               const std::function<void(void)> on_close = nullptr);

    /* Prompt mode: lets the operator pick a position, then calls on_done with
     * (altitude, lat, lon, speed). */
    GeoMapView(int32_t altitude,
               GeoPos::alt_unit altitude_unit,
               GeoPos::spd_unit speed_unit,
               float lat,
               float lon,
               const std::function<void(int32_t, float, float, int32_t)> on_done);

    ~GeoMapView() override;

    GeoMapView(const GeoMapView&) = delete;
    GeoMapView& operator=(const GeoMapView&) = delete;

    std::string title() const override { return "Map view"; }

    void focus() override;
    void on_show() override;

    void update_position(float lat, float lon, uint16_t angle, int32_t altitude,
                         int32_t speed = 0);
    void update_my_position(float lat, float lon, int32_t altitude);
    void update_my_orientation(uint16_t angle, bool refresh = false);
    void update_tag(const std::string tag);

    void clear_markers();
    MapMarkerStored store_marker(const GeoMarker& marker);
    MapType get_map_type() const { return geomap.get_map_type(); }

    GeoMap& map() { return geomap; }

   private:
    void setup();

    const std::function<void(int32_t, float, float, int32_t)> on_done_{};
    std::function<void(void)> on_close_{nullptr};

    GeoMapMode mode_{DISPLAY};
    int32_t altitude_{0};
    int32_t speed_{0};
    GeoPos::alt_unit altitude_unit_{};
    GeoPos::spd_unit speed_unit_{};
    float lat_{0.0f};
    float lon_{0.0f};
    uint16_t angle_{invalid_angle};

    GeoPos geopos{{0, 0}, altitude_unit_, speed_unit_};

    /* Below the position fields, above the status bar. */
    GeoMap geomap{{0, GEOMAP_BANNER_HEIGHT, screen_width,
                   screen_height - 16 - GEOMAP_BANNER_HEIGHT}};

    Button button_ok{{screen_width - 15 * 8, 0, 15 * 8, 16}, "OK"};
};

}  // namespace ui

#endif /*__HOST_UI_GEOMAP_H__*/
