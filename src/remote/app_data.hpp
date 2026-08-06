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

#include <cstdint>
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
};
JsonValue to_json(const AppSummary& a);

/* What kind of panel a currently-open app's data can be rendered as. */
enum class PanelKind : uint8_t {
    Table,
    Spectrum,
    Receiver,
    Console,
    Map,
    Adsb,
    Form,
    Screen,
};
const char* panel_kind_name(PanelKind k);

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
    double heading_deg{0.0};
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
};
JsonValue to_json(const PanelData& p);

/* Just the kind-specific object, with no wrapper. This is what the web
 * portal's `data` field carries: PANELS.md's contract is
 * {app_id, panel_kind, title, data}, where `data` is the payload alone
 * (a `map` panel's renderer is handed {markers: [...]}, not
 * {kind: "map", map: {markers: [...]}}). to_json(PanelData) builds its
 * self-describing form on top of this. */
JsonValue panel_payload(const PanelData& p);

}  // namespace remote

#endif /*__MB200_REMOTE_APP_DATA_H__*/
