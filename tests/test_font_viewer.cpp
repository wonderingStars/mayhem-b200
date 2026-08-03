/*
 * mayhem-b200 — font viewer grid-maths tests.
 *
 * Expected values are transcribed from upstream's DebugFontsView::display_font
 * (firmware/application/external/font_viewer/ui_font_viewer.cpp):
 *
 *     cpl      = ((screen_width / char_width) - 6) & 0xF8
 *     line_pos = y_offset + ((c / cpl) + 2) * char_height
 *     x        = ((c % cpl) + 5) * char_width
 *     ascii    = c + 0x20
 *
 * plus the two built-in fonts' declared cell sizes, so the cpl inputs the app
 * feeds are themselves verified.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui.hpp"
#include "ui_font_fixed_5x8.hpp"
#include "ui_font_fixed_8x16.hpp"
#include "ui_font_viewer.hpp"

using namespace app::font_viewer_detail;

/* --- chars per line -------------------------------------------------------- */

TEST(font_cpl_matches_upstream_formula) {
    /* 8x16 on a 240-wide screen: (240/8 - 6) & 0xF8 = 24 & 0xF8 = 24. */
    CHECK_EQ(chars_per_line(240, 8), 24);
    /* 5x8: (240/5 - 6) & 0xF8 = 42 & 0xF8 = 40. */
    CHECK_EQ(chars_per_line(240, 5), 40);
    /* A wide font drops below 8/line but is masked down to a multiple of 8:
     * (240/16 - 6) & 0xF8 = 9 & 0xF8 = 8. */
    CHECK_EQ(chars_per_line(240, 16), 8);
}

TEST(font_cpl_guards_bad_width) {
    CHECK_EQ(chars_per_line(240, 0), 0);
    CHECK_EQ(chars_per_line(240, -4), 0);
}

/* --- grid cell ------------------------------------------------------------- */

TEST(font_grid_cell_row_col) {
    /* cpl = 24. First glyph is top-left. */
    CHECK_EQ(grid_cell(0, 24).row, 0);
    CHECK_EQ(grid_cell(0, 24).col, 0);

    /* Last column of the first row. */
    CHECK_EQ(grid_cell(23, 24).row, 0);
    CHECK_EQ(grid_cell(23, 24).col, 23);

    /* First glyph of the second row. */
    CHECK_EQ(grid_cell(24, 24).row, 1);
    CHECK_EQ(grid_cell(24, 24).col, 0);

    /* The last displayed glyph, 0xDF = 223: 223/24 = 9, 223%24 = 7. */
    CHECK_EQ(grid_cell(last_glyph_index, 24).row, 9);
    CHECK_EQ(grid_cell(last_glyph_index, 24).col, 7);
}

TEST(font_grid_cell_guards_bad_cpl) {
    CHECK_EQ(grid_cell(5, 0).row, 0);
    CHECK_EQ(grid_cell(5, 0).col, 0);
}

/* --- pixel position -------------------------------------------------------- */

TEST(font_glyph_pos_matches_upstream) {
    /* Glyph 0, cpl 24, y_offset 32, 8x16 cell.
     * x = (0+5)*8 = 40; y = 32 + (0+2)*16 = 64. */
    const ui::Point p0 = glyph_pos(0, 24, 32, 8, 16);
    CHECK_EQ(p0.x(), 40);
    CHECK_EQ(p0.y(), 64);

    /* Glyph 24 wraps to row 1: x = 40; y = 32 + (1+2)*16 = 80. */
    const ui::Point p24 = glyph_pos(24, 24, 32, 8, 16);
    CHECK_EQ(p24.x(), 40);
    CHECK_EQ(p24.y(), 80);

    /* Glyph 25: x = (1+5)*8 = 48; y = 80. */
    const ui::Point p25 = glyph_pos(25, 24, 32, 8, 16);
    CHECK_EQ(p25.x(), 48);
    CHECK_EQ(p25.y(), 80);

    /* Small font: cpl 40, y_offset 200, 5x8 cell. Glyph 40 -> row 1.
     * x = (0+5)*5 = 25; y = 200 + (1+2)*8 = 224. */
    const ui::Point ps = glyph_pos(40, 40, 200, 5, 8);
    CHECK_EQ(ps.x(), 25);
    CHECK_EQ(ps.y(), 224);
}

/* --- ascii mapping --------------------------------------------------------- */

TEST(font_ascii_for_index) {
    CHECK_EQ(int{ascii_for_index(0)}, 0x20);
    CHECK_EQ(int{ascii_for_index(0x40)}, 0x60);
    CHECK_EQ(int{ascii_for_index(last_glyph_index)}, 0xFF);
}

/* --- built-in font cell sizes (the cpl inputs the app actually feeds) ------- */

TEST(font_builtin_cell_sizes) {
    CHECK_EQ(ui::font::fixed_8x16.char_width(), ui::Dim{8});
    CHECK_EQ(ui::font::fixed_8x16.line_height(), ui::Dim{16});
    CHECK_EQ(ui::font::fixed_5x8.char_width(), ui::Dim{5});
    CHECK_EQ(ui::font::fixed_5x8.line_height(), ui::Dim{8});
}

TEST(font_builtin_glyph_lookup) {
    /* A printable glyph reports the font's cell size and non-null pixels. */
    const auto g = ui::font::fixed_8x16.glyph('A');
    CHECK_EQ(g.w(), 8);
    CHECK_EQ(g.h(), 16);
    CHECK(g.pixels() != nullptr);
}
