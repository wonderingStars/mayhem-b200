/*
 * mayhem-b200 — App Manager.
 *
 * Copyright (C) 2024 zxkmm (original design)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_app_manager.hpp"

#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

namespace app {

AppManagerView::AppManagerView() {
    add_children({
        &labels_,
        &menu_view_,
        &text_info_,
        &na_labels_,
        &button_back_,
    });

    menu_view_.on_highlight = [this](size_t index) { update_info(index); };

    button_back_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };

    populate();
}

void AppManagerView::populate() {
    rows_ = app_manager_rows();

    menu_view_.clear();
    for (size_t i = 0; i < rows_.size(); i++) {
        const auto& r = rows_[i];
        /* A trailing marker so a hardware-limited app is obvious in the list
         * without having to highlight it. */
        const std::string label =
            r.display_name + (r.hardware_limited ? " *" : "");
        menu_view_.add_item({label, r.color, [] {}});
    }
}

void AppManagerView::update_info(size_t index) {
    if (index >= rows_.size()) {
        text_info_.set("");
        return;
    }
    const auto& r = rows_[index];
    std::string info = r.id + "  " + category_name(r.category);
    if (r.hardware_limited) info += " *N/A";
    text_info_.set(info);
}

void AppManagerView::on_show() {
    View::on_show();
    menu_view_.focus();
    update_info(menu_view_.highlighted_index());
}

}  // namespace app

#include "bitmaps.hpp"

namespace {
const app::Registrar reg_app_manager{{
    "app_manager",
    "AppManager",
    app::Category::Settings,
    ui::Color::cyan(),  /* upstream icon_color = cyan */
    &ui::bitmap_icon_utilities,
    [] { return std::make_unique<app::AppManagerView>(); },
    true  /* SD-card app management does not apply on a host */
}};
}  // namespace
