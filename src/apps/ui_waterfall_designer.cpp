/*
 * mayhem-b200 — waterfall gradient designer implementation.
 *
 * Copyright (C) 2025 Belousov Oleg (gradient interpolation)
 * Copyleft Mr. Robot; Copyright HTotoo (designer UI)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_waterfall_designer.hpp"

#include "../core/file_path.hpp"
#include "../core/fs_utils.hpp"
#include "../core/string_format.hpp"
#include "app_context.hpp"
#include "theme.hpp"
#include "ui_alphanum.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <cctype>

namespace app {

using namespace ui;
namespace detail = waterfall_designer_detail;

namespace {

std::string waterfalls_directory() {
    return core::data_directory() + "/WATERFALLS";
}

bool ends_with_txt(const std::string& s) {
    if (s.size() < 4) return false;
    std::string tail = s.substr(s.size() - 4);
    for (auto& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return tail == ".txt";
}

const std::vector<std::string> default_profile{
    "0,0,0,0", "86,0,0,255", "171,0,255,0", "255,255,0,0"};

}  // namespace

/* --- WaterfallColorPickerView ---------------------------------------------- */

WaterfallColorPickerView::WaterfallColorPickerView(std::string color_str) {
    add_children({&labels_, &field_index_, &field_red_, &field_green_,
                  &field_blue_, &field_step_, &button_save_});

    detail::Level l{};
    if (detail::parse_level(color_str, l)) {
        index_ = l.index;
        red_ = l.r;
        green_ = l.g;
        blue_ = l.b;
    }

    field_index_.set_value(index_, false);
    field_red_.set_value(red_, false);
    field_green_.set_value(green_, false);
    field_blue_.set_value(blue_, false);
    field_step_.set_value(1, false);

    field_index_.on_change = [this](int32_t) { update(); };
    field_red_.on_change = [this](int32_t) { update(); };
    field_green_.on_change = [this](int32_t) { update(); };
    field_blue_.on_change = [this](int32_t) { update(); };

    field_step_.on_change = [this](int32_t) {
        const int32_t s = field_step_.value();
        field_index_.set_step(s);
        field_red_.set_step(s);
        field_green_.set_step(s);
        field_blue_.set_step(s);
    };

    button_save_.on_select = [this](Button&) {
        if (on_save) on_save(build_color_str());
        if (auto* nav = globals().nav) nav->pop();
    };
}

void WaterfallColorPickerView::update() {
    index_ = static_cast<uint8_t>(field_index_.value());
    red_ = static_cast<uint8_t>(field_red_.value());
    green_ = static_cast<uint8_t>(field_green_.value());
    blue_ = static_cast<uint8_t>(field_blue_.value());
    set_dirty();
}

std::string WaterfallColorPickerView::build_color_str() const {
    return to_string_dec_uint(index_) + "," + to_string_dec_uint(red_) + "," +
           to_string_dec_uint(green_) + "," + to_string_dec_uint(blue_);
}

void WaterfallColorPickerView::paint(Painter& painter) {
    painter.fill_rectangle(screen_rect(), Theme::getInstance()->bg_darkest->background);
    const Rect swatch{176, 16, 56, 128};
    painter.fill_rectangle(swatch, ui::Color(red_, green_, blue_));
    painter.draw_rectangle(swatch, Theme::getInstance()->fg_light->foreground);
}

void WaterfallColorPickerView::on_show() {
    View::on_show();
    focus();
    set_dirty();
}

void WaterfallColorPickerView::focus() {
    button_save_.focus();
}

/* --- WaterfallProfilePickerView -------------------------------------------- */

WaterfallProfilePickerView::WaterfallProfilePickerView() {
    add_children({&header_, &menu_, &empty_note_, &button_back_});

    directory_ = waterfalls_directory();
    core::ensure_directory(directory_);
    header_.set(".txt in WATERFALLS:");

    std::vector<core::DirEntry> entries;
    core::ListOptions options{".txt"};
    options.include_directories = false;
    core::list_directory(directory_, entries, options);

    if (entries.empty()) {
        empty_note_.set("No .txt profiles in\n" + directory_);
        menu_.hidden(true);
    } else {
        empty_note_.hidden(true);
        for (const auto& e : entries) {
            const std::string full = core::path_join(directory_, e.name);
            menu_.add_item({e.name,
                            Theme::getInstance()->fg_light->foreground,
                            [this, full]() {
                                if (on_pick) on_pick(full);
                                if (auto* nav = globals().nav) nav->pop();
                            }});
        }
    }

    button_back_.on_select = [](Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void WaterfallProfilePickerView::on_show() {
    View::on_show();
    focus();
}

void WaterfallProfilePickerView::focus() {
    if (menu_.item_count() > 0)
        menu_.focus();
    else
        button_back_.focus();
}

/* --- WaterfallDesignerView ------------------------------------------------- */

WaterfallDesignerView::WaterfallDesignerView() {
    add_children({&header_, &menu_, &button_new_, &button_open_, &button_save_,
                  &button_add_, &button_del_, &button_edit_, &text_status_,
                  &text_note1_, &text_note2_, &text_note3_});

    text_note1_.set(STR_COLOR_LIGHT_GREY "Designs the RX waterfall");
    text_note2_.set(STR_COLOR_LIGHT_GREY "palette. Live waterfall needs");
    text_note3_.set(STR_COLOR_LIGHT_GREY "a receiver (n/a on B200).");

    menu_.on_highlight = [this](size_t i) { highlighted_index_ = i; };

    button_new_.on_select = [this](Button&) { on_new_profile(); };
    button_open_.on_select = [this](Button&) { on_open_profile(); };
    button_save_.on_select = [this](Button&) { on_save_profile(); };
    button_add_.on_select = [this](Button&) { on_add_level(); };
    button_del_.on_select = [this](Button&) { on_remove_level(); };
    button_edit_.on_select = [this](Button&) { on_edit_color(); };

    profile_levels_ = default_profile;
    refresh_menu();
    refresh_preview();
    set_status("Default palette (unsaved)");
}

void WaterfallDesignerView::refresh_menu() {
    menu_.clear();

    for (size_t i = 0; i < profile_levels_.size(); i++) {
        const std::string& line = profile_levels_[i];
        detail::Level l{};
        if (detail::parse_level(line, l)) {
            menu_.add_item({line, ui::Color(l.r, l.g, l.b), [this, i]() {
                                highlighted_index_ = i;
                                on_edit_color();
                            }});
        } else {
            /* Comments/headers show greyed and are not editable colours. */
            menu_.add_item({line, ui::Color::grey(), [this, i]() {
                                highlighted_index_ = i;
                            }});
        }
    }

    if (highlighted_index_ >= profile_levels_.size())
        highlighted_index_ = profile_levels_.empty() ? 0 : profile_levels_.size() - 1;
    if (!profile_levels_.empty())
        menu_.set_highlighted(highlighted_index_);

    set_dirty();
}

void WaterfallDesignerView::refresh_preview() {
    preview_.load_levels(profile_levels_);
    set_dirty();
}

void WaterfallDesignerView::on_new_profile() {
    profile_levels_ = default_profile;
    current_profile_path_.clear();
    highlighted_index_ = 0;
    refresh_menu();
    refresh_preview();
    set_status("New default (unsaved)");
}

void WaterfallDesignerView::on_open_profile() {
    auto picker = std::make_unique<WaterfallProfilePickerView>();
    picker->on_pick = [this](const std::string& path) {
        std::vector<std::string> lines;
        if (!core::read_lines(path, lines)) {
            set_status(STR_COLOR_RED "Open failed");
            return;
        }

        profile_levels_.clear();
        for (auto& line : lines) {
            /* read_lines already strips CRLF; drop blank lines like upstream. */
            std::string entry = trim(line);
            if (!entry.empty()) profile_levels_.push_back(std::move(entry));
        }

        current_profile_path_ = path;
        highlighted_index_ = 0;
        refresh_menu();
        refresh_preview();
        set_status(STR_COLOR_GREEN "Loaded " + core::filename(path));
    };
    if (auto* nav = globals().nav) nav->push(std::move(picker));
}

void WaterfallDesignerView::on_save_profile() {
    if (profile_levels_.empty()) {
        set_status(STR_COLOR_RED "List is empty");
        return;
    }

    if (!current_profile_path_.empty()) {
        save_to(current_profile_path_);
        return;
    }

    auto* nav = globals().nav;
    if (nav == nullptr) return;

    filename_buffer_.clear();
    text_prompt(*nav, filename_buffer_, 32u, ENTER_KEYBOARD_MODE_ALPHA,
                [this](std::string& buffer) {
                    if (buffer.empty()) return;
                    std::string name = buffer;
                    if (!ends_with_txt(name)) name += ".txt";
                    const std::string dir = waterfalls_directory();
                    core::ensure_directory(dir);
                    save_to(core::path_join(dir, name));
                });
}

void WaterfallDesignerView::save_to(const std::string& path) {
    std::string content;
    for (const auto& line : profile_levels_) {
        content += line;
        content += '\n';
    }

    const auto result = core::write_file(path, content);
    if (result) {
        current_profile_path_ = path;
        set_status(STR_COLOR_GREEN "Saved " + core::filename(path));
    } else {
        set_status(STR_COLOR_RED "Write failed");
    }
}

void WaterfallDesignerView::on_add_level() {
    size_t insert_pos = std::min(highlighted_index_, profile_levels_.size());

    /* Don't insert a colour above a comment/header row; put it after instead. */
    if (insert_pos < profile_levels_.size() &&
        !detail::is_color_level(profile_levels_[insert_pos]))
        insert_pos++;

    profile_levels_.insert(profile_levels_.begin() + insert_pos, "128,128,128,128");
    highlighted_index_ = insert_pos;

    refresh_menu();
    refresh_preview();
    on_edit_color();
}

void WaterfallDesignerView::on_remove_level() {
    if (highlighted_index_ >= profile_levels_.size()) return;
    if (!detail::is_color_level(profile_levels_[highlighted_index_])) return;

    profile_levels_.erase(profile_levels_.begin() + highlighted_index_);
    refresh_menu();
    refresh_preview();
    set_status("Level removed");
}

void WaterfallDesignerView::on_edit_color() {
    if (highlighted_index_ >= profile_levels_.size()) return;
    if (!detail::is_color_level(profile_levels_[highlighted_index_])) return;

    const size_t index = highlighted_index_;
    auto picker = std::make_unique<WaterfallColorPickerView>(profile_levels_[index]);
    picker->on_save = [this, index](const std::string& new_color) {
        if (index >= profile_levels_.size()) return;
        profile_levels_[index] = new_color;
        refresh_menu();
        refresh_preview();
    };
    if (auto* nav = globals().nav) nav->push(std::move(picker));
}

void WaterfallDesignerView::set_status(const std::string& s) {
    text_status_.set(s);
    set_dirty();
}

void WaterfallDesignerView::paint(Painter& painter) {
    painter.fill_rectangle(screen_rect(), Theme::getInstance()->bg_darkest->background);

    /* Live preview of the interpolated 256-colour gradient. */
    for (int x = 0; x < 240; x++) {
        int idx = (x * 256) / 240;
        if (idx > 255) idx = 255;
        painter.draw_vline({x, preview_top}, preview_height, preview_.lut[idx]);
    }
    painter.draw_rectangle({0, preview_top, 240, preview_height},
                           Theme::getInstance()->fg_light->foreground);
}

void WaterfallDesignerView::on_show() {
    View::on_show();
    if (menu_.item_count() > 0)
        menu_.focus();
    else
        button_new_.focus();
    set_dirty();
}

void WaterfallDesignerView::focus() {
    if (menu_.item_count() > 0)
        menu_.focus();
    else
        button_new_.focus();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_waterfall_designer{{
    "waterfall_designer",
    "Wtf Design",
    app::Category::Utilities,
    ui::Color::cyan(),
    &ui::bitmap_icon_notepad,
    [] { return std::make_unique<app::WaterfallDesignerView>(); },
}};
}  // namespace
