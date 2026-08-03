/*
 * mayhem-b200 — settings screens.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_settings.hpp"

#include "app_context.hpp"
#include "app_registry.hpp"
#include "bitmaps.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"

#include "../audio/audio_out.hpp"
#include "../radio/receiver_model.hpp"

#include <cstdint>
#include <cstdlib>

namespace app {

/* --- Radio -------------------------------------------------------------- */

SetRadioView::SetRadioView() {
    add_children({
        &labels_,
        &field_ppb_,
        &check_converter_,
        &opt_converter_mode_,
        &field_converter_freq_,
        &na_labels_,
        &button_save_,
        &button_cancel_,
    });

    field_ppb_.set_value(core::pmem::correction_ppb());

    check_converter_.set_value(core::pmem::config_converter());
    opt_converter_mode_.set_by_value(core::pmem::config_updown_converter() ? 1 : 0);

    /* config_converter_freq is stored signed for compatibility with the
     * settings store; the up/down direction lives in its own flag, so the field
     * only ever shows the magnitude. */
    const int64_t stored_freq = core::pmem::config_converter_freq();
    const uint64_t magnitude =
        (stored_freq < 0) ? static_cast<uint64_t>(-stored_freq)
                          : static_cast<uint64_t>(stored_freq);
    field_converter_freq_.set_value(magnitude, false);

    button_save_.on_select = [this](ui::Button&) { save(); };
    button_cancel_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void SetRadioView::on_show() {
    View::on_show();
    button_save_.focus();
}

void SetRadioView::save() {
    core::pmem::set_correction_ppb(field_ppb_.value());
    core::pmem::set_config_converter(check_converter_.value());
    core::pmem::set_config_updown_converter(opt_converter_mode_.selected_index_value() != 0);
    core::pmem::set_config_converter_freq(static_cast<int64_t>(field_converter_freq_.value()));
    core::pmem::save();
    if (auto* nav = globals().nav) nav->pop();
}

/* --- Audio -------------------------------------------------------------- */

SetAudioView::SetAudioView() {
    add_children({
        &labels_,
        &field_volume_,
        &checkbox_beep_,
        &button_save_,
        &button_cancel_,
    });

    field_volume_.set_value(core::pmem::headphone_volume());
    checkbox_beep_.set_value(core::pmem::beep_on_packets());

    button_save_.on_select = [this](ui::Button&) { save(); };
    button_cancel_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void SetAudioView::on_show() {
    View::on_show();
    button_save_.focus();
}

void SetAudioView::save() {
    const auto volume = static_cast<uint8_t>(field_volume_.value());
    core::pmem::set_headphone_volume(volume);
    core::pmem::set_beep_on_packets(checkbox_beep_.value());
    core::pmem::save();

    /* Push the new volume onto the live audio path so it takes effect at once,
     * not only after the next app relaunch. */
    if (auto* out = globals().audio_out) out->set_volume(volume);
    if (auto* rx = globals().receiver) rx->set_volume(volume);

    if (auto* nav = globals().nav) nav->pop();
}

/* --- Display ------------------------------------------------------------ */

SetDisplayView::SetDisplayView() {
    add_children({
        &labels_,
        &options_theme_,
        &checkbox_menuset_,
        &na_labels_,
        &button_save_,
        &button_cancel_,
    });

    checkbox_menuset_.set_value(true);

    options_theme_.on_change = [this](size_t, ui::OptionsField::value_t v) {
        selected_ = v;
    };
    selected_ = core::pmem::ui_theme_id();
    options_theme_.set_by_value(selected_);

    button_save_.on_select = [this](ui::Button&) { save(); };
    button_cancel_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void SetDisplayView::on_show() {
    View::on_show();
    options_theme_.focus();
}

void SetDisplayView::save() {
    ui::Theme::ThemeId theme_id{};
    if (settings_theme_id_from_option(selected_, theme_id) &&
        static_cast<uint8_t>(selected_) != core::pmem::ui_theme_id()) {
        core::pmem::set_ui_theme_id(static_cast<uint8_t>(selected_));
        ui::Theme::SetTheme(theme_id);
        if (checkbox_menuset_.value()) {
            /* .v is the RGB565 value the menu-colour field stores. */
            core::pmem::set_menu_color(ui::Theme::getInstance()->bg_medium->background.v);
        }
    }
    core::pmem::save();
    set_dirty();
    if (auto* nav = globals().nav) nav->pop();
}

/* --- Settings menu ------------------------------------------------------ */

SettingsMenuView::SettingsMenuView() {
    set_max_rows(2);  // wider tiles, as upstream's settings grid uses
}

void SettingsMenuView::on_populate() {
    auto* nav = globals().nav;

    add_items({
        {"Radio", ui::Color::dark_cyan(), &ui::bitmap_icon_peripherals_details,
         [nav] {
             if (nav) nav->push(std::make_unique<SetRadioView>());
         }},
        {"Audio", ui::Color::dark_cyan(), &ui::bitmap_icon_speaker,
         [nav] {
             if (nav) nav->push(std::make_unique<SetAudioView>());
         }},
        {"Display", ui::Color::dark_cyan(), &ui::bitmap_icon_setup,
         [nav] {
             if (nav) nav->push(std::make_unique<SetDisplayView>());
         }},
        {"Reset Settings", ui::Color::red(), &ui::bitmap_icon_setup,
         [] {
             ui::display_modal(
                 "Reset settings?",
                 "This resets EVERY setting\nto its default value.",
                 ui::YESNO,
                 [](bool choice) {
                     if (!choice) return;
                     core::pmem::defaults();
                     core::pmem::save();
                     /* defaults() clears ui_theme_id back to 0 (DefaultGrey);
                      * apply it immediately so the screen matches the store. */
                     ui::Theme::SetTheme(ui::Theme::ThemeId::DefaultGrey);
                 });
         }},
    });
}

}  // namespace app

namespace {
const app::Registrar reg_settings{{"settings_ui", "Settings", app::Category::Settings,
                                   ui::Color::dark_cyan(), &ui::bitmap_icon_setup,
                                   [] { return std::make_unique<app::SettingsMenuView>(); }}};
}  // namespace
