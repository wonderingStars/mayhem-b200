/*
 * mayhem-b200 — signal level meter.
 *
 * Ported from firmware/application/external/level/. Upstream Level is an RX app
 * that shows the tuned channel's signal strength: RSSI (min/avg/max, 0..255 from
 * the HackRF), channel power in dB, and an "RxSat" ADC-saturation gauge read
 * from an M4 performance counter, with a full LNA/VGA/AMP/BW/MODE control panel
 * and an optional radar-detector-style beep whose pitch tracks the level.
 *
 * The host maps the parts a B200 + UHD actually provide:
 *   - RF (wideband) level and channel level are read as dBFS floats from the
 *     ReceiverModel (rf_level_db() / channel_level_db()), not as HackRF 0..255
 *     RSSI, so the readouts and bar are in dB.
 *   - One continuous gain control replaces the HackRF's separate LNA/VGA/AMP.
 * Parts that depend on PortaPack-only hardware/firmware are shown as an
 * on-screen "not applicable on B200" note rather than faked (see .cpp): the M4
 * RxSat performance counter, CTCSS decode, and the baseband level->beep.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2018 Furrtek
 * Copyright (C) 2023 gullradriel, Nilorea Studio Inc. (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_LEVEL_H__
#define __MB200_UI_LEVEL_H__

#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <cmath>
#include <cstdint>
#include <string>

namespace app {

/* --- Pure scaling helpers (tested in tests/test_level.cpp) --------------- */

/* Upstream LevelView::map(): a linear remap of `value` from [in_low, in_high]
 * onto [out_low, out_high]. Upstream is unguarded; a zero-width input range
 * divides by zero on x86, so the host returns out_low there (the value the
 * original produced at the low end). */
inline int32_t level_map_range(int32_t value,
                               int32_t in_low, int32_t in_high,
                               int32_t out_low, int32_t out_high) {
    if (in_high == in_low) return out_low;
    return out_low + (value - in_low) * (out_high - out_low) / (in_high - in_low);
}

/* Map a dBFS reading onto a 0..255 bar value (ui::VuMeter's range), clamped to
 * the endpoints so a reading outside [floor_db, ceil_db] pins the bar rather
 * than wrapping. */
inline uint8_t level_db_to_bar255(float db, float floor_db, float ceil_db) {
    if (ceil_db <= floor_db || db <= floor_db) return 0;
    if (db >= ceil_db) return 255;
    const float frac = (db - floor_db) / (ceil_db - floor_db);
    long v = std::lround(static_cast<double>(frac) * 255.0);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return static_cast<uint8_t>(v);
}

/* Map a dBFS reading onto 0..100 percent, clamped. */
inline int level_db_to_percent(float db, float floor_db, float ceil_db) {
    if (ceil_db <= floor_db || db <= floor_db) return 0;
    if (db >= ceil_db) return 100;
    const float frac = (db - floor_db) / (ceil_db - floor_db);
    long v = std::lround(static_cast<double>(frac) * 100.0);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return static_cast<int>(v);
}

/* Display range for the bars, in dBFS. -100 is roughly a B200's noise floor at
 * moderate gain; 0 dBFS is full scale. */
constexpr float kLevelFloorDb = -100.0f;
constexpr float kLevelCeilDb = 0.0f;

class LevelView : public ui::View {
   public:
    LevelView();
    ~LevelView() override;

    LevelView(const LevelView&) = delete;
    LevelView& operator=(const LevelView&) = delete;

    std::string title() const override { return "Level"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void update_readouts();

    radio::ReceiverModel& receiver_;
    uint32_t frame_counter_{0};

    ui::Labels labels_{
        {{0, 2}, "Freq", ui::Color::light_grey()},
        {{0, 20}, "Mode", ui::Color::light_grey()},
        {{120, 20}, "Gain", ui::Color::light_grey()},
        {{0, 38}, "Vol", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 2}};
    ui::FrequencyStepView step_view_{{132, 2}, field_frequency_};

    ui::OptionsField options_mode_{
        {40, 20},
        5,
        {{"AM  ", static_cast<int32_t>(radio::ReceiverModel::Mode::AMAudio)},
         {"NFM ", static_cast<int32_t>(radio::ReceiverModel::Mode::NarrowbandFMAudio)},
         {"WFM ", static_cast<int32_t>(radio::ReceiverModel::Mode::WidebandFMAudio)},
         {"SPEC", static_cast<int32_t>(radio::ReceiverModel::Mode::SpectrumAnalysis)}}};

    ui::NumberField field_gain_{{160, 20}, 3, {0, 76}, 1, ' '};
    ui::NumberField field_volume_{{40, 38}, 2, {0, 99}, 1, ' '};

    ui::Text text_rf_{{0, 60, 240, 16}, "RF     --.- dB"};
    ui::VuMeter rf_meter_{{8, 78, 224, 14}, 28, true};

    ui::Text text_ch_{{0, 98, 240, 16}, "CH     --.- dB"};
    ui::VuMeter ch_meter_{{8, 116, 224, 14}, 28, true};

    ui::Text text_sql_{{0, 136, 240, 16}, ""};

    ui::Labels na_notes_{
        {{0, 162}, "Not applicable on B200:", ui::Color::yellow()},
        {{0, 178}, " RxSat: needs M4 counter", ui::Color::grey()},
        {{0, 194}, " LNA/VGA/AMP: 1 gain ctl", ui::Color::grey()},
        {{0, 210}, " CTCSS decode: not ported", ui::Color::grey()},
        {{0, 226}, " 0-255 RSSI -> dBFS shown", ui::Color::grey()},
        {{0, 250}, "Live readings need a USRP.", ui::Color::light_grey()},
    };
};

}  // namespace app

#endif /*__MB200_UI_LEVEL_H__*/
