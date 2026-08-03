/*
 * mayhem-b200 — Hard Reset.
 *
 * Copyright (C) 2026 Pezsma (original design)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_hard_reset.hpp"

#include "app_context.hpp"
#include "settings.hpp"
#include "string_format.hpp"
#include "theme.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"

namespace app {

HardResetView::HardResetView() {
    add_children({
        &labels_,
        &check_reset_settings_,
        &check_delete_inis_,
        &text_ini_count_,
        &na_labels_,
        &text_status_,
        &button_reset_,
        &button_cancel_,
    });

    text_ini_count_.set_style(ui::Theme::getInstance()->fg_magenta);
    text_status_.set_style(ui::Theme::getInstance()->fg_green);

    /* Match upstream's default: reset the settings, and only pre-check the
     * .ini deletion when there is actually something to delete. The counts fill
     * these in on_show(). */
    check_reset_settings_.set_value(true);
    check_delete_inis_.set_value(false);

    button_reset_.on_select = [this](ui::Button&) {
        /* The confirmation upstream gates the erase behind. */
        ui::display_modal(
            "Reset settings?",
            "Checked items are reset to\ndefaults. This cannot be\nundone. Continue?",
            ui::YESNO,
            [this](bool choice) {
                if (choice) do_reset();
            });
    };

    button_cancel_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void HardResetView::on_show() {
    View::on_show();
    refresh_counts();
    button_cancel_.focus();
}

void HardResetView::refresh_counts() {
    ini_count_ = hard_reset_count_ini_files(core::settings_directory());
    text_ini_count_.set("(" + to_string_dec_uint(ini_count_) + " .ini in SETTINGS)");

    /* Follow upstream: only pre-check the deletion when it would do something. */
    check_delete_inis_.set_value(ini_count_ > 0);
}

void HardResetView::do_reset() {
    std::string result;

    if (check_reset_settings_.value()) {
        const bool ok = hard_reset_clear_settings(core::settings());
        result += ok ? "Settings cleared. " : "Settings clear FAILED. ";
    }

    if (check_delete_inis_.value()) {
        const uint16_t removed = hard_reset_delete_ini_files(core::settings_directory());
        result += "Deleted " + to_string_dec_uint(removed) + " .ini. ";
    }

    if (result.empty()) result = "Nothing selected.";

    text_status_.set(result);
    refresh_counts();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_hard_reset{{
    "hard_reset",
    "Hard Reset",
    app::Category::Settings,
    ui::Color::red(),  /* upstream icon_color = red */
    &ui::bitmap_icon_setup,
    [] { return std::make_unique<app::HardResetView>(); },
    false  /* genuinely functional on the host */
}};
}  // namespace
