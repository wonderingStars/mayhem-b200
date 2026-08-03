/*
 * mayhem-b200 — Antenna length calculator (ported from Mayhem's antenna_length /
 * ui_whipcalc). Computes full/half/quarter-wave (and eighth-wave) antenna
 * lengths from a frequency, in metric and imperial, and matches the result
 * against a telescopic-whip element table.
 *
 * Upstream: application/external/antenna_length/ui_whipcalc.{hpp,cpp}
 *   Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 *   Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_WHIPCALC_H__
#define __MB200_UI_WHIPCALC_H__

#include "ui.hpp"
#include "ui_widget.hpp"
#include "ui_freq_field.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* Pure calculation helpers, split out from the View so they can be tested
 * without a display. These are a faithful transcription of the upstream
 * arithmetic. Note: like upstream, no antenna velocity factor is applied — the
 * geometric free-space length is reported. */
namespace antenna {

/* Speed of light, matching the constants baked into the upstream app. */
constexpr double speed_of_light_mps = 299792458.0;
constexpr double speed_of_light_fps = 983571087.90472;

/* Antenna length for the fraction `eighths`/8 of a wavelength, in metres.
 * eighths = 8 → full wave, 4 → half, 2 → quarter, etc. Returns 0 for a 0 Hz
 * frequency (upstream shows "infinity+" in that case instead). */
double antenna_length_m(uint64_t freq_hz, int eighths);
/* Same, in feet. */
double antenna_length_ft(uint64_t freq_hz, int eighths);

/* Result of walking a telescopic whip's element table for a target length.
 * `elements` is the count of fully/partially extended segments; `quarter` is
 * 0..3 (quarters of the last segment), indexing {"", " 1/4", " 1/2", " 3/4"}. */
struct AntennaMatch {
    int elements{0};
    int quarter{0};
};

/* Faithful port of the element-matching loop. `elements` is the cumulative
 * extended length of each segment in millimetres (ascending). `length_mm` is
 * the wanted antenna length in millimetres. */
AntennaMatch match_antenna(double length_mm, const std::vector<uint16_t>& elements);

}  // namespace antenna

class WhipCalcView : public ui::View {
   public:
    WhipCalcView();

    std::string title() const override { return "Ant. length"; }
    void on_show() override;

   private:
    struct antenna_entry {
        std::string label{};
        std::vector<uint16_t> elements{};
    };

    std::vector<antenna_entry> antenna_db{};

    void update_result();

    static constexpr const char* frac_str[4] = {"", " 1/4", " 1/2", " 3/4"};

    ui::Labels labels{
        {{2 * 8, 1 * 16}, "Frequency:", ui::Color::light_grey()},
        {{7 * 8, 2 * 16}, "Wave:", ui::Color::light_grey()},
        {{5 * 8, 3 * 16}, "Metric:", ui::Color::light_grey()},
        {{3 * 8, 4 * 16}, "Imperial:", ui::Color::light_grey()}};

    ui::FrequencyField field_frequency{
        {13 * 8, 1 * 16}};

    ui::OptionsField options_type{
        {13 * 8, 2 * 16},
        7,
        {{"Full", 8},
         {"Half", 4},
         {"Quarter", 2},
         {"3/4", 6},
         {"1/8", 1},
         {"3/8", 3},
         {"5/8", 5},
         {"7/8", 7}}};

    ui::Text text_result_metric{
        {13 * 8, 3 * 16, 10 * 16, 16},
        "-"};
    ui::Text text_result_imperial{
        {13 * 8, 4 * 16, 10 * 16, 16},
        "-"};
    ui::Console console{
        {0, 6 * 16, 240, 160}};

    ui::Button button_exit{
        {240 - 96 - 8, 304 - 32 - 4, 96, 32},
        "Back"};
};

}  // namespace app

#endif /*__MB200_UI_WHIPCALC_H__*/
