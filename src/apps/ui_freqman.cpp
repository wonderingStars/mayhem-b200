/*
 * mayhem-b200 — the Frequency Manager UI (implementation).
 *
 * See ui_freqman.hpp for the port notes. The parsing, serialisation and file
 * format all live in core/freqman_db.*; this file is only the views.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2023 Kyle Reed
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_freqman.hpp"

#include "app_context.hpp"
#include "receiver_model.hpp"
#include "string_format.hpp"
#include "theme.hpp"
#include "ui_alphanum.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace app {

/* ==== FreqmanEntryList ================================================== */

FreqmanEntryList::FreqmanEntryList(ui::Rect parent_rect)
    : ui::Widget{parent_rect} {
    set_focusable(true);
    const int w = parent_rect.width();
    const int h = parent_rect.height();
    line_max_length_ = static_cast<uint8_t>((w - 8) / char_width);
    visible_lines_ = h >= char_height ? static_cast<size_t>(h / char_height) : 1;
}

void FreqmanEntryList::set_parent_rect(const ui::Rect new_parent_rect) {
    line_max_length_ = static_cast<uint8_t>((new_parent_rect.width() - 8) / char_width);
    visible_lines_ = new_parent_rect.height() >= char_height
                         ? static_cast<size_t>(new_parent_rect.height() / char_height)
                         : 1;
    ui::Widget::set_parent_rect(new_parent_rect);
}

size_t FreqmanEntryList::entry_count() const {
    return db_ ? db_->entry_count() : 0;
}

void FreqmanEntryList::set_db(core::FreqmanDB& db) {
    db_ = &db;
    start_index_ = 0;
    selected_index_ = 0;
    set_dirty();
}

void FreqmanEntryList::set_index(size_t index) {
    start_index_ = 0;
    selected_index_ = 0;
    adjust_selected_index(static_cast<int>(index));
}

size_t FreqmanEntryList::get_index() const {
    return start_index_ + selected_index_;
}

void FreqmanEntryList::on_focus() {
    set_dirty();
}

void FreqmanEntryList::on_blur() {
    set_dirty();
}

void FreqmanEntryList::adjust_selected_index(int delta) {
    const int32_t count = static_cast<int32_t>(entry_count());
    const int32_t new_index = static_cast<int32_t>(selected_index_) + delta;

    if (new_index < 0) {
        start_index_ = static_cast<size_t>(
            std::max<int32_t>(static_cast<int32_t>(start_index_) + new_index, 0));
        selected_index_ = 0;
    } else if (new_index >= static_cast<int32_t>(visible_lines_)) {
        start_index_ = static_cast<size_t>(std::min<int32_t>(
            static_cast<int32_t>(start_index_) + delta, count - static_cast<int32_t>(visible_lines_)));
        selected_index_ = visible_lines_ - 1;
    } else {
        selected_index_ = static_cast<size_t>(std::min<int32_t>(new_index, count - 1));
    }
}

bool FreqmanEntryList::on_key(const ui::KeyEvent key) {
    if (entry_count() == 0)
        return false;

    if (key == ui::KeyEvent::Select) {
        if (on_select) {
            on_select(get_index());
            return true;
        }
        return false;
    }

    if (key == ui::KeyEvent::Right) {
        if (on_leave) {
            on_leave();
            return true;
        }
        return false;
    }

    int delta = 0;
    if (key == ui::KeyEvent::Up && get_index() > 0)
        delta = -1;
    else if (key == ui::KeyEvent::Down && get_index() + 1 < entry_count())
        delta = 1;
    else
        return false;

    adjust_selected_index(delta);
    set_dirty();
    return true;
}

bool FreqmanEntryList::on_encoder(const ui::EncoderEvent delta) {
    if (entry_count() == 0)
        return false;
    adjust_selected_index(delta);
    set_dirty();
    return true;
}

void FreqmanEntryList::paint(ui::Painter& painter) {
    const auto rect = screen_rect();
    auto* theme = ui::Theme::getInstance();

    if (entry_count() == 0) {
        painter.fill_rectangle(rect, theme->bg_darkest->background);
        painter.draw_string(rect.location() + ui::Point{5 * 8, 5 * 16},
                            *theme->bg_darkest, "Empty list");
        painter.draw_rectangle(
            rect, has_focus() ? theme->bg_darkest->foreground : theme->bg_darkest->background);
        return;
    }

    /* Warn (in yellow) when the file is larger than the firmware would load. */
    const bool over_max = entry_count() > core::freqman_default_max_entries;
    const ui::Style* base_style = over_max ? theme->fg_yellow : theme->bg_darkest;

    for (size_t offset = 0; offset < visible_lines_; ++offset) {
        std::string text{};
        const size_t index = start_index_ + offset;
        const ui::Point line_position =
            rect.location() + ui::Point{4, 1 + static_cast<int>(offset) * char_height};
        const bool is_selected = offset == selected_index_;
        const ui::Style* style = base_style;

        if (index < entry_count()) {
            const auto entry = (*db_)[static_cast<core::FreqmanDB::Index>(index)];
            if (entry.type != core::freqman_type::Unknown)
                text = core::pretty_string(entry, line_max_length_);
            if (entry.type == core::freqman_type::Raw)
                style = theme->fg_light;
        }

        /* Pad every slot to full width so a shorter (or empty) line clears the
         * one that was there before, without a separate fill. */
        if (text.length() < line_max_length_)
            text.resize(line_max_length_, ' ');

        const ui::Style s = is_selected ? style->invert() : *style;
        painter.draw_string(line_position, s, text);
    }

    painter.draw_rectangle(
        rect, has_focus() ? theme->bg_darkest->foreground : theme->bg_darkest->background);
}

/* ==== FrequencyEditView ================================================= */

FrequencyEditView::FrequencyEditView(core::freqman_entry entry)
    : entry_{std::move(entry)} {
    nav_ = app::globals().nav;

    add_children({&labels,
                  &field_type,
                  &field_freq_a,
                  &field_freq_b,
                  &field_modulation,
                  &field_bandwidth,
                  &field_step,
                  &field_tone,
                  &field_description,
                  &text_validation,
                  &button_save,
                  &button_cancel});

    /* Populate the option tables, then set the initial selection without firing
     * the change handlers (which are wired afterwards). */
    populate_modulation_options();
    populate_step_options();
    populate_tone_options();

    field_type.set_by_value(static_cast<int32_t>(entry_.type), false);
    field_freq_a.set_value(static_cast<uint64_t>(entry_.frequency_a), false);
    field_freq_b.set_value(static_cast<uint64_t>(entry_.frequency_b), false);

    field_modulation.set_by_value(
        core::is_valid(entry_.modulation) ? static_cast<int32_t>(entry_.modulation) : -1, false);
    populate_bandwidth_options();
    field_bandwidth.set_by_value(
        core::is_valid(entry_.bandwidth) ? static_cast<int32_t>(entry_.bandwidth) : -1, false);
    field_step.set_by_value(
        core::is_valid(entry_.step) ? static_cast<int32_t>(entry_.step) : -1, false);
    field_tone.set_by_value(
        core::is_valid(entry_.tone) ? static_cast<int32_t>(entry_.tone) : -1, false);

    temp_desc_ = entry_.description;
    field_description.set_text(entry_.description);

    /* Change handlers. */
    field_type.on_change = [this](size_t, ui::OptionsField::value_t v) {
        entry_.type = static_cast<core::freqman_type>(v);
        refresh_ui();
    };

    field_freq_a.on_change = [this](uint64_t f) {
        entry_.frequency_a = static_cast<int64_t>(f);
        refresh_ui();
    };

    field_freq_b.on_change = [this](uint64_t f) {
        entry_.frequency_b = static_cast<int64_t>(f);
        refresh_ui();
    };

    field_modulation.on_change = [this](size_t, ui::OptionsField::value_t v) {
        entry_.modulation = (v < 0) ? core::freqman_invalid_index
                                    : static_cast<core::freqman_index_t>(v);
        /* Bandwidth is defined per-modulation, so it resets when the modulation
         * changes. */
        entry_.bandwidth = core::freqman_invalid_index;
        populate_bandwidth_options();
        field_bandwidth.set_by_value(-1, false);
        refresh_ui();
    };

    field_bandwidth.on_change = [this](size_t, ui::OptionsField::value_t v) {
        entry_.bandwidth = (v < 0) ? core::freqman_invalid_index
                                   : static_cast<core::freqman_index_t>(v);
    };

    field_step.on_change = [this](size_t, ui::OptionsField::value_t v) {
        entry_.step = (v < 0) ? core::freqman_invalid_index
                              : static_cast<core::freqman_index_t>(v);
    };

    field_tone.on_change = [this](size_t, ui::OptionsField::value_t v) {
        entry_.tone = (v < 0) ? core::freqman_invalid_index
                              : static_cast<core::freqman_index_t>(v);
    };

    field_description.on_select = [this](ui::TextField&) {
        if (!nav_)
            return;
        temp_desc_ = entry_.description;
        ui::text_prompt(
            *nav_, temp_desc_, core::freqman_max_desc_size, ENTER_KEYBOARD_MODE_ALPHA,
            [this](std::string& s) {
                entry_.description = s;
                field_description.set_text(s);
            });
    };

    button_save.on_select = [this](ui::Button&) {
        entry_.description = temp_desc_;
        if (on_save)
            on_save(entry_);
        if (nav_)
            nav_->pop();
    };

    button_cancel.on_select = [this](ui::Button&) {
        if (nav_)
            nav_->pop();
    };

    refresh_ui();
}

void FrequencyEditView::on_show() {
    ui::View::on_show();
    focus();
}

void FrequencyEditView::focus() {
    field_type.focus();
}

void FrequencyEditView::populate_modulation_options() {
    ui::OptionsField::options_t opts;
    opts.push_back({"None", -1});
    for (size_t i = 0; i < core::freqman_modulation_count; ++i)
        opts.push_back({core::freqman_entry_get_modulation_string(static_cast<core::freqman_index_t>(i)),
                        static_cast<ui::OptionsField::value_t>(i)});
    field_modulation.set_options(std::move(opts));
}

void FrequencyEditView::populate_bandwidth_options() {
    ui::OptionsField::options_t opts;
    opts.push_back({"None", -1});
    if (core::is_valid(entry_.modulation)) {
        const size_t n = core::freqman_bandwidth_count(entry_.modulation);
        for (size_t i = 0; i < n; ++i)
            opts.push_back(
                {core::freqman_entry_get_bandwidth_string(entry_.modulation,
                                                          static_cast<core::freqman_index_t>(i)),
                 static_cast<ui::OptionsField::value_t>(i)});
    }
    field_bandwidth.set_options(std::move(opts));
}

void FrequencyEditView::populate_step_options() {
    ui::OptionsField::options_t opts;
    opts.push_back({"None", -1});
    const size_t n = core::freqman_step_count();
    for (size_t i = 0; i < n; ++i)
        opts.push_back({core::freqman_entry_get_step_string_short(static_cast<core::freqman_index_t>(i)),
                        static_cast<ui::OptionsField::value_t>(i)});
    field_step.set_options(std::move(opts));
}

void FrequencyEditView::populate_tone_options() {
    ui::OptionsField::options_t opts;
    opts.push_back({"None", -1});
    const size_t n = core::freqman_tone_key_count();
    for (size_t i = 0; i < n; ++i)
        opts.push_back({core::freqman_tone_key_value_string(static_cast<core::freqman_index_t>(i)),
                        static_cast<ui::OptionsField::value_t>(i)});
    field_tone.set_options(std::move(opts));
}

void FrequencyEditView::refresh_ui() {
    auto* theme = ui::Theme::getInstance();

    const bool is_range = entry_.type == core::freqman_type::Range;
    const bool is_ham = entry_.type == core::freqman_type::HamRadio;
    const bool is_repeater = entry_.type == core::freqman_type::Repeater;
    const bool has_freq_b = is_range || is_ham || is_repeater;

    /* Grey out the fields that do not apply to the current entry type. */
    field_freq_b.set_style(has_freq_b ? theme->bg_darkest : theme->fg_medium);
    field_step.set_style(is_range ? theme->bg_darkest : theme->fg_medium);
    field_tone.set_style(is_ham ? theme->bg_darkest : theme->fg_medium);
    field_freq_b.set_dirty();
    field_step.set_dirty();
    field_tone.set_dirty();

    if (core::is_valid(entry_)) {
        text_validation.set("Valid");
        text_validation.set_style(theme->fg_green);
    } else {
        text_validation.set("Error");
        text_validation.set_style(theme->fg_red);
    }
    text_validation.set_dirty();
}

/* ==== FrequencyManagerView ============================================== */

size_t FrequencyManagerView::current_category_index_ = 0;

FrequencyManagerView::FrequencyManagerView() {
    nav_ = app::globals().nav;

    add_children({&label_category,
                  &options_category,
                  &list_view,
                  &button_edit,
                  &button_add,
                  &button_del,
                  &button_up,
                  &button_down,
                  &button_tune,
                  &button_new_list,
                  &button_del_list,
                  &button_exit});

    options_category.on_change = [this](size_t index, ui::OptionsField::value_t) {
        change_category(index);
    };

    /* Select moves focus into the action area; Right steers out of the list. */
    list_view.on_select = [this](size_t) { button_edit.focus(); };
    list_view.on_leave = [this]() { button_edit.focus(); };

    button_edit.on_select = [this](ui::Button&) { on_edit_entry(); };
    button_add.on_select = [this](ui::Button&) { on_add_entry(); };
    button_del.on_select = [this](ui::Button&) { on_del_entry(); };
    button_up.on_select = [this](ui::Button&) { on_move_entry(-1); };
    button_down.on_select = [this](ui::Button&) { on_move_entry(1); };
    button_tune.on_select = [this](ui::Button&) { on_tune_entry(); };
    button_new_list.on_select = [this](ui::Button&) { on_add_category(); };
    button_del_list.on_select = [this](ui::Button&) { on_del_category(); };
    button_exit.on_select = [this](ui::Button&) {
        if (nav_)
            nav_->pop();
    };

    refresh_categories();
}

void FrequencyManagerView::on_show() {
    ui::View::on_show();
    focus();
}

void FrequencyManagerView::focus() {
    if (options_category.options().empty()) {
        if (!warned_no_lists_) {
            warned_no_lists_ = true;
            if (nav_)
                ui::display_modal(*nav_, "Freqman",
                                  "No lists in FREQMAN.\nUse New Lst to create one.");
        }
        button_new_list.focus();
        return;
    }

    if (db_.is_open() && !db_.empty())
        list_view.focus();
    else
        options_category.focus();
}

void FrequencyManagerView::refresh_categories(const std::string& select_stem) {
    const auto files = core::get_freqman_files();  // sorted stems

    ui::OptionsField::options_t opts;
    opts.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i)
        opts.push_back({files[i], static_cast<ui::OptionsField::value_t>(i)});

    /* Work out which category to land on: an explicitly requested stem, else the
     * persisted index clamped into range. */
    size_t sel = files.empty() ? 0 : std::min(current_category_index_, files.size() - 1);
    if (!select_stem.empty()) {
        const auto it = std::find(files.begin(), files.end(), select_stem);
        if (it != files.end())
            sel = static_cast<size_t>(it - files.begin());
    }

    options_category.set_options(std::move(opts));

    if (!files.empty()) {
        options_category.set_selected_index(sel, false);
        change_category(sel);
    } else {
        db_.close();
        list_view.set_db(db_);
        list_view.set_dirty();
    }
}

void FrequencyManagerView::change_category(size_t new_index) {
    if (options_category.options().empty())
        return;

    current_category_index_ = new_index;
    options_category.set_selected_index(new_index, false);

    const std::string stem = current_category();
    if (stem.empty())
        return;

    db_.close();
    db_.open_list(stem, /*create=*/false);
    list_view.set_db(db_);
    list_view.set_dirty();
}

std::string FrequencyManagerView::current_category() const {
    if (options_category.options().empty())
        return {};
    return options_category.selected_index_name();
}

size_t FrequencyManagerView::current_index() const {
    return list_view.get_index();
}

core::freqman_entry FrequencyManagerView::current_entry() const {
    return db_[static_cast<core::FreqmanDB::Index>(current_index())];
}

void FrequencyManagerView::refresh_list(int delta_selected) {
    long ni = static_cast<long>(list_view.get_index()) + delta_selected;
    if (ni < 0)
        ni = 0;
    list_view.set_index(static_cast<size_t>(ni));
    list_view.set_dirty();
}

void FrequencyManagerView::on_add_category() {
    if (!nav_)
        return;
    temp_buffer_.clear();
    ui::text_prompt(
        *nav_, temp_buffer_, 20, ENTER_KEYBOARD_MODE_ALPHA, [this](std::string& name) {
            const std::string stem = trim(name);
            if (stem.empty())
                return;
            const auto files = core::get_freqman_files();
            const bool exists = std::find(files.begin(), files.end(), stem) != files.end();
            if (!exists)
                core::create_freqman_file(stem);  // don't clobber an existing list
            refresh_categories(stem);
        });
}

void FrequencyManagerView::on_del_category() {
    if (!nav_)
        return;
    const std::string stem = current_category();
    if (stem.empty())
        return;

    ui::display_modal(
        *nav_, "Delete list", "Delete " + stem + "?\nAre you sure?", ui::YESNO,
        [this, stem](bool choice) {
            if (!choice)
                return;
            db_.close();
            core::delete_freqman_file(stem);
            refresh_categories();
        });
}

void FrequencyManagerView::on_add_entry() {
    if (!db_.is_open()) {
        if (nav_)
            ui::display_modal(*nav_, "Freqman", "Create or open a list first.");
        return;
    }

    core::freqman_entry entry{};
    entry.type = core::freqman_type::Single;
    entry.frequency_a = 100'000'000;
    entry.description = std::string{"Entry "} + to_string_dec_uint(db_.entry_count());

    /* Insert below the current selection, as upstream does. */
    db_.insert_entry(static_cast<core::FreqmanDB::Index>(current_index() + 1), entry);
    refresh_list(1);
}

void FrequencyManagerView::on_del_entry() {
    if (!db_.is_open() || db_.empty())
        return;
    if (!nav_)
        return;

    const auto idx = current_index();
    const std::string label = trim(core::pretty_string(current_entry(), 23));

    ui::display_modal(
        *nav_, "Delete", "Delete " + label + "\nAre you sure?", ui::YESNO,
        [this, idx](bool choice) {
            if (!choice)
                return;
            db_.delete_entry(static_cast<core::FreqmanDB::Index>(idx));
            refresh_list();
        });
}

void FrequencyManagerView::on_edit_entry() {
    if (!db_.is_open() || db_.empty())
        return;
    if (!nav_)
        return;

    const auto idx = current_index();
    auto edit = std::make_unique<FrequencyEditView>(current_entry());
    edit->on_save = [this, idx](const core::freqman_entry& e) {
        db_.replace_entry(static_cast<core::FreqmanDB::Index>(idx), e);
        list_view.set_dirty();
    };
    nav_->push(std::move(edit));
}

void FrequencyManagerView::on_move_entry(int delta) {
    if (!db_.is_open() || db_.empty())
        return;

    const long i = static_cast<long>(current_index());
    const long j = i + delta;
    if (j < 0 || j >= static_cast<long>(db_.entry_count()))
        return;

    /* Neighbour swap. FreqmanDB has no move, so read both and write them back
     * exchanged. */
    const auto a = db_[static_cast<core::FreqmanDB::Index>(i)];
    const auto b = db_[static_cast<core::FreqmanDB::Index>(j)];
    db_.replace_entry(static_cast<core::FreqmanDB::Index>(i), b);
    db_.replace_entry(static_cast<core::FreqmanDB::Index>(j), a);

    list_view.set_index(static_cast<size_t>(j));
    list_view.set_dirty();
}

void FrequencyManagerView::on_tune_entry() {
    if (!db_.is_open() || db_.empty())
        return;
    if (!nav_)
        return;

    const auto entry = current_entry();
    if (entry.frequency_a <= 0) {
        ui::display_modal(*nav_, "Tune", "Entry has no frequency.");
        return;
    }

    auto* rx = app::globals().receiver;
    if (rx == nullptr) {
        ui::display_modal(*nav_, "Tune", "No receiver available\non this build.");
        return;
    }

    rx->set_target_frequency(static_cast<uint64_t>(entry.frequency_a));
    const std::string freq = trim(to_string_short_freq(static_cast<uint64_t>(entry.frequency_a)));
    ui::display_modal(*nav_, "Tune", "Tuned to\n" + freq + " MHz");
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_freqman{{"freqman", "Freq. Manager", app::Category::Utilities,
                                  ui::Color::green(), &ui::bitmap_icon_freqman,
                                  [] { return std::make_unique<app::FrequencyManagerView>(); }}};
}  // namespace
