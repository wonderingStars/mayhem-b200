/*
 * mayhem-b200 — Hard Reset.
 *
 * Port of firmware/application/external/hard_reset/. On a PortaPack that app
 * wipes three things a factory-fresh unit would not have: the persistent-memory
 * struct in the LPC43xx's backup RAM ("P.mem"), the *.ini files under the SD
 * card's SETTINGS folder, and any corrupt / version-mismatched external apps
 * (.ppma/.ppmp) on the SD card. When P.mem is wiped it then forces a touch
 * recalibration, because the calibration lived in P.mem.
 *
 * On a B200 host most of that hardware does not exist, so the reset is mapped to
 * the parts that do:
 *
 *   "Reset all settings"  — the host has no backup RAM; every persistent value
 *                           (including the [pmem] section) lives in the single
 *                           INI store behind core::settings(). Clearing it and
 *                           saving makes every accessor fall back to its
 *                           built-in default, which is exactly what a hard reset
 *                           means. Genuinely functional.
 *
 *   "Delete app .ini"     — per-app radio state lives in <data>/SETTINGS/*.ini,
 *                           the direct analog of upstream's settings folder.
 *                           Deleting them is genuinely functional.
 *
 * Deliberately absent, and shown on screen as N/A, because faking them would be
 * a lie: the SD-card bad-app scan (apps are compiled in on a host, there is no
 * .ppma/.ppmp to be corrupt), the backup-RAM P.mem wipe as a distinct step, and
 * the touch recalibration (the host OS owns the pointer).
 *
 * Copyright (C) 2026 Pezsma (original design)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_HARD_RESET_H__
#define __MB200_UI_HARD_RESET_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include "settings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace app {

/* ------------------------------------------------------------------------
 * Pure reset logic — no widgets, so the tests can exercise it directly.
 * --------------------------------------------------------------------- */

/* Host equivalent of the PortaPack "P.mem reset". There is no backup RAM on a
 * host: the persistent settings live in an INI store, so a reset is a clear() of
 * every section followed by a save(). Every core::settings() / core::pmem read
 * then returns its built-in default, because get_*() only returns a stored value
 * when has() is true. Returns save()'s result (false only if the file could not
 * be written). */
inline bool hard_reset_clear_settings(core::Settings& settings) {
    settings.clear();
    return settings.save();
}

/* Counts the *.ini files directly in dir (non-recursive, case-insensitive on the
 * extension), mirroring upstream count_ini_files_in_directory(). A missing
 * directory counts as zero rather than throwing — a first run has no SETTINGS
 * folder yet. */
inline uint16_t hard_reset_count_ini_files(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec) return 0;

    uint16_t count = 0;
    for (std::filesystem::directory_iterator it{dir, ec}, end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!std::filesystem::is_regular_file(it->path(), fec) || fec) continue;

        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".ini") count++;
    }
    return count;
}

/* Deletes every *.ini directly in dir and returns how many were removed. The
 * paths are collected before any deletion so the directory iterator cannot be
 * invalidated mid-walk — the reason upstream had to break-and-restart its loop.
 * Non-recursive, so it never touches an app's own sub-folders. */
inline uint16_t hard_reset_delete_ini_files(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec) return 0;

    std::vector<std::filesystem::path> targets;
    for (std::filesystem::directory_iterator it{dir, ec}, end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!std::filesystem::is_regular_file(it->path(), fec) || fec) continue;

        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".ini") targets.push_back(it->path());
    }

    uint16_t removed = 0;
    for (const auto& t : targets) {
        std::error_code dec;
        if (std::filesystem::remove(t, dec) && !dec) removed++;
    }
    return removed;
}

/* ------------------------------------------------------------------------
 * View
 * --------------------------------------------------------------------- */

class HardResetView : public ui::View {
   public:
    HardResetView();

    std::string title() const override { return "Hard Reset"; }
    void on_show() override;

   private:
    void refresh_counts();
    void do_reset();

    ui::Labels labels_{
        {{1 * 8, 0 * 16}, "Reset stored settings to", ui::Color::light_grey()},
        {{1 * 8, 1 * 16}, "their defaults. On a B200", ui::Color::light_grey()},
        {{1 * 8, 2 * 16}, "these live in an INI file,", ui::Color::light_grey()},
        {{1 * 8, 3 * 16}, "not PortaPack backup RAM.", ui::Color::light_grey()},
    };

    /* Clears core::settings() (the whole INI store, [pmem] included). */
    ui::Checkbox check_reset_settings_{
        {1 * 8, 5 * 16}, 22, "Reset all settings"};

    /* Deletes <data>/SETTINGS/*.ini — per-app radio state. */
    ui::Checkbox check_delete_inis_{
        {1 * 8, 7 * 16}, 22, "Delete app .ini files"};

    ui::Text text_ini_count_{
        {3 * 8, 8 * 16 + 4, 24 * 8, 1 * 16}, ""};

    ui::Labels na_labels_{
        {{1 * 8, 11 * 16}, "N/A on B200 (PortaPack", ui::Color::light_grey()},
        {{1 * 8, 12 * 16}, "hardware a host lacks):", ui::Color::light_grey()},
        {{1 * 8, 13 * 16}, "SD-card bad-app scan,", ui::Color::light_grey()},
        {{1 * 8, 14 * 16}, "P.mem wipe, touch cal.", ui::Color::light_grey()},
    };

    ui::Text text_status_{
        {1 * 8, 15 * 16 + 8, 28 * 8, 1 * 16}, ""};

    ui::Button button_reset_{
        {8, 268, 104, 32}, "Reset"};
    ui::Button button_cancel_{
        {128, 268, 104, 32}, "Cancel"};

    uint16_t ini_count_{0};
};

}  // namespace app

#endif /*__MB200_UI_HARD_RESET_H__*/
