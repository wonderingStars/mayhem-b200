/*
 * mayhem-b200 — UI layer tests.
 *
 * Covers the geometry and drawing helpers, the widget behaviours apps depend
 * on, and the display's scroll-region arithmetic — the last of which must match
 * the ILI9341's exactly or ported waterfall code draws in the wrong place.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "display.hpp"
#include "file_path.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_menu.hpp"
#include "ui_spectrum.hpp"
#include "ui_widget.hpp"

#include <vector>

/* --- Geometry -------------------------------------------------------------- */

TEST(rect_contains_is_half_open) {
    ui::Rect r{10, 10, 20, 20};
    CHECK(r.contains({10, 10}));
    CHECK(r.contains({29, 29}));
    /* The right and bottom edges are exclusive. */
    CHECK(!r.contains({30, 20}));
    CHECK(!r.contains({20, 30}));
    CHECK(!r.contains({9, 10}));
}

TEST(rect_intersect) {
    ui::Rect a{0, 0, 100, 100};
    ui::Rect b{50, 50, 100, 100};
    const auto i = a.intersect(b);
    CHECK_EQ(i.left(), 50);
    CHECK_EQ(i.top(), 50);
    CHECK_EQ(i.width(), 50);
    CHECK_EQ(i.height(), 50);

    CHECK(a.intersect({200, 200, 10, 10}).is_empty());
}

TEST(color_rgb565_roundtrip) {
    ui::Color white = ui::Color::white();
    CHECK_EQ(static_cast<int>(white.r()), 0xF8);
    CHECK_EQ(static_cast<int>(white.g()), 0xFC);
    CHECK_EQ(static_cast<int>(white.b()), 0xF8);

    ui::Color black = ui::Color::black();
    CHECK_EQ(static_cast<int>(black.v), 0);
}

/* --- Display --------------------------------------------------------------- */

TEST(display_fill_and_read_back) {
    host::display.init();
    host::display.scroll_disable();

    host::display.fill_rectangle({10, 10, 4, 4}, ui::Color::red());

    std::vector<ui::ColorRGB888> pixels(16);
    host::display.read_pixels({10, 10, 4, 4}, pixels);

    for (const auto& p : pixels) {
        CHECK_EQ(static_cast<int>(p.r), 0xF8);
        CHECK_EQ(static_cast<int>(p.g), 0);
        CHECK_EQ(static_cast<int>(p.b), 0);
    }
}

TEST(display_clips_offscreen_draws) {
    host::display.init();
    /* Must not write out of bounds; a crash here is the failure mode. */
    host::display.fill_rectangle({-50, -50, 20, 20}, ui::Color::green());
    host::display.fill_rectangle({230, 310, 100, 100}, ui::Color::green());
    host::display.draw_pixel({-1, -1}, ui::Color::blue());
    host::display.draw_pixel({1000, 1000}, ui::Color::blue());

    /* The partially visible rect should have coloured the visible corner. */
    std::vector<ui::ColorRGB888> pixel(1);
    host::display.read_pixels({239, 319, 1, 1}, pixel);
    CHECK_EQ(static_cast<int>(pixel[0].g), 0xFC);
}

TEST(display_scroll_area_y_matches_ili9341_arithmetic) {
    host::display.init();
    host::display.scroll_set_area(100, 300);  /* 200-line region */

    /* At position 0 the mapping is the identity, offset by the region top. */
    CHECK_EQ(host::display.scroll_area_y(0), 100);
    CHECK_EQ(host::display.scroll_area_y(50), 150);
    /* Offsets wrap within the region. */
    CHECK_EQ(host::display.scroll_area_y(200), 100);
    CHECK_EQ(host::display.scroll_area_y(250), 150);

    host::display.scroll_disable();
}

TEST(display_scroll_advances_and_wraps) {
    host::display.init();
    host::display.scroll_set_area(0, 10);  /* small region keeps the maths clear */

    /* scroll(1) steps the origin back one line and returns the row that the
     * newest line should be written to. */
    const auto first = host::display.scroll(1);
    CHECK_EQ(first, 9);
    const auto second = host::display.scroll(1);
    CHECK_EQ(second, 8);

    /* Ten more steps must come back around rather than run off the region. */
    for (int i = 0; i < 8; i++) host::display.scroll(1);
    const auto wrapped = host::display.scroll(1);
    CHECK(wrapped >= 0 && wrapped < 10);

    host::display.scroll_disable();
}

TEST(display_scroll_disable_restores_identity) {
    host::display.init();
    host::display.scroll_set_area(0, 100);
    host::display.scroll(5);
    host::display.scroll_disable();
    CHECK_EQ(host::display.scroll_area_y(42), 42);
}

/* --- Widgets --------------------------------------------------------------- */

TEST(number_field_clamps_and_steps) {
    ui::NumberField f{{0, 0}, 3, {0, 99}, 5, ' '};

    f.set_value(50);
    CHECK_EQ(f.value(), 50);

    f.on_encoder(+1);
    CHECK_EQ(f.value(), 55);

    f.set_value(1000);
    CHECK_EQ(f.value(), 99);

    f.set_value(-1000);
    CHECK_EQ(f.value(), 0);

    /* Stepping down from the floor must stay at the floor, not underflow. */
    f.on_encoder(-1);
    CHECK_EQ(f.value(), 0);
}

TEST(number_field_loops_when_configured) {
    ui::NumberField f{{0, 0}, 2, {0, 9}, 1, ' ', true};

    f.set_value(9);
    f.on_encoder(+1);
    CHECK_EQ(f.value(), 0);

    f.on_encoder(-1);
    CHECK_EQ(f.value(), 9);
}

TEST(number_field_change_callback_fires_once) {
    ui::NumberField f{{0, 0}, 3, {0, 99}, 1, ' '};

    int calls = 0;
    int last = -1;
    f.on_change = [&](int32_t v) {
        calls++;
        last = v;
    };

    f.set_value(42);
    CHECK_EQ(calls, 1);
    CHECK_EQ(last, 42);

    /* Setting the same value again must not re-fire. */
    f.set_value(42);
    CHECK_EQ(calls, 1);

    /* Suppressed updates must not fire at all. */
    f.set_value(7, false);
    CHECK_EQ(calls, 1);
}

TEST(options_field_selection_and_lookup) {
    ui::OptionsField o{{0, 0}, 6, {{"one", 1}, {"two", 2}, {"three", 3}}};

    CHECK_EQ(o.selected_index(), size_t{0});
    CHECK_EQ(o.selected_index_value(), 1);

    CHECK(o.set_by_value(3));
    CHECK_EQ(o.selected_index(), size_t{2});
    CHECK_STR_EQ(o.selected_index_name(), "three");

    /* An absent value leaves the selection untouched. */
    CHECK(!o.set_by_value(99));
    CHECK_EQ(o.selected_index(), size_t{2});

    /* Encoder movement is clamped to the ends. */
    o.on_encoder(+10);
    CHECK_EQ(o.selected_index(), size_t{2});
    o.on_encoder(-10);
    CHECK_EQ(o.selected_index(), size_t{0});
}

TEST(options_field_nearest_value) {
    ui::OptionsField o{{0, 0}, 8, {{"1M", 1000000}, {"2M", 2000000}, {"4M", 4000000}}};

    o.set_by_nearest_value(2400000);
    CHECK_EQ(o.selected_index_value(), 2000000);

    o.set_by_nearest_value(3900000);
    CHECK_EQ(o.selected_index_value(), 4000000);
}

TEST(options_field_empty_is_safe) {
    ui::OptionsField o{{0, 0}, 4, {}};
    CHECK_STR_EQ(o.selected_index_name(), "");
    CHECK_EQ(o.selected_index_value(), 0);
    CHECK(!o.on_encoder(+1));
}

TEST(checkbox_toggles_and_notifies) {
    ui::Checkbox c{{0, 0}, 5, "Test"};

    bool observed = false;
    int calls = 0;
    c.on_select = [&](ui::Checkbox&, bool v) {
        observed = v;
        calls++;
    };

    CHECK(!c.value());
    c.on_key(ui::KeyEvent::Select);
    CHECK(c.value());
    CHECK(observed);
    CHECK_EQ(calls, 1);

    c.on_key(ui::KeyEvent::Select);
    CHECK(!c.value());
    CHECK_EQ(calls, 2);
}

TEST(frequency_field_steps_and_clamps) {
    ui::FrequencyField f{{0, 0}};
    f.set_range(70'000'000, 6'000'000'000ull);
    f.set_value(100'000'000);

    /* Default step is 25 kHz. */
    CHECK_EQ(f.step(), int64_t{25'000});
    f.on_encoder(+1);
    CHECK_EQ(f.value(), uint64_t{100'025'000});

    /* Winding far below the floor must saturate, never wrap. */
    f.on_encoder(-100000);
    CHECK_EQ(f.value(), uint64_t{70'000'000});

    /* And above the ceiling. */
    f.on_encoder(1000000);
    CHECK_EQ(f.value(), uint64_t{6'000'000'000ull});
}

TEST(frequency_field_select_cycles_step) {
    ui::FrequencyField f{{0, 0}};
    const auto before = f.step_index();
    f.on_key(ui::KeyEvent::Select);
    CHECK(f.step_index() != before);

    /* Cycling all the way round returns to the start. */
    for (size_t i = 1; i < ui::FrequencyField::step_count; i++)
        f.on_key(ui::KeyEvent::Select);
    CHECK_EQ(f.step_index(), before);
}

TEST(menu_view_navigates_and_selects) {
    ui::MenuView m{{0, 0, 240, 240}};

    int selected = -1;
    m.add_item({"A", ui::Color::white(), [&] { selected = 0; }});
    m.add_item({"B", ui::Color::white(), [&] { selected = 1; }});
    m.add_item({"C", ui::Color::white(), [&] { selected = 2; }});

    CHECK_EQ(m.item_count(), size_t{3});
    CHECK_EQ(m.highlighted_index(), size_t{0});

    m.on_key(ui::KeyEvent::Down);
    CHECK_EQ(m.highlighted_index(), size_t{1});

    m.on_key(ui::KeyEvent::Select);
    CHECK_EQ(selected, 1);

    /* Up at the top and Down at the bottom are declined so focus can move on. */
    m.set_highlighted(0);
    CHECK(!m.on_key(ui::KeyEvent::Up));
    m.set_highlighted(2);
    CHECK(!m.on_key(ui::KeyEvent::Down));
}

TEST(menu_view_encoder_clamps) {
    ui::MenuView m{{0, 0, 240, 240}};
    m.add_item({"A", ui::Color::white(), nullptr});
    m.add_item({"B", ui::Color::white(), nullptr});

    m.on_encoder(+50);
    CHECK_EQ(m.highlighted_index(), size_t{1});
    m.on_encoder(-50);
    CHECK_EQ(m.highlighted_index(), size_t{0});
}

TEST(widget_hidden_state_and_dirty) {
    ui::Rectangle r{{0, 0, 10, 10}, ui::Color::red()};
    CHECK(!r.hidden());

    r.set_clean();
    CHECK(!r.dirty());

    r.hidden(true);
    CHECK(r.hidden());

    r.hidden(false);
    CHECK(!r.hidden());
    CHECK(r.dirty());
}

TEST(view_child_management) {
    ui::View v{{0, 0, 240, 240}};
    ui::Rectangle a{{0, 0, 10, 10}, ui::Color::red()};
    ui::Rectangle b{{0, 20, 10, 10}, ui::Color::blue()};

    v.add_children({&a, &b});
    CHECK_EQ(v.children().size(), size_t{2});
    CHECK_EQ(a.parent(), &v);

    v.remove_child(&a);
    CHECK_EQ(v.children().size(), size_t{1});
    CHECK(a.parent() == nullptr);

    /* Adding a widget that already has a parent must be refused, not silently
     * corrupt the first parent's child list. */
    ui::View other{{0, 0, 240, 240}};
    other.add_child(&b);
    CHECK_EQ(other.children().size(), size_t{0});
    CHECK_EQ(b.parent(), &v);
}

TEST(widget_screen_pos_accumulates_parents) {
    ui::View outer{{10, 20, 200, 200}};
    ui::View inner{{5, 6, 100, 100}};
    ui::Rectangle leaf{{1, 2, 10, 10}, ui::Color::red()};

    outer.add_child(&inner);
    inner.add_child(&leaf);

    CHECK_EQ(leaf.screen_pos().x(), 16);
    CHECK_EQ(leaf.screen_pos().y(), 28);
}

/* --- Spectrum helpers ------------------------------------------------------ */

TEST(bins_to_columns_peak_decimates) {
    /* Four bins into two columns: each column reports the louder of its pair,
     * so a narrow carrier survives at a wide span. */
    const std::vector<float> db{-100.0f, -20.0f, -90.0f, -95.0f};
    std::vector<float> out;
    ui::spectrum::bins_to_columns(db, 2, out);

    CHECK_EQ(out.size(), size_t{2});
    CHECK_NEAR(out[0], -20.0, 1e-4);
    CHECK_NEAR(out[1], -90.0, 1e-4);
}

TEST(bins_to_columns_interpolates_when_upscaling) {
    const std::vector<float> db{0.0f, 10.0f};
    std::vector<float> out;
    ui::spectrum::bins_to_columns(db, 3, out);

    CHECK_EQ(out.size(), size_t{3});
    CHECK_NEAR(out[0], 0.0, 1e-4);
    CHECK_NEAR(out[1], 5.0, 1e-4);
    CHECK_NEAR(out[2], 10.0, 1e-4);
}

TEST(bins_to_columns_handles_empty_input) {
    std::vector<float> out;
    ui::spectrum::bins_to_columns({}, 8, out);
    CHECK_EQ(out.size(), size_t{8});
}

TEST(db_to_index_spans_the_palette) {
    CHECK_EQ(static_cast<int>(ui::spectrum::db_to_index(-100.0f, -100.0f, -20.0f)), 0);
    CHECK_EQ(static_cast<int>(ui::spectrum::db_to_index(-20.0f, -100.0f, -20.0f)), 255);
    /* Out-of-range values clamp rather than wrapping to the wrong colour. */
    CHECK_EQ(static_cast<int>(ui::spectrum::db_to_index(-200.0f, -100.0f, -20.0f)), 0);
    CHECK_EQ(static_cast<int>(ui::spectrum::db_to_index(50.0f, -100.0f, -20.0f)), 255);
    /* Midpoint lands mid-palette. */
    const int mid = ui::spectrum::db_to_index(-60.0f, -100.0f, -20.0f);
    CHECK(mid > 120 && mid < 136);
}

/* --- Paths ----------------------------------------------------------------- */

TEST(data_directories_are_distinct_and_nonempty) {
    const auto data = core::data_directory();
    CHECK(!data.empty());
    CHECK(core::captures_directory() != core::freqman_directory());
    CHECK(core::captures_directory().find("CAPTURES") != std::string::npos);
    CHECK(core::freqman_directory().find("FREQMAN") != std::string::npos);
}
