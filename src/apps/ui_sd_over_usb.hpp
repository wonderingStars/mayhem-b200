/*
 * mayhem-b200 — SD over USB (hardware-limited N/A screen).
 *
 * Upstream (application/external/sdusb) rebooted the PortaPack into a USB
 * mass-storage mode so the host PC could read the device's microSD card as a
 * removable drive. A B200 is itself a USB peripheral with no SD card and no
 * application processor to expose one — there is nothing to bridge. The host's
 * own filesystem is already directly accessible, so this view explains the
 * situation rather than pretending to start anything.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SD_OVER_USB_H__
#define __MB200_UI_SD_OVER_USB_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <string>

namespace app {

/* Result of asking to start SD-over-USB. On a PortaPack the "Run" button
 * rebooted into USB mass-storage mode; on a B200 host there is no SD card to
 * expose, so the answer is always "unavailable" and nothing is started. Kept
 * as a plain value with no side effects so tests can assert the no-op without
 * constructing the view. */
struct SdUsbStartResult {
    bool started;         /* always false on a host build */
    std::string message;  /* one-line human explanation */
};

class SdOverUsbView : public ui::View {
   public:
    SdOverUsbView();

    std::string title() const override { return "SD over USB"; }

    void on_show() override;

    /* Pure decision, no side effects. There is no SD card on a B200 host, so
     * this reports unavailable and starts nothing. Static so tests can call it
     * without a UI. */
    static SdUsbStartResult attempt_start();

   private:
    void render();

    ui::Console console_{{0, 4, 240, 244}};
    ui::Button button_run_{{16, 260, 96, 28}, "Run"};
    ui::Button button_back_{{128, 260, 96, 28}, "Back"};
};

}  // namespace app

#endif /*__MB200_UI_SD_OVER_USB_H__*/
