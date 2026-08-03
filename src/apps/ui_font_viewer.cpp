/*
 * mayhem-b200 — font glyph viewer implementation.
 *
 * Copyright (C) 2023 Mark Thompson
 * copyleft 2024 zxkmm
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_font_viewer.hpp"

#include "../core/string_format.hpp"
#include "theme.hpp"
#include "ui_font_fixed_5x8.hpp"
#include "ui_font_fixed_8x16.hpp"

namespace app {

using namespace ui;
using namespace app::font_viewer_detail;

uint16_t FontViewerView::display_font(Painter& painter,
                                      uint16_t y_offset,
                                      const Style* font_style,
                                      std::string_view font_name,
                                      bool is_big_font) {
    const int char_w = font_style->font.char_width();
    const int char_h = font_style->font.line_height();
    if (char_w <= 0 || char_h <= 0) return y_offset;

    const int cpl = chars_per_line(screen_width, char_w);
    if (cpl <= 0) return y_offset;

    uint16_t line_pos = y_offset;

    painter.draw_string({0, static_cast<int>(y_offset)}, *font_style, font_name);

    const Style& highlight = is_big_font
                                 ? *Theme::getInstance()->fg_red
                                 : *Theme::getInstance()->bg_important_small;

    /* ASCII 0x20..0xFF, i.e. glyph offsets 0..0xDF. */
    for (int c = 0; c <= last_glyph_index; c++) {
        const GridCell cell = grid_cell(c, cpl);
        line_pos = static_cast<uint16_t>(y_offset + (cell.row + 2) * char_h);

        if (cell.col == 0)
            painter.draw_string({0, line_pos}, *font_style,
                                "0x" + to_string_hex(ascii_for_index(c), 2));

        const Point p{(cell.col + 5) * char_w, line_pos};
        const char glyph = static_cast<char>(ascii_for_index(c));

        if (c == field_cursor.value())
            painter.draw_char(p, highlight, glyph);
        else
            painter.draw_char(p, *font_style, glyph);
    }

    return static_cast<uint16_t>(line_pos + char_h);
}

void FontViewerView::paint(Painter& painter) {
    /* A View subclass that overrides paint() replaces the default background
     * fill, so clear the area first — otherwise the grid draws over stale
     * pixels from whatever was here before. */
    painter.fill_rectangle(screen_rect(), Theme::getInstance()->bg_darkest->background);

    uint16_t line_pos =
        display_font(painter, 32, Theme::getInstance()->bg_darkest, "Fixed 8x16", true);
    display_font(painter, line_pos + 16, Theme::getInstance()->bg_darkest_small,
                 "Fixed 5x8", false);

    paint_zoomed_glyph(painter);
}

void FontViewerView::update_address_text() {
    text_address.set("0x" + to_string_hex(ascii_for_index(field_cursor.value()), 2));
}

void FontViewerView::paint_zoomed_glyph(Painter& painter) {
    const int zoom = field_zoom.value();
    if (zoom <= 0) return;

    const char glyph = static_cast<char>(ascii_for_index(field_cursor.value()));
    painter.draw_char({screen_width / 2, screen_height / 2},
                      *Theme::getInstance()->bg_darkest, glyph,
                      static_cast<uint8_t>(zoom));
}

FontViewerView::FontViewerView() {
    add_children({&field_cursor, &field_zoom, &text_address});
    set_focusable(true);

    field_cursor.on_change = [this](int32_t) {
        update_address_text();
        set_dirty();
    };
    field_zoom.on_change = [this](int32_t) {
        set_dirty();
    };
}

void FontViewerView::on_show() {
    View::on_show();
    field_cursor.focus();
    update_address_text();
    set_dirty();
}

void FontViewerView::focus() {
    field_cursor.focus();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_font_viewer{{
    "font_viewer",
    "Font Viewer",
    app::Category::Debug,
    ui::Color::cyan(),
    &ui::bitmap_icon_debug,
    [] { return std::make_unique<app::FontViewerView>(); },
}};
}  // namespace
