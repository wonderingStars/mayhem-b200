/*
 * mayhem-b200 — IQ capture to disk.
 *
 * Writes interleaved int16 I/Q to a .C16 file with a Mayhem-format .TXT
 * sidecar, so recordings are interchangeable with PortaPack captures.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_CAPTURE_APP_H__
#define __MB200_CAPTURE_APP_H__

#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <string>

namespace app {

class CaptureView : public ui::View {
   public:
    CaptureView();
    ~CaptureView() override;

    std::string title() const override { return "Capture"; }

    void on_show() override;
    void on_frame_sync() override;

   private:
    void toggle_capture();
    void refresh_status();

    radio::ReceiverModel& receiver_;

    ui::Labels labels_{
        {{0, 4}, "Centre", ui::Color::light_grey()},
        {{0, 22}, "Rate", ui::Color::light_grey()},
        {{0, 40}, "Gain", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{56, 4}};

    ui::OptionsField options_rate_{
        {56, 22},
        9,
        {{"  1.0 MHz", 1'000'000},
         {"  2.0 MHz", 2'000'000},
         {"  2.4 MHz", 2'400'000},
         {"  4.0 MHz", 4'000'000},
         {"  8.0 MHz", 8'000'000},
         {" 10.0 MHz", 10'000'000},
         {" 20.0 MHz", 20'000'000}}};

    ui::NumberField field_gain_{{56, 40}, 3, {0, 76}, 1, ' '};

    ui::Button button_record_{{0, 64, 112, 28}, "Record"};
    ui::Button button_back_{{128, 64, 112, 28}, "Back"};

    ui::Console console_{{0, 100, 240, 204}};

    uint32_t frame_counter_{0};
    bool last_capturing_{false};
};

}  // namespace app

#endif /*__MB200_CAPTURE_APP_H__*/
