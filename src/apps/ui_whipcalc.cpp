/*
 * mayhem-b200 — Antenna length calculator (host port of Mayhem's ui_whipcalc).
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (original)
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_whipcalc.hpp"

#include "app_context.hpp"
#include "string_format.hpp"
#include "ui_navigation.hpp"

namespace app {

namespace antenna {

double antenna_length_m(uint64_t freq_hz, int eighths) {
    if (freq_hz == 0) return 0.0;
    const double divider = static_cast<double>(eighths) / 8.0;
    return (speed_of_light_mps / static_cast<double>(freq_hz)) * divider;
}

double antenna_length_ft(uint64_t freq_hz, int eighths) {
    if (freq_hz == 0) return 0.0;
    const double divider = static_cast<double>(eighths) / 8.0;
    return (speed_of_light_fps / static_cast<double>(freq_hz)) * divider;
}

/* Faithful transcription of the upstream get_decimals() (string_format.cpp).
 * The multiplier is a plain int here (upstream int16_t) to avoid a lossy-cast
 * warning; the values used (10, 12, 100) fit either type identically. */
static double get_decimals(double num, int mult, bool round = false) {
    num -= static_cast<int>(num);  // keep decimals only
    num *= mult;                   // shift decimals into integers
    if (!round) return num;
    int intnum = static_cast<int>(num);  // round up if necessary
    num -= intnum;
    if (num > .5) intnum++;
    return intnum;
}

AntennaMatch match_antenna(double length_mm, const std::vector<uint16_t>& elements) {
    AntennaMatch result{};
    uint16_t element = 0;
    int refined_quarter = 0;

    for (element = 0; element < elements.size(); element++) {
        if (length_mm == elements[element]) {  // exact element in length
            element++;                         // real element is +1 (zero-based vector)
            break;
        } else if (length_mm < elements[element]) {
            /* Upstream reads elements[element-1] here; guard element==0 (a
             * needed length shorter than the first segment) to avoid an
             * out-of-bounds read that upstream would perform. */
            const double prev = (element == 0) ? 0.0 : static_cast<double>(elements[element - 1]);
            const double remain = length_mm - prev;                                    // mm needed from this element
            const double this_element = static_cast<double>(elements[element]) - prev;  // total mm on this element
            const double quarter = (remain * 4) / this_element;
            if (quarter - static_cast<int>(quarter) > 0.5) {  // closer to next quarter
                refined_quarter = static_cast<int>(quarter) + 1;
                if (refined_quarter == 4) {  // closer to next element
                    refined_quarter = 0;
                    element++;
                }
            } else {
                refined_quarter = static_cast<int>(quarter);
            }
            break;
        }
    }

    result.elements = static_cast<int>(element);
    result.quarter = refined_quarter;
    return result;
}

}  // namespace antenna

void WhipCalcView::update_result() {
    console.clear(true);

    const int eighths = static_cast<int>(options_type.selected_index_value());
    const uint64_t freq = field_frequency.value();

    if (freq == 0) {
        text_result_metric.set("infinity+");
        text_result_imperial.set("infinity+");
        return;
    }

    // Metric
    double length = antenna::antenna_length_m(freq, eighths);
    auto m = to_string_dec_int(static_cast<int>(length), 0);
    double calclength = antenna::get_decimals(length, 100);  // cm
    auto cm = to_string_dec_int(static_cast<int>(calclength), 0);
    auto mm = to_string_dec_int(static_cast<int>(antenna::get_decimals(calclength, 10, true)), 0);
    text_result_metric.set(m + "m " + cm + "." + mm + "cm");

    // Imperial
    double calclength_i = antenna::antenna_length_ft(freq, eighths);
    auto feet = to_string_dec_int(static_cast<int>(calclength_i), 0);
    calclength_i = antenna::get_decimals(calclength_i, 12);  // inches
    auto inch = to_string_dec_int(static_cast<int>(calclength_i), 0);
    auto inch_c = to_string_dec_int(static_cast<int>(antenna::get_decimals(calclength_i, 10, true)), 0);
    text_result_imperial.set(feet + "ft " + inch + "." + inch_c + "in");

    // Whip element matching, in millimetres.
    const double length_mm = length * 1000.0;
    for (const antenna_entry& ant : antenna_db) {
        auto match = antenna::match_antenna(length_mm, ant.elements);
        console.write(ant.label + ": " + to_string_dec_int(match.elements, 1) +
                      frac_str[match.quarter] + " elements\n");
    }
}

WhipCalcView::WhipCalcView() {
    add_children({&labels,
                  &field_frequency,
                  &options_type,
                  &text_result_metric,
                  &text_result_imperial,
                  &console,
                  &button_exit});

    /* The upstream app can load extra whip definitions from ANTENNAS.TXT on the
     * SD card. The host has no such reader wired up, so only the built-in
     * default (Mayhem's ANT500) is provided. */
    antenna_db.push_back({"ANT500", {185, 315, 450, 586, 724, 862}});

    options_type.set_selected_index(2);  // Quarter wave
    options_type.on_change = [this](size_t, ui::OptionsField::value_t) {
        this->update_result();
    };

    field_frequency.set_step_index(10);  // 1 MHz step
    field_frequency.on_change = [this](uint64_t) {
        this->update_result();
    };

    button_exit.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void WhipCalcView::on_show() {
    View::on_show();
    field_frequency.focus();
    update_result();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_whipcalc{{"antenna_length", "Ant. length",
                                   app::Category::Utilities, ui::Color::cyan(),
                                   nullptr,
                                   [] { return std::make_unique<app::WhipCalcView>(); }}};
}  // namespace
