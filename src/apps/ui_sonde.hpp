/*
 * mayhem-b200 — Radiosonde receiver view.
 *
 * Port of firmware/application/apps/ui_sonde.*. The protocol parsing and the
 * signal chain (firmware/common/sonde_packet.* and firmware/baseband/proc_sonde.*)
 * live in sonde_packet.hpp, which also carries the provenance notes and the
 * list of deliberate departures from upstream.
 *
 * Two things upstream's screen has that this one cannot:
 *
 *   - "See QR": upstream renders the geo: URI as a QR code through QRCodeView.
 *     There is no QR renderer on the host, so the URI itself is shown instead.
 *   - the RSSI / channel-power bargraphs and the packet beep, both of which
 *     read M4 baseband state that has no counterpart here.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SONDE_H__
#define __MB200_UI_SONDE_H__

#include "../radio/receiver_model.hpp"
#include "sonde_packet.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_geomap.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

namespace app {


/* --- View ----------------------------------------------------------------- */

class SondeView : public ui::View {
   public:
    static constexpr uint64_t initial_target_frequency = 402'700'000;

    SondeView();
    ~SondeView() override;

    SondeView(const SondeView&) = delete;
    SondeView& operator=(const SondeView&) = delete;

    std::string title() const override { return "Radiosnd RX"; }

    void on_show() override;
    void on_frame_sync() override;

    /* The decoder's packet handler. Public because it is the whole of what the
     * view does with a frame; the decode itself is tested against
     * sonde::Decoder in tests/test_sonde.cpp, which needs no radio, while this
     * function does (it reads globals().receiver through the constructor). */
    void on_packet(const sonde::Packet& packet);
    sonde::Decoder& decoder() { return decoder_; }

   private:
    void update_front_end();
    void update_status();
    void open_log();
    void log_packet(const sonde::Packet& packet);

    radio::ReceiverModel& receiver_;
    sonde::Decoder decoder_{};
    sonde::Rs41Calibration calibration_{};

    std::vector<dsp::cfloat> sample_buffer_{};
    double configured_rate_{0.0};
    double configured_offset_{1e30}; /* forces a configure() on the first frame */
    uint32_t frame_counter_{0};

    bool logging_{false};
    bool use_crc_{false};
    std::ofstream log_file_{};
    std::string log_path_{};

    std::string sonde_id_{};
    sonde::GPS_data gps_info_{};
    sonde::temp_humid temp_humid_info_{};

    ui::GeoMapView* geomap_view_{nullptr};
    std::time_t last_timestamp_update_{0};
    int32_t last_altitude_{0};

    ui::Labels labels_{
        {{0, 2}, "Freq", ui::Color::light_grey()},
        {{184, 2}, "G", ui::Color::light_grey()},
        {{0, 20}, "Type:", ui::Color::light_grey()},
        {{0, 36}, "ID:", ui::Color::light_grey()},
        {{0, 52}, "Time:", ui::Color::light_grey()},
        {{0, 68}, "Vbatt:", ui::Color::light_grey()},
        {{0, 84}, "Frame:", ui::Color::light_grey()},
        {{0, 100}, "Temp:", ui::Color::light_grey()},
        {{0, 116}, "Humidity:", ui::Color::light_grey()},
        {{0, 132}, "Pressure:", ui::Color::light_grey()},
        {{0, 148}, "VSpeed:", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 2}};
    ui::FrequencyStepView step_view_{{136, 2}, field_frequency_};
    ui::NumberField field_gain_{{200, 2}, 3, {0, 76}, 1, ' '};

    ui::Text text_type_{{80, 20, 152, 16}, "..."};
    ui::Text text_serial_{{80, 36, 152, 16}, "..."};
    ui::Text text_timestamp_{{80, 52, 152, 16}, "..."};
    ui::Text text_voltage_{{80, 68, 96, 16}, "..."};
    ui::Text text_frame_{{80, 84, 96, 16}, "..."};
    ui::Text text_temp_{{80, 100, 96, 16}, "..."};
    ui::Text text_humid_{{80, 116, 96, 16}, "..."};
    ui::Text text_press_{{80, 132, 96, 16}, "..."};
    ui::Text text_vspeed_{{80, 148, 96, 16}, "..."};

    ui::Checkbox check_log_{{184, 100}, 3, "Log", true};
    ui::Checkbox check_crc_{{184, 124}, 3, "CRC", true};

    ui::GeoPos geopos_{{0, 166}, ui::GeoPos::alt_unit::METERS, ui::GeoPos::spd_unit::HIDDEN};

    ui::Text text_geouri_{{0, 216, 240, 16}, ""};
    ui::Text text_status_{{0, 232, 240, 16}, ""};

    /* The tap limitation is a property of the host build, not of a missing
     * signal, so it is stated on screen rather than left to look like silence. */
    ui::Labels notes_{
        {{0, 250}, "Tap: wideband", ui::Color::yellow()},
        {{0, 266}, "snapshot only;", ui::Color::grey()},
        {{0, 282}, "see sonde_packet", ui::Color::grey()},
    };

    ui::Button button_map_{{144, 250, 92, 46}, "See on map"};
};

}  // namespace app

#endif /*__MB200_UI_SONDE_H__*/
