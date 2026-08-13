/*
 * mayhem-b200 — widget toolkit.
 *
 * A host reimplementation of the PortaPack firmware's widget set. The class
 * names, member functions and paint semantics deliberately match
 * firmware/common/ui_widget.hpp so that app code reads the same on both sides;
 * what is gone is the ChibiOS, persistent-memory and CPLD coupling.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version. See LICENSE.GPL-2.0-or-later.
 */

#ifndef __HOST_UI_WIDGET_H__
#define __HOST_UI_WIDGET_H__

#include "ui.hpp"
#include "ui_focus.hpp"
#include "ui_painter.hpp"
#include "ui_text.hpp"
#include "theme.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

/* Screen-wide repaint flag, same contract as the firmware: any widget marking
 * itself dirty also raises this, and the top-level paint loop clears it. */
void dirty_set();
void dirty_clear();
bool is_dirty();

class Context {
   public:
    FocusManager& focus_manager() { return focus_manager_; }

   private:
    FocusManager focus_manager_{};
};

class Widget {
   public:
    Widget()
        : _parent_rect{} {}

    explicit Widget(Rect parent_rect)
        : _parent_rect{parent_rect} {}

    Widget(const Widget&) = delete;
    Widget(Widget&&) = delete;
    Widget& operator=(const Widget&) = delete;
    Widget& operator=(Widget&&) = delete;

    virtual ~Widget();

    Point screen_pos() const;
    Size size() const;
    Rect screen_rect() const;
    Rect parent_rect() const;
    virtual void set_parent_rect(const Rect new_parent_rect);

    Widget* parent() const;
    void set_parent(Widget* const widget);

    virtual void on_after_attach() {}
    virtual void on_before_detach() {}

    bool hidden() const { return flags.hidden; }
    void hidden(bool hide);

    virtual void focus();
    virtual void on_focus();
    virtual void blur();
    virtual void on_blur();
    bool focusable() const;
    void set_focusable(const bool value);
    bool has_focus() const;

    virtual void paint(Painter& painter) = 0;

    virtual void on_show() {}
    virtual void on_hide() {}

    /* Called once per UI frame on every visible widget. Default does nothing;
     * widgets that animate or poll a data source override it. The firmware got
     * this via message-queue callbacks from the M4; on the host a simple tick is
     * both sufficient and easier to reason about. */
    virtual void on_frame_sync() {}

    virtual bool on_key(const KeyEvent event);
    virtual bool on_encoder(const EncoderEvent event);
    virtual bool on_touch(const TouchEvent event);
    virtual bool on_keyboard(const KeyboardEvent event);
    virtual const std::vector<Widget*>& children() const;

    virtual Context& context() const;

    void set_style(const Style* new_style);
    const Style& style() const;

    int16_t id = 0;

    void set_dirty();
    bool dirty() const;
    void set_clean();

    void drawn(bool v);
    bool drawn() const { return flags.drawn; }

    bool highlighted() const;
    void set_highlighted(const bool value);

   protected:
    void dirty_overlapping_children_in_rect(const Rect& child_rect);

   private:
    Rect _parent_rect;
    const Style* style_{nullptr};
    Widget* parent_{nullptr};

    struct flags_t {
        bool dirty : 1;
        bool hidden : 1;
        bool focusable : 1;
        bool highlighted : 1;
        bool drawn : 1;
    };

    flags_t flags{
        /*dirty=*/true,
        /*hidden=*/false,
        /*focusable=*/false,
        /*highlighted=*/false,
        /*drawn=*/false,
    };

    static const std::vector<Widget*> no_children;
};

class View : public Widget {
   public:
    View() = default;
    explicit View(Rect parent_rect) { set_parent_rect(parent_rect); }

    void paint(Painter& painter) override;

    void add_child(Widget* const widget);
    void add_children(const std::initializer_list<Widget*> children);
    void remove_child(Widget* const widget);
    void remove_children(const std::vector<Widget*>& children);
    const std::vector<Widget*>& children() const override;

    virtual std::string title() const;

    /* The registry id of the app this view IS, or "" for anything else — a
     * menu, or a sub-view an app pushed on top of itself.
     *
     * Set once by app::AppRegistry::add()'s factory wrapper, so every push
     * site records it without knowing it has to. The web portal reads it back
     * off the navigation stack to name the app the operator is actually in:
     * a remote key press navigates the device exactly as the local encoder
     * does, so the portal cannot assume it launched what is on screen, and
     * naming the wrong app puts one app's data under another app's title.
     *
     * It lives on the view rather than in a side table keyed by pointer
     * because that identity has to die with the view. A freed app view's
     * address is reused by whatever is allocated next — including a sub-view
     * of a different app — and a side table would then answer with the dead
     * app's name for a live view belonging to someone else. */
    const std::string& app_id() const { return app_id_; }
    void set_app_id(std::string registry_id) { app_id_ = std::move(registry_id); }

    /* Propagates a per-frame tick to the whole subtree. */
    void on_frame_sync() override;

   protected:
    std::vector<Widget*> children_{};
    void invalidate_child(Widget* const widget);

   private:
    std::string app_id_{};
};

class Rectangle : public Widget {
   public:
    Rectangle() : Rectangle{{}, {}} {}
    explicit Rectangle(Color c);
    Rectangle(Rect parent_rect, Color c);

    void paint(Painter& painter) override;

    void set_color(const Color c);
    void set_outline(const bool outline);

   private:
    Color color{};
    bool outline_{false};
};

class Text : public Widget {
   public:
    std::function<void(Text&)> on_select{};

    Text() : text{""} {}
    Text(Rect parent_rect, std::string text);
    explicit Text(Rect parent_rect);

    void set(std::string_view value);
    std::string get() const;

    void paint(Painter& painter) override;
    bool on_touch(const TouchEvent event) override;

   protected:
    std::string text;
};

/* A fixed set of static captions painted in one pass — cheaper than one Text
 * widget per label, which is why the firmware has it. */
class Labels : public Widget {
   public:
    struct Label {
        Point pos;
        std::string text;
        Color color;
    };

    Labels(const Labels&) = delete;
    Labels& operator=(const Labels&) = delete;

    Labels(std::initializer_list<Label> labels);
    Labels(Rect parent_rect, std::initializer_list<Label> labels);

    void paint(Painter& painter) override;
    void set_labels(std::initializer_list<Label> labels);

   private:
    std::vector<Label> labels_;
};

class Button : public Widget {
   public:
    std::function<void(Button&)> on_select{};
    std::function<void(Button&)> on_touch_release{};
    std::function<bool(Button&, KeyEvent)> on_dir{};

    Button(Rect parent_rect, std::string text, bool instant_exec = false);
    Button(Rect parent_rect, std::string text, Color bg, Color fg, bool instant_exec = false);

    void set_text(std::string_view value);
    std::string text() const;

    void set_bg_color(Color c);
    void set_fg_color(Color c);

    void paint(Painter& painter) override;
    bool on_key(const KeyEvent key) override;
    bool on_touch(const TouchEvent event) override;

   private:
    std::string text_;
    bool instant_exec_{false};
    bool has_custom_colors_{false};
    Color bg_{};
    Color fg_{};
};

class Checkbox : public Widget {
   public:
    std::function<void(Checkbox&, bool)> on_select{};

    Checkbox(Point parent_pos, size_t length, std::string text, bool small_text = false);

    void set_text(std::string_view value);
    bool value() const;
    void set_value(const bool value);

    void paint(Painter& painter) override;
    bool on_key(const KeyEvent key) override;
    bool on_touch(const TouchEvent event) override;

   private:
    std::string text_;
    bool small_{false};
    bool value_{false};
};

/* A cycling list of (label, value) pairs. Values are int32_t; apps that need a
 * richer payload key off the selected index. */
class OptionsField : public Widget {
   public:
    using name_t = std::string;
    using value_t = int32_t;
    using option_t = std::pair<name_t, value_t>;
    using options_t = std::vector<option_t>;

    std::function<void(size_t, value_t)> on_change{};
    std::function<void(void)> on_show_options{};

    OptionsField(Point parent_pos, int length, options_t options, bool centered = false);

    const options_t& options() const { return options_; }
    void set_options(options_t new_options);

    size_t selected_index() const;
    const name_t& selected_index_name() const;
    value_t selected_index_value() const;
    void set_selected_index(const size_t new_index, bool trigger_change = true);
    /* Selects the first option whose value matches; no-op when absent. */
    bool set_by_value(value_t v, bool trigger_change = true);
    /* Selects the first option whose value is >= v (options must be ordered). */
    void set_by_nearest_value(value_t v, bool trigger_change = true);

    void set_length(int len);

    void paint(Painter& painter) override;
    bool on_encoder(const EncoderEvent delta) override;
    bool on_key(const KeyEvent key) override;
    bool on_touch(const TouchEvent event) override;

   private:
    options_t options_;
    size_t selected_index_{0};
    int length_;
    bool centered_{false};
};

class NumberField : public Widget {
   public:
    using range_t = std::pair<int32_t, int32_t>;

    std::function<void(NumberField&)> on_select{};
    std::function<void(int32_t)> on_change{};

    NumberField(Point parent_pos,
                int length,
                range_t range,
                int32_t step,
                char fill_char,
                bool can_loop = false);

    int32_t value() const;
    void set_value(int32_t new_value, bool trigger_change = true);
    void set_range(const int32_t min, const int32_t max);
    void set_step(int32_t new_step) { step_ = new_step; }
    int32_t step() const { return step_; }

    void paint(Painter& painter) override;
    bool on_key(const KeyEvent key) override;
    bool on_encoder(const EncoderEvent delta) override;
    bool on_touch(const TouchEvent event) override;

   protected:
    int32_t clip_value(int32_t value) const;

    range_t range_;
    int32_t step_;
    int length_;
    char fill_char_;
    bool can_loop_;
    int32_t value_{0};
};

/* Seven-segment style frequency readout used by the receiver apps. */
class BigFrequency : public Widget {
   public:
    explicit BigFrequency(Rect parent_rect, uint64_t frequency = 0);

    void set(const uint64_t frequency);
    void paint(Painter& painter) override;

   private:
    uint64_t frequency_{0};

    void draw_digit(Painter& painter, Point p, uint8_t digit, Color color);
};

class ProgressBar : public Widget {
   public:
    explicit ProgressBar(Rect parent_rect);

    void set_max(const uint32_t max);
    void set_value(const uint32_t value);
    void paint(Painter& painter) override;

   private:
    uint32_t max_{100};
    uint32_t value_{0};
};

/* A scrolling text log. Unlike the firmware's, this keeps its lines so the view
 * can be repainted after being hidden — the host has memory to spare. */
class Console : public Widget {
   public:
    explicit Console(Rect parent_rect);

    void clear(bool clear_screen = true);
    void write(std::string_view message);
    void writeln(std::string_view message);
    void enable_scrolling(bool enable);
    void set_max_lines(size_t n) { max_lines_ = n; }

    /* Read-only access to the log, for the web portal's console panel providers
     * (src/remote/provider_pocsag.cpp and friends). The four text decoders keep
     * their decoded output in a Console and nowhere else, so this is the only
     * way to publish it without changing how an app produces it.
     *
     * pending() matters as much as lines(): write() only commits a line on '\n'
     * or at the wrap column, so a decoder that emits a short message with
     * write() rather than writeln() — POCSAG's message body does exactly that —
     * leaves that message sitting here. paint() draws it as the last line, so a
     * reader that skips it silently loses text that is on the screen. */
    const std::deque<std::string>& lines() const { return lines_; }
    const std::string& pending() const { return pending_; }

    void paint(Painter& painter) override;
    void on_show() override;

   private:
    std::deque<std::string> lines_{};
    std::string pending_{};
    size_t max_lines_{256};
    bool scrolling_{true};

    int visible_lines() const;
    int columns() const;
};

/* Blinks on activity; the firmware uses it to show packets arriving. */
class ActivityDot : public Widget {
   public:
    ActivityDot(Rect parent_rect, Color color);

    void toggle();
    void reset();
    void paint(Painter& painter) override;

   private:
    Color color_;
    bool on_{false};
};

/* Horizontal bargraph, 0..255, used for RSSI and audio level. */
class VuMeter : public Widget {
   public:
    VuMeter(Rect parent_rect, uint32_t LEDs, bool show_max);

    void set_value(const uint8_t new_value);
    void set_mark(const uint8_t new_mark);
    void paint(Painter& painter) override;

   private:
    uint32_t leds_;
    bool show_max_;
    uint8_t value_{0};
    uint8_t mark_{0};
    uint8_t max_{0};
    uint8_t split_{0};
};

}  // namespace ui

#endif /*__HOST_UI_WIDGET_H__*/
