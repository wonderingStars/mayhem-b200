/*
 * mayhem-b200 — wideband spectrum and waterfall.
 *
 * Where Mayhem's Looking Glass sweeps the HackRF's 20 MHz window across a range
 * to build a wide picture, a B200 can capture up to 56 MHz in one shot. This
 * app therefore shows a single live span rather than a sweep, which is both
 * simpler and faster to update.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_SPECTRUM_APP_H__
#define __MB200_SPECTRUM_APP_H__

#include "../dsp/fft.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_spectrum.hpp"
#include "ui_widget.hpp"

#include <string>
#include <vector>

namespace app {

class SpectrumAnalysisView : public ui::View {
   public:
    SpectrumAnalysisView();
    ~SpectrumAnalysisView() override;

    std::string title() const override { return "Spectrum"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void tune_to_column(int x);

    radio::ReceiverModel& receiver_;
    radio::ReceiverModel::Mode previous_mode_{};

    ui::Labels labels_{
        {{0, 4}, "Centre", ui::Color::light_grey()},
        {{0, 22}, "Span", ui::Color::light_grey()},
        {{120, 22}, "Gain", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{56, 4}};

    ui::OptionsField options_span_{
        {56, 22},
        6,
        {{" 1 MHz", 1'000'000},
         {" 2 MHz", 2'000'000},
         {" 4 MHz", 4'000'000},
         {" 8 MHz", 8'000'000},
         {"16 MHz", 16'000'000},
         {"20 MHz", 20'000'000}}};

    ui::NumberField field_gain_{{168, 22}, 3, {0, 76}, 1, ' '};
    ui::Checkbox check_peak_{{0, 40}, 4, "Peak", true};

    ui::spectrum::SpectrumWidget trace_{{0, 64, 240, 80}};
    ui::spectrum::FrequencyScale freq_scale_{{0, 146, 240, 20}};
    ui::spectrum::WaterfallWidget waterfall_{{0, 166, 240, 138}};

    dsp::Fft fft_{2048};
    std::vector<float> window_{};
    std::vector<dsp::cfloat> samples_{};
    std::vector<float> spectrum_db_{};
};

}  // namespace app

#endif /*__MB200_SPECTRUM_APP_H__*/
