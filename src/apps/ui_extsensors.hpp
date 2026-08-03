/*
 * mayhem-b200 — external / on-board sensor viewer (host N/A screen).
 *
 * Upstream (application/external/extsensors) read sensor modules attached to a
 * PortaPack's I2C bus — GPS position, orientation/tilt, environment
 * temperature/humidity/pressure and ambient light — auto-scanning the bus every
 * few seconds and printing whatever devices answered. A USRP B200 is a USB SDR
 * front end with no I2C sensor bus and no on-board sensors, so there is nothing
 * to read here. This view says so plainly rather than inventing a reading.
 *
 * Copyright (C) 2023 Bernd Herzog (upstream extsensors)
 * Copyright (C) 2026 mayhem-b200 contributors (host N/A screen)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_EXTSENSORS_H__
#define __MB200_UI_EXTSENSORS_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <string>
#include <vector>

namespace app {

class ExtSensorsView : public ui::View {
   public:
    ExtSensorsView();

    std::string title() const override { return "Ext Sensors"; }

    void on_show() override;

    /* Pure, testable facts about this app on a B200 host.
     *
     * A B200 has no I2C sensor bus and no on-board sensors, so sensor data is
     * never available — the app can only report that, never a value. */
    static bool sensors_available() { return false; }

    /* The exact lines the screen shows. Deterministic (no hardware, no clock),
     * so a test can assert it reports "unavailable" and carries no reading. */
    static std::vector<std::string> status_report();

   private:
    ui::Console console_{{0, 4, 240, 272}};
    ui::Button button_done_{{72, 280, 96, 24}, "Back"};
};

}  // namespace app

#endif /*__MB200_UI_EXTSENSORS_H__*/
