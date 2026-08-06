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
        jm.set("heading_deg", JsonValue::number(mk.heading_deg));
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

JsonValue to_json(const ScreenData& s) {
    JsonValue v = JsonValue::object();
    v.set("message", JsonValue::string(s.message));
    return v;
}

JsonValue panel_payload(const PanelData& p) {
    switch (p.kind) {
        case PanelKind::Table: return to_json(p.table);
        case PanelKind::Spectrum: return to_json(p.spectrum);
        case PanelKind::Receiver: return to_json(p.receiver);
        case PanelKind::Console: return to_json(p.console);
        case PanelKind::Map: return to_json(p.map);
        case PanelKind::Adsb: return to_json(p.adsb);
        case PanelKind::Form: return to_json(p.form);
        case PanelKind::Screen: return to_json(p.screen);
    }
    return JsonValue::object();
}

JsonValue to_json(const PanelData& p) {
    JsonValue v = JsonValue::object();
    v.set("kind", JsonValue::string(panel_kind_name(p.kind)));
    v.set(panel_kind_name(p.kind), panel_payload(p));
    return v;
}

}  // namespace remote
