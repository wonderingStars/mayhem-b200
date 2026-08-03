/*
 * mayhem-b200 — FM Radio (broadcast listening app).
 *
 * Copyright (C) 2024 HTotoo
 * Copyright (C) 2025 RocketGod
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_fmradio.hpp"

#include "../audio/audio_out.hpp"
#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"

#include <cmath>

namespace app {

using Mode = radio::ReceiverModel::Mode;
using AmConfig = radio::ReceiverModel::AmConfig;
using NfmConfig = radio::ReceiverModel::NfmConfig;
using WfmConfig = radio::ReceiverModel::WfmConfig;

/* --- View ----------------------------------------------------------------- */

FmRadioView::FmRadioView()
    : receiver_{*globals().receiver} {
    add_children({&field_frequency_,
                  &step_view_,
                  &labels_,
                  &field_gain_,
                  &field_volume_,
                  &rf_meter_,
                  &af_meter_,
                  &options_modulation_,
                  &options_bw_,
                  &button_fav_save_,
                  &text_save_help_,
                  &na_notes_});

    for (auto& b : buttons_fav_) add_child(&b);

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    /* Upstream's empty-slot fill, applied to the just-loaded settings. */
    FmRadioFavourite favs[kFmRadioFavouriteSlots]{};
    for (size_t i = 0; i < kFmRadioFavouriteSlots; ++i) {
        favs[i] = {fav_freq_[i], fav_mod_[i], fav_bw_[i]};
    }
    fmradio_fill_empty_favourites(favs, kFmRadioFavouriteSlots);
    for (size_t i = 0; i < kFmRadioFavouriteSlots; ++i) {
        fav_freq_[i] = favs[i].frequency;
        fav_mod_[i] = favs[i].modulation;
        fav_bw_[i] = favs[i].bandwidth;
    }

    if (receiver_.target_frequency() == 0) {
        receiver_.set_target_frequency(kFmRadioDefaultFrequency);
    }
    field_frequency_.set_value(receiver_.target_frequency(), false);
    field_frequency_.on_change = [this](uint64_t hz) { receiver_.set_target_frequency(hz); };

    /* Upstream field_frequency.set_step(25000) — index 8 of the host's step
     * table is 25 kHz. */
    for (size_t i = 0; i < ui::FrequencyField::step_count; ++i) {
        if (static_cast<uint64_t>(ui::FrequencyField::steps[i]) == kFmRadioFrequencyStep) {
            field_frequency_.set_step_index(i);
            break;
        }
    }

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    field_volume_.set_value(receiver_.volume(), false);
    field_volume_.on_change = [this](int32_t v) {
        receiver_.set_volume(static_cast<uint8_t>(v));
    };

    options_modulation_.on_change = [this](size_t, int32_t v) {
        change_mode(static_cast<FmRadioModulation>(v));
    };

    button_fav_save_.on_select = [this](ui::Button&) {
        save_armed_ = !save_armed_;
        text_save_help_.set(save_armed_ ? "Select slot" : "");
    };

    for (size_t i = 0; i < buttons_fav_.size(); ++i) {
        buttons_fav_[i].on_select = [this, i](ui::Button&) { on_favourite_clicked(i); };
    }

    update_favourite_texts();

    /* Upstream starts in WFM. */
    options_modulation_.set_by_value(static_cast<int32_t>(FmRadioModulation::Wfm), false);
    change_mode(FmRadioModulation::Wfm);
}

FmRadioView::~FmRadioView() = default;

void FmRadioView::populate_bandwidth_options(core::freqman_index_t table, int32_t default_value) {
    /* The host equivalent of upstream freqman_set_bandwidth_option(): rebuild
     * the OptionsField from the freqman table for this modulation. */
    ui::OptionsField::options_t options;
    const size_t n = core::freqman_bandwidth_count(table);
    for (size_t i = 0; i < n; ++i) {
        const auto index = static_cast<core::freqman_index_t>(i);
        options.emplace_back(core::freqman_entry_get_bandwidth_string(table, index),
                             core::freqman_entry_get_bandwidth_value(table, index));
    }
    options_bw_.set_options(std::move(options));

    /* Restore the persisted selection when it is still valid for this table,
     * otherwise fall back to upstream's per-mode default. */
    const bool restored =
        (radio_bw_ < options_bw_.options().size()) &&
        (options_bw_.set_by_value(options_bw_.options()[radio_bw_].second, false));
    if (!restored) options_bw_.set_by_value(default_value, false);

    options_bw_.on_change = [this](size_t index, int32_t value) {
        radio_bw_ = static_cast<uint8_t>(index);
        switch (receiver_.mode()) {
            case Mode::AMAudio:
                receiver_.set_am_configuration(static_cast<AmConfig>(value));
                break;
            case Mode::NarrowbandFMAudio:
                receiver_.set_nfm_configuration(static_cast<NfmConfig>(value));
                break;
            case Mode::WidebandFMAudio:
                receiver_.set_wfm_configuration(static_cast<WfmConfig>(value));
                break;
            default:
                break;
        }
    };
}

void FmRadioView::change_mode(FmRadioModulation modulation) {
    modulation_ = modulation;
    const auto cfg = fmradio_mode_config(modulation);

    receiver_.set_mode(cfg.mode);

    /* Upstream re-tunes the front end identically for every mode. */
    receiver_.set_sampling_rate(kFmRadioSamplingRate);
    if (auto* r = globals().radio) r->set_rx_bandwidth(kFmRadioBasebandBandwidth);

    populate_bandwidth_options(cfg.bandwidth_table, cfg.default_bandwidth_value);

    const int32_t bw_value = options_bw_.options().empty()
                                 ? cfg.default_bandwidth_value
                                 : options_bw_.selected_index_value();

    switch (cfg.mode) {
        case Mode::AMAudio:
            /* SSB is chosen by the modulation control, not by the bandwidth
             * list — see the header for upstream's off-by-one here. */
            if (modulation == FmRadioModulation::Usb || modulation == FmRadioModulation::Lsb) {
                receiver_.set_am_configuration(cfg.am_config);
            } else {
                receiver_.set_am_configuration(static_cast<AmConfig>(bw_value));
            }
            break;
        case Mode::NarrowbandFMAudio:
            receiver_.set_nfm_configuration(static_cast<NfmConfig>(bw_value));
            break;
        case Mode::WidebandFMAudio:
            receiver_.set_wfm_configuration(static_cast<WfmConfig>(bw_value));
            break;
        default:
            break;
    }

    /* The host audio output runs at one rate for the whole session; the mode's
     * audio rate is what the resampler in ReceiverModel targets internally.
     * Kept in the config table because it is part of upstream's behaviour and
     * the tests check it. */
    (void)cfg.audio_rate_hz;

    set_dirty();
}

void FmRadioView::on_favourite_clicked(size_t index) {
    if (index >= kFmRadioFavouriteSlots) return;

    if (save_armed_) {
        save_armed_ = false;
        fav_freq_[index] = field_frequency_.value();
        fav_mod_[index] = static_cast<int32_t>(modulation_);
        fav_bw_[index] = radio_bw_;
        update_favourite_texts();
        text_save_help_.set("");
        set_dirty();
        return;
    }

    field_frequency_.set_value(fav_freq_[index]);
    radio_bw_ = fav_bw_[index];
    options_modulation_.set_by_value(fav_mod_[index], false);
    change_mode(static_cast<FmRadioModulation>(fav_mod_[index]));
}

void FmRadioView::update_favourite_texts() {
    for (size_t i = 0; i < buttons_fav_.size(); ++i) {
        buttons_fav_[i].set_text(fmradio_to_nice_freq(fav_freq_[i]));
    }
}

void FmRadioView::update_meters() {
    const float rf_db = receiver_.rf_level_db();
    /* -100..0 dBFS onto 0..255, the same range the Level app uses. */
    const float clamped = (rf_db < -100.0f) ? -100.0f : ((rf_db > 0.0f) ? 0.0f : rf_db);
    rf_meter_.set_value(static_cast<uint8_t>(std::lround((clamped + 100.0f) * 2.55f)));

    if (auto* ao = globals().audio_out) {
        const float peak = ao->take_peak();
        const float p = (peak < 0.0f) ? 0.0f : ((peak > 1.0f) ? 1.0f : peak);
        af_meter_.set_value(static_cast<uint8_t>(std::lround(p * 255.0f)));
    }
}

void FmRadioView::on_show() {
    ui::View::on_show();
    field_frequency_.focus();
    if (!receiver_.running()) receiver_.start();
}

void FmRadioView::on_hide() {
    ui::View::on_hide();
}

void FmRadioView::on_frame_sync() {
    ui::View::on_frame_sync();
    frame_counter_++;
    if ((frame_counter_ % 6) == 0) update_meters();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream main.cpp: app_location_t::RX. bitmap_icon_speaker is the closest
 * stock icon for a listening app. */
const app::Registrar reg_fmradio{{"fmradio", "FM Radio", app::Category::Receive,
                                  ui::Color::green(), &ui::bitmap_icon_speaker,
                                  [] { return std::make_unique<app::FmRadioView>(); }}};
}  // namespace
