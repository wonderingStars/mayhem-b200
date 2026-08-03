/*
 * mayhem-b200 — MCU Temperature (host N/A screen).
 *
 * Upstream (application/external/mcu_temperature) plotted the die temperature of
 * the PortaPack's LPC43xx microcontroller over time, sampling it once a second
 * from the chip's on-die temperature sensor and drawing a scrolling bar graph.
 *
 * A USRP B200 is a USB SDR front end driven by a host PC: there is no LPC43xx
 * and no application microcontroller whose die temperature could be read. The
 * host CPU's own thermal sensors are a different device entirely and are not what
 * this app measured, so there is nothing equivalent to show. This view says so
 * plainly rather than inventing a number.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (upstream)
 * Copyright (C) 2026 mayhem-b200 contributors (host N/A screen)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_MCU_TEMPERATURE_H__
#define __MB200_UI_MCU_TEMPERATURE_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <string>
#include <vector>

namespace app {

class McuTemperatureView : public ui::View {
   public:
    McuTemperatureView();

    std::string title() const override { return "MCU Temp"; }

    void on_show() override;

    /* Pure, testable facts about this app on a B200 host.
     *
     * There is no LPC43xx microcontroller on a B200, so a die temperature is
     * never available — the app can only report that, never a value. */
    static bool temperature_available() { return false; }

    /* The exact lines the screen shows. Deterministic (no hardware, no clock),
     * so a test can assert it reports "unavailable" and carries no reading. */
    static std::vector<std::string> status_report();

   private:
    ui::Console console_{{0, 4, 240, 272}};
    ui::Button button_done_{{72, 280, 96, 24}, "Back"};
};

}  // namespace app

#endif /*__MB200_UI_MCU_TEMPERATURE_H__*/
