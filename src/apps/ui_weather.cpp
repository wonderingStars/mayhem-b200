/*
 * mayhem-b200 — Weather / TPMS receiver (views).
 *
 * The decoders live in ui_weather.hpp so the tests can reach them without the
 * UI; this file is the two ui::View subclasses and the app registration.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2023 Mark Thompson (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_weather.hpp"

#include "../core/string_format.hpp"
#include "../radio/receiver_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_navigation.hpp"

#include <memory>

namespace app {

/* --- Detail view (TPMSRecentEntryDetailView) ------------------------------- */

TpmsDetailView::TpmsDetailView(const tpms::RecentEntry& entry,
                               const tpms::PressureUnit pressure_unit,
                               const tpms::TempUnit temp_unit) {
    add_children({&labels_, &text_type_, &text_id_, &text_signal_, &text_pressure_,
                  &text_temperature_, &text_flags_, &text_count_, &button_done_});

    text_type_.set(tpms::reading_type_name(entry.type));
    text_id_.set(to_string_hex(entry.id, 8));
    text_signal_.set(tpms::signal_type_name(entry.signal_type));

    if (entry.has_pressure) {
        text_pressure_.set(trim(tpms::format_pressure(entry.last_pressure, pressure_unit)) +
                           " " + tpms::pressure_unit_name(pressure_unit));
    } else {
        text_pressure_.set("---");
    }

    if (entry.has_temperature) {
        text_temperature_.set(
            trim(tpms::format_temperature(entry.last_temperature, temp_unit)) + " " +
            ((temp_unit == tpms::TempUnit::Fahrenheit) ? "F" : "C"));
    } else {
        text_temperature_.set("---");
    }

    text_flags_.set(entry.has_flags ? to_string_hex(entry.last_flags, 2) : "--");
    text_count_.set(to_string_dec_uint(entry.received_count, 4));

    button_done_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void TpmsDetailView::on_show() {
    View::on_show();
    button_done_.focus();
}

/* --- Main view (TPMSAppView) ----------------------------------------------- */

WeatherView::WeatherView() {
    add_children({&options_band_, &options_pressure_, &options_temp_, &field_gain_,
                  &text_status_, &text_note_, &recent_view_});

    text_note_.set("TPMS 315/433MHz. See notes.");

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
    }

    auto* receiver = globals().receiver;

    options_band_.on_change = [this](size_t, int32_t hz) {
        if (auto* rx = globals().receiver) {
            rx->set_target_frequency(static_cast<uint64_t>(hz));
        }
        /* A retune invalidates every partially-built frame. */
        decoder_.reset();
    };
    if (receiver != nullptr) {
        if (!options_band_.set_by_value(
                static_cast<int32_t>(receiver->target_frequency()), false)) {
            options_band_.set_selected_index(0, true);
        }
        field_gain_.set_value(static_cast<int32_t>(receiver->gain()), false);
    }

    options_pressure_.on_change = [this](size_t, int32_t v) {
        pressure_unit_ = static_cast<tpms::PressureUnit>(v);
        recent_view_.table().set_dirty();
    };
    options_temp_.on_change = [this](size_t, int32_t v) {
        temp_unit_ = static_cast<tpms::TempUnit>(v);
        recent_view_.table().set_dirty();
    };

    field_gain_.on_change = [](int32_t db) {
        if (auto* rx = globals().receiver) rx->set_gain(db);
    };

    /* Row renderer, laid out as upstream's RecentEntriesTable specialization. */
    recent_view_.table().on_draw = [this](const tpms::RecentEntry& entry,
                                          const ui::Rect& target_rect,
                                          ui::Painter& painter,
                                          const ui::Style& style,
                                          ui::RecentEntriesColumns& columns) {
        std::string line = to_string_dec_uint(static_cast<uint32_t>(entry.type), 2) + " ";

        std::string lid = to_string_hex(entry.id, 8);
        lid.resize(columns.at(1).second, ' ');
        line += lid;

        line += entry.has_pressure
                    ? ("  " + tpms::format_pressure(entry.last_pressure, pressure_unit_))
                    : "     ";
        line += entry.has_temperature
                    ? ("  " + tpms::format_temperature(entry.last_temperature, temp_unit_))
                    : "     ";

        line += (entry.received_count > 999)
                    ? " +++"
                    : (" " + to_string_dec_uint(entry.received_count, 3));

        line += entry.has_flags ? (" " + to_string_hex(entry.last_flags, 2)) : "   ";

        const auto chars = static_cast<size_t>(target_rect.width() / 8);
        line.resize(chars, ' ');
        painter.draw_string(target_rect.location(), style, line);
    };

    recent_view_.on_select = [this](const tpms::RecentEntry& entry) {
        if (auto* nav = globals().nav) {
            nav->push(std::make_unique<TpmsDetailView>(entry, pressure_unit_, temp_unit_));
        }
    };

    decoder_.set_packet_handler(
        [this](tpms::SignalType type, const std::vector<uint8_t>& chips) {
            on_frame(type, chips);
        });

    update_status();
}

void WeatherView::set_parent_rect(const ui::Rect new_parent_rect) {
    View::set_parent_rect(new_parent_rect);
    recent_view_.set_parent_rect({0, header_height, new_parent_rect.width(),
                                  new_parent_rect.height() - header_height});
}

void WeatherView::on_show() {
    View::on_show();
    options_band_.focus();

    if (auto* rx = globals().receiver) {
        /* proc_tpms runs the front end at 2.4576 MHz; the host asks for the
         * nearest thing the USRP will give and derives its rates from what it
         * actually got (see TpmsDecoder::configure). */
        rx->set_sampling_rate(2'457'600.0);
        rx->set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
        if (!rx->running()) rx->start();
        decoder_.configure(rx->sampling_rate());
    }
    update_status();
}

void WeatherView::on_frame(const tpms::SignalType signal_type,
                           const std::vector<uint8_t>& chips) {
    candidates_++;

    const auto reading = tpms::decode_chips(signal_type, chips);
    if (!reading.valid()) return;

    decoded_++;

    auto& entry = ui::on_packet(recent_, tpms::RecentEntry::Key{reading.type, reading.id});
    entry.update(reading);
    entry.signal_type = signal_type;
    recent_view_.table().set_dirty();
}

void WeatherView::update_status() {
    auto* rx = globals().receiver;
    if (rx == nullptr) {
        text_status_.set("no radio");
        text_note_.set("No receiver: decode idle.");
        return;
    }

    text_status_.set(to_string_dec_uint(decoded_, 4) + "/" +
                     to_string_dec_uint(candidates_, 4));

    /* Honest about the tap: take_spectrum_samples() hands back the tail of the
     * most recent DSP block and drops the rest, so frames that straddle a gap
     * are lost. See the header for the tap this app actually wants. */
    text_note_.set(rx->running() ? "Tap is gapped: see notes."
                                 : "Receiver stopped.");
}

void WeatherView::on_frame_sync() {
    View::on_frame_sync();

    auto* rx = globals().receiver;
    if (rx == nullptr) return;

    decoder_.configure(rx->sampling_rate());

    if (rx->take_spectrum_samples(samples_, 32768)) {
        decoder_.process(samples_);
    }

    frame_counter_++;
    if ((frame_counter_ % 30) == 0) update_status();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_weather{{"weather", "Weather/TPMS", app::Category::Receive,
                                  ui::Color::green(), &ui::bitmap_icon_thermometer,
                                  [] { return std::make_unique<app::WeatherView>(); }}};
}  // namespace
