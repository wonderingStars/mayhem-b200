/*
 * mayhem-b200 — radio setup and device information.
 *
 * Exposes the settings that are specific to a USRP and have no PortaPack
 * equivalent: capture bandwidth, antenna port, LO offset and the AD936x's own
 * corrections. Also reports the device's real capability ranges.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_DEVICE_APP_H__
#define __MB200_DEVICE_APP_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <string>

namespace app {

class DeviceView : public ui::View {
   public:
    DeviceView();

    std::string title() const override { return "Radio"; }

    void on_show() override;
    void on_frame_sync() override;

   private:
    void refresh_info();
    void refresh_antennas();

    ui::Labels labels_{
        {{0, 4}, "Rate", ui::Color::light_grey()},
        {{0, 26}, "Ant", ui::Color::light_grey()},
        {{0, 48}, "LO off", ui::Color::light_grey()},
    };

    ui::OptionsField options_rate_{
        {56, 4},
        9,
        {{"  1.0 MHz", 1'000'000},
         {"  2.0 MHz", 2'000'000},
         {"  2.4 MHz", 2'400'000},
         {"  4.0 MHz", 4'000'000},
         {"  8.0 MHz", 8'000'000},
         {" 10.0 MHz", 10'000'000},
         {" 16.0 MHz", 16'000'000},
         {" 20.0 MHz", 20'000'000}}};

    ui::OptionsField options_antenna_{{56, 26}, 8, {}};

    ui::OptionsField options_lo_offset_{
        {56, 48},
        9,
        {{"     off", 0},
         {" 250 kHz", 250'000},
         {" 500 kHz", 500'000},
         {"   1 MHz", 1'000'000},
         {"   2 MHz", 2'000'000}}};

    ui::Checkbox check_dc_{{0, 70}, 10, "DC correct", true};
    ui::Checkbox check_iq_{{0, 94}, 10, "IQ balance", true};

    ui::Button button_reconnect_{{0, 120, 112, 24}, "Reconnect"};
    ui::Button button_back_{{128, 120, 112, 24}, "Back"};

    ui::Console console_{{0, 150, 240, 154}};

    uint32_t frame_counter_{0};
};

}  // namespace app

#endif /*__MB200_DEVICE_APP_H__*/
