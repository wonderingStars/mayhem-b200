/*
 * mayhem-b200 — Radiosonde receiver view (implementation).
 *
 * The protocol and the signal chain live in sonde_packet.*; this file is the
 * screen upstream's apps/ui_sonde.cpp draws.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_sonde.hpp"

#include "../core/file_path.hpp"
#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_navigation.hpp"

#include <cmath>
#include <ctime>
#include <utility>

namespace app {

/* --- View ----------------------------------------------------------------- */

SondeView::SondeView()
    : receiver_{*globals().receiver} {
    add_children({&labels_,
                  &field_frequency_,
                  &step_view_,
                  &field_gain_,
                  &text_type_,
                  &text_serial_,
                  &text_timestamp_,
                  &text_voltage_,
                  &text_frame_,
                  &text_temp_,
                  &text_humid_,
                  &text_press_,
                  &text_vspeed_,
                  &check_log_,
                  &check_crc_,
                  &geopos_,
                  &text_geouri_,
                  &text_status_,
                  &notes_,
                  &button_map_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    /* Upstream sets a 500 Hz step for fine tuning; the host's step table has no
     * 500, so 100 Hz is used — finer, never coarser. */
    field_frequency_.set_step_index(2);
    field_frequency_.set_value(initial_target_frequency, false);
    field_frequency_.on_change = [this](uint64_t hz) { receiver_.set_target_frequency(hz); };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    geopos_.set_read_only(true);

    check_log_.set_value(logging_);
    check_log_.on_select = [this](ui::Checkbox&, bool v) {
        logging_ = v;
        if (logging_) open_log();
    };

    check_crc_.set_value(use_crc_);
    check_crc_.on_select = [this](ui::Checkbox&, bool v) { use_crc_ = v; };

    button_map_.on_select = [this](ui::Button&) {
        auto* nav = globals().nav;
        if (nav == nullptr) return;
        auto view = std::make_unique<ui::GeoMapView>(
            sonde_id_,
            geopos_.altitude(),
            ui::GeoPos::alt_unit::METERS,
            ui::GeoPos::spd_unit::HIDDEN,
            geopos_.lat(),
            geopos_.lon(),
            /* Upstream passes a heading out of range so the marker is drawn as
             * a cross rather than a bearing arrow. */
            999,
            [this]() { geomap_view_ = nullptr; });
        geomap_view_ = view.get();
        nav->push(std::move(view));
    };

    decoder_.set_calibration(&calibration_);
    decoder_.set_packet_handler([this](const sonde::Packet& p) { this->on_packet(p); });

    /* proc_sonde's front end: 2.4576 Msps, tuned to the 402.7 MHz sonde band. */
    receiver_.set_sampling_rate(2457600.0);
    receiver_.set_target_frequency(initial_target_frequency);
    /* Nothing here needs audio, and SpectrumAnalysis leaves the wideband tap
     * running while skipping the demodulator and the sound card. */
    receiver_.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);

    update_status();
}

SondeView::~SondeView() {
    if (log_file_.is_open()) log_file_.close();
}

void SondeView::on_show() {
    ui::View::on_show();
    field_frequency_.focus();
    if (!receiver_.running()) receiver_.start();
}

void SondeView::update_front_end() {
    const double rate = receiver_.sampling_rate();

    double offset = 0.0;
    if (auto* r = globals().radio) {
        const double lo = r->rx_frequency();
        if (lo > 0.0) offset = static_cast<double>(receiver_.target_frequency()) - lo;
    }

    if (rate != configured_rate_ || std::fabs(offset - configured_offset_) > 100.0) {
        decoder_.configure(rate, offset);
        configured_rate_ = rate;
        configured_offset_ = offset;
    }
}

void SondeView::on_frame_sync() {
    ui::View::on_frame_sync();
    frame_counter_++;

    if (receiver_.running()) {
        update_front_end();
        /* Best effort: this is a snapshot tap, not a stream. See the note on
         * Decoder in sonde_packet.hpp. */
        if (receiver_.take_spectrum_samples(sample_buffer_, 4096) && !sample_buffer_.empty())
            decoder_.feed(sample_buffer_.data(), sample_buffer_.size());
    }

    if ((frame_counter_ % 15) == 0) update_status();
}

void SondeView::update_status() {
    text_status_.set("frames " + to_string_dec_uint(decoder_.packets_total()) +
                     "  bits " + to_string_dec_uint(decoder_.bits_rs41()));
}

void SondeView::open_log() {
    if (log_file_.is_open()) return;

    const std::string dir = core::data_directory() + "/LOGS";
    if (!core::ensure_directory(dir)) return;

    log_path_ = dir + "/SONDE_" + to_string_timestamp_now() + ".TXT";
    log_file_.open(log_path_, std::ios::out | std::ios::app);
}

void SondeView::log_packet(const sonde::Packet& packet) {
    if (!log_file_.is_open()) return;
    log_file_ << sonde::format_timestamp(packet.received_at()) << ' '
              << packet.symbols_formatted().data << '\n';
    log_file_.flush();
}

void SondeView::on_packet(const sonde::Packet& packet) {
    /* Reject a bad packet only when the operator asked for it, as upstream. */
    if (use_crc_ && !packet.crc_ok()) return;

    /* Past this line the screen is being written, so the portal has a row to
     * publish (see packets_shown() in the header). */
    packets_shown_++;

    text_type_.set(packet.type_string());

    sonde_id_ = packet.serial_number();  /* also the map marker tag */
    text_serial_.set(sonde_id_);

    text_timestamp_.set(sonde::format_timestamp(packet.received_at()));

    text_voltage_.set(unit_auto_scale(static_cast<double>(packet.battery_voltage()), 2, 2) + "V");

    text_frame_.set(to_string_dec_uint(packet.frame()));

    temp_humid_info_ = packet.get_temp_humid();
    if (temp_humid_info_.humid != 0)
        text_humid_.set(to_string_decimal(temp_humid_info_.humid, 1) + "%");
    if (temp_humid_info_.temp != 0)
        text_temp_.set(to_string_decimal(temp_humid_info_.temp, 1) + "\xB0" "C");

    if (packet.get_pressure() != 0)
        text_press_.set(to_string_decimal(packet.get_pressure(), 1) + " hPa");

    gps_info_ = packet.get_GPS_data();

    const std::time_t now = std::chrono::system_clock::to_time_t(packet.received_at());
    if (last_timestamp_update_ != 0 && last_altitude_ != 0) {
        const int32_t time_diff = static_cast<int32_t>(now - last_timestamp_update_);
        if (time_diff >= 10) {  /* update only every 10 seconds */
            const float vspeed =
                static_cast<float>(static_cast<int32_t>(gps_info_.alt) - last_altitude_) /
                static_cast<float>(time_diff);
            last_timestamp_update_ = now;
            last_altitude_ = static_cast<int32_t>(gps_info_.alt);
            text_vspeed_.set(to_string_decimal(vspeed, 1) + " m/s");
        }
    } else {  /* first valid packet: remember time + altitude */
        last_timestamp_update_ = now;
        last_altitude_ = geopos_.altitude();
    }

    if (gps_info_.is_valid()) {  /* only update when valid, to prevent flashing */
        /* The same gate the screen updates behind, so what the portal plots and
         * what the device draws are the same accepted fix. */
        fix_ = gps_info_;
        geopos_.set_altitude(static_cast<int32_t>(gps_info_.alt));
        geopos_.set_lat(gps_info_.lat);
        geopos_.set_lon(gps_info_.lon);
        text_geouri_.set(sonde::geo_uri(gps_info_.lat, gps_info_.lon));
        if (geomap_view_ != nullptr) {
            geomap_view_->update_tag(sonde_id_);
            geomap_view_->update_position(gps_info_.lat, gps_info_.lon, 400,
                                          static_cast<int32_t>(gps_info_.alt), 0);
        }
    }

    if (logging_) log_packet(packet);
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_radiosonde{{"radiosonde", "Radiosnde", app::Category::Receive,
                                     ui::Color::green(), &ui::bitmap_icon_sonde,
                                     [] { return std::make_unique<app::SondeView>(); }}};
}  // namespace
