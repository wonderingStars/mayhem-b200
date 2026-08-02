/*
 * mayhem-b200 — persistent settings.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (pmem semantics)
 * Copyright (C) 2023 Kyle Reed (app_settings semantics)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "settings.hpp"

#include "file_path.hpp"
#include "string_format.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace core {

namespace {

/* The firmware writes CRLF and so do we: these files get opened in whatever
 * text editor the user has, and a stray LF-only file still reads as one long
 * line in a few of them. The parser strips '\r' regardless. */
constexpr const char* line_ending = "\r\n";

std::string strip_newlines(std::string_view value) {
    std::string out{value};
    for (char& c : out) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return out;
}

bool parse_i64_text(std::string_view sv, int64_t& out) {
    const std::string s = ::trim(sv);
    if (s.empty()) return false;

    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (end != s.c_str() + s.size()) return false;
    if (errno == ERANGE) return false;

    out = static_cast<int64_t>(v);
    return true;
}

bool parse_u64_text(std::string_view sv, uint64_t& out) {
    const std::string s = ::trim(sv);
    /* strtoull happily turns "-1" into ULLONG_MAX. In a settings file that is
     * never what the user meant, so reject it rather than store a surprise. */
    if (s.empty() || s.front() == '-') return false;

    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (end != s.c_str() + s.size()) return false;
    if (errno == ERANGE) return false;

    out = static_cast<uint64_t>(v);
    return true;
}

bool parse_f32_text(std::string_view sv, float& out) {
    const std::string s = ::trim(sv);
    if (s.empty()) return false;

    errno = 0;
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return false;
    if (errno == ERANGE) return false;

    out = v;
    return true;
}

bool parse_bool_text(std::string_view sv, bool& out) {
    std::string s = ::trim(sv);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (s == "1" || s == "true" || s == "yes" || s == "on") {
        out = true;
        return true;
    }
    if (s == "0" || s == "false" || s == "no" || s == "off") {
        out = false;
        return true;
    }

    /* Anything else numeric follows the firmware's rule: non-zero is true. */
    int64_t n = 0;
    if (parse_i64_text(s, n)) {
        out = (n != 0);
        return true;
    }

    return false;
}

std::string format_float(float v) {
    /* 9 significant digits is the shortest decimal form guaranteed to recover
     * an IEEE-754 single exactly. The firmware writes 6 fixed decimals, which
     * loses both magnitude and precision; this file is ours, so accuracy wins. */
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    return std::string{buf};
}

/* Reads a whole file. Returns false if it could not be opened. */
bool read_file(const std::string& path, std::string& out) {
    std::ifstream f{path, std::ios::binary};
    if (!f) return false;

    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const std::string& path, const std::string& text) {
    const auto parent = std::filesystem::path{path}.parent_path();
    if (!parent.empty() && !ensure_directory(parent.string())) return false;

    std::ofstream f{path, std::ios::binary | std::ios::trunc};
    if (!f) return false;

    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return f.good();
}

/* Splits INI text into lines, dropping the '\r' of a CRLF pair. */
std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t start = 0;

    while (start <= text.size()) {
        const auto nl = text.find('\n', start);
        auto line = (nl == std::string_view::npos) ? text.substr(start)
                                                   : text.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);

        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }

    return lines;
}

}  // namespace

/* --------------------------------------------------------------------- */

std::string settings_directory() {
    return (std::filesystem::path{data_directory()} / "SETTINGS").string();
}

std::string settings_file_path() {
    return (std::filesystem::path{data_directory()} / "settings.ini").string();
}

/* Settings ------------------------------------------------------------- */

Settings::Settings()
    : path_{settings_file_path()} {}

Settings::Settings(std::string file_path)
    : path_{std::move(file_path)} {}

const Settings::Section* Settings::find_section(std::string_view name) const {
    for (const auto& s : sections_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

Settings::Section& Settings::section_for(std::string_view name) {
    for (auto& s : sections_) {
        if (s.name == name) return s;
    }
    sections_.push_back(Section{std::string{name}, {}});
    return sections_.back();
}

const Settings::Entry* Settings::find_entry(std::string_view section,
                                            std::string_view key) const {
    const auto* s = find_section(section);
    if (s == nullptr) return nullptr;

    for (const auto& e : s->entries) {
        if (e.key == key) return &e;
    }
    return nullptr;
}

void Settings::set_raw(std::string_view section, std::string_view key,
                       std::string value) {
    auto& s = section_for(section);
    for (auto& e : s.entries) {
        if (e.key == key) {
            e.value = std::move(value);
            dirty_ = true;
            return;
        }
    }
    s.entries.push_back(Entry{std::string{key}, std::move(value)});
    dirty_ = true;
}

bool Settings::has(std::string_view section, std::string_view key) const {
    return find_entry(section, key) != nullptr;
}

bool Settings::erase(std::string_view section, std::string_view key) {
    for (auto& s : sections_) {
        if (s.name != section) continue;

        for (auto it = s.entries.begin(); it != s.entries.end(); ++it) {
            if (it->key == key) {
                s.entries.erase(it);
                dirty_ = true;
                return true;
            }
        }
    }
    return false;
}

bool Settings::erase_section(std::string_view section) {
    for (auto it = sections_.begin(); it != sections_.end(); ++it) {
        if (it->name == section) {
            sections_.erase(it);
            dirty_ = true;
            return true;
        }
    }
    return false;
}

void Settings::clear() {
    sections_.clear();
    dirty_ = true;
}

bool Settings::get_bool(std::string_view section, std::string_view key,
                        bool def) const {
    const auto* e = find_entry(section, key);
    if (e == nullptr) return def;

    bool v = def;
    return parse_bool_text(e->value, v) ? v : def;
}

int64_t Settings::get_int(std::string_view section, std::string_view key,
                          int64_t def) const {
    const auto* e = find_entry(section, key);
    if (e == nullptr) return def;

    int64_t v = def;
    return parse_i64_text(e->value, v) ? v : def;
}

uint64_t Settings::get_uint(std::string_view section, std::string_view key,
                            uint64_t def) const {
    const auto* e = find_entry(section, key);
    if (e == nullptr) return def;

    uint64_t v = def;
    return parse_u64_text(e->value, v) ? v : def;
}

float Settings::get_float(std::string_view section, std::string_view key,
                          float def) const {
    const auto* e = find_entry(section, key);
    if (e == nullptr) return def;

    float v = def;
    return parse_f32_text(e->value, v) ? v : def;
}

std::string Settings::get_string(std::string_view section, std::string_view key,
                                 std::string_view def) const {
    const auto* e = find_entry(section, key);
    /* No parse step: an empty stored value is a legitimate empty string, which
     * is exactly the "zero is not absent" case. */
    return (e == nullptr) ? std::string{def} : e->value;
}

std::string Settings::get_raw(std::string_view section, std::string_view key,
                              std::string_view def) const {
    const auto* e = find_entry(section, key);
    return (e == nullptr) ? std::string{def} : e->value;
}

void Settings::set_bool(std::string_view section, std::string_view key, bool value) {
    set_raw(section, key, value ? "1" : "0");
}

void Settings::set_int(std::string_view section, std::string_view key, int64_t value) {
    set_raw(section, key, to_string_dec_int(value));
}

void Settings::set_uint(std::string_view section, std::string_view key,
                        uint64_t value) {
    set_raw(section, key, to_string_dec_uint(value));
}

void Settings::set_float(std::string_view section, std::string_view key, float value) {
    set_raw(section, key, format_float(value));
}

void Settings::set_string(std::string_view section, std::string_view key,
                          std::string_view value) {
    set_raw(section, key, strip_newlines(value));
}

std::vector<std::string> Settings::sections() const {
    std::vector<std::string> names;
    names.reserve(sections_.size());
    for (const auto& s : sections_) names.push_back(s.name);
    return names;
}

std::vector<std::string> Settings::keys(std::string_view section) const {
    std::vector<std::string> names;
    const auto* s = find_section(section);
    if (s == nullptr) return names;

    names.reserve(s->entries.size());
    for (const auto& e : s->entries) names.push_back(e.key);
    return names;
}

std::string Settings::to_string() const {
    std::string out;

    /* The unnamed section goes first and without a header, so that a file
     * written by an older build with no sections still reads back. */
    const auto* global = find_section({});
    if (global != nullptr) {
        for (const auto& e : global->entries)
            out += e.key + "=" + e.value + line_ending;
    }

    for (const auto& s : sections_) {
        if (s.name.empty()) continue;

        if (!out.empty()) out += line_ending;
        out += "[" + s.name + "]" + line_ending;
        for (const auto& e : s.entries)
            out += e.key + "=" + e.value + line_ending;
    }

    return out;
}

void Settings::from_string(std::string_view text) {
    sections_.clear();

    std::string current_section;

    for (const auto raw_line : split_lines(text)) {
        const std::string line = ::trim(raw_line);
        if (line.empty()) continue;
        if (line.front() == ';' || line.front() == '#') continue;

        if (line.front() == '[') {
            /* A header must close. If it does not, the line is junk — drop it
             * and carry on in the section we were already in. */
            if (line.back() != ']') continue;
            current_section = ::trim(std::string_view{line}.substr(1, line.size() - 2));
            /* Materialise the section even when empty, so keys() and
             * sections() report a header the user wrote by hand. */
            section_for(current_section);
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;  /* not a key=value line */

        const std::string key = ::trim(std::string_view{line}.substr(0, eq));
        if (key.empty()) continue;

        const std::string value = ::trim(std::string_view{line}.substr(eq + 1));
        set_raw(current_section, key, value);
    }

    dirty_ = false;
}

bool Settings::load() {
    sections_.clear();
    dirty_ = false;

    std::string text;
    if (!read_file(path_, text)) return false;

    from_string(text);
    return true;
}

bool Settings::save() const {
    if (!write_file(path_, to_string())) return false;

    dirty_ = false;
    return true;
}

Settings& settings() {
    static Settings* instance = [] {
        auto* s = new Settings{};
        s->load();  /* absent on a first run; an empty store is correct then */
        return s;
    }();
    return *instance;
}

/* pmem ----------------------------------------------------------------- */

namespace pmem {

namespace {
constexpr const char* section = "pmem";
}

uint64_t target_frequency() {
    return settings().get_uint(section, "target_frequency", target_frequency_reset_value);
}
void set_target_frequency(uint64_t v) {
    settings().set_uint(section, "target_frequency", v);
}

uint8_t headphone_volume() {
    const auto v = settings().get_uint(section, "headphone_volume", 40);
    return static_cast<uint8_t>(std::min<uint64_t>(v, 99));
}
void set_headphone_volume(uint8_t v) {
    settings().set_uint(section, "headphone_volume", std::min<uint8_t>(v, 99));
}

int32_t correction_ppb() {
    const auto v = settings().get_int(section, "correction_ppb", ppb_reset_value);
    return static_cast<int32_t>(std::clamp<int64_t>(v, INT32_MIN, INT32_MAX));
}
void set_correction_ppb(int32_t v) {
    settings().set_int(section, "correction_ppb", v);
}

bool load_app_settings() {
    return settings().get_bool(section, "load_app_settings", true);
}
void set_load_app_settings(bool v) {
    settings().set_bool(section, "load_app_settings", v);
}

bool save_app_settings() {
    return settings().get_bool(section, "save_app_settings", true);
}
void set_save_app_settings(bool v) {
    settings().set_bool(section, "save_app_settings", v);
}

bool config_splash() {
    return settings().get_bool(section, "config_splash", true);
}
void set_config_splash(bool v) {
    settings().set_bool(section, "config_splash", v);
}

bool show_gui_return_icon() {
    return settings().get_bool(section, "show_gui_return_icon", false);
}
void set_gui_return_icon(bool v) {
    settings().set_bool(section, "show_gui_return_icon", v);
}

bool hide_clock() {
    return settings().get_bool(section, "hide_clock", false);
}
void set_clock_hidden(bool v) {
    settings().set_bool(section, "hide_clock", v);
}

bool clock_with_date() {
    return settings().get_bool(section, "clock_with_date", false);
}
void set_clock_with_date(bool v) {
    settings().set_bool(section, "clock_with_date", v);
}

bool beep_on_packets() {
    return settings().get_bool(section, "beep_on_packets", false);
}
void set_beep_on_packets(bool v) {
    settings().set_bool(section, "beep_on_packets", v);
}

bool config_converter() {
    return settings().get_bool(section, "config_converter", false);
}
void set_config_converter(bool v) {
    settings().set_bool(section, "config_converter", v);
}

bool config_updown_converter() {
    return settings().get_bool(section, "config_updown_converter", false);
}
void set_config_updown_converter(bool v) {
    settings().set_bool(section, "config_updown_converter", v);
}

int64_t config_converter_freq() {
    return settings().get_int(section, "config_converter_freq", 0);
}
void set_config_converter_freq(int64_t v) {
    settings().set_int(section, "config_converter_freq", v);
}

uint32_t config_freq_rx_correction() {
    const auto v = settings().get_uint(section, "freq_rx_correction", 0);
    return static_cast<uint32_t>(std::min<uint64_t>(v, UINT32_MAX));
}
void set_config_freq_rx_correction(uint32_t v) {
    settings().set_uint(section, "freq_rx_correction", v);
}

uint32_t config_freq_tx_correction() {
    const auto v = settings().get_uint(section, "freq_tx_correction", 0);
    return static_cast<uint32_t>(std::min<uint64_t>(v, UINT32_MAX));
}
void set_config_freq_tx_correction(uint32_t v) {
    settings().set_uint(section, "freq_tx_correction", v);
}

bool config_freq_rx_correction_updown() {
    return settings().get_bool(section, "freq_rx_correction_updown", false);
}
void set_freq_rx_correction_updown(bool v) {
    settings().set_bool(section, "freq_rx_correction_updown", v);
}

bool config_freq_tx_correction_updown() {
    return settings().get_bool(section, "freq_tx_correction_updown", false);
}
void set_freq_tx_correction_updown(bool v) {
    settings().set_bool(section, "freq_tx_correction_updown", v);
}

uint8_t ui_theme_id() {
    const auto v = settings().get_uint(section, "ui_theme_id", 0);
    return static_cast<uint8_t>(std::min<uint64_t>(v, 255));
}
void set_ui_theme_id(uint8_t v) {
    settings().set_uint(section, "ui_theme_id", v);
}

uint16_t menu_color() {
    /* ui::Color::grey() is RGB565 (128,128,128) = 0x8410 — the firmware's
     * default menu colour. */
    const auto v = settings().get_uint(section, "menu_color", 0x8410);
    return static_cast<uint16_t>(std::min<uint64_t>(v, 0xFFFF));
}
void set_menu_color(uint16_t v) {
    settings().set_uint(section, "menu_color", v);
}

void defaults() {
    settings().erase_section(section);
}

bool save() {
    return settings().save();
}

}  // namespace pmem

/* BoundSetting --------------------------------------------------------- */

BoundSetting::BoundSetting(std::string name, bool* target)
    : name_{std::move(name)}, target_{target}, type_{Type::Bool} {}
BoundSetting::BoundSetting(std::string name, uint8_t* target)
    : name_{std::move(name)}, target_{target}, type_{Type::U8} {}
BoundSetting::BoundSetting(std::string name, int32_t* target)
    : name_{std::move(name)}, target_{target}, type_{Type::I32} {}
BoundSetting::BoundSetting(std::string name, uint32_t* target)
    : name_{std::move(name)}, target_{target}, type_{Type::U32} {}
BoundSetting::BoundSetting(std::string name, int64_t* target)
    : name_{std::move(name)}, target_{target}, type_{Type::I64} {}
BoundSetting::BoundSetting(std::string name, uint64_t* target)
    : name_{std::move(name)}, target_{target}, type_{Type::U64} {}
BoundSetting::BoundSetting(std::string name, float* target)
    : name_{std::move(name)}, target_{target}, type_{Type::Float} {}
BoundSetting::BoundSetting(std::string name, std::string* target)
    : name_{std::move(name)}, target_{target}, type_{Type::String} {}

void BoundSetting::parse(std::string_view value) const {
    switch (type_) {
        case Type::Bool: {
            bool v = false;
            if (parse_bool_text(value, v)) as<bool>() = v;
            break;
        }
        case Type::U8: {
            uint64_t v = 0;
            if (parse_u64_text(value, v) && v <= 0xFF)
                as<uint8_t>() = static_cast<uint8_t>(v);
            break;
        }
        case Type::I32: {
            int64_t v = 0;
            if (parse_i64_text(value, v) && v >= INT32_MIN && v <= INT32_MAX)
                as<int32_t>() = static_cast<int32_t>(v);
            break;
        }
        case Type::U32: {
            uint64_t v = 0;
            if (parse_u64_text(value, v) && v <= UINT32_MAX)
                as<uint32_t>() = static_cast<uint32_t>(v);
            break;
        }
        case Type::I64: {
            int64_t v = 0;
            if (parse_i64_text(value, v)) as<int64_t>() = v;
            break;
        }
        case Type::U64: {
            uint64_t v = 0;
            if (parse_u64_text(value, v)) as<uint64_t>() = v;
            break;
        }
        case Type::Float: {
            float v = 0.0f;
            if (parse_f32_text(value, v)) as<float>() = v;
            break;
        }
        case Type::String:
            as<std::string>() = ::trim(value);
            break;
    }
}

std::string BoundSetting::to_string() const {
    switch (type_) {
        case Type::Bool:
            return as<bool>() ? "1" : "0";
        case Type::U8:
            return to_string_dec_uint(as<uint8_t>());
        case Type::I32:
            return to_string_dec_int(as<int32_t>());
        case Type::U32:
            return to_string_dec_uint(as<uint32_t>());
        case Type::I64:
            return to_string_dec_int(as<int64_t>());
        case Type::U64:
            return to_string_dec_uint(as<uint64_t>());
        case Type::Float:
            return format_float(as<float>());
        case Type::String:
            return strip_newlines(as<std::string>());
    }
    return {};
}

/* Per-app store -------------------------------------------------------- */

namespace {

std::string app_settings_path(const std::string& store_name) {
    return (std::filesystem::path{settings_directory()} / (store_name + ".ini")).string();
}

}  // namespace

bool load_settings(const std::string& store_name, SettingBindings& bindings) {
    std::string text;
    if (!read_file(app_settings_path(store_name), text)) return false;

    for (const auto raw_line : split_lines(text)) {
        const std::string line = ::trim(raw_line);
        if (line.empty()) continue;
        if (line.front() == ';' || line.front() == '#' || line.front() == '[') continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = ::trim(std::string_view{line}.substr(0, eq));
        const auto value = std::string_view{line}.substr(eq + 1);

        auto it = std::find_if(bindings.begin(), bindings.end(),
                               [&key](const BoundSetting& b) { return b.name() == key; });
        if (it != bindings.end()) it->parse(value);
    }

    return true;
}

bool save_settings(const std::string& store_name, const SettingBindings& bindings) {
    std::string text;
    for (const auto& b : bindings)
        text += b.name() + "=" + b.to_string() + line_ending;

    return write_file(app_settings_path(store_name), text);
}

SettingsStore::SettingsStore(std::string store_name, SettingBindings bindings)
    : store_name_{std::move(store_name)}, bindings_{std::move(bindings)} {
    reload();
}

SettingsStore::~SettingsStore() {
    /* A destructor that throws during stack unwinding would take the app with
     * it, and a settings file is never worth that. */
    try {
        save();
    } catch (...) {
    }
}

void SettingsStore::reload() {
    loaded_ = load_settings(store_name_, bindings_);
}

bool SettingsStore::save() const {
    return save_settings(store_name_, bindings_);
}

/* app_settings --------------------------------------------------------- */

namespace app_settings {

void bind_common(AppSettings& s, SettingBindings& bindings) {
    if (has_mode(s.mode, Mode::Tx)) {
        bindings.emplace_back("tx_frequency", &s.tx_frequency);
        bindings.emplace_back("tx_gain", &s.tx_gain);
    }

    if (has_mode(s.mode, Mode::Rx)) {
        bindings.emplace_back("rx_frequency", &s.rx_frequency);
        bindings.emplace_back("rx_gain", &s.rx_gain);
        bindings.emplace_back("rx_agc", &s.rx_agc);
        bindings.emplace_back("modulation", &s.modulation);
        bindings.emplace_back("am_config_index", &s.am_config_index);
        bindings.emplace_back("nfm_config_index", &s.nfm_config_index);
        bindings.emplace_back("wfm_config_index", &s.wfm_config_index);
        bindings.emplace_back("squelch", &s.squelch);
    }

    bindings.emplace_back("baseband_bandwidth", &s.baseband_bandwidth);
    bindings.emplace_back("sampling_rate", &s.sampling_rate);
    bindings.emplace_back("step", &s.step);
    bindings.emplace_back("volume", &s.volume);
}

SettingsManager::SettingsManager(std::string app_name, Mode mode,
                                 SettingBindings additional_settings)
    : SettingsManager{std::move(app_name), mode, Hook{}, Hook{},
                      std::move(additional_settings)} {}

SettingsManager::SettingsManager(std::string app_name, Mode mode, Hook capture,
                                 Hook apply, SettingBindings additional_settings)
    : app_name_{std::move(app_name)},
      capture_{std::move(capture)},
      apply_{std::move(apply)} {
    settings_.mode = mode;
    build_bindings(std::move(additional_settings));

    const bool radio_settings_enabled = pmem::load_app_settings();

    /* Seed from the live radio first, so that a key missing from the file
     * leaves the radio's own default in place rather than a struct default. */
    if (capture_ && radio_settings_enabled) capture_(settings_);

    loaded_ = load_settings(app_name_, bindings_);

    if (loaded_ && apply_ && radio_settings_enabled) apply_(settings_);
}

void SettingsManager::build_bindings(SettingBindings additional_settings) {
    bindings_ = std::move(additional_settings);
    bindings_.reserve(bindings_.size() + 14);
    bind_common(settings_, bindings_);
}

bool SettingsManager::save() {
    if (capture_ && pmem::save_app_settings()) capture_(settings_);
    return save_settings(app_name_, bindings_);
}

SettingsManager::~SettingsManager() {
    try {
        save();
    } catch (...) {
    }
}

}  // namespace app_settings

}  // namespace core
