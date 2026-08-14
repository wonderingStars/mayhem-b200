/*
 * mayhem-b200 — web portal data model and JSON writer.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_data.hpp"

#include <cmath>
#include <cstdio>

namespace remote {

/* --- JsonValue --------------------------------------------------------- */

JsonValue JsonValue::boolean(bool b) {
    JsonValue v;
    v.type_ = Type::Bool;
    v.bool_ = b;
    return v;
}

JsonValue JsonValue::number(double d) {
    JsonValue v;
    v.type_ = Type::Number;
    v.num_ = d;
    return v;
}

JsonValue JsonValue::integer(int64_t i) {
    JsonValue v;
    v.type_ = Type::Integer;
    v.int_ = i;
    return v;
}

JsonValue JsonValue::string(std::string s) {
    JsonValue v;
    v.type_ = Type::String;
    v.str_ = std::move(s);
    return v;
}

JsonValue JsonValue::array() {
    JsonValue v;
    v.type_ = Type::Array;
    return v;
}

JsonValue JsonValue::object() {
    JsonValue v;
    v.type_ = Type::Object;
    return v;
}

JsonValue& JsonValue::push_back(JsonValue v) {
    if (type_ == Type::Array) arr_.push_back(std::move(v));
    return *this;
}

JsonValue& JsonValue::set(std::string key, JsonValue v) {
    if (type_ != Type::Object) return *this;
    for (auto& kv : obj_) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return *this;
        }
    }
    obj_.emplace_back(std::move(key), std::move(v));
    return *this;
}

std::string JsonValue::dump() const {
    std::string out;
    dump_into(out);
    return out;
}

void JsonValue::escape_into(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

void JsonValue::dump_into(std::string& out) const {
    switch (type_) {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += bool_ ? "true" : "false";
            break;
        case Type::Number:
            out += format_number(num_);
            break;
        case Type::Integer:
            out += std::to_string(int_);
            break;
        case Type::String:
            escape_into(str_, out);
            break;
        case Type::Array: {
            out.push_back('[');
            for (size_t i = 0; i < arr_.size(); i++) {
                if (i > 0) out.push_back(',');
                arr_[i].dump_into(out);
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            for (size_t i = 0; i < obj_.size(); i++) {
                if (i > 0) out.push_back(',');
                escape_into(obj_[i].first, out);
                out.push_back(':');
                obj_[i].second.dump_into(out);
            }
            out.push_back('}');
            break;
        }
    }
}

std::string format_number(double d) {
    if (!std::isfinite(d)) return "0";

    /* Whole numbers (the common case: frequencies, counts) print with no
     * decimal point at all. */
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", d);
        return buf;
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", d);
    std::string s{buf};

    /* Trim trailing zeros, then a trailing '.' if that's all that's left. */
    const size_t last_nonzero = s.find_last_not_of('0');
    if (last_nonzero != std::string::npos) s.erase(last_nonzero + 1);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

/* --- Data model: to_json() ---------------------------------------------- */

JsonValue to_json(const AppSummary& a) {
    JsonValue v = JsonValue::object();
    v.set("id", JsonValue::string(a.id));
    /* display_name / icon, not name / icon_name: these key names are the wire
     * contract with internal/portal/client's App struct, which is what the
     * browser's app grid is built from. */
    v.set("display_name", JsonValue::string(a.name));
    v.set("category", JsonValue::string(a.category));
    v.set("hardware_limited", JsonValue::boolean(a.hardware_limited));
    v.set("icon", JsonValue::string(a.icon_name));
    /* panel_kind is OMITTED, not emptied, when the app has no provider: the
     * browser reads its absence as "unknown/none" and draws no badge, and an
     * empty string would have to be special-cased on every hop (Go's
     * omitempty would drop it again anyway, so a "" that meant something
     * could not survive the re-encode).
     *
     * Screen is filtered here rather than at the registration site because
     * this is the wire, and the rule is about the wire: every app can be
     * mirrored as a framebuffer, so "screen" says nothing about whether an
     * app has a native panel. Advertising it would badge the entire grid. */
    if (a.panel_kind.has_value() && *a.panel_kind != PanelKind::Screen)
        v.set("panel_kind", JsonValue::string(panel_kind_name(*a.panel_kind)));
    return v;
}

const char* panel_kind_name(PanelKind k) {
    switch (k) {
        case PanelKind::Table: return "table";
        case PanelKind::Spectrum: return "spectrum";
        case PanelKind::Receiver: return "receiver";
        case PanelKind::Console: return "console";
        case PanelKind::Map: return "map";
        case PanelKind::Adsb: return "adsb";
        case PanelKind::Form: return "form";
        case PanelKind::Screen: return "screen";
        case PanelKind::Image: return "image";
        case PanelKind::GeoTable: return "geotable";
        case PanelKind::Ais: return "ais";
    }
    return "screen";
}

JsonValue to_json(const TableData& t) {
    JsonValue v = JsonValue::object();
    JsonValue cols = JsonValue::array();
    for (const auto& c : t.columns) cols.push_back(JsonValue::string(c));
    v.set("columns", std::move(cols));

    JsonValue rows = JsonValue::array();
    for (const auto& row : t.rows) {
        JsonValue r = JsonValue::array();
        for (const auto& cell : row) r.push_back(JsonValue::string(cell));
        rows.push_back(std::move(r));
    }
    v.set("rows", std::move(rows));
    return v;
}

JsonValue to_json(const SpectrumData& s) {
    JsonValue v = JsonValue::object();
    v.set("centre_hz", JsonValue::integer(static_cast<int64_t>(s.centre_hz)));
    v.set("span_hz", JsonValue::number(s.span_hz));
    JsonValue bins = JsonValue::array();
    for (float b : s.bins_db) bins.push_back(JsonValue::number(static_cast<double>(b)));
    v.set("bins_db", std::move(bins));
    return v;
}

JsonValue to_json(const ReceiverData& r) {
    JsonValue v = JsonValue::object();
    v.set("mode", JsonValue::string(r.mode));
    v.set("frequency_hz", JsonValue::integer(static_cast<int64_t>(r.frequency_hz)));
    v.set("gain_db", JsonValue::number(r.gain_db));
    v.set("squelch", JsonValue::integer(r.squelch));
    v.set("volume", JsonValue::integer(r.volume));
    v.set("channel_level_db", JsonValue::number(r.channel_level_db));
    v.set("rf_level_db", JsonValue::number(r.rf_level_db));
    v.set("squelch_open", JsonValue::boolean(r.squelch_open));
    v.set("running", JsonValue::boolean(r.running));
    return v;
}

JsonValue to_json(const ConsoleData& c) {
    JsonValue v = JsonValue::object();
    JsonValue lines = JsonValue::array();
    for (const auto& l : c.lines) lines.push_back(JsonValue::string(l));
    v.set("lines", std::move(lines));
    return v;
}

JsonValue to_json(const MapData& m) {
    JsonValue v = JsonValue::object();
    JsonValue markers = JsonValue::array();
    for (const auto& mk : m.markers) {
        JsonValue jm = JsonValue::object();
        jm.set("lat", JsonValue::number(mk.lat));
        jm.set("lon", JsonValue::number(mk.lon));
        jm.set("label", JsonValue::string(mk.label));
        /* Omitted, not zeroed, when the source had no heading. */
        if (mk.heading_deg.has_value())
            jm.set("heading_deg", JsonValue::number(*mk.heading_deg));
        markers.push_back(std::move(jm));
    }
    v.set("markers", std::move(markers));
    return v;
}

JsonValue to_json(const AdsbData& a) {
    JsonValue v = JsonValue::object();

    JsonValue list = JsonValue::array();
    for (const auto& ac : a.aircraft) {
        JsonValue j = JsonValue::object();
        j.set("icao", JsonValue::string(ac.icao));
        j.set("callsign", JsonValue::string(ac.callsign));

        /* has_pos is what the browser filters and draws on; lat/lon are only
         * emitted when they mean something, so a missing fix can never be
         * mistaken for a position off the Gulf of Guinea. */
        j.set("has_pos", JsonValue::boolean(ac.pos_valid));
        if (ac.pos_valid) {
            j.set("lat", JsonValue::number(ac.lat));
            j.set("lon", JsonValue::number(ac.lon));
        }
        if (ac.altitude_valid) j.set("altitude_ft", JsonValue::integer(ac.altitude_ft));
        j.set("on_ground", JsonValue::boolean(ac.on_ground));
        if (ac.velocity_valid) {
            j.set("speed_kt", JsonValue::integer(ac.speed_kt));
            j.set("heading_deg", JsonValue::integer(ac.heading_deg));
            j.set("vertical_rate_fpm", JsonValue::integer(ac.vertical_rate_fpm));
        }
        if (ac.squawk != 0) j.set("squawk", JsonValue::integer(ac.squawk));
        j.set("messages", JsonValue::integer(ac.messages));
        j.set("age_s", JsonValue::integer(ac.age_s));
        j.set("rssi", JsonValue::integer(ac.amp));
        if (!ac.state.empty()) j.set("state", JsonValue::string(ac.state));
        if (!ac.info.empty()) j.set("info", JsonValue::string(ac.info));
        list.push_back(std::move(j));
    }
    v.set("aircraft", std::move(list));

    if (a.home_valid) {
        JsonValue home = JsonValue::object();
        home.set("lat", JsonValue::number(a.home_lat));
        home.set("lon", JsonValue::number(a.home_lon));
        v.set("home", std::move(home));
    }

    JsonValue stats = JsonValue::object();
    stats.set("frames_seen", JsonValue::integer(a.frames_seen));
    stats.set("frames_accepted", JsonValue::integer(a.frames_accepted));
    v.set("stats", std::move(stats));

    return v;
}

/* Every optional below is a key that is written or not written at all. There is
 * no branch anywhere in here that substitutes a zero, an empty string or a
 * sentinel for a value a vessel has not broadcast — see the AisVessel comment
 * in app_data.hpp for why that is the load-bearing rule of this payload. */
JsonValue to_json(const AisVessel& v) {
    JsonValue j = JsonValue::object();
    /* The key, and the only field that is unconditional. */
    j.set("mmsi", JsonValue::string(v.mmsi));
    if (!v.name.empty()) j.set("name", JsonValue::string(v.name));
    if (!v.callsign.empty()) j.set("callsign", JsonValue::string(v.callsign));
    if (!v.destination.empty()) j.set("destination", JsonValue::string(v.destination));

    /* Both or neither: a lone latitude is not a position, and 0,0 is not
     * "unknown". Unlike the ADS-B payload there is no has_pos companion — the
     * keys' presence IS the answer, and the browser tests for lat. */
    if (v.pos_valid) {
        j.set("lat", JsonValue::number(v.lat));
        j.set("lon", JsonValue::number(v.lon));
    }

    if (v.sog_kn.has_value()) j.set("sog_kn", JsonValue::number(*v.sog_kn));
    if (v.cog_deg.has_value()) j.set("cog_deg", JsonValue::number(*v.cog_deg));
    if (v.heading_deg.has_value()) j.set("heading_deg", JsonValue::number(*v.heading_deg));
    if (v.nav_status.has_value()) j.set("nav_status", JsonValue::integer(*v.nav_status));

    j.set("msgs", JsonValue::integer(v.msgs));
    if (!v.time.empty()) j.set("time", JsonValue::string(v.time));
    return j;
}

JsonValue to_json(const AisData& a) {
    JsonValue v = JsonValue::object();

    JsonValue list = JsonValue::array();
    for (const auto& vessel : a.vessels) list.push_back(to_json(vessel));
    v.set("vessels", std::move(list));

    /* An object rather than a bare number, so the counts the app already keeps
     * (CRC and length failures) can join it later without moving anything. */
    JsonValue stats = JsonValue::object();
    stats.set("packets_valid", JsonValue::integer(a.packets_valid));
    v.set("stats", std::move(stats));

    return v;
}

JsonValue to_json(const FormData& f) {
    JsonValue v = JsonValue::object();
    JsonValue fields = JsonValue::array();
    for (const auto& fld : f.fields) {
        JsonValue jf = JsonValue::object();
        jf.set("label", JsonValue::string(fld.label));
        jf.set("value", JsonValue::string(fld.value));
        fields.push_back(std::move(jf));
    }
    v.set("fields", std::move(fields));
    return v;
}

JsonValue to_json(const ImageData& i, uint32_t have_rev) {
    JsonValue v = JsonValue::object();
    if (!i.app_name.empty()) v.set("app_name", JsonValue::string(i.app_name));
    v.set("width", JsonValue::integer(i.width));
    v.set("height", JsonValue::integer(i.height));
    /* The only format there is. Named on the wire anyway so a future 16-bit
     * or palettised payload cannot be mistaken for this one. */
    v.set("format", JsonValue::string("rgb888"));
    v.set("rev", JsonValue::integer(i.rev));

    /* Three separate reasons to send no pixels, all of them legitimate:
     * nothing decoded yet (rgb empty, rev 0), the client already holds this
     * rev, or — defensively — a payload whose length disagrees with the stated
     * geometry, which the browser would render as diagonal garbage. */
    const size_t expected =
        static_cast<size_t>(i.width) * static_cast<size_t>(i.height) * 3u;
    if (!i.rgb.empty() && i.rgb.size() == expected && i.rev != have_rev)
        v.set("data_b64", JsonValue::string(base64_encode(i.rgb.data(), i.rgb.size())));

    if (!i.note.empty()) v.set("note", JsonValue::string(i.note));
    return v;
}

JsonValue to_json(const GeoTableData& g) {
    JsonValue v = JsonValue::object();
    if (!g.app_name.empty()) v.set("app_name", JsonValue::string(g.app_name));

    /* Byte-for-byte the flat table shape a `table` panel emits, so an app can
     * be upgraded from one kind to the other without a single cell moving. */
    v.set("table", to_json(g.table));

    JsonValue markers = JsonValue::array();
    for (const auto& mk : g.markers) {
        JsonValue jm = JsonValue::object();
        jm.set("lat", JsonValue::number(mk.lat));
        jm.set("lon", JsonValue::number(mk.lon));
        jm.set("label", JsonValue::string(mk.label));
        if (mk.heading_deg.has_value())
            jm.set("heading_deg", JsonValue::number(*mk.heading_deg));
        if (!mk.kind.empty()) jm.set("kind", JsonValue::string(mk.kind));
        markers.push_back(std::move(jm));
    }
    JsonValue map = JsonValue::object();
    map.set("markers", std::move(markers));
    v.set("map", std::move(map));
    return v;
}

JsonValue to_json(const ScreenData& s) {
    JsonValue v = JsonValue::object();
    v.set("message", JsonValue::string(s.message));
    return v;
}

JsonValue panel_payload(const PanelData& p, uint32_t have_image_rev) {
    switch (p.kind) {
        case PanelKind::Table: return to_json(p.table);
        case PanelKind::Spectrum: return to_json(p.spectrum);
        case PanelKind::Receiver: return to_json(p.receiver);
        case PanelKind::Console: return to_json(p.console);
        case PanelKind::Map: return to_json(p.map);
        case PanelKind::Adsb: return to_json(p.adsb);
        case PanelKind::Form: return to_json(p.form);
        case PanelKind::Screen: return to_json(p.screen);
        case PanelKind::Image: return to_json(p.image, have_image_rev);
        case PanelKind::GeoTable: return to_json(p.geotable);
        case PanelKind::Ais: return to_json(p.ais);
    }
    return JsonValue::object();
}

JsonValue to_json(const PanelData& p) {
    JsonValue v = JsonValue::object();
    v.set("kind", JsonValue::string(panel_kind_name(p.kind)));
    v.set(panel_kind_name(p.kind), panel_payload(p));
    return v;
}

/* --- Pixel and payload helpers ------------------------------------------- */

std::string base64_encode(const uint8_t* data, size_t count) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    if (data == nullptr || count == 0) return out;
    out.reserve(((count + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 3 <= count; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back(kAlphabet[v & 0x3F]);
    }

    const size_t rest = count - i;
    if (rest == 1) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out += "==";
    } else if (rest == 2) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool rgb565_is_blank(const uint16_t* pixels, size_t count) {
    if (pixels == nullptr) return true;
    for (size_t i = 0; i < count; i++) {
        if (pixels[i] != 0) return false;
    }
    return true;
}

uint64_t rgb565_content_hash(const uint16_t* pixels, size_t count) {
    /* FNV-1a, 64-bit. Fed the two bytes of each pixel separately so the hash
     * of a buffer does not depend on this host's endianness. */
    uint64_t h = 1469598103934665603ULL;
    if (pixels == nullptr) return h;
    for (size_t i = 0; i < count; i++) {
        const uint16_t v = pixels[i];
        h = (h ^ static_cast<uint8_t>(v & 0xFF)) * 1099511628211ULL;
        h = (h ^ static_cast<uint8_t>((v >> 8) & 0xFF)) * 1099511628211ULL;
    }
    return h;
}

void rgb565_to_rgb888(const uint16_t* pixels, size_t count, std::vector<uint8_t>& out) {
    out.clear();
    if (pixels == nullptr || count == 0) return;
    out.resize(count * 3);

    for (size_t i = 0; i < count; i++) {
        const uint16_t v = pixels[i];
        const uint32_t r5 = (v >> 11) & 0x1F;
        const uint32_t g6 = (v >> 5) & 0x3F;
        const uint32_t b5 = v & 0x1F;
        out[i * 3 + 0] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
        out[i * 3 + 1] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
        out[i * 3 + 2] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
    }
}

uint32_t ImageRevCounter::observe(uint64_t content_hash) {
    if (!seeded_ || content_hash != last_hash_) {
        last_hash_ = content_hash;
        seeded_ = true;
        rev_++;
    }
    return rev_;
}

}  // namespace remote
