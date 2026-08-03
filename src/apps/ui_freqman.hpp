/*
 * mayhem-b200 — the Frequency Manager UI.
 *
 * Ported from firmware/application/apps/ui_freqman.* (the FrequencyManagerView
 * and FrequencyEditView) and the entry-list widget from
 * firmware/application/ui/ui_freqlist.*. It edits Mayhem .TXT frequency lists in
 * core::freqman_directory() through core::FreqmanDB, so the files stay byte-for-
 * byte interchangeable with a PortaPack.
 *
 * What the manager does:
 *   - lists the .TXT files ("categories") in core::freqman_directory()
 *   - opens one and shows its entries in a scrolling list
 *   - adds / edits / deletes / reorders entries, and creates / deletes lists
 *   - "tunes to" the selected entry via
 *     app::globals().receiver->set_target_frequency()
 *
 * Deliberate deviations from upstream, and why:
 *   - Upstream's FreqManUIList lives in a shared ui_freqlist.* pair. The porting
 *     contract forbids adding shared files, so the equivalent widget
 *     (FreqmanEntryList) is defined here, in the manager's own files.
 *   - Reorder (move up / move down) has no upstream equivalent in ui_freqman;
 *     it is implemented locally as a neighbour swap through FreqmanDB.
 *   - There is no FrequencyKeypadView on the host, so the whole entry is edited
 *     in one FrequencyEditView (frequencies via ui::FrequencyField) rather than
 *     through the manager's separate quick-edit buttons.
 *   - Upstream's FrequencySaveView / FrequencyLoadView are helper views consumed
 *     by the RX/TX apps, not by the manager itself, and are out of scope here.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2023 Kyle Reed
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_FREQMAN_H__
#define __MB200_UI_FREQMAN_H__

#include "freqman_db.hpp"

#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_painter.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace ui {
class NavigationView;
}

namespace app {

/* A scrolling, focusable list of freqman entries backed directly by a
 * core::FreqmanDB. The behaviour (selection tracking, scroll window, key and
 * encoder handling, raw-line colouring, "too large" yellow warning) mirrors the
 * firmware's ui::FreqManUIList. */
class FreqmanEntryList : public ui::Widget {
   public:
    /* Fired with the selected index when Select is pressed. */
    std::function<void(size_t)> on_select{};
    /* Fired when the user steers out of the list (Right). */
    std::function<void()> on_leave{};

    explicit FreqmanEntryList(ui::Rect parent_rect);

    void paint(ui::Painter& painter) override;
    void on_focus() override;
    void on_blur() override;
    bool on_key(const ui::KeyEvent key) override;
    bool on_encoder(const ui::EncoderEvent delta) override;

    void set_parent_rect(const ui::Rect new_parent_rect) override;

    void set_db(core::FreqmanDB& db);
    void set_index(size_t index);
    size_t get_index() const;

   private:
    void adjust_selected_index(int delta);
    size_t entry_count() const;

    static constexpr int char_height = 16;
    static constexpr int char_width = 8;

    core::FreqmanDB* db_{nullptr};
    uint8_t line_max_length_{29};
    size_t visible_lines_{1};
    size_t start_index_{0};
    size_t selected_index_{0};
};

/* Full-screen entry editor: type, both frequencies, modulation, bandwidth,
 * step, CTCSS tone and description, with live validation. Ported from
 * firmware's FrequencyEditView. */
class FrequencyEditView : public ui::View {
   public:
    /* Called with the edited entry when Save is pressed (before the pop). */
    std::function<void(const core::freqman_entry&)> on_save{};

    explicit FrequencyEditView(core::freqman_entry entry);
    std::string title() const override { return "Freqman Edit"; }

    /* The host nav establishes focus through on_show(), not focus(); on_show
     * simply defers to focus() so the upstream method keeps its meaning. */
    void on_show() override;
    void focus() override;

   private:
    ui::NavigationView* nav_{nullptr};
    core::freqman_entry entry_{};
    std::string temp_desc_{};

    void refresh_ui();
    void populate_modulation_options();
    void populate_bandwidth_options();
    void populate_step_options();
    void populate_tone_options();

    ui::Labels labels{
        {{5 * 8, 1 * 16}, "Edit Frequency Entry", ui::Color::light_grey()},
        {{0 * 8, 3 * 16}, "Entry Type :", ui::Color::light_grey()},
        {{0 * 8, 4 * 16}, "Frequency A:", ui::Color::light_grey()},
        {{0 * 8, 5 * 16}, "Frequency B:", ui::Color::light_grey()},
        {{0 * 8, 6 * 16}, "Modulation :", ui::Color::light_grey()},
        {{0 * 8, 7 * 16}, "Bandwidth  :", ui::Color::light_grey()},
        {{0 * 8, 8 * 16}, "Step       :", ui::Color::light_grey()},
        {{0 * 8, 9 * 16}, "Tone Freq  :", ui::Color::light_grey()},
        {{0 * 8, 10 * 16}, "Description:", ui::Color::light_grey()},
    };

    ui::OptionsField field_type{
        {13 * 8, 3 * 16},
        8,
        {
            {"Single", 0},
            {"Range", 1},
            {"HamRadio", 2},
            {"Repeater", 3},
            {"Raw", 4},
        }};

    ui::FrequencyField field_freq_a{{13 * 8, 4 * 16}};
    ui::FrequencyField field_freq_b{{13 * 8, 5 * 16}};

    ui::OptionsField field_modulation{{13 * 8, 6 * 16}, 5, {}};
    ui::OptionsField field_bandwidth{{13 * 8, 7 * 16}, 12, {}};
    ui::OptionsField field_step{{13 * 8, 8 * 16}, 10, {}};
    ui::OptionsField field_tone{{13 * 8, 9 * 16}, 10, {}};

    ui::TextField field_description{
        {13 * 8, 10 * 16, 17 * 8, 1 * 16},
        ""};

    ui::Text text_validation{
        {13 * 8, 12 * 16, 6 * 8, 1 * 16},
        ""};

    /* Bottom row. screen_height is 320, so UI_POS_Y_BOTTOM(3) == 272; the view
     * is 304 tall, leaving these flush against its bottom. Written as literals
     * because the UI_POS_* macros expand to unqualified ui:: names that do not
     * resolve inside namespace app. */
    ui::Button button_save{
        {0, 272, 15 * 8, 2 * 16},
        "Save"};

    ui::Button button_cancel{
        {120, 272, 15 * 8, 2 * 16},
        "Cancel"};
};

/* The Frequency Manager: category picker, entry list, and the action buttons.
 * Registered as the "freqman" app under Category::Utilities. */
class FrequencyManagerView : public ui::View {
   public:
    FrequencyManagerView();
    std::string title() const override { return "Freqman"; }

    /* See FrequencyEditView: the host nav focuses via on_show(). */
    void on_show() override;
    void focus() override;

   private:
    ui::NavigationView* nav_{nullptr};
    std::string temp_buffer_{};
    bool warned_no_lists_{false};

    /* Persist the selected category across pushes of the app, like upstream. */
    static size_t current_category_index_;

    void refresh_categories(const std::string& select_stem = {});
    void change_category(size_t new_index);
    std::string current_category() const;
    size_t current_index() const;
    core::freqman_entry current_entry() const;
    void refresh_list(int delta_selected = 0);

    void on_add_category();
    void on_del_category();
    void on_add_entry();
    void on_del_entry();
    void on_edit_entry();
    void on_move_entry(int delta);
    void on_tune_entry();

    core::FreqmanDB db_{};

    ui::Labels label_category{
        {{0, 2}, "F:", ui::Color::light_grey()}};

    ui::OptionsField options_category{
        {2 * 8, 2},
        26,
        {}};

    /* List fills the middle; three rows of buttons sit below it. */
    FreqmanEntryList list_view{
        {0, 3 * 8, 240, 198}};

    /* Row 1: entry edit / add / delete. */
    ui::Button button_edit{
        {0, 222, 80, 26},
        "Edit"};
    ui::Button button_add{
        {80, 222, 80, 26},
        "Add"};
    ui::Button button_del{
        {160, 222, 80, 26},
        "Del"};

    /* Row 2: reorder up / down, and tune the radio to the entry. */
    ui::Button button_up{
        {0, 250, 80, 26},
        "Up"};
    ui::Button button_down{
        {80, 250, 80, 26},
        "Down"};
    ui::Button button_tune{
        {160, 250, 80, 26},
        "Tune"};

    /* Row 3: list (category) management and exit. */
    ui::Button button_new_list{
        {0, 278, 80, 26},
        "New Lst"};
    ui::Button button_del_list{
        {80, 278, 80, 26},
        "Del Lst"};
    ui::Button button_exit{
        {160, 278, 80, 26},
        "Exit"};
};

}  // namespace app

#endif /*__MB200_UI_FREQMAN_H__*/
