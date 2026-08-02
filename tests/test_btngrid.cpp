/*
 * mayhem-b200 — icon grid menu and icon bitmap tests.
 *
 * Expected values come from the upstream implementation
 * (firmware/application/ui/ui_btngrid.cpp) and from the upstream generated
 * arrays in firmware/application/bitmap.hpp — not from what this port happens
 * to produce.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "bitmaps.hpp"
#include "display.hpp"
#include "ui.hpp"
#include "ui_btngrid.hpp"
#include "ui_painter.hpp"

#include <functional>
#include <string>
#include <vector>

namespace {

/* BtnGridView is abstract (on_populate is the subclass's job, exactly as
 * upstream). This is the smallest thing that can be instantiated. */
class TestGrid : public ui::BtnGridView {
   public:
    using ui::BtnGridView::BtnGridView;

    std::function<void()> populate{};

    void on_populate() override {
        if (populate) populate();
    }
};

ui::GridItem item(std::string text, std::function<void()> on_select = nullptr) {
    return ui::GridItem{std::move(text), ui::Color::white(), &ui::bitmap_icon_generic,
                        std::move(on_select)};
}

/* A grid two tile-rows tall: 96 / 48 = 2 rows on screen, 3 tiles per row, so
 * six tiles are visible and a longer list has to scroll. */
constexpr ui::Rect small_grid{0, 0, 240, 96};

bool bitmap_bit(const ui::Bitmap& b, int x, int y) {
    const size_t i = static_cast<size_t>(y) * static_cast<size_t>(b.size.width()) +
                     static_cast<size_t>(x);
    return (b.data[i >> 3] & (1U << (i & 7))) != 0;
}

}  // namespace

/* --- Layout ---------------------------------------------------------------- */

TEST(btngrid_layout_follows_parent_rect_and_rows) {
    TestGrid g{small_grid};

    CHECK_EQ(g.rows(), 3);
    /* (height / button_h) * rows — upstream's formula, button_h defaults to 48. */
    CHECK_EQ(g.displayed_max_items(), size_t{6});

    /* set_max_rows widens the tiles: two per row, so four fit on screen. */
    g.set_max_rows(2);
    CHECK_EQ(g.rows(), 2);
    CHECK_EQ(g.displayed_max_items(), size_t{4});

    /* A rect too short for one tile row still shows a row rather than nothing. */
    g.set_max_rows(3);
    g.set_parent_rect({0, 0, 240, 20});
    CHECK_EQ(g.displayed_max_items(), size_t{3});
}

TEST(btngrid_tiles_carry_item_text_and_bitmap) {
    TestGrid g{small_grid};
    g.add_item(item("Alpha"));
    g.add_item(item("Beta"));

    CHECK(g.item_view(0) != nullptr);
    CHECK_STR_EQ(g.item_view(0)->text(), "Alpha");
    CHECK_STR_EQ(g.item_view(1)->text(), "Beta");
    CHECK(g.item_view(0)->bitmap() == &ui::bitmap_icon_generic);

    /* Slots past the end of the list are hidden, not left showing stale text. */
    CHECK(g.item_view(2)->hidden());
    CHECK(!g.item_view(0)->hidden());

    /* Out of range is nullptr, not undefined behaviour. */
    CHECK(g.item_view(g.displayed_max_items()) == nullptr);
}

/* --- Keyboard navigation --------------------------------------------------- */

TEST(btngrid_arrow_keys_cross_row_boundaries) {
    TestGrid g{small_grid};
    for (int i = 0; i < 7; i++) g.add_item(item("i" + std::to_string(i)));

    CHECK_EQ(g.highlighted_index(), uint32_t{0});

    /* Right walks along a row and then over its end into the next row. */
    g.on_key(ui::KeyEvent::Right);
    g.on_key(ui::KeyEvent::Right);
    CHECK_EQ(g.highlighted_index(), uint32_t{2});  /* last tile of row 0 */
    CHECK(g.on_key(ui::KeyEvent::Right));
    CHECK_EQ(g.highlighted_index(), uint32_t{3});  /* first tile of row 1 */

    /* Left goes back over the same boundary. */
    CHECK(g.on_key(ui::KeyEvent::Left));
    CHECK_EQ(g.highlighted_index(), uint32_t{2});

    /* Down and Up move a whole row at a time. */
    CHECK(g.on_key(ui::KeyEvent::Down));
    CHECK_EQ(g.highlighted_index(), uint32_t{5});
    CHECK(g.on_key(ui::KeyEvent::Up));
    CHECK_EQ(g.highlighted_index(), uint32_t{2});
}

TEST(btngrid_arrow_keys_clamp_at_the_edges) {
    TestGrid g{small_grid};
    for (int i = 0; i < 7; i++) g.add_item(item("i" + std::to_string(i)));

    /* Left and Up out of the grid are declined, so the dispatcher can move
     * focus somewhere else instead. The cursor must not move. */
    CHECK(!g.on_key(ui::KeyEvent::Left));
    CHECK_EQ(g.highlighted_index(), uint32_t{0});
    CHECK(!g.on_key(ui::KeyEvent::Up));
    CHECK_EQ(g.highlighted_index(), uint32_t{0});

    /* Past the last item the request is clamped to it and accepted. */
    g.set_highlighted(6);
    CHECK(g.on_key(ui::KeyEvent::Right));
    CHECK_EQ(g.highlighted_index(), uint32_t{6});
    CHECK(g.on_key(ui::KeyEvent::Down));
    CHECK_EQ(g.highlighted_index(), uint32_t{6});
}

TEST(btngrid_keys_on_empty_grid_are_harmless) {
    TestGrid g{small_grid};

    CHECK(!g.on_key(ui::KeyEvent::Down));
    CHECK(!g.on_key(ui::KeyEvent::Right));
    CHECK_EQ(g.highlighted_index(), uint32_t{0});

    /* Select on an empty grid must not read past the end of the item list. */
    g.on_key(ui::KeyEvent::Select);
    CHECK_EQ(g.item_count(), size_t{0});
}

/* --- Encoder --------------------------------------------------------------- */

TEST(btngrid_encoder_moves_one_tile_at_a_time) {
    TestGrid g{small_grid};
    for (int i = 0; i < 7; i++) g.add_item(item("i" + std::to_string(i)));

    g.on_encoder(+1);
    CHECK_EQ(g.highlighted_index(), uint32_t{1});
    g.on_encoder(+3);
    CHECK_EQ(g.highlighted_index(), uint32_t{4});
    g.on_encoder(-2);
    CHECK_EQ(g.highlighted_index(), uint32_t{2});
}

TEST(btngrid_encoder_clamps_at_both_ends) {
    TestGrid g{small_grid};
    for (int i = 0; i < 7; i++) g.add_item(item("i" + std::to_string(i)));

    /* Winding far past the end lands on the last item and is accepted. */
    CHECK(g.on_encoder(+1000));
    CHECK_EQ(g.highlighted_index(), uint32_t{6});

    /* Winding far below zero is declined outright — upstream returns false for
     * a negative target rather than clamping — and the cursor stays put. */
    CHECK(!g.on_encoder(-1000));
    CHECK_EQ(g.highlighted_index(), uint32_t{6});

    /* Same at the bottom: one step below the first item changes nothing. */
    g.set_highlighted(0);
    CHECK(!g.on_encoder(-1));
    CHECK_EQ(g.highlighted_index(), uint32_t{0});
}

/* --- Scrolling ------------------------------------------------------------- */

TEST(btngrid_scrolls_by_whole_rows) {
    TestGrid g{small_grid};
    for (int i = 0; i < 10; i++) g.add_item(item("i" + std::to_string(i)));

    CHECK_EQ(g.displayed_max_items(), size_t{6});
    CHECK_EQ(g.offset_index(), size_t{0});

    /* Item 6 is one past the visible window: offset = 6 - 6 + 3, rounded down
     * to a whole row, i.e. 3. */
    g.set_highlighted(6);
    CHECK_EQ(g.offset_index(), size_t{3});
    CHECK_STR_EQ(g.item_view(0)->text(), "i3");

    /* The last item pins the window to the end of the list: 10 - 6 = 4. */
    g.set_highlighted(9);
    CHECK_EQ(g.offset_index(), size_t{4});
    CHECK_STR_EQ(g.item_view(0)->text(), "i4");

    /* Coming back to the top rounds down to a whole row again. */
    g.set_highlighted(0);
    CHECK_EQ(g.offset_index(), size_t{0});
    CHECK_STR_EQ(g.item_view(0)->text(), "i0");
}

TEST(btngrid_scroll_offset_snaps_to_the_start_of_a_row) {
    TestGrid g{small_grid};
    for (int i = 0; i < 12; i++) g.add_item(item("i" + std::to_string(i)));

    /* Reaching item 7 needs the window to move by 7 - 6 + 3 = 4, which is not a
     * whole number of rows; upstream rounds it back down to 3. Skipping that
     * would leave the top row starting mid-row, so Up and Down would step
     * diagonally through the list. */
    g.set_highlighted(7);
    CHECK_EQ(g.offset_index(), size_t{3});
    CHECK_EQ(g.offset_index() % static_cast<size_t>(g.rows()), size_t{0});
    CHECK_STR_EQ(g.item_view(0)->text(), "i3");
    CHECK_STR_EQ(g.item_view(4)->text(), "i7");
}

TEST(btngrid_paging_moves_the_window_and_the_cursor) {
    TestGrid g{small_grid};
    for (int i = 0; i < 10; i++) g.add_item(item("i" + std::to_string(i)));

    g.set_highlighted(0);
    CHECK(g.arrow_down_enabled);
    CHECK(!g.arrow_up_enabled);

    /* One page down: max offset is 10 - 6 = 4, and the cursor was off the new
     * page so it moves to the first item on it. */
    g.page_down();
    CHECK_EQ(g.offset_index(), size_t{4});
    CHECK_EQ(g.highlighted_index(), uint32_t{4});

    /* Back up. Item 4 is still on the new page (0..5), so only the window
     * moves and the cursor is left alone. */
    g.page_up();
    CHECK_EQ(g.offset_index(), size_t{0});
    CHECK_EQ(g.highlighted_index(), uint32_t{4});

    /* And down again — item 4 is on that page too (4..9). */
    g.page_down();
    CHECK_EQ(g.offset_index(), size_t{4});
    CHECK_EQ(g.highlighted_index(), uint32_t{4});

    /* Paging again cannot move the window, so it drops the cursor on the last
     * item instead. */
    g.page_down();
    CHECK_EQ(g.offset_index(), size_t{4});
    CHECK_EQ(g.highlighted_index(), uint32_t{9});
    CHECK(!g.arrow_down_enabled);

    /* Now the cursor is off the page we are moving to, so it lands on the last
     * item of that page (0 + 6 - 1), not on the item it was on. */
    g.page_up();
    CHECK_EQ(g.offset_index(), size_t{0});
    CHECK_EQ(g.highlighted_index(), uint32_t{5});
}

TEST(btngrid_arrows_track_the_ends_of_the_list) {
    TestGrid g{small_grid};

    /* Empty list: neither arrow, and no underflow computing "size - 1". */
    g.show_hide_arrows();
    CHECK(!g.arrow_up_enabled);
    CHECK(!g.arrow_down_enabled);

    for (int i = 0; i < 4; i++) g.add_item(item("i" + std::to_string(i)));

    g.set_highlighted(0);
    CHECK(!g.arrow_up_enabled);
    CHECK(g.arrow_down_enabled);

    g.set_highlighted(3);
    CHECK(g.arrow_up_enabled);
    CHECK(!g.arrow_down_enabled);
}

/* --- Selection ------------------------------------------------------------- */

TEST(btngrid_select_invokes_the_highlighted_callback) {
    TestGrid g{small_grid};

    std::vector<int> fired;
    for (int i = 0; i < 6; i++) g.add_item(item("i" + std::to_string(i), [&fired, i] { fired.push_back(i); }));

    g.set_highlighted(4);
    g.on_key(ui::KeyEvent::Select);

    CHECK_EQ(fired.size(), size_t{1});
    CHECK_EQ(fired.at(0), 4);

    /* select_highlighted() is the same action by another name. */
    g.set_highlighted(1);
    g.select_highlighted();
    CHECK_EQ(fired.size(), size_t{2});
    CHECK_EQ(fired.at(1), 1);
}

TEST(btngrid_select_on_item_without_callback_is_a_no_op) {
    TestGrid g{small_grid};
    g.add_item(item("no handler"));

    /* Must not throw or call through a null std::function. */
    g.on_key(ui::KeyEvent::Select);
    CHECK_EQ(g.highlighted_index(), uint32_t{0});
}

TEST(btngrid_tile_select_runs_the_items_callback) {
    TestGrid g{small_grid};

    int fired = -1;
    g.add_item(item("a", [&fired] { fired = 0; }));
    g.add_item(item("b", [&fired] { fired = 1; }));

    /* The tile widgets carry the callback, which is how touch and the focused
     * tile's Select key both reach the item. */
    g.item_view(1)->on_select();
    CHECK_EQ(fired, 1);
}

/* --- add / insert / clear -------------------------------------------------- */

TEST(btngrid_insert_item_places_an_item_at_an_index) {
    TestGrid g{small_grid};
    g.add_item(item("A"));
    g.add_item(item("B"));
    g.add_item(item("C"));

    g.insert_item(item("X"), 1);

    CHECK_EQ(g.item_count(), size_t{4});
    CHECK_STR_EQ(g.item_view(0)->text(), "A");
    CHECK_STR_EQ(g.item_view(1)->text(), "X");
    CHECK_STR_EQ(g.item_view(2)->text(), "B");
    CHECK_STR_EQ(g.item_view(3)->text(), "C");
}

TEST(btngrid_insert_item_at_or_past_the_end_appends) {
    TestGrid g{small_grid};
    g.add_item(item("A"));
    g.add_item(item("B"));

    /* position == size() is not "before the last", it is "append". */
    g.insert_item(item("Y"), 2);
    /* And so is anything beyond it. */
    g.insert_item(item("Z"), 99);

    CHECK_EQ(g.item_count(), size_t{4});
    CHECK_STR_EQ(g.item_view(2)->text(), "Y");
    CHECK_STR_EQ(g.item_view(3)->text(), "Z");
}

TEST(btngrid_insert_item_at_zero_becomes_the_first_item) {
    TestGrid g{small_grid};
    g.add_item(item("A"));
    g.insert_item(item("..") , 0);

    CHECK_STR_EQ(g.item_view(0)->text(), "..");
    CHECK_STR_EQ(g.item_view(1)->text(), "A");
}

TEST(btngrid_add_items_and_clear) {
    TestGrid g{small_grid};
    g.add_items({item("A"), item("B"), item("C")});
    CHECK_EQ(g.item_count(), size_t{3});

    g.clear();
    CHECK_EQ(g.item_count(), size_t{0});
    /* clear() drops the tile widgets too; the grid rebuilds them on reload. */
    CHECK(g.item_view(0) == nullptr);

    g.populate = [&g] { g.add_item(item("fresh")); };
    g.reload_items();
    CHECK_EQ(g.item_count(), size_t{1});
    CHECK(g.item_view(0) != nullptr);
    CHECK_STR_EQ(g.item_view(0)->text(), "fresh");
}

TEST(btngrid_reload_repopulates_from_on_populate) {
    TestGrid g{small_grid};

    int calls = 0;
    g.populate = [&g, &calls] {
        calls++;
        g.add_item(item("one"));
        g.add_item(item("two"));
    };

    g.reload_items();
    CHECK_EQ(calls, 1);
    CHECK_EQ(g.item_count(), size_t{2});

    /* Reloading replaces the list rather than appending to it. */
    g.reload_items();
    CHECK_EQ(calls, 2);
    CHECK_EQ(g.item_count(), size_t{2});
}

/* --- Blacklist ------------------------------------------------------------- */

TEST(btngrid_blacklisted_items_are_dropped) {
    ui::set_blacklist("Games\nDebug\n");

    CHECK(ui::is_blacklisted("Games"));
    CHECK(ui::is_blacklisted("Debug"));
    /* A prefix of a listed name is not itself listed — the commas guard that. */
    CHECK(!ui::is_blacklisted("Game"));
    CHECK(!ui::is_blacklisted("Audio"));

    TestGrid g{small_grid};
    g.add_items({item("Audio"), item("Games"), item("Debug"), item("Search")});
    CHECK_EQ(g.item_count(), size_t{2});
    CHECK_STR_EQ(g.item_view(0)->text(), "Audio");
    CHECK_STR_EQ(g.item_view(1)->text(), "Search");

    /* insert_item honours it too. */
    g.insert_item(item("Games"), 0);
    CHECK_EQ(g.item_count(), size_t{2});

    ui::set_blacklist("");
    CHECK(!ui::is_blacklisted("Games"));
}

/* --- Painting -------------------------------------------------------------- */

TEST(btngrid_tile_paints_its_icon_to_the_framebuffer) {
    host::display.init();
    host::display.scroll_disable();

    TestGrid g{small_grid};
    g.set_menu_color(ui::Color::grey());
    g.add_item(ui::GridItem{"Alpha", ui::Color::white(), &ui::bitmap_icon_generic, nullptr});

    ui::Painter painter;
    g.item_view(0)->paint(painter);

    /* Tile 0 is 240/3 = 80 wide and 48 tall at the origin. With the icon and a
     * line of text stacked and centred, the 16x16 bitmap starts at x = 32,
     * y = 5: content is 16 + 16 high, the 16px slack gives max(4, 16/3) = 5 of
     * spacing, so the block is 37 tall and sits 5 px down. */
    std::vector<ui::ColorRGB888> px(1);

    /* Icon pixel (2,1) is set in bitmap_icon_generic's top border. */
    host::display.read_pixels({34, 6, 1, 1}, px);
    CHECK_EQ(static_cast<int>(px[0].r), 0xF8);
    CHECK_EQ(static_cast<int>(px[0].g), 0xFC);
    CHECK_EQ(static_cast<int>(px[0].b), 0xF8);

    /* Icon pixel (0,0) is clear, so it takes the tile's background colour. */
    host::display.read_pixels({32, 5, 1, 1}, px);
    CHECK_EQ(static_cast<int>(px[0].r), 0x78);
    CHECK_EQ(static_cast<int>(px[0].g), 0x7C);
    CHECK_EQ(static_cast<int>(px[0].b), 0x78);
}

/* --- Icon bitmaps ---------------------------------------------------------- */

TEST(bitmaps_table_is_complete_and_well_formed) {
    const auto* table = ui::icon_table();
    const size_t count = ui::icon_table_size();

    CHECK(count >= 31);  /* the 30 registry icons plus the fallback */

    for (size_t i = 0; i < count; i++) {
        const auto& e = table[i];
        const std::string where = std::string{"icon "} + (e.name ? e.name : "<null>");

        CHECK(e.name != nullptr);
        CHECK(e.bitmap != nullptr);
        if (e.bitmap == nullptr) continue;

        /* A null data pointer would draw garbage or crash. */
        CHECK(e.bitmap->data != nullptr);

        /* The declared size must account for exactly the bytes stored. */
        const size_t bits = static_cast<size_t>(e.bitmap->size.width()) *
                            static_cast<size_t>(e.bitmap->size.height());
        CHECK_EQ((bits + 7) / 8, e.data_size);

        /* Every menu icon upstream is 16x16; anything else would misalign the
         * tile layout, which centres on that size. */
        CHECK_EQ(e.bitmap->size.width(), 16);
        CHECK_EQ(e.bitmap->size.height(), 16);
    }
}

TEST(bitmaps_registry_icons_are_all_present) {
    static const char* const names[] = {
        "receivers", "transmit", "transceivers", "scanner", "capture", "replay",
        "looking", "utilities", "games", "setup", "adsb", "ais", "aprs",
        "speaker", "btle", "pocsag", "sonde", "search", "remote", "thermometer",
        "rds", "touchtunes", "microphone", "dir", "freqman", "trim", "notepad",
        "debug", "peripherals_details", "previous", "generic"};

    for (const char* n : names) {
        const ui::Bitmap* b = ui::icon_by_name(n);
        CHECK(b != nullptr);
        if (b != nullptr) CHECK(b->data != nullptr);
    }

    CHECK(ui::icon_by_name("no_such_icon") == nullptr);
    CHECK(ui::icon_by_name("") == nullptr);
    CHECK(ui::icon_by_name("adsb") == &ui::bitmap_icon_adsb);
    CHECK(ui::icon_by_name("previous") == &ui::bitmap_icon_previous);
}

TEST(bitmaps_pixels_match_upstream_art) {
    /* bitmap_icon_previous is a left-pointing arrow with a solid two-pixel
     * shaft: rows 7 and 8 are entirely set, row 0 is entirely clear, and the
     * arrow head sits at x=6..7 on row 1. Straight off upstream's array. */
    for (int x = 0; x < 16; x++) {
        CHECK(!bitmap_bit(ui::bitmap_icon_previous, x, 0));
        CHECK(bitmap_bit(ui::bitmap_icon_previous, x, 7));
        CHECK(bitmap_bit(ui::bitmap_icon_previous, x, 8));
    }
    CHECK(bitmap_bit(ui::bitmap_icon_previous, 6, 1));
    CHECK(bitmap_bit(ui::bitmap_icon_previous, 7, 1));
    CHECK(!bitmap_bit(ui::bitmap_icon_previous, 5, 1));
    CHECK(!bitmap_bit(ui::bitmap_icon_previous, 8, 1));

    /* bitmap_icon_adsb is an aircraft seen from above: a two-pixel-wide nose at
     * x=7..8 on row 0, and full-width wings on rows 8 and 9. */
    CHECK(bitmap_bit(ui::bitmap_icon_adsb, 7, 0));
    CHECK(bitmap_bit(ui::bitmap_icon_adsb, 8, 0));
    CHECK(!bitmap_bit(ui::bitmap_icon_adsb, 6, 0));
    for (int x = 0; x < 16; x++) {
        CHECK(bitmap_bit(ui::bitmap_icon_adsb, x, 8));
        CHECK(bitmap_bit(ui::bitmap_icon_adsb, x, 9));
    }
}
