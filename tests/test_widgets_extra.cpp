/*
 * mayhem-b200 — tests for the app-facing widgets in src/ui/ui_widget_extra.*.
 *
 * Expected values are derived from firmware/common/ui_widget.cpp and
 * firmware/application/ui/ui_tabview.cpp (the SymField radix/right-alignment
 * rules, the TextEdit cursor and insert/overwrite rules, the FloatField
 * clamp/loop rules and the GraphEq band weighting), not from what this port
 * happens to produce.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "display.hpp"
#include "input.hpp"
#include "ui.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <string>
#include <vector>

namespace {

/* An 8x8 box outline; the image widgets only need something non-null. */
const uint8_t test_bitmap_data[8] = {0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF};
const ui::Bitmap test_bitmap{{8, 8}, test_bitmap_data};

const uint8_t test_bitmap2_data[8] = {0x18, 0x18, 0x18, 0xFF, 0xFF, 0x18, 0x18, 0x18};
const ui::Bitmap test_bitmap2{{8, 8}, test_bitmap2_data};

/* Painting writes into the host framebuffer; give it a known state first. */
void prepare_display() {
    host::display.init();
    host::display.scroll_disable();
}

}  // namespace

/* --- SymField: construction and defaults ----------------------------------- */

TEST(symfield_dec_defaults_to_zeroes) {
    ui::SymField f{{0, 0}, 4, ui::SymField::Type::Dec};

    /* value_ is resized to `length` and every NUL is mapped to the first legal
     * symbol by ensure_all_symbols(). */
    CHECK_STR_EQ(f.to_string(), "0000");
    CHECK_EQ(f.length(), size_t{4});
    CHECK_EQ(f.to_integer(), uint64_t{0});

    /* Upstream parks the cursor on the rightmost (least significant) slot. */
    CHECK_EQ(f.selected(), size_t{3});
    CHECK(!f.editing());
}

TEST(symfield_zero_length_is_clamped_to_one) {
    ui::SymField f{{0, 0}, 0, ui::SymField::Type::Dec};

    CHECK_EQ(f.length(), size_t{1});
    CHECK_EQ(f.selected(), size_t{0});
    CHECK_STR_EQ(f.to_string(), "0");
}

TEST(symfield_symbol_lists_match_upstream) {
    ui::SymField oct{{0, 0}, 2, ui::SymField::Type::Oct};
    ui::SymField dec{{0, 0}, 2, ui::SymField::Type::Dec};
    ui::SymField hex{{0, 0}, 2, ui::SymField::Type::Hex};
    ui::SymField alpha{{0, 0}, 2, ui::SymField::Type::Alpha};

    CHECK_STR_EQ(oct.symbol_list(), "01234567");
    CHECK_STR_EQ(dec.symbol_list(), "0123456789");
    CHECK_STR_EQ(hex.symbol_list(), "0123456789ABCDEF");
    CHECK_STR_EQ(alpha.symbol_list(), " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");

    /* Alpha's first symbol is a space, so an Alpha field starts blank. */
    CHECK_STR_EQ(alpha.to_string(), "  ");
}

/* --- SymField: integer get/set --------------------------------------------- */

TEST(symfield_integer_roundtrip_per_radix) {
    ui::SymField dec{{0, 0}, 4, ui::SymField::Type::Dec};
    dec.set_value(uint64_t{1234});
    CHECK_STR_EQ(dec.to_string(), "1234");
    CHECK_EQ(dec.to_integer(), uint64_t{1234});

    ui::SymField hex{{0, 0}, 4, ui::SymField::Type::Hex};
    hex.set_value(uint64_t{0xABCD});
    CHECK_STR_EQ(hex.to_string(), "ABCD");
    CHECK_EQ(hex.to_integer(), uint64_t{0xABCD});

    ui::SymField oct{{0, 0}, 3, ui::SymField::Type::Oct};
    oct.set_value(uint64_t{8});
    CHECK_STR_EQ(oct.to_string(), "010");
    CHECK_EQ(oct.to_integer(), uint64_t{8});
}

TEST(symfield_integer_wider_than_field_keeps_low_digits) {
    /* Upstream fills right-to-left and simply runs out of slots. */
    ui::SymField f{{0, 0}, 2, ui::SymField::Type::Dec};
    f.set_value(uint64_t{345});

    CHECK_STR_EQ(f.to_string(), "45");
    CHECK_EQ(f.to_integer(), uint64_t{45});
}

TEST(symfield_integer_api_is_inert_without_a_radix) {
    /* Custom and Alpha have no radix. Upstream's "% 0" is undefined on x86, so
     * the host port makes it an explicit no-op instead. */
    ui::SymField f{{0, 0}, 3, std::string{"ABC"}};
    const std::string before = f.to_string();

    f.set_value(uint64_t{5});
    CHECK_STR_EQ(f.to_string(), before);
    CHECK_EQ(f.to_integer(), uint64_t{0});
}

/* --- SymField: string get/set and symbol clamping -------------------------- */

TEST(symfield_string_value_is_right_aligned) {
    ui::SymField f{{0, 0}, 5, ui::SymField::Type::Dec};
    f.set_value(std::string_view{"42"});

    /* The NUL padding is not a legal symbol, so it collapses to '0'. */
    CHECK_STR_EQ(f.to_string(), "00042");
    CHECK_EQ(f.to_integer(), uint64_t{42});
}

TEST(symfield_illegal_symbols_collapse_to_first_symbol) {
    ui::SymField f{{0, 0}, 3, ui::SymField::Type::Dec};
    f.set_value(std::string_view{"9Z9"});

    CHECK_STR_EQ(f.to_string(), "909");
}

TEST(symfield_overlong_string_is_rejected) {
    ui::SymField f{{0, 0}, 3, ui::SymField::Type::Dec};
    f.set_value(std::string_view{"123"});
    CHECK_STR_EQ(f.to_string(), "123");

    /* Upstream refuses rather than guessing which end to truncate. */
    f.set_value(std::string_view{"1234"});
    CHECK_STR_EQ(f.to_string(), "123");
}

TEST(symfield_changing_the_symbol_list_revalidates_every_slot) {
    /* The custom-list constructor delegates to Type::Custom (list "01"), so
     * every slot starts as '0'; swapping in "ABC" must remap them all. */
    ui::SymField f{{0, 0}, 4, std::string{"ABC"}};
    CHECK_STR_EQ(f.to_string(), "AAAA");
    CHECK_STR_EQ(f.symbol_list(), "ABC");

    f.set_symbol_list("XYZ");
    CHECK_STR_EQ(f.to_string(), "XXXX");

    /* An empty list would make the field unpaintable; upstream ignores it. */
    f.set_symbol_list("");
    CHECK_STR_EQ(f.symbol_list(), "XYZ");
    CHECK_STR_EQ(f.to_string(), "XXXX");
}

TEST(symfield_set_symbol_validates_and_bounds_check) {
    ui::SymField f{{0, 0}, 3, ui::SymField::Type::Dec};

    f.set_symbol(0, '7');
    CHECK_EQ(f.get_symbol(0), '7');

    /* 'X' is not in the decimal list. */
    f.set_symbol(1, 'X');
    CHECK_EQ(f.get_symbol(1), '0');

    /* Out-of-range index: no write, no crash. */
    f.set_symbol(99, '5');
    CHECK_STR_EQ(f.to_string(), "700");
    CHECK_EQ(f.get_symbol(99), char{0});
}

TEST(symfield_offsets_are_clamped_to_the_symbol_list) {
    ui::SymField f{{0, 0}, 3, ui::SymField::Type::Dec};
    f.set_value(std::string_view{"007"});

    CHECK_EQ(f.get_offset(2), size_t{7});
    CHECK_EQ(f.get_offset(0), size_t{0});
    /* Out-of-range index reads as offset 0. */
    CHECK_EQ(f.get_offset(50), size_t{0});

    /* An offset past the end of the symbol list is ignored, not wrapped. */
    f.set_offset(2, 99);
    CHECK_EQ(f.get_symbol(2), '7');

    f.set_offset(2, 3);
    CHECK_EQ(f.get_symbol(2), '3');
}

TEST(symfield_encoder_clamps_at_both_ends_of_the_symbol_list) {
    ui::SymField f{{0, 0}, 3, ui::SymField::Type::Dec};
    /* selected_ starts on the rightmost slot. */

    CHECK(f.on_encoder(+1));
    CHECK_STR_EQ(f.to_string(), "001");

    /* Winding far past the last symbol saturates on '9'; it must not wrap or
     * carry into the neighbouring slot. */
    f.on_encoder(+100);
    CHECK_STR_EQ(f.to_string(), "009");

    f.on_encoder(-100);
    CHECK_STR_EQ(f.to_string(), "000");
}

TEST(symfield_encoder_respects_a_custom_symbol_list_length) {
    ui::SymField f{{0, 0}, 1, std::string{"ABC"}};
    CHECK_STR_EQ(f.to_string(), "A");

    f.on_encoder(+2);
    CHECK_STR_EQ(f.to_string(), "C");

    /* Only three symbols exist, so this must stay on 'C'. */
    f.on_encoder(+5);
    CHECK_STR_EQ(f.to_string(), "C");
}

TEST(symfield_explicit_edits_gate_the_encoder) {
    ui::SymField f{{0, 0}, 3, ui::SymField::Type::Dec, /*explicit_edits=*/true};

    /* Nothing moves until the field has been Selected. */
    CHECK(!f.on_encoder(+1));
    CHECK(!f.on_key(ui::KeyEvent::Left));
    CHECK_STR_EQ(f.to_string(), "000");

    CHECK(f.on_key(ui::KeyEvent::Select));
    CHECK(f.editing());

    CHECK(f.on_encoder(+1));
    CHECK_STR_EQ(f.to_string(), "001");

    /* Select again leaves edit mode. */
    CHECK(f.on_key(ui::KeyEvent::Select));
    CHECK(!f.editing());
    CHECK(!f.on_encoder(+1));
    CHECK_STR_EQ(f.to_string(), "001");
}

TEST(symfield_left_right_walk_slots_and_decline_at_the_edges) {
    ui::SymField f{{0, 0}, 3, ui::SymField::Type::Dec};
    CHECK_EQ(f.selected(), size_t{2});

    CHECK(f.on_key(ui::KeyEvent::Left));
    CHECK_EQ(f.selected(), size_t{1});
    CHECK(f.on_key(ui::KeyEvent::Left));
    CHECK_EQ(f.selected(), size_t{0});

    /* Declining at the edge is what lets focus move to the next widget. */
    CHECK(!f.on_key(ui::KeyEvent::Left));
    CHECK_EQ(f.selected(), size_t{0});

    CHECK(f.on_key(ui::KeyEvent::Right));
    CHECK(f.on_key(ui::KeyEvent::Right));
    CHECK_EQ(f.selected(), size_t{2});
    CHECK(!f.on_key(ui::KeyEvent::Right));
}

TEST(symfield_up_down_only_edit_while_in_edit_mode) {
    ui::SymField f{{0, 0}, 2, ui::SymField::Type::Dec};

    CHECK(!f.on_key(ui::KeyEvent::Up));
    CHECK_STR_EQ(f.to_string(), "00");

    f.on_key(ui::KeyEvent::Select);  /* toggles editing_ on */
    CHECK(f.on_key(ui::KeyEvent::Up));
    CHECK_STR_EQ(f.to_string(), "01");
    CHECK(f.on_key(ui::KeyEvent::Down));
    CHECK_STR_EQ(f.to_string(), "00");
}

TEST(symfield_on_change_fires_only_on_a_real_change) {
    ui::SymField f{{0, 0}, 3, ui::SymField::Type::Dec};

    int calls = 0;
    f.on_change = [&](ui::SymField&) { calls++; };

    f.set_symbol(0, '5');
    CHECK_EQ(calls, 1);

    /* Same symbol again: no notification. */
    f.set_symbol(0, '5');
    CHECK_EQ(calls, 1);

    f.set_value(uint64_t{123});
    CHECK_EQ(calls, 2);
}

TEST(symfield_paint_covers_focus_and_edit_styling) {
    prepare_display();
    ui::Painter painter;

    ui::SymField plain{{0, 0}, 4, ui::SymField::Type::Hex};
    plain.paint(painter);

    ui::SymField explicit_field{{0, 32}, 4, ui::SymField::Type::Hex, true};
    explicit_field.focus();
    explicit_field.on_key(ui::KeyEvent::Select);
    CHECK(explicit_field.editing());
    explicit_field.paint(painter);
    explicit_field.blur();
}

/* --- TextEdit -------------------------------------------------------------- */

TEST(textedit_starts_with_the_cursor_after_the_text) {
    std::string s = "abc";
    ui::TextEdit e{s, 8, {0, 0}, 4};

    CHECK_STR_EQ(e.value(), "abc");
    CHECK_EQ(e.cursor(), uint32_t{3});
    CHECK(e.insert_mode());
}

TEST(textedit_left_right_move_the_cursor_and_decline_at_the_ends) {
    std::string s = "abc";
    ui::TextEdit e{s, 8, {0, 0}, 4};

    CHECK(e.on_key(ui::KeyEvent::Left));
    CHECK_EQ(e.cursor(), uint32_t{2});
    CHECK(e.on_key(ui::KeyEvent::Left));
    CHECK(e.on_key(ui::KeyEvent::Left));
    CHECK_EQ(e.cursor(), uint32_t{0});

    CHECK(!e.on_key(ui::KeyEvent::Left));
    CHECK_EQ(e.cursor(), uint32_t{0});

    CHECK(e.on_key(ui::KeyEvent::Right));
    CHECK(e.on_key(ui::KeyEvent::Right));
    CHECK(e.on_key(ui::KeyEvent::Right));
    CHECK_EQ(e.cursor(), uint32_t{3});

    /* The cursor may sit one past the last character, but no further. */
    CHECK(!e.on_key(ui::KeyEvent::Right));
    CHECK_EQ(e.cursor(), uint32_t{3});
}

TEST(textedit_set_cursor_clamps_to_the_text_length) {
    std::string s = "abc";
    ui::TextEdit e{s, 8, {0, 0}, 4};

    e.set_cursor(1);
    CHECK_EQ(e.cursor(), uint32_t{1});

    e.set_cursor(9999);
    CHECK_EQ(e.cursor(), uint32_t{3});
}

TEST(textedit_insert_and_delete_edit_the_bound_string) {
    std::string s = "abc";
    ui::TextEdit e{s, 8, {0, 0}, 4};

    e.set_cursor(2);
    e.char_add('X');
    CHECK_STR_EQ(s, "abXc");
    CHECK_EQ(e.cursor(), uint32_t{3});

    /* Backspace removes the character to the left of the cursor. */
    e.char_delete();
    CHECK_STR_EQ(s, "abc");
    CHECK_EQ(e.cursor(), uint32_t{2});

    /* Deleting at the very start is a no-op, not an underflow. */
    e.set_cursor(0);
    e.char_delete();
    CHECK_STR_EQ(s, "abc");
    CHECK_EQ(e.cursor(), uint32_t{0});
}

TEST(textedit_respects_max_length) {
    std::string s = "abcd";
    ui::TextEdit e{s, 5, {0, 0}, 8};

    e.char_add('e');
    CHECK_STR_EQ(s, "abcde");

    /* At max_length insertion stops. */
    e.char_add('f');
    CHECK_STR_EQ(s, "abcde");
    CHECK_EQ(e.cursor(), uint32_t{5});
}

TEST(textedit_max_length_never_shrinks_an_existing_string) {
    /* max_length_ is max(max_length, str.length()) so a long pre-existing value
     * is not silently truncated. */
    std::string s = "abcdefgh";
    ui::TextEdit e{s, 3, {0, 0}, 8};

    e.char_add('i');
    CHECK_STR_EQ(s, "abcdefgh");
}

TEST(textedit_overwrite_mode_replaces_and_stops_at_the_end) {
    std::string s = "abcd";
    ui::TextEdit e{s, 8, {0, 0}, 8};

    e.set_overwrite_mode();
    CHECK(!e.insert_mode());

    e.set_cursor(1);
    e.char_add('Z');
    CHECK_STR_EQ(s, "aZcd");
    CHECK_EQ(e.cursor(), uint32_t{2});

    /* Overwrite cannot extend the string. */
    e.set_cursor(4);
    e.char_add('Q');
    CHECK_STR_EQ(s, "aZcd");
    CHECK_EQ(e.cursor(), uint32_t{4});

    e.set_insert_mode();
    e.char_add('Q');
    CHECK_STR_EQ(s, "aZcdQ");
}

TEST(textedit_encoder_wraps_around_the_ends) {
    std::string s = "abc";
    ui::TextEdit e{s, 8, {0, 0}, 4};

    e.set_cursor(0);
    /* Below zero wraps to one-past-the-end. */
    e.on_encoder(-1);
    CHECK_EQ(e.cursor(), uint32_t{3});

    /* And past the end wraps back to zero. */
    e.on_encoder(+1);
    CHECK_EQ(e.cursor(), uint32_t{0});

    e.on_encoder(+2);
    CHECK_EQ(e.cursor(), uint32_t{2});
}

TEST(textedit_keyboard_types_and_backspaces) {
    std::string s;
    ui::TextEdit e{s, 8, {0, 0}, 8};

    CHECK(e.on_keyboard('h'));
    CHECK(e.on_keyboard('i'));
    CHECK_STR_EQ(s, "hi");

    CHECK(e.on_keyboard(8));
    CHECK_STR_EQ(s, "h");

    /* Non-printable, non-backspace keys are declined so the view can use them. */
    CHECK(!e.on_keyboard(1));
    CHECK_STR_EQ(s, "h");
}

TEST(textedit_short_select_toggles_insert_mode) {
    input::reset();  /* no key is held, so this is a short press */

    std::string s = "abc";
    ui::TextEdit e{s, 8, {0, 0}, 4};

    CHECK(e.insert_mode());
    CHECK(e.on_key(ui::KeyEvent::Select));
    CHECK(!e.insert_mode());
    CHECK(e.on_key(ui::KeyEvent::Select));
    CHECK(e.insert_mode());

    /* The text is untouched by a short press. */
    CHECK_STR_EQ(s, "abc");
}

TEST(textedit_paint_scrolls_to_keep_the_cursor_visible) {
    prepare_display();
    ui::Painter painter;

    /* Text longer than the four visible columns: paint must not read past the
     * end of the string when it shifts the window. */
    std::string s = "abcdefghij";
    ui::TextEdit e{s, 32, {0, 0}, 4};
    e.set_cursor(10);
    e.paint(painter);

    e.set_cursor(0);
    e.paint(painter);

    e.set_overwrite_mode();
    e.set_cursor(3);
    e.paint(painter);

    CHECK_STR_EQ(s, "abcdefghij");
}

/* --- TextField ------------------------------------------------------------- */

TEST(textfield_set_text_notifies_and_reads_back) {
    ui::TextField f{{0, 0, 80, 16}, "start"};

    CHECK_STR_EQ(f.get_text(), "start");

    int calls = 0;
    f.on_change = [&](ui::TextField&) { calls++; };

    f.set_text("changed");
    CHECK_STR_EQ(f.get_text(), "changed");
    CHECK_EQ(calls, 1);
}

TEST(textfield_declines_input_without_handlers) {
    ui::TextField f{{0, 0, 80, 16}, "x"};

    CHECK(!f.on_key(ui::KeyEvent::Select));
    CHECK(!f.on_encoder(+1));

    int selects = 0;
    ui::EncoderEvent last_delta = 0;
    f.on_select = [&](ui::TextField&) { selects++; };
    f.on_encoder_change = [&](ui::TextField&, ui::EncoderEvent d) { last_delta = d; };

    CHECK(f.on_key(ui::KeyEvent::Select));
    CHECK_EQ(selects, 1);
    CHECK(f.on_encoder(-3));
    CHECK_EQ(last_delta, -3);

    /* Anything that is not Select stays unhandled so focus can move. */
    CHECK(!f.on_key(ui::KeyEvent::Down));
}

/* --- FloatField (NumberField-style range handling) ------------------------- */

TEST(floatfield_clamps_to_its_range) {
    ui::FloatField f{{0, 0}, 5, {0.0f, 10.0f}, 0.5f, ' ', /*can_loop=*/false, 1};

    f.set_value(5.0f);
    CHECK_NEAR(f.value(), 5.0, 1e-6);

    f.set_value(1000.0f);
    CHECK_NEAR(f.value(), 10.0, 1e-6);

    f.set_value(-1000.0f);
    CHECK_NEAR(f.value(), 0.0, 1e-6);
}

TEST(floatfield_encoder_steps_and_saturates) {
    ui::FloatField f{{0, 0}, 5, {0.0f, 10.0f}, 0.5f, ' ', false, 1};

    f.on_encoder(+1);
    CHECK_NEAR(f.value(), 0.5, 1e-6);

    f.on_encoder(+3);
    CHECK_NEAR(f.value(), 2.0, 1e-6);

    /* Winding far below the floor must saturate, never wrap. */
    f.on_encoder(-1000);
    CHECK_NEAR(f.value(), 0.0, 1e-6);

    f.on_encoder(+1000);
    CHECK_NEAR(f.value(), 10.0, 1e-6);
}

TEST(floatfield_loops_when_configured) {
    ui::FloatField f{{0, 0}, 5, {0.0f, 10.0f}, 1.0f, ' ', /*can_loop=*/true, 1};

    f.set_value(10.0f);
    f.on_encoder(+1);
    CHECK_NEAR(f.value(), 0.0, 1e-6);

    f.on_encoder(-1);
    CHECK_NEAR(f.value(), 10.0, 1e-6);
}

TEST(floatfield_narrowing_the_range_pulls_the_value_in) {
    ui::FloatField f{{0, 0}, 5, {0.0f, 10.0f}, 1.0f, ' ', false, 1};
    f.set_value(8.0f);

    int calls = 0;
    f.on_change = [&](float) { calls++; };

    f.set_range(0.0f, 5.0f);
    CHECK_NEAR(f.value(), 5.0, 1e-6);
    /* set_range re-clamps without notifying, as upstream does. */
    CHECK_EQ(calls, 0);
}

TEST(floatfield_change_callback_fires_once_per_distinct_value) {
    ui::FloatField f{{0, 0}, 5, {0.0f, 10.0f}, 1.0f, ' ', false, 1};

    int calls = 0;
    float last = -1.0f;
    f.on_change = [&](float v) {
        calls++;
        last = v;
    };

    f.set_value(3.0f);
    CHECK_EQ(calls, 1);
    CHECK_NEAR(last, 3.0, 1e-6);

    f.set_value(3.0f);
    CHECK_EQ(calls, 1);

    f.set_value(4.0f, false);
    CHECK_EQ(calls, 1);
    CHECK_NEAR(f.value(), 4.0, 1e-6);
}

TEST(floatfield_wrap_callback_reports_direction) {
    ui::FloatField f{{0, 0}, 5, {0.0f, 10.0f}, 1.0f, ' ', true, 1};

    int wraps = 0;
    int direction = 0;
    f.on_wrap = [&](int32_t d) {
        wraps++;
        direction = d;
    };

    f.set_value(10.0f);
    f.on_encoder(+1);  /* 10 -> 0 */
    CHECK_EQ(wraps, 1);
    CHECK_EQ(direction, 1);

    f.on_encoder(-1);  /* 0 -> 10 */
    CHECK_EQ(wraps, 2);
    CHECK_EQ(direction, -1);
}

TEST(floatfield_paints_within_its_width) {
    prepare_display();
    ui::Painter painter;

    ui::FloatField f{{0, 0}, 5, {0.0f, 1000.0f}, 1.0f, '0', false, 3};
    f.set_value(123.456f);
    f.paint(painter);

    /* A value wider than the field must be clipped, not overrun it. */
    f.set_value(999.999f);
    f.paint(painter);
}

/* --- Waveform -------------------------------------------------------------- */

TEST(waveform_paints_an_empty_buffer_without_touching_memory) {
    prepare_display();
    ui::Painter painter;

    /* No data at all: the widget must still clear its rectangle. */
    ui::Waveform w{{0, 0, 100, 50}, nullptr, 0, 0, false, ui::Color::green()};
    w.paint(painter);
    CHECK_EQ(w.length(), uint32_t{0});

    /* Non-zero length with a null buffer is the dangerous case: upstream would
     * dereference it. */
    w.set_length(64);
    CHECK_EQ(w.length(), uint32_t{64});
    w.paint(painter);

    /* A real buffer with zero length must also draw nothing. */
    std::vector<int16_t> data(8, 0);
    w.set_data(data.data());
    w.set_length(0);
    w.paint(painter);
}

TEST(waveform_paints_analog_and_digital_data) {
    prepare_display();
    ui::Painter painter;

    std::vector<int16_t> data{0, 16000, -16000, 32767, -32768, 100, -100, 0};

    ui::Waveform analog{{0, 0, 120, 64}, data.data(), static_cast<uint32_t>(data.size()),
                        0, false, ui::Color::green()};
    analog.paint(painter);

    ui::Waveform digital{{0, 64, 120, 32}, data.data(), static_cast<uint32_t>(data.size()),
                         0, true, ui::Color::yellow()};
    digital.paint(painter);

    /* A single sample exercises the analog loop's degenerate case. */
    ui::Waveform one{{0, 100, 120, 32}, data.data(), 1, 0, false, ui::Color::red()};
    one.paint(painter);
}

TEST(waveform_offset_and_length_are_observable) {
    std::vector<int16_t> data(32, 0);
    ui::Waveform w{{0, 0, 100, 50}, data.data(), 16, 0, false, ui::Color::green()};

    CHECK_EQ(w.offset(), uint32_t{0});
    w.set_offset(8);
    CHECK_EQ(w.offset(), uint32_t{8});

    w.set_length(4);
    CHECK_EQ(w.length(), uint32_t{4});
}

TEST(waveform_cursors_beyond_the_pair_are_ignored) {
    prepare_display();
    ui::Painter painter;

    std::vector<int16_t> data(16, 1000);
    ui::Waveform w{{0, 0, 100, 50}, data.data(), 16, 0, false, ui::Color::green()};

    w.set_cursor(0, 10);
    w.set_cursor(1, 90);
    /* Index 2 does not exist; upstream silently drops it. */
    w.set_cursor(2, 50);

    w.paint(painter);

    /* Cursors past the right edge are clamped to the widget width. */
    w.set_cursor(1, 5000);
    w.paint(painter);
}

TEST(waveform_pause_only_responds_when_clickable) {
    prepare_display();
    ui::Painter painter;

    std::vector<int16_t> data(16, 0);

    ui::Waveform plain{{0, 0, 100, 50}, data.data(), 16, 0, false, ui::Color::green(), false};
    CHECK(!plain.is_clickable());
    CHECK(!plain.on_key(ui::KeyEvent::Select));
    CHECK(!plain.on_keyboard(32));
    CHECK(!plain.is_paused());

    ui::Waveform clickable{{0, 0, 100, 50}, data.data(), 16, 0, false, ui::Color::green(), true};
    CHECK(clickable.is_clickable());

    int selects = 0;
    clickable.on_select = [&](ui::Waveform&) { selects++; };

    CHECK(clickable.on_key(ui::KeyEvent::Select));
    CHECK(clickable.is_paused());
    CHECK_EQ(selects, 1);

    /* Painting while paused draws the placeholder, once. */
    clickable.paint(painter);
    clickable.paint(painter);

    CHECK(clickable.on_key(ui::KeyEvent::Select));
    CHECK(!clickable.is_paused());
    CHECK_EQ(selects, 2);
}

/* --- GraphEq --------------------------------------------------------------- */

TEST(grapheq_bars_follow_the_upstream_band_weighting) {
    ui::GraphEq eq{{0, 0, 240, 288}};
    eq.set_parent_rect({0, 0, 240, 288});

    ui::AudioSpectrum spectrum{};
    /* Bins 1 and 2 cover band 0 (375-750 Hz at 375 Hz per bin). */
    spectrum.db[1] = 200;
    spectrum.db[2] = 200;

    eq.update_audio_spectrum(spectrum);

    /* Band 0: avg 200, no boost, target = 200*288/255 = 225, first rise is
     * 0.8 * 225 = 180. */
    CHECK_NEAR(eq.bar_height(0), 180, 1);
    /* Band 1 averages bins 2..4 = 200/3 = 66.7, target 75, rise 60. */
    CHECK_NEAR(eq.bar_height(1), 60, 1);
    /* Nothing is present in the top band. */
    CHECK_EQ(eq.bar_height(10), ui::Dim{0});

    /* Out-of-range bars read as zero rather than trapping. */
    CHECK_EQ(eq.bar_height(99), ui::Dim{0});
}

TEST(grapheq_decays_to_silence_in_one_frame) {
    ui::GraphEq eq{{0, 0, 240, 288}};
    eq.set_parent_rect({0, 0, 240, 288});

    ui::AudioSpectrum loud{};
    loud.db.fill(255);
    eq.update_audio_spectrum(loud);
    CHECK(eq.bar_height(0) > 0);

    /* fall_speed is 1.0, so silence drops the bars immediately. */
    const ui::AudioSpectrum silence{};
    eq.update_audio_spectrum(silence);
    CHECK_EQ(eq.bar_height(0), ui::Dim{0});
}

TEST(grapheq_pause_only_responds_when_clickable) {
    ui::GraphEq plain{{0, 0, 240, 288}};
    CHECK(!plain.is_clickable());
    CHECK(!plain.on_key(ui::KeyEvent::Select));
    CHECK(!plain.is_paused());

    ui::GraphEq clickable{{0, 0, 240, 288}, true};
    int selects = 0;
    clickable.on_select = [&](ui::GraphEq&) { selects++; };

    CHECK(clickable.on_key(ui::KeyEvent::Select));
    CHECK(clickable.is_paused());
    CHECK_EQ(selects, 1);

    CHECK(clickable.on_keyboard(32));
    CHECK(!clickable.is_paused());
    CHECK_EQ(selects, 2);
}

TEST(grapheq_paints_only_once_drawn) {
    prepare_display();
    ui::Painter painter;

    ui::GraphEq eq{{0, 0, 240, 288}};
    eq.set_parent_rect({0, 0, 240, 288});

    ui::AudioSpectrum spectrum{};
    spectrum.db.fill(180);
    eq.update_audio_spectrum(spectrum);

    /* Upstream skips the whole draw for a widget that was not laid out. */
    eq.paint(painter);

    eq.drawn(true);
    eq.paint(painter);

    /* Falling bars take the "clear the vacated segments" path. */
    const ui::AudioSpectrum silence{};
    eq.update_audio_spectrum(silence);
    eq.paint(painter);
}

/* --- LiveDateTime ---------------------------------------------------------- */

TEST(livedatetime_formats_like_upstream) {
    ui::LiveDateTime dt{{0, 0, 240, 16}};
    dt.set_datetime({2026, 8, 2, 13, 45, 7});

    /* Date on, seconds off is the default. */
    CHECK_STR_EQ(dt.string(), "2026-08-02 13:45");

    dt.set_seconds_enabled(true);
    CHECK_STR_EQ(dt.string(), "2026-08-02 13:45:07");

    /* With the date hidden the time keeps its column via 11 spaces of padding. */
    dt.set_date_enabled(false);
    CHECK_STR_EQ(dt.string(), "           13:45:07");

    dt.set_hide_clock(true);
    CHECK_STR_EQ(dt.string(), "");
}

TEST(livedatetime_refreshes_from_the_system_clock) {
    ui::LiveDateTime dt{{0, 0, 240, 16}};

    /* The constructor already pulled the wall clock; the year is enough to show
     * it is a real time and not the 1970 default. */
    CHECK(dt.datetime().year >= 2020);

    dt.set_datetime({1999, 1, 1, 0, 0, 0});
    CHECK_STR_EQ(dt.string(), "1999-01-01 00:00");

    dt.on_frame_sync();
    CHECK(dt.datetime().year >= 2020);
}

TEST(livedatetime_paints) {
    prepare_display();
    ui::Painter painter;

    ui::LiveDateTime dt{{0, 0, 240, 16}};
    dt.set_datetime({2026, 12, 31, 23, 59, 59});
    dt.set_seconds_enabled(true);
    dt.paint(painter);
    CHECK_STR_EQ(dt.string(), "2026-12-31 23:59:59");
}

/* --- ButtonWithEncoder ----------------------------------------------------- */

TEST(button_with_encoder_accumulates_delta) {
    ui::ButtonWithEncoder b{{0, 0, 80, 24}, "Tune"};

    CHECK_EQ(b.get_encoder_delta(), int32_t{0});

    int changes = 0;
    b.on_change = [&] { changes++; };

    CHECK(b.on_encoder(+3));
    CHECK_EQ(b.get_encoder_delta(), int32_t{3});
    CHECK_EQ(changes, 1);

    CHECK(b.on_encoder(-1));
    CHECK_EQ(b.get_encoder_delta(), int32_t{2});
    CHECK_EQ(changes, 2);

    /* A zero delta clears the change flag without notifying. */
    CHECK(b.on_encoder(0));
    CHECK_EQ(changes, 2);

    b.set_encoder_delta(0);
    CHECK_EQ(b.get_encoder_delta(), int32_t{0});
}

TEST(button_with_encoder_select_and_dir) {
    ui::ButtonWithEncoder b{{0, 0, 80, 24}, "Go"};
    CHECK_STR_EQ(b.text(), "Go");

    /* No handler attached: the key stays unhandled. */
    CHECK(!b.on_key(ui::KeyEvent::Select));

    int selects = 0;
    b.on_select = [&](ui::ButtonWithEncoder&) { selects++; };
    CHECK(b.on_key(ui::KeyEvent::Select));
    CHECK_EQ(selects, 1);

    /* Space and Enter mirror Select on the host keyboard. */
    CHECK(b.on_keyboard(32));
    CHECK(b.on_keyboard(10));
    CHECK_EQ(selects, 3);

    ui::KeyEvent seen = ui::KeyEvent::Select;
    b.on_dir = [&](ui::ButtonWithEncoder&, ui::KeyEvent k) {
        seen = k;
        return true;
    };
    CHECK(b.on_key(ui::KeyEvent::Down));
    CHECK(seen == ui::KeyEvent::Down);

    b.set_text("Stop");
    CHECK_STR_EQ(b.text(), "Stop");
}

TEST(button_with_encoder_touch_order) {
    ui::ButtonWithEncoder instant{{0, 0, 80, 24}, "Now", true};

    std::string order;
    instant.on_touch_press = [&](ui::ButtonWithEncoder&) { order += "P"; };
    instant.on_select = [&](ui::ButtonWithEncoder&) { order += "S"; };
    instant.on_touch_release = [&](ui::ButtonWithEncoder&) { order += "R"; };

    instant.on_touch({{1, 1}, ui::TouchEvent::Type::Start});
    instant.on_touch({{1, 1}, ui::TouchEvent::Type::End});
    /* instant_exec fires on_select on press, before release. */
    CHECK_STR_EQ(order, "PSR");

    ui::ButtonWithEncoder deferred{{0, 0, 80, 24}, "Later", false};
    std::string order2;
    deferred.on_touch_press = [&](ui::ButtonWithEncoder&) { order2 += "P"; };
    deferred.on_select = [&](ui::ButtonWithEncoder&) { order2 += "S"; };
    deferred.on_touch_release = [&](ui::ButtonWithEncoder&) { order2 += "R"; };

    deferred.on_touch({{1, 1}, ui::TouchEvent::Type::Start});
    deferred.on_touch({{1, 1}, ui::TouchEvent::Type::End});
    CHECK_STR_EQ(order2, "PRS");
}

TEST(button_with_encoder_paints) {
    prepare_display();
    ui::Painter painter;

    ui::ButtonWithEncoder b{{0, 0, 80, 24}, "Tune"};
    b.paint(painter);
    b.set_highlighted(true);
    b.paint(painter);
}

/* --- NewButton ------------------------------------------------------------- */

TEST(newbutton_select_uses_a_nullary_callback) {
    ui::NewButton b{{0, 0, 60, 60}, "Menu", &test_bitmap};

    CHECK_STR_EQ(b.text(), "Menu");
    CHECK(b.bitmap() == &test_bitmap);

    CHECK(!b.on_key(ui::KeyEvent::Select));

    int selects = 0;
    b.on_select = [&] { selects++; };
    CHECK(b.on_key(ui::KeyEvent::Select));
    CHECK_EQ(selects, 1);

    /* Touch releases select too. */
    b.on_touch({{1, 1}, ui::TouchEvent::Type::Start});
    CHECK(b.highlighted());
    b.on_touch({{1, 1}, ui::TouchEvent::Type::End});
    CHECK(!b.highlighted());
    CHECK_EQ(selects, 2);
}

TEST(newbutton_direction_keys_go_to_on_dir) {
    ui::NewButton b{{0, 0, 60, 60}, "Menu", nullptr};

    CHECK(!b.on_key(ui::KeyEvent::Right));

    ui::KeyEvent seen = ui::KeyEvent::Select;
    b.on_dir = [&](ui::NewButton&, ui::KeyEvent k) {
        seen = k;
        return true;
    };
    CHECK(b.on_key(ui::KeyEvent::Right));
    CHECK(seen == ui::KeyEvent::Right);
}

TEST(newbutton_paints_all_layouts) {
    prepare_display();
    ui::Painter painter;

    /* Nothing to draw: upstream bails out before touching the framebuffer. */
    ui::NewButton empty{{0, 0, 60, 60}, "", nullptr};
    empty.paint(painter);

    ui::NewButton with_both{{0, 0, 60, 60}, "Ok", &test_bitmap};
    with_both.paint(painter);

    with_both.set_vertical_center(true);
    with_both.paint(painter);

    /* A caption wider than the button must be clipped to fit. */
    ui::NewButton narrow{{0, 64, 24, 40}, "A very long caption", nullptr};
    narrow.paint(painter);
    narrow.set_vertical_center(true);
    narrow.paint(painter);

    ui::NewButton colored{{64, 64, 60, 60}, "Hot", &test_bitmap, ui::Color::red(), true};
    colored.set_bg_color(ui::Color::dark_blue());
    colored.set_color(ui::Color::yellow());
    CHECK(colored.color().v == ui::Color::yellow().v);
    colored.paint(painter);
}

/* --- Image / ImageButton / ImageToggle ------------------------------------- */

TEST(image_colors_invert) {
    ui::Image img{{0, 0, 8, 8}, &test_bitmap, ui::Color::white(), ui::Color::black()};

    CHECK(img.foreground().v == ui::Color::white().v);
    CHECK(img.background().v == ui::Color::black().v);

    img.invert_colors();
    CHECK(img.foreground().v == ui::Color::black().v);
    CHECK(img.background().v == ui::Color::white().v);
}

TEST(image_paints_nothing_without_a_bitmap) {
    prepare_display();
    ui::Painter painter;

    ui::Image img{};
    CHECK(img.bitmap_ptr() == nullptr);
    img.paint(painter);

    img.set_bitmap(&test_bitmap);
    CHECK(img.bitmap_ptr() == &test_bitmap);
    img.paint(painter);
}

TEST(image_button_selects) {
    ui::ImageButton b{{0, 0, 8, 8}, &test_bitmap, ui::Color::white(), ui::Color::black()};

    CHECK(!b.on_key(ui::KeyEvent::Select));

    int selects = 0;
    b.on_select = [&](ui::ImageButton&) { selects++; };

    CHECK(b.on_key(ui::KeyEvent::Select));
    CHECK(b.on_keyboard(10));
    CHECK_EQ(selects, 2);

    /* Anything else is declined. */
    CHECK(!b.on_key(ui::KeyEvent::Up));
    CHECK(!b.on_keyboard('x'));
}

TEST(image_toggle_swaps_bitmap_and_colors) {
    ui::ImageToggle t{{0, 0, 8, 8},
                      &test_bitmap,   /* true  */
                      &test_bitmap2,  /* false */
                      ui::Color::green(),
                      ui::Color::black(),
                      ui::Color::red(),
                      ui::Color::white()};

    /* It starts in the false state. */
    CHECK(!t.value());
    CHECK(t.bitmap_ptr() == &test_bitmap2);
    CHECK(t.foreground().v == ui::Color::red().v);
    CHECK(t.background().v == ui::Color::white().v);

    int changes = 0;
    bool last = false;
    t.on_change = [&](bool v) {
        changes++;
        last = v;
    };

    t.set_value(true);
    CHECK(t.value());
    CHECK(last);
    CHECK_EQ(changes, 1);
    CHECK(t.bitmap_ptr() == &test_bitmap);
    CHECK(t.foreground().v == ui::Color::green().v);
    CHECK(t.background().v == ui::Color::black().v);

    /* Setting the same value again must not re-notify. */
    t.set_value(true);
    CHECK_EQ(changes, 1);

    /* Select drives the built-in toggle. */
    CHECK(t.on_key(ui::KeyEvent::Select));
    CHECK(!t.value());
    CHECK_EQ(changes, 2);
}

/* --- ImageOptionsField ----------------------------------------------------- */

TEST(image_options_field_selection_and_clamping) {
    ui::ImageOptionsField f{{0, 0, 12, 12}, ui::Color::white(), ui::Color::black(), {}};

    int changes = 0;
    size_t last_index = 99;
    ui::ImageOptionsField::value_t last_value = -1;
    f.on_change = [&](size_t i, ui::ImageOptionsField::value_t v) {
        changes++;
        last_index = i;
        last_value = v;
    };

    /* set_options forces a notification for index 0. */
    f.set_options({{&test_bitmap, 10}, {&test_bitmap2, 20}, {&test_bitmap, 30}});
    CHECK_EQ(changes, 1);
    CHECK_EQ(last_index, size_t{0});
    CHECK_EQ(last_value, 10);

    CHECK(f.on_encoder(+1));
    CHECK_EQ(f.selected_index(), size_t{1});
    CHECK_EQ(f.selected_index_value(), size_t{20});

    /* Past the last option nothing moves. */
    f.on_encoder(+10);
    CHECK_EQ(f.selected_index(), size_t{1});

    /* And below zero the unsigned wrap is rejected, so it holds at the bottom. */
    f.on_encoder(-1);
    CHECK_EQ(f.selected_index(), size_t{0});
    f.on_encoder(-1);
    CHECK_EQ(f.selected_index(), size_t{0});
}

TEST(image_options_field_set_by_value_falls_back_to_first) {
    ui::ImageOptionsField f{{0, 0, 12, 12}, ui::Color::white(), ui::Color::black(),
                            {{&test_bitmap, 10}, {&test_bitmap2, 20}}};

    f.set_by_value(20);
    CHECK_EQ(f.selected_index(), size_t{1});

    /* Upstream defaults to index 0 when the value is absent. */
    f.set_by_value(999);
    CHECK_EQ(f.selected_index(), size_t{0});
}

TEST(image_options_field_paints_when_empty) {
    prepare_display();
    ui::Painter painter;

    /* Upstream dereferences options[0] here; the host port must not. */
    ui::ImageOptionsField empty{};
    empty.paint(painter);
    CHECK_EQ(empty.selected_index_value(), size_t{0});

    ui::ImageOptionsField f{{0, 0, 12, 12}, ui::Color::white(), ui::Color::black(),
                            {{&test_bitmap, 1}}};
    f.paint(painter);

    /* A null bitmap in the option list must not be drawn. */
    f.set_options({{nullptr, 1}});
    f.paint(painter);
}

TEST(image_options_field_select_advances) {
    ui::ImageOptionsField f{{0, 0, 12, 12}, ui::Color::white(), ui::Color::black(),
                            {{&test_bitmap, 1}, {&test_bitmap2, 2}}};

    CHECK(f.on_key(ui::KeyEvent::Select));
    CHECK_EQ(f.selected_index(), size_t{1});

    CHECK(f.on_keyboard('-'));
    CHECK_EQ(f.selected_index(), size_t{0});

    CHECK(!f.on_keyboard('q'));
}

/* --- OptionTabView --------------------------------------------------------- */

namespace {

/* set_enabled is protected upstream; a subclass is how apps reach it. */
class TestOptionTab : public ui::OptionTabView {
   public:
    TestOptionTab()
        : ui::OptionTabView{{0, 0, 240, 200}} {}

    using ui::OptionTabView::set_enabled;
};

}  // namespace

TEST(option_tab_view_starts_hidden_and_disabled) {
    TestOptionTab t;

    /* Upstream hides the pane in its constructor; the TabView reveals it. */
    CHECK(t.hidden());
    CHECK(!t.is_enabled());

    t.set_enabled(true);
    CHECK(t.is_enabled());

    t.set_enabled(false);
    CHECK(!t.is_enabled());

    /* set_type only relabels the checkbox; it must not disturb the state. */
    t.set_type("CTCSS");
    CHECK(!t.is_enabled());
}

TEST(option_tab_view_focus_lands_on_the_checkbox) {
    TestOptionTab t;
    t.focus();

    /* Focus went to a child, not to the pane itself. */
    CHECK(!t.has_focus());
    CHECK_EQ(t.children().size(), size_t{1});
    CHECK(t.children()[0]->has_focus());
    t.children()[0]->blur();
}

/* --- TabView --------------------------------------------------------------- */

TEST(tabview_shows_one_view_at_a_time) {
    ui::View a{{0, 24, 240, 280}};
    ui::View b{{0, 24, 240, 280}};
    ui::View c{{0, 24, 240, 280}};

    /* Upstream's tab content hides itself in its own constructor (see
     * EncodersConfigView and OptionTabView); TabView only toggles from there,
     * so plain Views have to be put into that state by hand. */
    a.hidden(true);
    b.hidden(true);
    c.hidden(true);

    ui::TabView tabs{{{"A", ui::Color::white(), &a},
                      {"B", ui::Color::cyan(), &b},
                      {"C", ui::Color::yellow(), &c}}};

    CHECK_EQ(tabs.tab_count(), size_t{3});
    CHECK_EQ(tabs.selected(), uint32_t{0});
    CHECK_EQ(tabs.children().size(), size_t{3});

    tabs.on_show();
    CHECK(!a.hidden());
    CHECK(b.hidden());
    CHECK(c.hidden());

    tabs.set_selected(2);
    CHECK_EQ(tabs.selected(), uint32_t{2});
    CHECK(a.hidden());
    CHECK(!c.hidden());

    /* Out-of-range selections are ignored. */
    tabs.set_selected(9);
    CHECK_EQ(tabs.selected(), uint32_t{2});
    CHECK(!c.hidden());
}

TEST(tabview_caps_at_max_tabs) {
    ui::View v0{{}}, v1{{}}, v2{{}}, v3{{}}, v4{{}}, v5{{}};

    ui::TabView tabs{{{"0", ui::Color::white(), &v0},
                      {"1", ui::Color::white(), &v1},
                      {"2", ui::Color::white(), &v2},
                      {"3", ui::Color::white(), &v3},
                      {"4", ui::Color::white(), &v4},
                      {"5", ui::Color::white(), &v5}}};

    CHECK_EQ(tabs.tab_count(), ui::MAX_TABS);
    CHECK_EQ(tabs.children().size(), ui::MAX_TABS);

    /* The sixth definition was dropped, so its view is never touched. */
    CHECK(!v5.hidden());
}

TEST(tabview_with_no_tabs_is_inert) {
    ui::TabView tabs(std::initializer_list<ui::TabView::TabDef>{});

    CHECK_EQ(tabs.tab_count(), size_t{0});
    CHECK_EQ(tabs.children().size(), size_t{0});
    /* Upstream divides screen_width by the tab count here. */
    tabs.on_show();
    tabs.focus();
    tabs.set_selected(0);
}

TEST(tabview_tolerates_a_tab_with_no_view) {
    /* A TabDef whose view is null must not be dereferenced. */
    ui::TabView tabs{{{"Only", ui::Color::white(), nullptr}}};

    CHECK_EQ(tabs.tab_count(), size_t{1});
    tabs.on_show();
    tabs.focus();
    tabs.set_selected(0);

    /* There is nowhere for focus to land: the selected tab is deliberately not
     * focusable and there is no content view behind it. */
    CHECK(!tabs.children()[0]->focusable());
    CHECK(!tabs.children()[0]->has_focus());
}

TEST(tabview_selected_tab_is_not_focusable) {
    ui::View a{{0, 24, 240, 280}};
    ui::View b{{0, 24, 240, 280}};

    ui::TabView tabs{{{"A", ui::Color::white(), &a}, {"B", ui::Color::white(), &b}}};
    tabs.set_selected(0);

    const auto& kids = tabs.children();
    CHECK_EQ(kids.size(), size_t{2});
    /* The current tab is highlighted and skipped by focus traversal; the other
     * one is reachable. */
    CHECK(kids[0]->highlighted());
    CHECK(!kids[0]->focusable());
    CHECK(!kids[1]->highlighted());
    CHECK(kids[1]->focusable());
}

TEST(tabview_tab_key_and_touch_switch_tabs) {
    ui::View a{{0, 24, 240, 280}};
    ui::View b{{0, 24, 240, 280}};

    ui::TabView tabs{{{"A", ui::Color::white(), &a}, {"B", ui::Color::white(), &b}}};
    tabs.on_show();

    auto* tab_b = tabs.children()[1];
    CHECK(tab_b->on_key(ui::KeyEvent::Select));
    CHECK_EQ(tabs.selected(), uint32_t{1});
    CHECK(!b.hidden());

    auto* tab_a = tabs.children()[0];
    CHECK(tab_a->on_touch({{1, 1}, ui::TouchEvent::Type::End}));
    CHECK_EQ(tabs.selected(), uint32_t{0});
    CHECK(!a.hidden());

    /* Anything else is declined. */
    CHECK(!tab_a->on_key(ui::KeyEvent::Down));
}

TEST(tabview_paints) {
    prepare_display();
    ui::Painter painter;

    ui::View a{{0, 24, 240, 280}};
    ui::View b{{0, 24, 240, 280}};

    ui::TabView tabs{{{"Alpha", ui::Color::white(), &a},
                      {"A caption too wide for one tab", ui::Color::cyan(), &b}}};
    tabs.on_show();

    for (auto* child : tabs.children()) child->paint(painter);

    tabs.children()[1]->focus();
    tabs.children()[1]->paint(painter);
    tabs.children()[1]->blur();
}
