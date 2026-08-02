/*
 * mayhem-b200 — the rest of the PortaPack widget set.
 *
 * ui_widget.hpp carries the widgets the shell itself needs. This file adds the
 * ones the *apps* need: text entry, symbol entry, waveform and spectrum
 * displays, image buttons and the tab strip. Class names, constructor
 * signatures, paint behaviour and key/encoder handling follow
 * firmware/common/ui_widget.hpp and application/ui/ui_tabview.hpp so app code
 * ports across unchanged.
 *
 * Deliberate host deviations (each one is commented at its definition):
 *   - LiveDateTime reads the system clock instead of the LPC43xx RTC, and has
 *     no "seconds not yet valid" placeholder because a host clock is always
 *     valid the moment the process starts.
 *   - TextEdit::on_focus/on_blur do not reconfigure switch long-press: on the
 *     host every key reports long presses all the time.
 *   - SymField::set_value(uint64_t), Waveform::paint and ImageOptionsField::paint
 *     guard against the degenerate inputs that trap on x86 (divide by zero,
 *     null buffers) where the ARM firmware merely produced nonsense.
 *   - BatteryIcon / BatteryTextField are NOT ported. There is no battery on a
 *     USRP B200; a widget showing a fake charge level would be a lie.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2025 HTotoo
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version. See LICENSE.GPL-2.0-or-later.
 */

#ifndef __HOST_UI_WIDGET_EXTRA_H__
#define __HOST_UI_WIDGET_EXTRA_H__

#include "theme.hpp"
#include "ui.hpp"
#include "ui_painter.hpp"
#include "ui_text.hpp"
#include "ui_widget.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ui {

/* --- LiveDateTime ---------------------------------------------------------
 * A clock that repaints itself once a second. The firmware drove this from
 * rtc_time::signal_tick_second; here on_frame_sync() polls the system clock,
 * which is the host equivalent per the porting contract (rule 5). */
class LiveDateTime : public Widget {
   public:
    /* Broken-down time, standing in for the firmware's rtc::RTC. */
    struct DateTime {
        uint16_t year{1970};
        uint8_t month{1};
        uint8_t day{1};
        uint8_t hour{0};
        uint8_t minute{0};
        uint8_t second{0};
    };

    explicit LiveDateTime(Rect parent_rect);

    void paint(Painter& painter) override;
    void on_frame_sync() override;

    void set_hide_clock(bool new_value);
    void set_seconds_enabled(bool new_value);
    void set_date_enabled(bool new_value);

    /* Host addition: lets a caller (and the tests) drive the display from a
     * known time instead of the wall clock. */
    void set_datetime(const DateTime& dt);
    const DateTime& datetime() const { return datetime_; }

    /* Rebuilds the rendered string from the system clock. */
    void refresh();

    std::string& string() { return text; }

   private:
    void format();

    bool hide_clock{false};
    bool date_enabled{true};
    bool seconds_enabled{false};

    DateTime datetime_{};
    std::string text{};
};

/* --- ButtonWithEncoder ----------------------------------------------------
 * A Button that also accumulates encoder movement while focused. TX apps use
 * it to nudge a value without leaving the button. */
class ButtonWithEncoder : public Widget {
   public:
    std::function<void(ButtonWithEncoder&)> on_select{};
    std::function<void(ButtonWithEncoder&)> on_touch_release{};
    std::function<void(ButtonWithEncoder&)> on_touch_press{};
    std::function<bool(ButtonWithEncoder&, KeyEvent)> on_dir{};
    std::function<void(ButtonWithEncoder&)> on_highlight{};
    std::function<void()> on_change{};

    /* instant_exec: fire on_select on touch-down instead of touch-release. */
    ButtonWithEncoder(Rect parent_rect, std::string text, bool instant_exec);
    ButtonWithEncoder(Rect parent_rect, std::string text)
        : ButtonWithEncoder{parent_rect, std::move(text), false} {}
    ButtonWithEncoder()
        : ButtonWithEncoder{{}, {}} {}

    void set_text(const std::string& value);
    std::string text() const;

    int32_t get_encoder_delta() const;
    void set_encoder_delta(const int32_t delta);

    void paint(Painter& painter) override;

    void on_focus() override;
    bool on_key(const KeyEvent key) override;
    bool on_encoder(const EncoderEvent delta) override;
    bool on_touch(const TouchEvent event) override;
    bool on_keyboard(const KeyboardEvent event) override;

   private:
    std::string text_;
    int32_t encoder_delta_{0};
    bool delta_change_{false};
    bool instant_exec_{false};
};

/* --- NewButton ------------------------------------------------------------
 * The bevelled button used by the main menu: an optional bitmap above an
 * optional caption. Note that on_select takes no argument, unlike Button's. */
class NewButton : public Widget {
   public:
    std::function<void(void)> on_select{};
    std::function<bool(NewButton&, KeyEvent)> on_dir{};
    std::function<void(NewButton&)> on_highlight{};

    NewButton(const NewButton&) = delete;
    NewButton& operator=(const NewButton&) = delete;

    NewButton(Rect parent_rect, std::string text, const Bitmap* bitmap);
    NewButton(Rect parent_rect,
              std::string text,
              const Bitmap* bitmap,
              Color color,
              bool vertical_center = false);
    NewButton()
        : NewButton{{}, {}, nullptr} {}

    void set_bitmap(const Bitmap* bitmap);
    void set_text(const std::string& value);
    void set_color(Color value);
    void set_bg_color(Color value);
    void set_vertical_center(bool value);

    std::string text() const;
    const Bitmap* bitmap() const;
    Color color() const;

    void on_focus() override;
    bool on_key(const KeyEvent key) override;
    bool on_touch(const TouchEvent event) override;
    bool on_keyboard(const KeyboardEvent event) override;

    void paint(Painter& painter) override;

   protected:
    virtual Style paint_style();

    Color color_;
    Color bg_color_{Theme::getInstance()->bg_medium->background};

   private:
    std::string text_;
    const Bitmap* bitmap_;
    bool vertical_center_{false};
};

/* --- Image ---------------------------------------------------------------- */
class Image : public Widget {
   public:
    Image();
    Image(const Rect parent_rect,
          const Bitmap* bitmap,
          const Color foreground,
          const Color background);

    Image(const Image&) = delete;
    Image(Image&&) = delete;
    Image& operator=(const Image&) = delete;
    Image& operator=(Image&&) = delete;

    void set_bitmap(const Bitmap* bitmap);
    void set_foreground(const Color color);
    void set_background(const Color color);
    void invert_colors();

    void paint(Painter& painter) override;

    /* Upstream's accessor. Only call it when a bitmap is set. */
    const Bitmap& bitmap() & { return *bitmap_; }
    /* Host addition so callers can check first. */
    const Bitmap* bitmap_ptr() const { return bitmap_; }

    Color foreground() const { return foreground_; }
    Color background() const { return background_; }

   private:
    const Bitmap* bitmap_;
    Color foreground_;
    Color background_;
};

class ImageButton : public Image {
   public:
    std::function<void(ImageButton&)> on_select{};

    ImageButton(const Rect parent_rect,
                const Bitmap* bitmap,
                const Color foreground,
                const Color background);

    bool on_key(const KeyEvent key) override;
    bool on_touch(const TouchEvent event) override;
    bool on_keyboard(const KeyboardEvent event) override;
};

/* A button that swaps image and colours when toggled. */
class ImageToggle : public ImageButton {
   public:
    std::function<void(bool value)> on_change{};

    ImageToggle(Rect parent_rect, const Bitmap* bitmap_);

    ImageToggle(Rect parent_rect,
                const Bitmap* bitmap_,
                Color foreground_true,
                Color foreground_false,
                Color background_);

    ImageToggle(Rect parent_rect,
                const Bitmap* bitmap_true,
                const Bitmap* bitmap_false,
                Color foreground_true,
                Color background_true,
                Color foreground_false,
                Color background_false);

    ImageToggle(const ImageToggle&) = delete;
    ImageToggle& operator=(const ImageToggle&) = delete;

    bool value() const;
    void set_value(bool b);

   private:
    /* Hidden: toggling is what this control does; an app overriding on_select
     * would break that. */
    using ImageButton::on_select;

    const Bitmap* bitmap_true_;
    const Bitmap* bitmap_false_;
    const Color foreground_true_;
    const Color background_true_;
    const Color foreground_false_;
    const Color background_false_;
    bool value_;
};

/* An OptionsField whose options are bitmaps rather than names. */
class ImageOptionsField : public Widget {
   public:
    using value_t = int32_t;
    using option_t = std::pair<const Bitmap*, value_t>;
    using options_t = std::vector<option_t>;

    std::function<void(size_t, value_t)> on_change{};
    std::function<void(void)> on_show_options{};

    ImageOptionsField(Rect parent_rect,
                      Color foreground,
                      Color background,
                      options_t options);

    ImageOptionsField()
        : ImageOptionsField{{},
                            Theme::getInstance()->bg_darkest->foreground,
                            Theme::getInstance()->bg_darkest->background,
                            {}} {}

    void set_options(options_t new_options);
    const options_t& options() const { return options_; }

    size_t selected_index() const;
    size_t selected_index_value() const;
    void set_selected_index(const size_t new_index);
    void set_by_value(value_t v);

    void paint(Painter& painter) override;

    void on_focus() override;
    bool on_encoder(const EncoderEvent delta) override;
    bool on_touch(const TouchEvent event) override;
    bool on_keyboard(const KeyboardEvent event) override;
    bool on_key(const KeyEvent event) override;

   private:
    options_t options_;
    size_t selected_index_{0};
    Color foreground_;
    Color background_;
};

/* --- TextEdit -------------------------------------------------------------
 * Bound to a caller-owned string. The widget renders the text and a cursor and
 * exposes the editing API; it does not put a keyboard on screen — that is the
 * text-entry view's job. */
class TextEdit : public Widget {
   public:
    TextEdit(std::string& str, Point position, uint32_t length = 30)
        : TextEdit{str, 64, position, length} {}

    /* str:        the string being edited (borrowed, not copied).
     * max_length: longest the string may become.
     * position:   top-left corner. Characters are 8 px wide, the control 16 tall.
     * length:     number of characters shown; the screen fits 30. */
    TextEdit(std::string& str, size_t max_length, Point position, uint32_t length = 30);

    TextEdit(const TextEdit&) = delete;
    TextEdit(TextEdit&&) = delete;
    TextEdit& operator=(const TextEdit&) = delete;
    TextEdit& operator=(TextEdit&&) = delete;

    const std::string& value() const;

    uint32_t cursor() const { return cursor_pos_; }
    bool insert_mode() const { return insert_mode_; }

    void set_cursor(uint32_t pos);
    void set_insert_mode();
    void set_overwrite_mode();

    void char_add(char c);
    void char_delete();

    void paint(Painter& painter) override;

    bool on_key(const KeyEvent key) override;
    bool on_encoder(const EncoderEvent delta) override;
    bool on_touch(const TouchEvent event) override;
    bool on_keyboard(const KeyboardEvent event) override;

    void on_focus() override;
    void on_blur() override;

   protected:
    std::string& text_;
    size_t max_length_;
    uint32_t char_count_;
    uint32_t cursor_pos_;
    bool insert_mode_;
};

/* --- TextField ------------------------------------------------------------
 * A focusable Text. Selecting it is how apps open a chooser or a text-entry
 * view; the field itself does not edit anything. */
class TextField : public Text {
   public:
    std::function<void(TextField&)> on_select{};
    std::function<void(TextField&)> on_change{};
    std::function<void(TextField&, EncoderEvent)> on_encoder_change{};

    TextField(Rect parent_rect, std::string text);

    const std::string& get_text() const;
    void set_text(std::string_view value);

    bool on_key(KeyEvent key) override;
    bool on_encoder(EncoderEvent delta) override;
    bool on_touch(TouchEvent event) override;

   private:
    /* set() bypasses on_change; make callers go through set_text(). */
    using Text::set;
};

/* --- FloatField -----------------------------------------------------------
 * NumberField's floating-point sibling; same range/step/loop semantics. */
class FloatField : public Widget {
   public:
    using range_t = std::pair<float, float>;

    std::function<void(FloatField&)> on_select{};
    std::function<void(float)> on_change{};
    std::function<void(int32_t)> on_wrap{};

    FloatField(Point parent_pos,
               int length,
               range_t range,
               float step,
               char fill_char,
               bool can_loop,
               uint8_t precision = 1);

    FloatField(Point parent_pos, int length, range_t range, float step, char fill_char)
        : FloatField{parent_pos, length, range, step, fill_char, true, 1} {}

    FloatField()
        : FloatField{{0, 0}, 1, {0.0f, 1.0f}, 1.0f, ' ', true, 1} {}

    FloatField(const FloatField&) = delete;
    FloatField(FloatField&&) = delete;

    float value() const;
    void set_value(float new_value, bool trigger_change = true);
    void set_range(const float min, const float max);
    void set_step(const float new_step);
    float step() const { return step_; }
    void set_precision(uint8_t precision);

    void paint(Painter& painter) override;

    bool on_key(const KeyEvent key) override;
    bool on_encoder(const EncoderEvent delta) override;
    bool on_touch(const TouchEvent event) override;
    bool on_keyboard(const KeyboardEvent event) override;

   private:
    range_t range_;
    float step_;
    int length_;
    char fill_char_;
    bool can_loop_;
    uint8_t precision_{1};
    float value_{0.0f};
};

/* --- SymField -------------------------------------------------------------
 * Character-by-character entry over a fixed symbol set. Nearly every TX app
 * uses one to type an address, an ID or a raw frame. Left/Right pick a slot,
 * the encoder walks that slot through the symbol list, and any symbol outside
 * the list collapses to the list's first entry. */
class SymField : public Widget {
   public:
    std::function<void(SymField&)> on_change{};

    enum class Type {
        Custom,
        Oct,
        Dec,
        Hex,
        Alpha,
    };

    /* explicit_edits: when true the field must be Selected before it can be
     * modified, and the whole field (not one slot) inverts on focus. Views with
     * several SymFields are much easier to navigate that way. */
    SymField(Point parent_pos,
             size_t length,
             Type type = Type::Dec,
             bool explicit_edits = false);

    SymField(Point parent_pos,
             size_t length,
             std::string symbol_list,
             bool explicit_edits = false);

    SymField(const SymField&) = delete;
    SymField(SymField&&) = delete;

    /* Symbol (character) at `index`; 0 when out of range. */
    char get_symbol(size_t index) const;
    void set_symbol(size_t index, char symbol);

    /* Position of the symbol at `index` within the symbol list. */
    size_t get_offset(size_t index) const;
    void set_offset(size_t index, size_t offset);

    /* Replacing the symbol list re-validates every slot. An empty list is
     * ignored — a field with no legal symbols could not be painted. */
    void set_symbol_list(std::string symbol_list);
    const std::string& symbol_list() const { return symbols_; }

    /* Integer value. OCT/DEC/HEX only; a no-op for Custom and Alpha, which have
     * no radix. */
    void set_value(uint64_t value);

    /* String value, right-aligned in the field. Characters outside the symbol
     * list become the list's first symbol. Values longer than the field are
     * rejected outright, as upstream does. */
    void set_value(std::string_view value);

    uint64_t to_integer() const;
    const std::string& to_string() const;

    size_t length() const { return value_.length(); }
    size_t selected() const { return selected_; }
    bool editing() const { return editing_; }

    void paint(Painter& painter) override;
    bool on_key(KeyEvent key) override;
    bool on_encoder(EncoderEvent delta) override;
    bool on_touch(TouchEvent event) override;

   private:
    char ensure_valid(char symbol) const;
    void ensure_all_symbols();
    void set_symbol_internal(size_t index, char symbol);
    uint8_t get_radix() const;

    std::string symbols_{};
    std::string value_{};
    Type type_{Type::Custom};
    size_t selected_{0};
    bool editing_{false};
    bool explicit_edits_{true};
};

/* --- Waveform -------------------------------------------------------------
 * Plots a caller-owned int16_t buffer, either as a polyline (analog) or as
 * high/low runs (digital). The buffer is borrowed and must outlive the widget.
 * A clickable Waveform can be paused, which is how the wide apps stop paying
 * for the draw. */
class Waveform : public Widget {
   public:
    std::function<void(Waveform&)> on_select{};

    Waveform(Rect parent_rect,
             int16_t* data,
             uint32_t length,
             uint32_t offset,
             bool digital,
             Color color,
             bool clickable = false);

    Waveform(const Waveform&) = delete;
    Waveform(Waveform&&) = delete;
    Waveform& operator=(const Waveform&) = delete;
    Waveform& operator=(Waveform&&) = delete;

    void set_offset(const uint32_t new_offset);
    void set_length(const uint32_t new_length);
    void set_cursor(const uint32_t i, const int16_t position);
    void set_data(int16_t* new_data);

    bool is_paused() const;
    void set_paused(bool paused);
    bool is_clickable() const;

    uint32_t length() const { return length_; }
    uint32_t offset() const { return offset_; }

    void paint(Painter& painter) override;
    bool on_key(const KeyEvent key) override;
    bool on_touch(const TouchEvent event) override;
    bool on_keyboard(const KeyboardEvent event) override;

   private:
    const Color cursor_colors[2] = {Theme::getInstance()->fg_cyan->foreground,
                                    Theme::getInstance()->fg_magenta->foreground};

    int16_t* data_;
    uint32_t length_;
    uint32_t offset_;
    bool digital_{false};
    Color color_;
    int16_t cursors[2]{};
    bool show_cursors{false};
    bool paused_{false};
    bool clickable_{false};
    bool if_ever_painted_pause{false};
};

/* The audio spectrum snapshot GraphEq consumes. The firmware got this from the
 * baseband core over a message queue; on the host the DSP thread fills it in
 * place. Layout matches AudioSpectrum in firmware/common/message.hpp. */
struct AudioSpectrum {
    std::array<uint8_t, 128> db{};
};

/* --- GraphEq --------------------------------------------------------------
 * Eleven-band segmented bar display driven by an AudioSpectrum. */
class GraphEq : public Widget {
   public:
    std::function<void(GraphEq&)> on_select{};

    explicit GraphEq(Rect parent_rect, bool clickable = false);
    GraphEq(const GraphEq&) = delete;
    GraphEq(GraphEq&&) = delete;
    GraphEq& operator=(const GraphEq&) = delete;
    GraphEq& operator=(GraphEq&&) = delete;

    bool is_paused() const;
    void set_paused(bool paused);
    bool is_clickable() const;

    void paint(Painter& painter) override;
    bool on_key(const KeyEvent key) override;
    bool on_touch(const TouchEvent event) override;
    bool on_keyboard(const KeyboardEvent event) override;
    void set_parent_rect(const Rect new_parent_rect) override;

    void update_audio_spectrum(const AudioSpectrum& spectrum);
    void set_theme(Color base_color, Color peak_color);

    static constexpr int NUM_BARS = 11;
    Dim bar_height(size_t bar) const;

   private:
    bool is_calculated{false};
    bool paused_{false};
    bool clickable_{false};
    bool needs_background_redraw{true};
    Color base_color{255, 0, 255};
    Color peak_color{255, 255, 255};
    std::vector<Dim> bar_heights;
    std::vector<Dim> prev_bar_heights;

    Dim y_top = 2 * 16;
    Dim RENDER_HEIGHT = 288;
    Dim BAR_WIDTH = 20;
    Dim HORIZONTAL_OFFSET = 2;
    static const int BAR_SPACING = 2;
    static const int SEGMENT_HEIGHT = 10;

    /* Band edges in Hz, straight from upstream. */
    static constexpr std::array<int16_t, NUM_BARS + 1> FREQUENCY_BANDS = {
        375, 750, 1500, 2250, 3375, 4875, 6750, 9375, 13125, 16875, 20625, 24375};

    void calculate_params();
};

/* --- OptionTabView --------------------------------------------------------
 * The "Transmit <thing>" pane the multi-modulation TX apps stack behind a
 * TabView: a single enable checkbox plus whatever the subclass adds. */
class OptionTabView : public View {
   public:
    explicit OptionTabView(Rect parent_rect);

    void focus() override;

    bool is_enabled();
    void set_type(std::string type);

   protected:
    bool enabled{false};

    void set_enabled(bool value);

   private:
    Checkbox check_enable{{2 * 8, 0}, 20, "", false};
};

/* --- TabView --------------------------------------------------------------
 * Ported from firmware/application/ui/ui_tabview.hpp. The tab strip owns the
 * Tab widgets; the tabbed *views* are owned by whoever built the TabView and
 * are only shown and hidden here.
 *
 * Contract inherited from upstream: a tab's content view must arrive HIDDEN.
 * TabView shows the selected view and hides the one it moves away from, but it
 * never hides a view it has not selected, so anything that starts visible stays
 * visible on top of the others. Upstream's tab content calls hidden(true) in
 * its own constructor (see EncodersConfigView, OptionTabView). Do the same. */
constexpr size_t MAX_TABS = 5;

class Tab : public Widget {
   public:
    Tab();

    void paint(Painter& painter) override;

    bool on_key(const KeyEvent key) override;
    bool on_touch(const TouchEvent event) override;

    void set(uint32_t index, Dim width, std::string text, Color text_color);

   private:
    std::string text_{};
    Color text_color_{};
    uint32_t index_{};
};

class TabView : public View {
   public:
    struct TabDef {
        std::string text;
        Color color;
        View* view;
    };

    explicit TabView(std::initializer_list<TabDef> tab_definitions);

    void focus() override;
    void on_show() override;

    void set_selected(uint32_t index);
    uint32_t selected() const { return current_tab; }
    size_t tab_count() const { return n_tabs; }

   private:
    size_t n_tabs{};
    std::array<Tab, MAX_TABS> tabs{};
    std::array<View*, MAX_TABS> views{};
    uint32_t current_tab{0};
};

}  // namespace ui

#endif /*__HOST_UI_WIDGET_EXTRA_H__*/
