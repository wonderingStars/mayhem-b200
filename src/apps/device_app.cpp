/*
 * mayhem-b200 — radio setup and device information.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "device_app.hpp"

#include "../core/string_format.hpp"
#include "../radio/receiver_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

#include <cmath>

namespace app {

namespace {

std::string mhz(double hz) {
    return to_string_decimal(static_cast<float>(hz / 1e6), 3) + " MHz";
}

}  // namespace

DeviceView::DeviceView() {
    add_children({&labels_, &options_rate_, &options_antenna_, &options_lo_offset_,
                  &check_dc_, &check_iq_, &button_reconnect_, &button_back_, &console_});

    auto* receiver = globals().receiver;
    auto* r = globals().radio;

    if (receiver != nullptr)
        options_rate_.set_by_nearest_value(
            static_cast<int32_t>(receiver->sampling_rate()), false);

    options_rate_.on_change = [this](size_t, int32_t value) {
        if (auto* rx = globals().receiver) {
            rx->set_sampling_rate(static_cast<double>(value));
            refresh_info();
        }
    };

    refresh_antennas();
    options_antenna_.on_change = [this](size_t index, int32_t) {
        auto* radio = globals().radio;
        if (radio == nullptr) return;
        const auto& antennas = radio->caps().rx_antennas;
        if (index < antennas.size()) {
            radio->set_rx_antenna(antennas[index]);
            refresh_info();
        }
    };

    if (r != nullptr)
        options_lo_offset_.set_by_nearest_value(static_cast<int32_t>(r->lo_offset()), false);

    options_lo_offset_.on_change = [this](size_t, int32_t value) {
        if (auto* radio = globals().radio) {
            radio->set_lo_offset(static_cast<double>(value));
            refresh_info();
        }
    };

    check_dc_.set_value(true);
    check_dc_.on_select = [](ui::Checkbox&, bool value) {
        if (auto* radio = globals().radio) radio->set_rx_dc_offset_auto(value);
    };

    check_iq_.set_value(true);
    check_iq_.on_select = [](ui::Checkbox&, bool value) {
        if (auto* radio = globals().radio) radio->set_rx_iq_balance_auto(value);
    };

    button_reconnect_.on_select = [this](ui::Button&) {
        auto* radio = globals().radio;
        auto* rx = globals().receiver;
        if (radio == nullptr) return;

        const bool was_running = (rx != nullptr) && rx->running();
        if (rx != nullptr) rx->stop();

        radio->close();
        radio->open("");

        refresh_antennas();
        refresh_info();

        if (was_running && rx != nullptr) rx->start();
    };

    button_back_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void DeviceView::refresh_antennas() {
    auto* radio = globals().radio;
    if (radio == nullptr) return;

    ui::OptionsField::options_t options;
    const auto& antennas = radio->caps().rx_antennas;
    for (size_t i = 0; i < antennas.size(); i++)
        options.emplace_back(antennas[i], static_cast<int32_t>(i));

    if (options.empty()) options.emplace_back("-", 0);
    options_antenna_.set_options(std::move(options));

    /* Reflect the port the device is actually on. */
    const auto& current = radio->rx_antenna();
    for (size_t i = 0; i < antennas.size(); i++) {
        if (antennas[i] == current) {
            options_antenna_.set_selected_index(i, false);
            break;
        }
    }
}

void DeviceView::on_show() {
    View::on_show();
    options_rate_.focus();
    refresh_info();
}

void DeviceView::on_frame_sync() {
    View::on_frame_sync();

    /* Stream counters move constantly; refresh about twice a second. */
    if ((++frame_counter_ % 30) == 0) refresh_info();
}

void DeviceView::refresh_info() {
    auto* radio = globals().radio;
    console_.clear();
    console_.enable_scrolling(false);

    if (radio == nullptr || !radio->is_open()) {
        console_.writeln(STR_COLOR_RED "No USRP connected.");
        console_.writeln("");
        console_.writeln("Connect a B200/B210 and press");
        console_.writeln("Reconnect.");
        if (radio != nullptr && !radio->last_error().empty()) {
            console_.writeln("");
            console_.writeln(truncate(radio->last_error(), 29));
        }
        return;
    }

    const auto& caps = radio->caps();

    console_.writeln(STR_COLOR_CYAN + caps.mboard +
                     (caps.serial.empty() ? "" : " " + caps.serial));
    console_.writeln("MCR   " + mhz(caps.master_clock_rate));
    console_.writeln("Freq  " + mhz(caps.rx_freq.min) + " - " + mhz(caps.rx_freq.max));
    console_.writeln("Gain  0 - " + to_string_dec_int(static_cast<int64_t>(caps.rx_gain.max)) + " dB");
    console_.writeln("Rate  " + mhz(radio->rx_rate()));
    console_.writeln("BW    " + mhz(radio->rx_bandwidth()));

    const auto& stats = radio->stats();
    const uint32_t over = stats.overflows.load();
    const uint32_t dropped = stats.rx_dropped.load();

    const char* over_color = (over == 0) ? STR_COLOR_GREEN : STR_COLOR_RED;
    console_.writeln(std::string{over_color} + "Ovf   " + to_string_dec_uint(over));

    const char* drop_color = (dropped == 0) ? STR_COLOR_GREEN : STR_COLOR_RED;
    console_.writeln(std::string{drop_color} + "Drop  " + to_string_dec_uint(dropped));
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_device{{"radio", "Radio Setup", app::Category::Settings,
                                 ui::Color::cyan(), &ui::bitmap_icon_peripherals_details,
                                 [] { return std::make_unique<app::DeviceView>(); }}};
}  // namespace
