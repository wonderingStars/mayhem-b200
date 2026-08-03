/*
 * mayhem-b200 — DOOM (Games), app shell only.
 *
 * Host port SHELL for PortaPack Mayhem's external/doom app (copyright RocketGod).
 *
 * Honest scope: the upstream Mayhem "Doom" is not id Software's DOOM and does
 * not use a WAD — it is a ~1200-line self-contained raycaster (a Wolfenstein/
 * Flipper-Zero-style clone with a hard-coded level, entities and a rendered
 * shotgun) driven from a 60 Hz frame tick. Porting its full render/game loop is
 * out of scope for this wave, so this view does NOT play anything. It states
 * that plainly, and additionally implements the piece a *real* id-DOOM port
 * (doomgeneric) would need first: locating an IWAD file under the app data
 * directory and reporting whether it is present and looks valid. That
 * WAD-presence logic (doom_wad::) is pure and unit tested; no engine is wired
 * up to consume the WAD.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_DOOM_H__
#define __MB200_UI_DOOM_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include "file_path.hpp"
#include "fs_utils.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace app {

/* ---- IWAD discovery (tested in test_doom.cpp) ----------------------------- *
 *
 * Header-only and hardware-free, like the morse decoder, so the tests can link
 * and exercise it without pulling in the whole UI. No engine consumes the WAD;
 * this only reports what a future doomgeneric port would need to find. */
namespace doom_wad {

/* Candidate IWAD filenames a doomgeneric port would look for, highest priority
 * first. Matching against the filesystem is case-insensitive on Windows. */
inline const std::vector<std::string>& iwad_names() {
    /* Retail IWADs, then the free Freedoom set, then the mission packs. */
    static const std::vector<std::string> names = {
        "doom.wad", "doom1.wad", "doom2.wad",
        "freedoom1.wad", "freedoom2.wad",
        "plutonia.wad", "tnt.wad"};
    return names;
}

/* Directory an IWAD is expected in: core::data_directory() + "/DOOM". */
inline std::string wad_directory() {
    return core::path_join(core::data_directory(), "DOOM");
}

struct WadStatus {
    bool present{false};
    /* When present: the full path of the WAD that was found. When absent: the
     * full path of the primary expected file, so the UI can tell the user
     * exactly where to drop one. */
    std::string path{};
    std::uintmax_t size{0};
    /* First four bytes are the ASCII magic "IWAD" or "PWAD". */
    bool looks_valid{false};
};

/* Scans `directory` for the first present IWAD candidate. Pure with respect to
 * its argument — the view passes wad_directory(), tests pass a temp dir. */
inline WadStatus find_wad(const std::string& directory) {
    WadStatus status{};

    for (const auto& name : iwad_names()) {
        const std::string candidate = core::path_join(directory, name);
        if (!core::is_regular_file(candidate)) continue;

        status.present = true;
        status.path = candidate;
        status.size = core::file_size(candidate);

        /* Peek the 4-byte WAD magic without reading the whole (multi-MB) file. */
        std::ifstream in(candidate, std::ios::binary);
        char magic[4] = {0, 0, 0, 0};
        if (in && in.read(magic, sizeof(magic))) {
            const std::string tag(magic, sizeof(magic));
            status.looks_valid = (tag == "IWAD" || tag == "PWAD");
        }
        return status;
    }

    /* None found: report where the primary IWAD should go. */
    status.present = false;
    status.path = core::path_join(directory, iwad_names().front());
    return status;
}

}  // namespace doom_wad

class DoomView : public ui::View {
   public:
    DoomView();

    std::string title() const override { return "Doom"; }

    void focus() override;
    void on_show() override;

   private:
    void refresh();

    ui::Console console_{{0, 4, 240, 244}};
    ui::Button button_recheck_{{8, 272, 100, 24}, "Re-check"};
    ui::Button button_back_{{132, 272, 100, 24}, "Back"};
};

}  // namespace app

#endif /*__MB200_UI_DOOM_H__*/
