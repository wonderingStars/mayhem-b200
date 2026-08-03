/*
 * mayhem-b200 — persistent-memory dump (host settings dump).
 *
 * Upstream (application/external/debug_pmem) dumped the LPC43xx's 256-byte
 * checksummed backup-RAM struct — target frequency, corrections, UI config bits,
 * plus the receiver/transmitter model state — to a text file for debugging.
 *
 * A host has no backup RAM; the firmware's persistent_memory is replaced here by
 * core::Settings (an INI file under the data directory, see core/settings.hpp).
 * So this stays genuinely useful: it dumps the process-wide settings store —
 * the [pmem] values and anything else in settings.ini — via
 * core::Settings::to_string(), reformatted for the screen. It shows host
 * settings, not LPC persistent memory.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (upstream)
 * Copyright (C) 2018 Furrtek (upstream)
 * Copyright (C) 2026 mayhem-b200 contributors (host settings dump)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_DEBUG_PMEM_H__
#define __MB200_UI_DEBUG_PMEM_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <string>

namespace core {
class Settings;
}

namespace app {

class DebugPMemView : public ui::View {
   public:
    DebugPMemView();

    std::string title() const override { return "Debug PMem"; }

    void on_show() override;

    /* Human-readable dump of a settings store. Pure and deterministic given `s`:
     * a named section becomes "[name]", each key becomes "  key: value", and the
     * unnamed section's keys print without a header. An empty store yields the
     * single line "(no settings stored)". Each line is newline-terminated.
     *
     * Kept free of the live core::settings() singleton so a test can feed it a
     * known store and assert the exact text. */
    static std::string format_settings_dump(const core::Settings& s);

   private:
    ui::Console console_{{0, 4, 240, 272}};
    ui::Button button_done_{{72, 280, 96, 24}, "Back"};
};

}  // namespace app

#endif /*__MB200_UI_DEBUG_PMEM_H__*/
