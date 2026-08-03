/*
 * mayhem-b200 — settings screens.
 *
 * A port of firmware/application/apps/ui_settings.* — but only the pages that
 * still mean something on a Windows host driving a USRP B200. Upstream's
 * settings app is itself a grid menu of setting pages; this keeps that shape
 * (SettingsMenuView is a BtnGridView, registered as the "settings_ui" app) and
 * ports the four pages a B200 user can actually change:
 *
 *   Radio    — reference frequency correction (ppb) and up/down converter.
 *              Backed by core::pmem::correction_ppb / config_converter*.
 *   Audio    — output volume (0..99) and beep-on-RX-packet.
 *              Backed by core::pmem::headphone_volume / beep_on_packets.
 *   Display  — theme selection, applied through ui::Theme::SetTheme.
 *              Backed by core::pmem::ui_theme_id / menu_color.
 *   Reset    — an action tile that clears every stored setting to its firmware
 *              default (core::pmem::defaults) behind a YES/NO confirmation.
 *
 * Pages that depend on PortaPack hardware a B200 lacks are deliberately absent —
 * backlight timeout, touchscreen calibration/threshold, battery gauge, encoder
 * dial, button speed, SD card, CLKOUT, DC bias, external TCXO, menu-colour RGB,
 * splash, RTC date/time. Each surviving page carries a short "N/A on B200" note
 * for the omitted fields a user would come looking for.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SETTINGS_H__
#define __MB200_UI_SETTINGS_H__

#include "ui.hpp"
#include "ui_btngrid.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include "theme.hpp"

#include <cstdint>
#include <string>

namespace app {

/* Maps a Display-page theme option index onto a ui::Theme::ThemeId, mirroring
 * upstream SetThemeView's `selected < Theme::ThemeId::MAX` guard: the option
 * value IS the ThemeId, so 0 -> DefaultGrey ... 5 -> Dark, and anything outside
 * [0, MAX) is rejected. Returns true and writes `out` only for a valid option.
 *
 * Inline so the tests can exercise it without linking the app translation unit;
 * it is pure logic with no widget or radio dependency. */
inline bool settings_theme_id_from_option(int32_t option, ui::Theme::ThemeId& out) {
    if (option < 0 || option >= ui::Theme::ThemeId::MAX)
        return false;
    out = static_cast<ui::Theme::ThemeId>(option);
    return true;
}

/* --- Radio -------------------------------------------------------------- */

class SetRadioView : public ui::View {
   public:
    SetRadioView();

    std::string title() const override { return "Radio"; }
    void on_show() override;

   private:
    void save();

    ui::Labels labels_{
        {{1 * 8, 1 * 16}, "Reference correction", ui::Color::light_grey()},
        {{1 * 8, 2 * 16}, "(all RX/TX, in ppb):", ui::Color::light_grey()},
        {{9 * 8, 3 * 16}, "ppb", ui::Color::light_grey()},
        {{1 * 8, 5 * 16}, "Up/down converter:", ui::Color::light_grey()},
        {{1 * 8, 7 * 16}, "Dir   LO frequency", ui::Color::light_grey()},
    };

    /* ppb, applied by the radio layer as a tuning offset. */
    ui::NumberField field_ppb_{
        {1 * 8, 3 * 16}, 7, {-100000, 100000}, 100, ' '};

    ui::Checkbox check_converter_{
        {1 * 8, 6 * 16}, 18, "Enable converter"};

    ui::OptionsField opt_converter_mode_{
        {1 * 8, 8 * 16}, 3, {{" + ", 0}, {" - ", 1}}};

    ui::FrequencyField field_converter_freq_{
        {5 * 8, 8 * 16}};

    ui::Labels na_labels_{
        {{1 * 8, 11 * 16}, "N/A on B200 (PortaPack", ui::Color::light_grey()},
        {{1 * 8, 12 * 16}, "Si5351 clock hardware):", ui::Color::light_grey()},
        {{1 * 8, 13 * 16}, "CLKOUT, DC bias voltage,", ui::Color::light_grey()},
        {{1 * 8, 14 * 16}, "external TCXO, ref source.", ui::Color::light_grey()},
    };

    ui::Button button_save_{
        {8, 268, 104, 32}, "Save"};
    ui::Button button_cancel_{
        {128, 268, 104, 32}, "Cancel"};
};

/* --- Audio -------------------------------------------------------------- */

class SetAudioView : public ui::View {
   public:
    SetAudioView();

    std::string title() const override { return "Audio"; }
    void on_show() override;

   private:
    void save();

    ui::Labels labels_{
        {{1 * 8, 1 * 16}, "Audio output volume", ui::Color::light_grey()},
        {{1 * 8, 2 * 16}, "(0-99):", ui::Color::light_grey()},
        {{1 * 8, 6 * 16}, "Beep on speaker/headphone", ui::Color::light_grey()},
        {{1 * 8, 7 * 16}, "when a packet is received", ui::Color::light_grey()},
        {{1 * 8, 8 * 16}, "(apps that support it):", ui::Color::light_grey()},
    };

    ui::NumberField field_volume_{
        {8 * 8, 2 * 16}, 3, {0, 99}, 1, ' '};

    ui::Checkbox checkbox_beep_{
        {1 * 8, 10 * 16}, 18, "Beep on RX packets"};

    ui::Button button_save_{
        {8, 268, 104, 32}, "Save"};
    ui::Button button_cancel_{
        {128, 268, 104, 32}, "Cancel"};
};

/* --- Display ------------------------------------------------------------ */

class SetDisplayView : public ui::View {
   public:
    SetDisplayView();

    std::string title() const override { return "Display"; }
    void on_show() override;

   private:
    void save();

    int32_t selected_{0};

    ui::Labels labels_{
        {{1 * 8, 1 * 16}, "Select a colour theme.", ui::Color::light_grey()},
        {{1 * 8, 2 * 16}, "Restart to fully apply.", ui::Color::light_grey()},
    };

    ui::OptionsField options_theme_{
        {1 * 8, 4 * 16},
        16,
        {{"Default - Grey", ui::Theme::ThemeId::DefaultGrey},
         {"Yellow", ui::Theme::ThemeId::Yellow},
         {"Aqua", ui::Theme::ThemeId::Aqua},
         {"Green", ui::Theme::ThemeId::Green},
         {"Red", ui::Theme::ThemeId::Red},
         {"Dark", ui::Theme::ThemeId::Dark}}};

    ui::Checkbox checkbox_menuset_{
        {1 * 8, 6 * 16}, 20, "Set menu colour too"};

    ui::Labels na_labels_{
        {{1 * 8, 10 * 16}, "N/A on B200 (the host OS", ui::Color::light_grey()},
        {{1 * 8, 11 * 16}, "owns the display):", ui::Color::light_grey()},
        {{1 * 8, 12 * 16}, "brightness, IPS type,", ui::Color::light_grey()},
        {{1 * 8, 13 * 16}, "backlight timeout, touch", ui::Color::light_grey()},
        {{1 * 8, 14 * 16}, "calibration, battery gauge.", ui::Color::light_grey()},
    };

    ui::Button button_save_{
        {8, 268, 104, 32}, "Save"};
    ui::Button button_cancel_{
        {128, 268, 104, 32}, "Cancel"};
};

/* --- Settings menu ------------------------------------------------------ */

class SettingsMenuView : public ui::BtnGridView {
   public:
    SettingsMenuView();

    std::string title() const override { return "Settings"; }

   protected:
    void on_populate() override;
};

}  // namespace app

#endif /*__MB200_UI_SETTINGS_H__*/
