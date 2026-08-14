/*
 * mayhem-b200 — web portal data model and JSON writer.
 *
 * The apps already compute structured results (ui::RecentEntriesTable rows,
 * FFT bins, receiver state); today that data only ever gets drawn into the
 * 240x320 framebuffer. This file defines the shapes that data is published
 * as (PanelData and friends) and a small, dependency-free JSON serializer to
 * write them out. Nothing here touches an app's own logic — see
 * app_bridge.hpp for the seam that pulls data out of a running app.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_REMOTE_APP_DATA_H__
#define __MB200_REMOTE_APP_DATA_H__

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace remote {

/* ---------------------------------------------------------------------------
 * JSON writer.
 *
 * The project has no JSON library and the portal only ever WRITES JSON (the
 * few POST bodies it accepts carry at most one known field and are read ad
 * hoc in remote_server.cpp), so this is a value builder plus dump() rather
 * than a parser/DOM. Object keys are kept in insertion order — no sorting —
 * so output is deterministic and easy to assert against in tests.
 * ------------------------------------------------------------------------- */
class JsonValue {
   public:
    JsonValue() : type_{Type::Null} {}

    static JsonValue null() { return JsonValue{}; }
    static JsonValue boolean(bool b);
    static JsonValue number(double d);
    static JsonValue integer(int64_t i);
    static JsonValue string(std::string s);
    static JsonValue array();
    static JsonValue object();

    /* Array only; appending to a non-array value is a no-op. Returns *this so
     * calls can be chained. */
    JsonValue& push_back(JsonValue v);

    /* Object only; setting on a non-object value is a no-op. Overwrites an
     * existing key rather than duplicating it. Returns *this for chaining. */
    JsonValue& set(std::string key, JsonValue v);

    /* Serializes this value, and everything nested inside it, to compact
     * JSON text. */
    std::string dump() const;

   private:
    enum class Type { Null, Bool, Number, Integer, String, Array, Object };

    void dump_into(std::string& out) const;
    static void escape_into(const std::string& s, std::string& out);

    Type type_;
    bool bool_{false};
    double num_{0.0};
    int64_t int_{0};
    std::string str_{};
    std::vector<JsonValue> arr_{};
    std::vector<std::pair<std::string, JsonValue>> obj_{};
};

/* Formats a double the way the portal wants every number on the wire:
 * whole values print with no decimal point ("5000000", not "5e+06" or
 * "5000000.000000") and fractional values print fixed-point with trailing
 * zeros trimmed. Never emits scientific notation or NaN/Inf. Exposed
 * because tests assert against it directly. */
std::string format_number(double d);

/* ---------------------------------------------------------------------------
 * Data model
 * ------------------------------------------------------------------------- */

/* What kind of panel a currently-open app's data can be rendered as.
 *
 * The enumerator's numeric value is never on the wire — panel_kind_name() is —
 * so new kinds are appended here rather than slotted in alphabetically.
 *
 * Declared above AppSummary because that struct carries one: the app list has
 * to say which apps have a real browser view, not just the one app that
 * happens to be open. */
enum class PanelKind : uint8_t {
    Table,
    Spectrum,
    Receiver,
    Console,
    Map,
    Adsb,
    Form,
    Screen,
    Image,
    GeoTable,
};
const char* panel_kind_name(PanelKind k);

/* One entry from app::AppRegistry, reshaped for the portal. */
struct AppSummary {
    std::string id;
    std::string name;
    std::string category;
    bool hardware_limited{false};
    /* ui::AppEntry's icon is an opaque framebuffer Bitmap* with no name, so
     * there is nothing to read a real icon name from; the app id doubles as
     * a stable slug the frontend can map to its own icon set. */
    std::string icon_name;
    /* The kind of panel this app's registered provider publishes, or nothing
     * at all when no provider is registered for it (see AppBridge's provider
     * map). This is what lets the browser's app grid badge which apps have a
     * real native view without keeping a list of app ids of its own — a list
     * that would be wrong the first time a provider is added or dropped.
     *
     * An optional rather than a string: "no provider" and "some provider that
     * published an empty kind" are different answers, and only the first one
     * is ever true. to_json() omits the key entirely when it is unset, so
     * absent on the wire means absent here. */
    std::optional<PanelKind> panel_kind{};
};
/* Emits panel_kind only for an app that really has a native panel:
 * PanelKind::Screen is the framebuffer mirror EVERY app has, so advertising
 * it would badge all 104 of them as having a native view. See to_json()'s
 * comment in app_data.cpp — this is the one gate that rule lives behind. */
JsonValue to_json(const AppSummary& a);

/* A ui::RecentEntriesTable, walked into rows of already-formatted strings.
 * See app_bridge.hpp's table_data_from_entries() for the generic adapter
 * that builds one of these from any app's Entries container. */
struct TableData {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
};
JsonValue to_json(const TableData& t);

/* One FFT frame: centre frequency and span describe the x-axis, bins_db is
 * the y-axis in dBFS, most-negative-frequency first (dsp::Fft::spectrum_db's
 * layout). Empty bins_db means no samples were available yet — never filled
 * with fabricated data. */
struct SpectrumData {
    uint64_t centre_hz{0};
    double span_hz{0.0};
    std::vector<float> bins_db;
};
JsonValue to_json(const SpectrumData& s);

/* radio::ReceiverModel's live state. */
struct ReceiverData {
    std::string mode;
    uint64_t frequency_hz{0};
    double gain_db{0.0};
    uint8_t squelch{0};
    uint8_t volume{0};
    float channel_level_db{-140.0f};
    float rf_level_db{-140.0f};
    bool squelch_open{false};
    bool running{false};
    /* From the attached radio's caps; valid only when a device is open —
     * absent bounds render the gain control read-only, per PANELS.md. */
    double gain_min_db{0.0};
    double gain_max_db{0.0};
    bool gain_range_valid{false};
};
JsonValue to_json(const ReceiverData& r);

/* A scrolling text log (e.g. a protocol console view). */
struct ConsoleData {
    std::vector<std::string> lines;
};
JsonValue to_json(const ConsoleData& c);

struct MapMarker {
    double lat{0.0};
    double lon{0.0};
    std::string label;
    /* Optional rather than a plain double defaulting to 0.0: most marker
     * sources carry no heading at all — WardriveMap builds its own GeoMarkers
     * with ui::invalid_angle (ui_wardrivemap.cpp's load_markers) — and a 0.0
     * on the wire would draw every one of them pointing due north. An optional
     * cannot be set and then silently dropped, which a parallel has_heading
     * flag could be; existing brace initializers that pass a double still
     * compile and still serialize the heading. */
    std::optional<double> heading_deg{};
};
struct MapData {
    std::vector<MapMarker> markers;
};
JsonValue to_json(const MapData& m);

/* One tracked aircraft, as ADS-B RX's AircraftRecentEntry already holds it.
 * Nothing here is decoded or derived by the portal: every field is copied
 * straight out of the app's own tracker (see src/apps/ui_adsb_rx.hpp). The
 * `*_valid` flags exist because "not heard yet" and "zero" are different
 * things -- an aircraft with no position fix is not at 0N 0E, and one whose
 * velocity message has not arrived is not stationary. */
struct AdsbAircraft {
    std::string icao;     /* 6 hex digits, upper case */
    std::string callsign; /* empty until a BDS 2,0 identity frame arrives */
    std::string info;     /* the app's own free-text info line */
    std::string state;    /* "current" | "recent" | "old" | "expired" | "invalid" */

    bool pos_valid{false};
    double lat{0.0};
    double lon{0.0};

    bool altitude_valid{false};
    int32_t altitude_ft{0};
    bool on_ground{false};

    bool velocity_valid{false};
    int32_t speed_kt{0};
    uint16_t heading_deg{0};
    int32_t vertical_rate_fpm{0};

    uint16_t squawk{0};
    uint32_t messages{0};
    uint32_t age_s{0};
    /* Frame amplitude, the app's stand-in for RSSI, on the same scale the
     * device's Amp column uses: roughly twice the preamble's power ratio to
     * the receiver's noise floor, so 0..255 covers about 21 dB of signal. */
    uint32_t amp{0};
};

/* The whole ADS-B picture for one poll. `home_valid` is false unless the
 * operator has told the app where the receiver is; the browser then asks for
 * one rather than inventing a range. */
struct AdsbData {
    std::vector<AdsbAircraft> aircraft;
    bool home_valid{false};
    double home_lat{0.0};
    double home_lon{0.0};
    uint32_t frames_seen{0};
    uint32_t frames_accepted{0};
};
JsonValue to_json(const AdsbData& a);

/* A read-only label/value form (settings-style views). */
struct FormField {
    std::string label;
    std::string value;
};
struct FormData {
    std::vector<FormField> fields;
};
JsonValue to_json(const FormData& f);

/* ---------------------------------------------------------------------------
 * Image panel
 *
 * A decoded picture an app is already holding in memory, published so the
 * browser can show it at its own scale instead of squinting at the 240x320
 * screen. The payload is deliberately dumb: width, height, rgb888 bytes.
 *
 * `rev` is what keeps a ~200 kB base64 blob off the wire on every 700 ms poll.
 * It increments ONLY when the pixels actually changed (see ImageRevCounter),
 * and the client tells the server which rev it already has; data_b64 is then
 * omitted rather than re-sent. rev 0 means nothing has been decoded yet — in
 * that state `rgb` is empty and `note` says so. A black frame is never
 * synthesized to fill the gap: "no picture yet" and "a picture that happens to
 * be black" are different answers, and only the first one is true here.
 * ------------------------------------------------------------------------- */
struct ImageData {
    std::string app_name; /* empty = omitted */
    uint32_t width{0};
    uint32_t height{0};
    uint32_t rev{0};
    /* width*height*3 bytes, R,G,B per pixel, first row first. Empty when
     * nothing has been decoded. */
    std::vector<uint8_t> rgb;
    std::string note; /* empty = omitted */
};

/* `have_rev` is the rev the client says it already holds (0 = "nothing"):
 * data_b64 is omitted when it matches, which is the whole point of rev. */
JsonValue to_json(const ImageData& i, uint32_t have_rev = 0);

/* ---------------------------------------------------------------------------
 * Geo-table panel
 *
 * A table and a map of the same entries, for apps whose rows carry a position:
 * the table is exactly the flat TableData shape the app already published, so
 * upgrading an app from `table` to `geotable` cannot change a single cell, and
 * the markers are a strict addition alongside it.
 *
 * The load-bearing rule is that an entry WITHOUT a position fix appears in the
 * table and NOT on the map. Absent stays absent; a station heard only through
 * a status packet is not at 0N 0E.
 * ------------------------------------------------------------------------- */
struct GeoTableMarker {
    double lat{0.0};
    double lon{0.0};
    std::string label;
    /* Absent unless the entry really carried one — see MapMarker. */
    std::optional<double> heading_deg{};
    /* A rendering hint for the browser's icon choice ("vessel", "station",
     * ...), not decoded data. Empty = omitted. */
    std::string kind;
};
struct GeoTableData {
    std::string app_name; /* empty = omitted */
    TableData table;
    std::vector<GeoTableMarker> markers;
};
JsonValue to_json(const GeoTableData& g);

/* The fallback: no structured provider exists (yet) for the open app, or no
 * app is open. `message` is shown to the operator instead of an empty panel. */
struct ScreenData {
    std::string message;
};
JsonValue to_json(const ScreenData& s);

/* A tagged union of the above, keyed by `kind`. Not a std::variant: every
 * member is small, default-constructible and harmless when unused, and a
 * plain struct keeps to_json()'s dispatch a simple switch rather than a
 * visitor. Only the member matching `kind` is ever populated or serialized. */
struct PanelData {
    PanelKind kind{PanelKind::Screen};
    TableData table{};
    SpectrumData spectrum{};
    ReceiverData receiver{};
    ConsoleData console{};
    MapData map{};
    AdsbData adsb{};
    FormData form{};
    ScreenData screen{};
    ImageData image{};
    GeoTableData geotable{};
};
JsonValue to_json(const PanelData& p);

/* Just the kind-specific object, with no wrapper. This is what the web
 * portal's `data` field carries: PANELS.md's contract is
 * {app_id, panel_kind, title, data}, where `data` is the payload alone
 * (a `map` panel's renderer is handed {markers: [...]}, not
 * {kind: "map", map: {markers: [...]}}). to_json(PanelData) builds its
 * self-describing form on top of this.
 *
 * `have_image_rev` is only consulted for PanelKind::Image and is threaded in
 * from the request (GET /api/panel?have_image_rev=N); it defaults to 0, which
 * means "the client has nothing", because a rev of 0 never carries pixels
 * anyway. Every other kind ignores it. */
JsonValue panel_payload(const PanelData& p, uint32_t have_image_rev = 0);

/* ---------------------------------------------------------------------------
 * Pixel and payload helpers
 *
 * Shared by the image providers (provider_apt/wefax/sstv), which all read the
 * same thing: a framebuffer of ui::Color, i.e. native-endian RGB565 laid out
 * rrrrrggggggbbbbb. They are declared on uint16_t rather than ui::Color so
 * this header keeps its independence from src/ui, and exposed rather than left
 * file-local in one provider so the other two do not each grow a copy.
 * ------------------------------------------------------------------------- */

/* Standard base64 (RFC 4648 section 4), '=' padded. */
std::string base64_encode(const uint8_t* data, size_t count);

/* True when every pixel is black — the state a freshly constructed or cleared
 * scan canvas is in, and the one case where there is genuinely no image to
 * publish. An empty buffer is blank too. */
bool rgb565_is_blank(const uint16_t* pixels, size_t count);

/* FNV-1a over the raw pixels. Used only to answer "did anything change since
 * the last frame", never as an identity the client sees. */
uint64_t rgb565_content_hash(const uint16_t* pixels, size_t count);

/* Expands to RGB888 with the low bits replicated, so a full-scale channel
 * reaches 0xFF rather than 0xF8. Exactly Display::composite_bgra()'s
 * expansion (src/ui/display.cpp) — the browser must not show a picture that
 * differs from the device's own screen by a systematic 3% darkening. */
void rgb565_to_rgb888(const uint16_t* pixels, size_t count, std::vector<uint8_t>& out);

/* Turns "the pixels changed" into Contract 4's monotonic `rev`.
 *
 * A raw content hash cannot be the rev: the contract says rev increments, and
 * a client comparing an unordered 64-bit value could not tell a new picture
 * from an old one it had already discarded. So the hash is kept privately and
 * a counter is bumped when it moves. The counter never bumps for an unchanged
 * frame, which is the whole reason it exists — every bump costs the browser a
 * fresh ~200 kB fetch.
 *
 * UI thread only: providers run solely from AppBridge::refresh(), so each one
 * holds a function-local static of this with no locking (app_bridge.hpp's file
 * header is the contract). */
class ImageRevCounter {
   public:
    /* Returns the rev for this frame. The first ever call yields 1, so 0 stays
     * reserved for "nothing decoded yet". */
    uint32_t observe(uint64_t content_hash);

    uint32_t rev() const { return rev_; }

   private:
    uint64_t last_hash_{0};
    uint32_t rev_{0};
    bool seeded_{false};
};

}  // namespace remote

#endif /*__MB200_REMOTE_APP_DATA_H__*/
