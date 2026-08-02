/*
 * mayhem-b200 — analog audio receiver.
 *
 * The direct counterpart of Mayhem's "Audio" app: big frequency readout, mode
 * and bandwidth selectors, gain, squelch, volume, a signal meter, a frequency
 * scale and a waterfall.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (original app design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_ANALOG_AUDIO_APP_H__
#define __MB200_ANALOG_AUDIO_APP_H__

#include "../dsp/fft.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_menu.hpp"
#include "ui_navigation.hpp"
#include "ui_spectrum.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace app {

class AnalogAudioView : public ui::View {
   public:
    AnalogAudioView();
    ~AnalogAudioView() override;

    std::string title() const override { return "Audio"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

    bool on_key(const ui::KeyEvent key) override;
    bool on_encoder(const ui::EncoderEvent delta) override;

   private:
    void apply_mode();
    void update_config_options();
    void update_readouts();
    void update_spectrum();
    void tune_to_column(int x);

    radio::ReceiverModel& receiver_;

    /* --- Widgets --- */
    ui::BigFrequency big_frequency_{{0, 4, 240, 52}};

    ui::Labels labels_{
        {{0, 60}, "Freq", ui::Color::light_grey()},
        {{0, 78}, "Mode", ui::Color::light_grey()},
        {{0, 96}, "Gain", ui::Color::light_grey()},
        {{0, 114}, "Sql", ui::Color::light_grey()},
        {{120, 114}, "Vol", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 60}};
    ui::FrequencyStepView step_view_{{128, 60}, field_frequency_};

    ui::OptionsField options_mode_{
        {40, 78},
        5,
        {{"AM  ", static_cast<int32_t>(radio::ReceiverModel::Mode::AMAudio)},
         {"NFM ", static_cast<int32_t>(radio::ReceiverModel::Mode::NarrowbandFMAudio)},
         {"WFM ", static_cast<int32_t>(radio::ReceiverModel::Mode::WidebandFMAudio)},
         {"SPEC", static_cast<int32_t>(radio::ReceiverModel::Mode::SpectrumAnalysis)}}};

    ui::OptionsField options_config_{{88, 78}, 9, {}};

    ui::NumberField field_gain_{{40, 96}, 3, {0, 76}, 1, ' '};
    ui::Checkbox check_agc_{{80, 92}, 3, "AGC", true};

    ui::NumberField field_squelch_{{40, 114}, 2, {0, 99}, 1, ' '};
    ui::NumberField field_volume_{{160, 114}, 2, {0, 99}, 1, ' '};

    ui::Text text_level_{{0, 132, 240, 16}, ""};

    ui::spectrum::FrequencyScale freq_scale_{{0, 150, 240, 20}};
    ui::spectrum::WaterfallWidget waterfall_{{0, 170, 240, 134}};

    /* --- Spectrum machinery --- */
    dsp::Fft fft_{1024};
    std::vector<float> window_{};
    std::vector<dsp::cfloat> samples_{};
    std::vector<float> spectrum_db_{};

    uint32_t frame_counter_{0};
};

}  // namespace app

#endif /*__MB200_ANALOG_AUDIO_APP_H__*/
