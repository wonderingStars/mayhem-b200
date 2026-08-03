/*
 * mayhem-b200 — Wipe SD card (hardware-limited N/A screen, refuses to act).
 *
 * Upstream (application/external/sd_wipe) overwrote the FAT area of the
 * PortaPack's microSD card with LFSR noise via disk_write() to the SD block
 * device — a destructive format targeting a card that only exists on PortaPack
 * hardware. A B200 host has no such card. The one thing this app must NOT do is
 * treat the host PC's own filesystem as a substitute and wipe it. So this view
 * is a hard refusal: it explains what the original did, why it does not apply,
 * and its "wipe" action is a guaranteed no-op that never opens, writes, or
 * deletes anything.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SD_WIPE_H__
#define __MB200_UI_SD_WIPE_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <string>

namespace app {

/* Outcome of asking to wipe. On a B200 host the only "SD card" that could be
 * meant is the host filesystem, and wiping that would be destroying the user's
 * own machine — so this always refuses. `performed` is always false and
 * `bytes_wiped` is always zero: the refusal is the behaviour, and tests assert
 * the no-op. Kept side-effect-free so it can be checked without a UI. */
struct WipeResult {
    bool performed;        /* always false — nothing is ever wiped */
    uint64_t bytes_wiped;  /* always 0 */
    std::string message;   /* one-line human explanation of the refusal */
};

class WipeSDView : public ui::View {
   public:
    WipeSDView();

    std::string title() const override { return "Wipe SD Card"; }

    void on_show() override;

    /* Pure decision, no side effects and no filesystem access whatsoever. A
     * B200 has no SD card, and this deliberately does not fall back to the host
     * disk — it refuses. Static so tests can assert the no-op without a UI. */
    static WipeResult attempt_wipe();

   private:
    void render();

    ui::Console console_{{0, 4, 240, 244}};
    ui::Button button_wipe_{{16, 260, 96, 28}, "Wipe"};
    ui::Button button_back_{{128, 260, 96, 28}, "Back"};
};

}  // namespace app

#endif /*__MB200_UI_SD_WIPE_H__*/
