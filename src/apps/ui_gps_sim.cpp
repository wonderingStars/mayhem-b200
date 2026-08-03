/*
 * mayhem-b200 — GPS L1 C/A baseband simulator (transmit) — view.
 *
 * See ui_gps_sim.hpp for the port's scope and honesty notes. The encoder lives
 * in the header (namespace gps); this file is the UI view plus registrar.
 *
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2020 Shao
 * Copyright (C) 2015-2020 osqzss (gps-sdr-sim algorithms)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_gps_sim.hpp"

#include "../core/string_format.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_modal.hpp"

namespace app {

GpsSimView::GpsSimView() {
    add_children({&labels_, &field_frequency_, &field_lat_, &field_lon_,
                  &field_alt_, &field_num_sats_, &field_week_, &field_tow_,
                  &text_status_, &console_, &button_start_});

    field_frequency_.set_step_index(4);  /* 5 kHz */
    field_frequency_.set_value(static_cast<uint64_t>(gps::L1_FREQ));
    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_frequency_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                                   static_cast<uint64_t>(caps.tx_freq.max));
    }

    /* Static position defaults (1/1000 deg): a plausible mid-latitude point. */
    field_lat_.set_value(37775);   /*  37.775 deg N */
    field_lon_.set_value(-122419); /* 122.419 deg W */
    field_alt_.set_value(10);      /* metres        */
    field_num_sats_.set_value(8);
    field_week_.set_value(280);
    field_tow_.set_value(0);

    button_start_.on_select = [this](ui::Button&) { toggle_tx(); };

    console_.enable_scrolling(true);
    console_.writeln(STR_COLOR_RED "WARNING: transmitting GPS is illegal");
    console_.writeln(STR_COLOR_RED "in almost all jurisdictions and can");
    console_.writeln(STR_COLOR_RED "disrupt navigation and timing. TX only");
    console_.writeln(STR_COLOR_RED "into a shielded cable/enclosure you own.");
    console_.writeln(STR_COLOR_LIGHT_GREY "Needs a USRP B200 for RF output.");

    refresh_status();
}

GpsSimView::~GpsSimView() {
    stop_tx();
}

void GpsSimView::focus() {
    button_start_.focus();
}

void GpsSimView::on_show() {
    View::on_show();
    refresh_status();
}

void GpsSimView::on_hide() {
    stop_tx();
    View::on_hide();
}

void GpsSimView::on_frame_sync() {
    View::on_frame_sync();
    if (!transmitting_) return;

    /* Keep the DSP-thread ring topped up from the frame thread (no view owns a
     * thread — the porting rule). Generate in modest blocks until the ring is
     * full or a block's worth of headroom is gone. */
    std::complex<float> block[4096];
    for (int guard = 0; guard < 64; guard++) {
        const size_t space = ring_.free_space();
        if (space < 4096) break;
        generator_.generate(block, 4096);
        ring_.write(block, 4096);
    }
}

std::vector<gps::SatConfig> GpsSimView::build_sat_list() const {
    std::vector<gps::SatConfig> sats;
    const int n = field_num_sats_.value();
    for (int prn = 1; prn <= n && prn <= gps::MAX_PRN; prn++) {
        gps::SatConfig c;
        c.prn = prn;
        c.power = 1.0f;
        c.doppler_hz = 0.0;        /* no geometry model — see header */
        c.code_phase_chips = 0.0;
        sats.push_back(c);
    }
    return sats;
}

void GpsSimView::toggle_tx() {
    if (transmitting_)
        stop_tx();
    else
        confirm_and_start();
}

void GpsSimView::confirm_and_start() {
    /* Explicit, unmistakable consent before any RF: a modal legality prompt the
     * first time in a session, then start only on YES. */
    if (warned_) {
        start_tx();
        return;
    }
    auto* nav = globals().nav;
    if (!nav) {
        start_tx();
        return;
    }
    ui::display_modal(
        *nav, "Transmit GPS?",
        "Radiating GPS L1 is illegal in\n"
        "most places and disrupts real\n"
        "receivers. Only into a cable or\n"
        "shield you own. Continue?",
        ui::YESNO, [this](bool choice) {
            if (choice) {
                warned_ = true;
                start_tx();
            }
        });
}

void GpsSimView::start_tx() {
    stop_tx();

    auto* tx = globals().transmitter;
    if (!tx) {
        ui::display_modal("No transmitter", "No transmitter wired.\nNeeds a USRP B200.");
        return;
    }

    const auto sats = build_sat_list();
    if (sats.empty()) {
        ui::display_modal("No satellites", "Set at least one satellite.");
        return;
    }

    generator_.configure(sample_rate_,
                         static_cast<uint16_t>(field_week_.value()),
                         static_cast<uint32_t>(field_tow_.value()),
                         sats);
    ring_.clear();

    /* Prime the ring before the DSP thread starts pulling. */
    std::complex<float> block[4096];
    for (int i = 0; i < 8; i++) {
        generator_.generate(block, 4096);
        ring_.write(block, 4096);
    }

    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(sample_rate_);
    tx->set_target_frequency(field_frequency_.value());
    tx->set_iq_source([this](std::complex<float>* out, size_t n) {
        return ring_.read(out, n);
    });

    if (!tx->start()) {
        ui::display_modal("TX failed", "TX start failed.\nNeeds a USRP B200.");
        tx->set_iq_source(nullptr);
        return;
    }

    transmitting_ = true;
    button_start_.set_text("Stop TX");
    console_.writeln(STR_COLOR_GREEN "TX started (unverified without RF).");
    refresh_status();
}

void GpsSimView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    ring_.clear();
    if (transmitting_) {
        transmitting_ = false;
        button_start_.set_text("Start TX");
        console_.writeln(STR_COLOR_LIGHT_GREY "TX stopped.");
    }
    refresh_status();
}

void GpsSimView::refresh_status() {
    const int n = field_num_sats_.value();
    std::string s = (transmitting_ ? STR_COLOR_GREEN "TX " : STR_COLOR_YELLOW "Idle ");
    s += to_string_dec_int(n) + " PRN";
    if (n != 1) s += "s";
    s += " @ 2.6Msps";
    text_status_.set(s);
    button_start_.set_text(transmitting_ ? "Stop TX" : "Start TX");
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_gpssim{{
    "gpssim",                    /* id, matches upstream short name       */
    "GPS Sim",                   /* menu caption                          */
    app::Category::Transmit,     /* upstream app_location_t::TX           */
    ui::Color::green(),          /* upstream icon_color                    */
    nullptr,                     /* no matching icon in the host set       */
    [] { return std::make_unique<app::GpsSimView>(); },
    false                        /* not hardware-limited beyond needing TX */
}};
}  // namespace
