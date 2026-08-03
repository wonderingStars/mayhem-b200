/*
 * mayhem-b200 — signal level meter.
 *
 * Copyright (C) 2023 gullradriel, Nilorea Studio Inc. (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_level.hpp"

#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"

#include <cmath>

namespace app {

using Mode = radio::ReceiverModel::Mode;

LevelView::LevelView()
    : receiver_{*globals().receiver} {
    add_children({&labels_,
                  &field_frequency_,
                  &step_view_,
                  &options_mode_,
                  &field_gain_,
                  &field_volume_,
                  &text_rf_,
                  &rf_meter_,
                  &text_ch_,
                  &ch_meter_,
                  &text_sql_,
                  &na_notes_});

    /* Ranges come from the device, not constants: a B210 reports a different
     * gain maximum than a B200mini. */
    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    field_frequency_.set_value(receiver_.target_frequency(), false);
    field_frequency_.on_change = [this](uint64_t hz) { receiver_.set_target_frequency(hz); };

    options_mode_.set_by_value(static_cast<int32_t>(receiver_.mode()), false);
    options_mode_.on_change = [this](size_t, int32_t value) {
        receiver_.set_mode(static_cast<Mode>(value));
    };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    field_volume_.set_value(receiver_.volume(), false);
    field_volume_.on_change = [this](int32_t v) {
        receiver_.set_volume(static_cast<uint8_t>(v));
    };
}

LevelView::~LevelView() {
    /* Leave the stream running, as the other RX apps do — returning to the app
     * feels instant that way, and the navigation layer handles audio. */
}

void LevelView::on_show() {
    View::on_show();
    field_frequency_.focus();
    if (!receiver_.running()) receiver_.start();
}

void LevelView::on_hide() {
    View::on_hide();
}

void LevelView::update_readouts() {
    const float rf_db = receiver_.rf_level_db();
    const float ch_db = receiver_.channel_level_db();

    text_rf_.set("RF   " + to_string_decimal(rf_db, 1) + " dBFS");
    text_ch_.set("CH   " + to_string_decimal(ch_db, 1) + " dBFS");

    rf_meter_.set_value(level_db_to_bar255(rf_db, kLevelFloorDb, kLevelCeilDb));
    ch_meter_.set_value(level_db_to_bar255(ch_db, kLevelFloorDb, kLevelCeilDb));

    text_sql_.set(std::string{"Squelch: "} +
                  (receiver_.squelch_open() ? STR_COLOR_GREEN "OPEN"
                                            : STR_COLOR_DARK_GREY "MUTE"));
}

void LevelView::on_frame_sync() {
    View::on_frame_sync();

    frame_counter_++;
    /* The level readouts change slowly; ~6 Hz at a 60 Hz UI loop is plenty and
     * avoids burning redraws. */
    if ((frame_counter_ % 10) == 0) update_readouts();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* No bitmap in bitmaps.hpp depicts a signal-strength bar meter, so per the
 * porting contract this uses a generic tile (nullptr) rather than a misleading
 * borrowed icon. */
const app::Registrar reg_level{{"level", "Level", app::Category::Receive,
                                ui::Color::green(), nullptr,
                                [] { return std::make_unique<app::LevelView>(); }}};
}  // namespace
