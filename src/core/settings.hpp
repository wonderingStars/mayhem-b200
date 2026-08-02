/*
 * mayhem-b200 — persistent settings.
 *
 * Replaces the firmware's `portapack::persistent_memory`, which is a 256-byte
 * checksummed struct in the LPC43xx's backup RAM. There is no backup RAM on a
 * host, and no reason to keep the bit-packing, so the same values live in a
 * plain INI file under core::data_directory().
 *
 * Two layers, mirroring the two the firmware has:
 *
 *   core::Settings   — a sectioned key/value store with typed accessors.
 *                      core::settings() is the process-wide instance backed by
 *                      <data>/settings.ini, and core::pmem holds the named
 *                      accessors that pmem used to provide.
 *
 *   core::app_settings::SettingsManager
 *                    — the firmware's application/app_settings.hpp pattern: an
 *                      RAII object an app view holds as a member, which reads
 *                      <data>/SETTINGS/<app>.ini on construction and writes it
 *                      back on destruction, so an app reopens on the frequency,
 *                      bandwidth and gain it was last used with.
 *
 * File format
 * -----------
 *   ; comment            (';' or '#', first non-blank character)
 *   key=value            (before any header: the unnamed section)
 *   [section]
 *   key=value
 *
 * Keys and section names are trimmed and compared case-sensitively. Values are
 * trimmed. A line without '=' is ignored and does not affect the lines around
 * it. Booleans are written as 1/0 and read as 1/0/true/false/yes/no/on/off
 * (case-insensitive). Floats are written with %.9g, which round-trips a float
 * exactly — a deviation from the firmware's fixed six decimals, chosen because
 * a settings file that silently loses precision is worse than an ugly one.
 *
 * An absent key is not the same as a stored zero: get_*() returns the caller's
 * default only when has() is false, so a deliberately stored 0, 0.0, false or
 * "" survives a save/load round trip.
 *
 * Not thread-safe. Everything here runs on the UI thread.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (pmem semantics)
 * Copyright (C) 2023 Kyle Reed (app_settings semantics)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_SETTINGS_H__
#define __MB200_SETTINGS_H__

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core {

/* <data>/SETTINGS — one .ini per app, as the firmware's SETTINGS directory. */
std::string settings_directory();

/* <data>/settings.ini — the global store behind core::settings(). */
std::string settings_file_path();

/* ------------------------------------------------------------------------
 * Settings
 * --------------------------------------------------------------------- */

class Settings {
   public:
    /* Backed by settings_file_path(). */
    Settings();
    explicit Settings(std::string file_path);

    const std::string& path() const { return path_; }
    void set_path(std::string p) { path_ = std::move(p); }

    /* Replaces the in-memory contents with the file's. Returns false if the
     * file could not be read; the store is left empty in that case, which is
     * the correct state for a first run. */
    bool load();

    /* Writes the file, creating the directory if needed. Sections are written
     * in the order they were first touched, keys in insertion order, so a
     * hand-edited file keeps its shape across a round trip. */
    bool save() const;

    /* True if a value has been stored, whatever that value is. This is how a
     * caller tells "absent" from "present and zero". */
    bool has(std::string_view section, std::string_view key) const;

    /* Removes one key. Returns true if it was there. */
    bool erase(std::string_view section, std::string_view key);

    /* Removes a whole section, including the unnamed one (section == ""). */
    bool erase_section(std::string_view section);

    void clear();

    /* True if anything has been set or erased since the last load()/save(). */
    bool dirty() const { return dirty_; }

    /* Typed reads. `def` is returned when the key is absent, and also when the
     * stored text cannot be parsed as the requested type. */
    bool get_bool(std::string_view section, std::string_view key, bool def) const;
    int64_t get_int(std::string_view section, std::string_view key, int64_t def) const;
    uint64_t get_uint(std::string_view section, std::string_view key, uint64_t def) const;
    float get_float(std::string_view section, std::string_view key, float def) const;
    std::string get_string(std::string_view section, std::string_view key,
                           std::string_view def) const;

    void set_bool(std::string_view section, std::string_view key, bool value);
    void set_int(std::string_view section, std::string_view key, int64_t value);
    void set_uint(std::string_view section, std::string_view key, uint64_t value);
    void set_float(std::string_view section, std::string_view key, float value);
    /* Newlines and carriage returns in `value` are replaced with spaces: they
     * cannot be represented in this format and would corrupt the file. */
    void set_string(std::string_view section, std::string_view key,
                    std::string_view value);

    /* Raw text of a stored value, or `def` if absent. Useful for migration. */
    std::string get_raw(std::string_view section, std::string_view key,
                        std::string_view def = {}) const;

    std::vector<std::string> sections() const;
    std::vector<std::string> keys(std::string_view section) const;

    /* Serialises to a string instead of a file — used by the tests and by
     * anything that wants to show the settings on screen. */
    std::string to_string() const;
    /* Parses INI text, replacing the current contents. Always succeeds:
     * unparseable lines are skipped, the rest are kept. */
    void from_string(std::string_view text);

   private:
    struct Entry {
        std::string key;
        std::string value;
    };
    struct Section {
        std::string name;
        std::vector<Entry> entries;
    };

    const Section* find_section(std::string_view name) const;
    Section& section_for(std::string_view name);
    const Entry* find_entry(std::string_view section, std::string_view key) const;
    void set_raw(std::string_view section, std::string_view key, std::string value);

    std::vector<Section> sections_{};
    std::string path_{};
    /* Mutable so that save() can stay const while clearing the flag. */
    mutable bool dirty_{false};
};

/* The process-wide store. Loaded from disk on first use. */
Settings& settings();

/* ------------------------------------------------------------------------
 * pmem — the named settings the firmware kept in persistent memory
 *
 * Only the values that still mean something on a B200 are here. Deliberately
 * absent, because faking them would be a lie:
 *
 *   backlight timeout, fake brightness, touchscreen calibration and threshold,
 *   encoder dial sensitivity/direction, CPLD config, SD-card speed, clkout,
 *   battery capacity/replaceable, TX amp disable, speaker disable
 *       — PortaPack hardware a B200 does not have.
 *   stealth mode, config login, config mode storage
 *       — behaviours of a handheld with a screen and buttons.
 *
 * Values live in the [pmem] section of core::settings(); nothing is written to
 * disk until core::pmem::save() or core::settings().save() is called.
 * --------------------------------------------------------------------- */

namespace pmem {

/* Firmware reset values, from common/portapack_persistent_memory.cpp. */
inline constexpr uint64_t target_frequency_reset_value = 100'000'000;
inline constexpr int32_t ppb_reset_value = 0;

uint64_t target_frequency();
void set_target_frequency(uint64_t v);

/* 0..99, the normalised volume the firmware's headphone_volume maps onto. */
uint8_t headphone_volume();
void set_headphone_volume(uint8_t v);

/* Reference-clock error in parts per billion. The firmware trims the Si5351;
 * here it is applied as a tuning offset by the radio layer. */
int32_t correction_ppb();
void set_correction_ppb(int32_t v);

bool load_app_settings();
void set_load_app_settings(bool v);
bool save_app_settings();
void set_save_app_settings(bool v);

bool config_splash();
void set_config_splash(bool v);

bool show_gui_return_icon();
void set_gui_return_icon(bool v);

bool hide_clock();
void set_clock_hidden(bool v);

bool clock_with_date();
void set_clock_with_date(bool v);

bool beep_on_packets();
void set_beep_on_packets(bool v);

/* Up/down converter in front of the radio. config_converter_freq() is the
 * converter's LO in Hz; config_updown_converter() picks up-conversion. */
bool config_converter();
void set_config_converter(bool v);
bool config_updown_converter();
void set_config_updown_converter(bool v);
int64_t config_converter_freq();
void set_config_converter_freq(int64_t v);

/* Fixed frequency trim, applied per direction. The *_updown flag selects
 * whether the correction is added (false) or subtracted (true). */
uint32_t config_freq_rx_correction();
void set_config_freq_rx_correction(uint32_t v);
uint32_t config_freq_tx_correction();
void set_config_freq_tx_correction(uint32_t v);
bool config_freq_rx_correction_updown();
void set_freq_rx_correction_updown(bool v);
bool config_freq_tx_correction_updown();
void set_freq_tx_correction_updown(bool v);

uint8_t ui_theme_id();
void set_ui_theme_id(uint8_t v);

/* RGB565, as ui::Color stores it. */
uint16_t menu_color();
void set_menu_color(uint16_t v);

/* Restores every value above to its firmware default. */
void defaults();

/* Convenience: core::settings().save(). */
bool save();

}  // namespace pmem

/* ------------------------------------------------------------------------
 * BoundSetting — the firmware's app_settings binding
 * --------------------------------------------------------------------- */

/* A named setting bound to a live variable. Same idea as the firmware's
 * BoundSetting, except the name is owned rather than a string_view: on a host
 * the allocation is free and a dangling name is not. */
class BoundSetting {
   public:
    BoundSetting(std::string name, bool* target);
    BoundSetting(std::string name, uint8_t* target);
    BoundSetting(std::string name, int32_t* target);
    BoundSetting(std::string name, uint32_t* target);
    BoundSetting(std::string name, int64_t* target);
    BoundSetting(std::string name, uint64_t* target);
    BoundSetting(std::string name, float* target);
    BoundSetting(std::string name, std::string* target);

    const std::string& name() const { return name_; }

    /* Writes the parsed value into the bound variable. Leaves it untouched if
     * the text does not parse — a corrupt line must not clobber a good
     * default. */
    void parse(std::string_view value) const;

    /* The value of the bound variable, formatted for the file. */
    std::string to_string() const;

   private:
    enum class Type : uint8_t { Bool, U8, I32, U32, I64, U64, Float, String };

    template <typename T>
    T& as() const {
        return *static_cast<T*>(target_);
    }

    std::string name_;
    void* target_;
    Type type_;
};

using SettingBindings = std::vector<BoundSetting>;

/* Reads <data>/SETTINGS/<store_name>.ini into the bound variables. Returns
 * false if the file does not exist or cannot be read. */
bool load_settings(const std::string& store_name, SettingBindings& bindings);

/* Writes the bound variables to <data>/SETTINGS/<store_name>.ini. */
bool save_settings(const std::string& store_name, const SettingBindings& bindings);

/* Loads on construction, saves on destruction. */
class SettingsStore {
   public:
    SettingsStore(std::string store_name, SettingBindings bindings);
    ~SettingsStore();

    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;

    void reload();
    bool save() const;

    bool loaded() const { return loaded_; }

   private:
    std::string store_name_;
    SettingBindings bindings_;
    bool loaded_{false};
};

/* ------------------------------------------------------------------------
 * app_settings — per-app radio state
 * --------------------------------------------------------------------- */

namespace app_settings {

enum class Mode : uint8_t {
    NoRf = 0x00,
    Rx = 0x01,
    Tx = 0x02,
    RxTx = 0x03,
};

constexpr bool has_mode(Mode value, Mode flag) {
    return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flag)) ==
           static_cast<uint8_t>(flag);
}

/* The common radio state every app persists. The field set follows the
 * firmware's AppSettings, with the HackRF's LNA/VGA/AMP triple collapsed into
 * the single continuous gain a B200 actually has (see doc/PORTING.md). */
struct AppSettings {
    Mode mode{Mode::Rx};

    uint64_t rx_frequency{pmem::target_frequency_reset_value};
    uint64_t tx_frequency{pmem::target_frequency_reset_value};

    /* Captured bandwidth and the analog filter behind it, both in Hz. */
    uint64_t sampling_rate{2'400'000};
    uint64_t baseband_bandwidth{2'500'000};

    /* Tuning step in Hz. */
    uint64_t step{25'000};

    /* dB. The B200's RX range is roughly 0..76 and TX 0..89.8, but the real
     * limits come from radio->caps() — these are only starting points. */
    float rx_gain{40.0f};
    float tx_gain{40.0f};
    bool rx_agc{false};

    /* radio::ReceiverModel::Mode and its sub-configurations, as integers so
     * that core does not have to include the radio layer. */
    uint8_t modulation{1};  /* NarrowbandFMAudio */
    uint8_t am_config_index{0};
    uint8_t nfm_config_index{1};
    uint8_t wfm_config_index{0};

    uint8_t squelch{0};
    uint8_t volume{40};
};

/* Adds the standard bindings for `settings` to `bindings`, choosing the RX
 * and/or TX groups from settings.mode. Exposed so an app that manages its own
 * store can reuse the same key names. */
void bind_common(AppSettings& settings, SettingBindings& bindings);

/* RAII wrapper. Hold one as a member of the app view, declared before any
 * control that reads the radio, exactly as the firmware does.
 *
 * The firmware's SettingsManager talks to receiver_model/transmitter_model
 * directly. Here the two hooks do that instead, so that core/ does not depend
 * on radio/:
 *
 *   capture — copies the live radio state into AppSettings. Called once before
 *             loading (so an absent key keeps the radio's own default) and
 *             again before saving.
 *   apply   — pushes AppSettings onto the radio. Called only if the file was
 *             read successfully.
 *
 * Either may be empty, in which case AppSettings is just a persisted struct.
 * Loading and saving also honour core::pmem::load_app_settings() and
 * save_app_settings(): additional bindings are always loaded, but the radio
 * hooks are skipped when the user has turned the feature off. */
class SettingsManager {
   public:
    using Hook = std::function<void(AppSettings&)>;

    SettingsManager(std::string app_name, Mode mode,
                    SettingBindings additional_settings = {});
    SettingsManager(std::string app_name, Mode mode, Hook capture, Hook apply,
                    SettingBindings additional_settings = {});
    ~SettingsManager();

    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;
    SettingsManager(SettingsManager&&) = delete;
    SettingsManager& operator=(SettingsManager&&) = delete;

    /* True if the .ini existed and was read. */
    bool loaded() const { return loaded_; }

    Mode mode() const { return settings_.mode; }
    const std::string& app_name() const { return app_name_; }

    AppSettings& raw() { return settings_; }
    const AppSettings& raw() const { return settings_; }

    /* Captures the radio state and writes the file. The destructor does this;
     * call it directly to checkpoint without tearing the app down. */
    bool save();

   private:
    void build_bindings(SettingBindings additional_settings);

    std::string app_name_;
    AppSettings settings_{};
    SettingBindings bindings_{};
    Hook capture_{};
    Hook apply_{};
    bool loaded_{false};
};

}  // namespace app_settings

}  // namespace core

#endif /*__MB200_SETTINGS_H__*/
