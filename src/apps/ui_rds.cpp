/*
 * mayhem-b200 — RDS transmitter view.
 *
 * See ui_rds.hpp for the full port notes. This file wires the widgets, builds
 * the group frames on the user's action and streams them through the shared
 * TransmitterModel in raw-IQ mode. Transmit begins only when the user presses
 * Start; a legality warning is shown on screen at all times.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (original)
 * Copyright (C) 2016 Furrtek (original)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_rds.hpp"

#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_alphanum.hpp"
#include "ui_navigation.hpp"

#include <ctime>

namespace app {

RdsView::RdsView() {
    add_children({&labels_, &warning_, &field_frequency_, &field_gain_, &sym_pi_code_,
                  &options_pty_, &check_tp_, &check_ta_, &check_ms_, &check_stereo_, &check_psn_,
                  &text_psn_, &button_psn_, &check_rt_, &button_rt_, &text_rt_, &check_ct_,
                  &button_tx_, &text_status_});

    /* Default to the middle of the FM broadcast band, clamped to what the device
     * reports it can transmit. */
    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_frequency_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                                   static_cast<uint64_t>(caps.tx_freq.max));
    }
    uint64_t default_freq = 100'000'000;
    if (auto* tx = globals().transmitter) {
        const uint64_t f = tx->target_frequency();
        if (f >= 76'000'000 && f <= 108'000'000) default_freq = f;
    }
    field_frequency_.set_value(default_freq, false);
    field_gain_.set_value(40, false);

    sym_pi_code_.set_value(static_cast<uint64_t>(0xF3E0));
    check_tp_.set_value(true);
    check_ta_.set_value(true);
    check_psn_.set_value(true);
    options_pty_.set_selected_index(0);

    text_psn_.set(psn_);
    text_rt_.set(radiotext_);

    button_psn_.on_select = [this](ui::Button&) {
        auto* nav = globals().nav;
        if (!nav) return;
        name_buffer_ = psn_;
        ui::text_prompt(*nav, name_buffer_, 8, ENTER_KEYBOARD_MODE_ALPHA, [this](std::string& s) {
            psn_ = s;
            text_psn_.set(psn_);
        });
    };

    button_rt_.on_select = [this](ui::Button&) {
        auto* nav = globals().nav;
        if (!nav) return;
        name_buffer_ = radiotext_;
        ui::text_prompt(*nav, name_buffer_, 64, ENTER_KEYBOARD_MODE_ALPHA, [this](std::string& s) {
            radiotext_ = s;
            text_rt_.set(radiotext_);
        });
    };

    button_tx_.on_select = [this](ui::Button&) {
        if (transmitting_)
            stop_tx();
        else
            start_tx();
    };

    set_status("Ready. Press Start to transmit.");
}

RdsView::~RdsView() {
    stop_tx();
}

void RdsView::focus() {
    button_tx_.focus();
}

void RdsView::on_hide() {
    stop_tx();
    View::on_hide();
}

void RdsView::set_status(std::string_view text) {
    text_status_.set(text);
    text_status_.set_dirty();
}

void RdsView::rebuild_frames() {
    flags_.PI_code = static_cast<uint16_t>(sym_pi_code_.to_integer());
    flags_.PTY = static_cast<uint8_t>(options_pty_.selected_index_value());
    flags_.DI = check_stereo_.value() ? 1 : 0;
    flags_.TP = check_tp_.value();
    flags_.TA = check_ta_.value();
    flags_.MS = check_ms_.value();

    if (check_psn_.value()) {
        /* Pad/truncate to the 8 characters a PSN carries. */
        std::string psn = psn_;
        psn.resize(8, ' ');
        rds::gen_PSN(frame_psn_, psn, &flags_);
    } else {
        frame_psn_.clear();
    }

    if (check_rt_.value())
        rds::gen_RadioText(frame_radiotext_, radiotext_, 0, &flags_);
    else
        frame_radiotext_.clear();

    if (check_ct_.value()) {
        std::time_t now = std::time(nullptr);
        std::tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        rds::gen_ClockTime(frame_datetime_, &flags_, static_cast<uint16_t>(lt.tm_year + 1900),
                           static_cast<uint8_t>(lt.tm_mon + 1), static_cast<uint8_t>(lt.tm_mday),
                           static_cast<uint8_t>(lt.tm_hour), static_cast<uint8_t>(lt.tm_min), 0);
    } else {
        frame_datetime_.clear();
    }
}

std::vector<uint32_t> RdsView::flatten_enabled_frames() {
    std::vector<uint32_t> blocks;
    auto append = [&blocks](const std::vector<rds::RDSGroup>& frame) {
        for (const auto& g : frame)
            for (int i = 0; i < 4; i++) blocks.push_back(g.block[i]);
    };
    append(frame_psn_);
    append(frame_radiotext_);
    append(frame_datetime_);
    return blocks;
}

void RdsView::start_tx() {
    auto* tx = globals().transmitter;
    if (!tx) {
        set_status("No transmitter. Needs a B200.");
        return;
    }

    rebuild_frames();
    auto blocks = flatten_enabled_frames();
    if (blocks.empty()) {
        set_status("Enable Name, Text or Time first.");
        return;
    }

    modulator_.set_blocks(std::move(blocks));

    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(rds::RdsModulator::kSampleRate);
    tx->set_target_frequency(field_frequency_.value());
    tx->set_gain(static_cast<double>(field_gain_.value()));
    tx->set_iq_source([this](std::complex<float>* out, size_t n) {
        return modulator_.generate(out, n);
    });

    if (!tx->start()) {
        tx->set_iq_source(nullptr);
        set_status("TX start failed. Needs a B200.");
        return;
    }

    set_transmitting(true);
    set_status("Transmitting RDS.");
}

void RdsView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    if (transmitting_) {
        set_transmitting(false);
        set_status("Stopped.");
    }
}

void RdsView::set_transmitting(bool on) {
    transmitting_ = on;
    button_tx_.set_text(on ? "Stop TX" : "Start TX");
    button_tx_.set_dirty();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_rdstx{{
    "rdstx",
    "RDS TX",
    app::Category::Transmit,
    ui::Color::green(),
    &ui::bitmap_icon_rds,
    [] { return std::make_unique<app::RdsView>(); },
    false,
}};
}  // namespace
