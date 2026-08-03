/*
 * mayhem-b200 — waterfall gradient designer.
 *
 * Ported from firmware/application/external/waterfall_designer/
 * ui_waterfall_designer.{hpp,cpp} (WaterfallDesignerView, by Mr. Robot / HTotoo),
 * together with the gradient interpolation from firmware/application/gradient.cpp
 * (Belousov Oleg), which defines the "index,R,G,B" profile format the app edits.
 *
 * What maps to the host and what does not (honesty rule, doc/PORTING.md):
 *   - Editing a colour profile — load/save a list of "index,R,G,B" control
 *     points, add/remove/recolour levels, and interpolate them into the 256-entry
 *     LUT — is pure file + arithmetic work and is implemented in full.
 *   - A live gradient preview bar (the 256 interpolated colours) is drawn, which
 *     is exactly what a gradient designer needs to judge its work.
 *   - Upstream also runs a live RX *waterfall* behind the editor so you can see
 *     the palette on real spectrum. That needs a running receiver + spectrum
 *     baseband; this build does not drive one, so the app says so on screen and
 *     shows the palette itself rather than a fake waterfall.
 *
 * The parser and the interpolation live in waterfall_designer_detail so they can
 * be unit tested against upstream's gradient.cpp formula.
 *
 * Copyright (C) 2025 Belousov Oleg (gradient interpolation)
 * Copyleft Mr. Robot; Copyright HTotoo (designer UI)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_WATERFALL_DESIGNER_H__
#define __MB200_UI_WATERFALL_DESIGNER_H__

#include "ui.hpp"
#include "ui_menu.hpp"
#include "ui_painter.hpp"
#include "ui_widget.hpp"

#include "../core/string_format.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace app {

/* Pure profile parsing + gradient interpolation, tested without a display. */
namespace waterfall_designer_detail {

struct Level {
    uint8_t index;
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

/* Strict decimal parse of one field, tolerating surrounding whitespace. No
 * exceptions (upstream is built -fno-exceptions and uses parse_int for exactly
 * this reason). Rejects empty, non-digit, and absurdly long fields. */
inline bool parse_uint_field(std::string_view s, int& out) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' ||
                     s[b - 1] == '\r' || s[b - 1] == '\n'))
        b--;
    if (a == b) return false;

    long v = 0;
    for (size_t i = a; i < b; i++) {
        const char c = s[i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
        if (v > 1000000) return false;
    }
    out = static_cast<int>(v);
    return true;
}

/* Parses an "index,R,G,B" line. False for a blank line, a comment ('#'), the
 * wrong number of columns, a non-numeric field, or any field outside 0..255 —
 * the same acceptance test upstream's parse_level / Gradient::load_file apply. */
inline bool parse_level(std::string_view line, Level& out) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.remove_suffix(1);
    if (line.empty() || line.front() == '#') return false;

    int vals[4];
    int col = 0;
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); i++) {
        if (i == line.size() || line[i] == ',') {
            if (col >= 4) return false;  /* too many columns */
            int v;
            if (!parse_uint_field(line.substr(start, i - start), v) || v < 0 || v > 255)
                return false;
            vals[col++] = v;
            start = i + 1;
        }
    }
    if (col != 4) return false;

    out = {static_cast<uint8_t>(vals[0]), static_cast<uint8_t>(vals[1]),
           static_cast<uint8_t>(vals[2]), static_cast<uint8_t>(vals[3])};
    return true;
}

inline bool is_color_level(std::string_view line) {
    Level l;
    return parse_level(line, l);
}

/* One colour component interpolated between two control points, transcribed
 * from Gradient::step (firmware/application/gradient.cpp):
 *     prev + (t * (val - prev) + range/2) / range
 * with the same division-by-zero guard (range clamped to >= 1). Integer maths,
 * rounds to nearest. */
inline int interp_component(int prev_val, int val, int t, int range) {
    if (range == 0) range = 1;
    return prev_val + (t * (val - prev_val) + range / 2) / range;
}

/* Builds the 256-entry colour LUT from control points, exactly as upstream's
 * Gradient does. A fresh Gradient is a single black point at index 0. */
class Gradient {
   public:
    std::array<ui::Color, 256> lut{};

    Gradient() { reset(); }

    void reset() {
        prev_index_ = prev_r_ = prev_g_ = prev_b_ = 0;
        step(0, 0, 0, 0);
    }

    /* The firmware's built-in default: black → blue → green → red. */
    void set_default() {
        reset();
        step(86, 0, 0, 255);
        step(171, 0, 255, 0);
        step(255, 255, 0, 0);
    }

    void step(int16_t index, int16_t r, int16_t g, int16_t b) {
        int16_t range = index - prev_index_;
        if (range == 0) range = 1;

        for (int16_t i = prev_index_; i <= index; i++) {
            const int t = i - prev_index_;
            const int nr = interp_component(prev_r_, r, t, range);
            const int ng = interp_component(prev_g_, g, t, range);
            const int nb = interp_component(prev_b_, b, t, range);
            if (i >= 0 && i <= 255)
                lut[i] = ui::Color(static_cast<uint8_t>(nr),
                                   static_cast<uint8_t>(ng),
                                   static_cast<uint8_t>(nb));
        }

        prev_index_ = index;
        prev_r_ = r;
        prev_g_ = g;
        prev_b_ = b;
    }

    /* Rebuilds from a list of profile lines, mirroring Gradient::load_file:
     * start at (0,0,0,0), apply every valid level in order, then flat-extend the
     * last colour out to index 255. */
    void load_levels(const std::vector<std::string>& lines) {
        reset();
        for (const auto& line : lines) {
            Level l;
            if (parse_level(line, l)) step(l.index, l.r, l.g, l.b);
        }
        step(255, prev_r_, prev_g_, prev_b_);
    }

   private:
    int16_t prev_index_ = 0;
    int16_t prev_r_ = 0;
    int16_t prev_g_ = 0;
    int16_t prev_b_ = 0;
};

/* Formats a Level back into its canonical "index,R,G,B" line. */
inline std::string level_to_string(const Level& l) {
    return to_string_dec_uint(l.index) + "," + to_string_dec_uint(l.r) + "," +
           to_string_dec_uint(l.g) + "," + to_string_dec_uint(l.b);
}

}  // namespace waterfall_designer_detail

/* Edits one "index,R,G,B" control point. Calls on_save with the new line. */
class WaterfallColorPickerView : public ui::View {
   public:
    std::function<void(const std::string&)> on_save{};

    explicit WaterfallColorPickerView(std::string color_str);

    std::string title() const override { return "Color Picker"; }
    void paint(ui::Painter& painter) override;
    void on_show() override;
    void focus() override;

   private:
    void update();
    std::string build_color_str() const;

    uint8_t index_{0};
    uint8_t red_{0};
    uint8_t green_{0};
    uint8_t blue_{0};

    ui::Labels labels_{
        {{0, 0}, "Index", ui::Theme::getInstance()->fg_light->foreground},
        {{0, 32}, "Red", ui::Theme::getInstance()->fg_light->foreground},
        {{0, 64}, "Green", ui::Theme::getInstance()->fg_light->foreground},
        {{0, 96}, "Blue", ui::Theme::getInstance()->fg_light->foreground},
        {{0, 128}, "Step", ui::Theme::getInstance()->fg_light->foreground}};

    ui::NumberField field_index_{{0, 16}, 3, {0, 255}, 1, ' '};
    ui::NumberField field_red_{{0, 48}, 3, {0, 255}, 1, ' '};
    ui::NumberField field_green_{{0, 80}, 3, {0, 255}, 1, ' '};
    ui::NumberField field_blue_{{0, 112}, 3, {0, 255}, 1, ' '};
    ui::NumberField field_step_{{0, 144}, 3, {1, 255}, 1, ' '};

    ui::Button button_save_{{0, 272, 240, 28}, "Save"};
};

/* Lists the .txt profiles in the waterfalls directory and returns the chosen
 * path through on_pick. */
class WaterfallProfilePickerView : public ui::View {
   public:
    std::function<void(const std::string&)> on_pick{};

    WaterfallProfilePickerView();

    std::string title() const override { return "Open Profile"; }
    void on_show() override;
    void focus() override;

   private:
    std::string directory_{};

    ui::Text header_{{0, 0, 240, 16}, ""};
    ui::Text empty_note_{{0, 40, 240, 48}, ""};
    ui::MenuView menu_{{0, 20, 240, 260}};
    ui::Button button_back_{{72, 280, 96, 22}, "Back"};
};

class WaterfallDesignerView : public ui::View {
   public:
    WaterfallDesignerView();

    std::string title() const override { return "Wtf Design"; }
    void paint(ui::Painter& painter) override;
    void on_show() override;
    void focus() override;

   private:
    void refresh_menu();
    void refresh_preview();
    void on_new_profile();
    void on_open_profile();
    void on_save_profile();
    void save_to(const std::string& path);
    void on_add_level();
    void on_remove_level();
    void on_edit_color();
    void set_status(const std::string& s);

    std::vector<std::string> profile_levels_{};
    std::string current_profile_path_{};
    size_t highlighted_index_{0};
    std::string filename_buffer_{};
    waterfall_designer_detail::Gradient preview_{};

    static constexpr ui::Dim preview_top = 132;
    static constexpr ui::Dim preview_height = 22;

    ui::Text header_{{0, 0, 240, 16}, "Gradient levels:"};

    ui::MenuView menu_{{0, 16, 240, 112}};

    ui::Button button_new_{{0, 158, 40, 28}, "New"};
    ui::Button button_open_{{40, 158, 40, 28}, "Open"};
    ui::Button button_save_{{80, 158, 40, 28}, "Save"};
    ui::Button button_add_{{120, 158, 40, 28}, "Add"};
    ui::Button button_del_{{160, 158, 40, 28}, "Del"};
    ui::Button button_edit_{{200, 158, 40, 28}, "Edit"};

    ui::Text text_status_{{0, 192, 240, 16}, ""};
    ui::Text text_note1_{{0, 224, 240, 16}, ""};
    ui::Text text_note2_{{0, 240, 240, 16}, ""};
    ui::Text text_note3_{{0, 256, 240, 16}, ""};
};

}  // namespace app

#endif /*__MB200_UI_WATERFALL_DESIGNER_H__*/
