/*
 * mayhem-b200 — font glyph viewer.
 *
 * Ported from firmware/application/external/font_viewer/ui_font_viewer.{hpp,cpp}
 * (DebugFontsView, by Mark Thompson / zxkmm). It renders every glyph of the two
 * built-in fixed fonts (ui::font::fixed_8x16 and fixed_5x8) as a grid, lets a
 * cursor pick one glyph, and shows that glyph zoomed. No hardware is involved —
 * this draws entirely from the font tables compiled into the binary, so it works
 * unchanged on the host.
 *
 * The grid arithmetic (characters-per-line, cell → pixel position, cursor index
 * → ASCII code) is factored into font_viewer_detail so it can be unit tested
 * against upstream's formulas without a display.
 *
 * Copyright (C) 2023 Mark Thompson
 * copyleft 2024 zxkmm
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_FONT_VIEWER_H__
#define __MB200_UI_FONT_VIEWER_H__

#include "ui.hpp"
#include "ui_painter.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace app {

/* Pure grid/index maths, testable without a display. Every formula here is a
 * literal transcription of upstream's DebugFontsView::display_font(). */
namespace font_viewer_detail {

/* The highest displayed glyph offset. Upstream loops `uint8_t c = 0; c <= 0xDF`,
 * i.e. ASCII 0x20 (space) through 0xFF, which is 0xE0 = 224 glyphs. */
inline constexpr int last_glyph_index = 0xDF;

/* Characters displayed per line: a whole number of eights that leaves room for
 * the six-character row label on the left. Upstream:
 *     ((screen_width / char_width) - 6) & 0xF8
 * Returns 0 for a non-positive char width (caller must guard). */
inline int chars_per_line(int screen_w, int char_w) {
    if (char_w <= 0) return 0;
    return ((screen_w / char_w) - 6) & 0xF8;
}

/* Grid cell of the index-th glyph: row = index / cpl, col = index % cpl. */
struct GridCell {
    int row;
    int col;
};

inline GridCell grid_cell(int index, int cpl) {
    if (cpl <= 0) return {0, 0};
    return {index / cpl, index % cpl};
}

/* Top-left pixel of the index-th glyph, matching upstream:
 *     line_pos = y_offset + ((index / cpl) + 2) * char_height
 *     x        = ((index % cpl) + 5) * char_width
 * The +2 rows leave space for the font name, the +5 columns for the row label. */
inline ui::Point glyph_pos(int index, int cpl, int y_offset, int char_w, int char_h) {
    const GridCell cell = grid_cell(index, cpl);
    return {(cell.col + 5) * char_w, y_offset + (cell.row + 2) * char_h};
}

/* The ASCII code the cursor index maps to: the grid starts at 0x20. */
inline uint8_t ascii_for_index(int index) {
    return static_cast<uint8_t>(index + 0x20);
}

}  // namespace font_viewer_detail

class FontViewerView : public ui::View {
   public:
    FontViewerView();

    std::string title() const override { return "Font Viewer"; }

    void paint(ui::Painter& painter) override;
    void on_show() override;
    void focus() override;

   private:
    /* Draws one font's grid starting at y_offset; returns the y just past it so
     * the caller can stack the next font below. */
    uint16_t display_font(ui::Painter& painter,
                          uint16_t y_offset,
                          const ui::Style* font_style,
                          std::string_view font_name,
                          bool is_big_font);
    void update_address_text();
    void paint_zoomed_glyph(ui::Painter& painter);

    ui::NumberField field_cursor{
        {0, 0},
        4,
        {0, font_viewer_detail::last_glyph_index},
        1,
        ' '};

    ui::NumberField field_zoom{
        {6 * 8, 0},
        4,
        {0, 32},
        1,
        ' '};

    ui::Text text_address{
        {120, 0, 120, 16},
        "0x20"};
};

}  // namespace app

#endif /*__MB200_UI_FONT_VIEWER_H__*/
