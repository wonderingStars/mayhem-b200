/*
 * mayhem-b200 — tests for the decoder result table, text entry and modals.
 *
 * Expected values come from upstream's implementation
 * (firmware/application/recent_entries.hpp, ui_textentry.cpp, ui_alphanum.cpp,
 * ui_navigation.cpp), not from what this port happens to produce.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui.hpp"
#include "ui_alphanum.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"
#include "ui_recent_entries.hpp"

#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

/* The minimum an entry type has to provide: a Key, an invalid_key sentinel, a
 * construct-from-key constructor and key(). Modelled on AISRecentEntry. */
struct TestEntry {
    using Key = uint32_t;

    static constexpr Key invalid_key = 0xffffffffu;

    uint32_t id{0};
    size_t count{0};

    TestEntry() = default;
    explicit TestEntry(Key k)
        : id{k} {}

    Key key() const { return id; }
};

using TestEntries = ui::RecentEntries<TestEntry>;

std::vector<uint32_t> keys_of(const TestEntries& entries) {
    std::vector<uint32_t> out;
    for (const auto& e : entries) out.push_back(e.key());
    return out;
}

/* Walks the cursor to the bottom of the table, whatever the length. */
void cursor_to_bottom(ui::RecentEntriesTable<TestEntries>& table, size_t count) {
    for (size_t i = 0; i < count; i++) table.advance(1);
}

}  // namespace

/* --- Container: ordering --------------------------------------------------- */

TEST(recent_entries_are_ordered_newest_first) {
    TestEntries entries;

    ui::on_packet(entries, 1u);
    ui::on_packet(entries, 2u);
    ui::on_packet(entries, 3u);

    CHECK_EQ(entries.size(), size_t{3});
    CHECK_EQ(entries.front().key(), 3u);
    CHECK_EQ(entries.back().key(), 1u);

    const auto keys = keys_of(entries);
    CHECK_EQ(keys[0], 3u);
    CHECK_EQ(keys[1], 2u);
    CHECK_EQ(keys[2], 1u);
}

TEST(recent_entries_new_entry_is_constructed_from_its_key) {
    TestEntries entries;
    auto& entry = ui::on_packet(entries, 42u);

    CHECK_EQ(entry.id, 42u);
    CHECK_EQ(entry.count, size_t{0});
    /* The reference must be the front of the list — that is what callers
     * update after the call. */
    CHECK(&entry == &entries.front());
}

/* --- Container: find-or-create --------------------------------------------- */

TEST(recent_entries_existing_key_is_updated_not_duplicated) {
    TestEntries entries;

    auto& first = ui::on_packet(entries, 7u);
    first.count = 5;

    ui::on_packet(entries, 8u);
    CHECK_EQ(entries.size(), size_t{2});
    CHECK_EQ(entries.front().key(), 8u);

    auto& again = ui::on_packet(entries, 7u);

    /* Same key seen again: one entry, contents preserved, moved to the front. */
    CHECK_EQ(entries.size(), size_t{2});
    CHECK_EQ(again.key(), 7u);
    CHECK_EQ(again.count, size_t{5});
    CHECK_EQ(entries.front().key(), 7u);
    CHECK_EQ(entries.back().key(), 8u);
}

TEST(recent_entries_find_locates_by_key_and_reports_absence) {
    TestEntries entries;
    ui::on_packet(entries, 10u);
    ui::on_packet(entries, 20u);

    auto found = ui::find_entry(entries, 10u);
    CHECK(found != entries.end());
    CHECK_EQ(found->key(), 10u);

    CHECK(ui::find_entry(entries, 30u) == entries.end());

    /* The upstream spelling resolves to the same thing. */
    CHECK(ui::find(entries, 20u) != entries.end());

    const TestEntries& const_entries = entries;
    CHECK(ui::find_entry(const_entries, 20u) != const_entries.end());
    CHECK(ui::find_entry(const_entries, 99u) == const_entries.end());
}

/* --- Container: eviction ---------------------------------------------------- */

TEST(recent_entries_evict_oldest_past_the_default_maximum) {
    TestEntries entries;

    /* Upstream's default bound is 64 and it drops from the back, so after 70
     * distinct keys the oldest six are gone. */
    for (uint32_t i = 1; i <= 70; i++) ui::on_packet(entries, i);

    CHECK_EQ(entries.size(), size_t{64});
    CHECK_EQ(entries.front().key(), 70u);
    CHECK_EQ(entries.back().key(), 7u);
    CHECK(ui::find_entry(entries, 6u) == entries.end());
    CHECK(ui::find_entry(entries, 7u) != entries.end());
}

TEST(recent_entries_evict_honours_an_explicit_maximum) {
    TestEntries entries;

    for (uint32_t i = 1; i <= 10; i++) ui::on_packet(entries, i, 4);

    CHECK_EQ(entries.size(), size_t{4});
    CHECK_EQ(entries.front().key(), 10u);
    CHECK_EQ(entries.back().key(), 7u);

    /* Re-seeing a key only reorders; nothing is evicted because nothing is
     * added. */
    ui::on_packet(entries, 8u, 4);
    CHECK_EQ(entries.size(), size_t{4});
    CHECK_EQ(entries.front().key(), 8u);
    CHECK_EQ(entries.back().key(), 7u);
}

TEST(truncate_entries_trims_from_the_back_only) {
    TestEntries entries;
    for (uint32_t i = 1; i <= 5; i++) ui::on_packet(entries, i);

    ui::truncate_entries(entries, 3);
    CHECK_EQ(entries.size(), size_t{3});
    CHECK_EQ(entries.front().key(), 5u);
    CHECK_EQ(entries.back().key(), 3u);

    /* A limit above the size does nothing. */
    ui::truncate_entries(entries, 100);
    CHECK_EQ(entries.size(), size_t{3});

    /* Zero empties it. */
    ui::truncate_entries(entries, 0);
    CHECK_EQ(entries.size(), size_t{0});
}

/* --- Container: the scroll window ------------------------------------------- */

TEST(range_around_centres_the_window_on_the_item) {
    TestEntries entries;
    for (uint32_t i = 1; i <= 10; i++) ui::on_packet(entries, i);
    /* Front is key 10, back is key 1. */

    auto item = std::begin(entries);
    std::advance(item, 5);  // key 5

    const auto range = ui::range_around(entries, item, 4);
    CHECK_EQ(std::distance(range.first, range.second), std::ptrdiff_t{4});
    /* Walks back count/2 first, then forward: keys 7,6,5,4. */
    CHECK_EQ(range.first->key(), 7u);
}

TEST(range_around_fills_forward_when_the_item_is_at_the_top) {
    TestEntries entries;
    for (uint32_t i = 1; i <= 10; i++) ui::on_packet(entries, i);

    const auto range = ui::range_around(entries, std::begin(entries), 4);
    CHECK_EQ(std::distance(range.first, range.second), std::ptrdiff_t{4});
    CHECK_EQ(range.first->key(), 10u);
}

TEST(range_around_stops_at_the_end_of_a_short_list) {
    TestEntries entries;
    ui::on_packet(entries, 1u);
    ui::on_packet(entries, 2u);

    const auto range = ui::range_around(entries, std::begin(entries), 10);
    CHECK_EQ(std::distance(range.first, range.second), std::ptrdiff_t{2});
}

TEST(range_around_is_empty_for_an_empty_list_or_zero_count) {
    TestEntries empty;
    const auto r1 = ui::range_around(empty, std::begin(empty), 8);
    CHECK(r1.first == r1.second);

    TestEntries entries;
    ui::on_packet(entries, 1u);
    const auto r2 = ui::range_around(entries, std::begin(entries), 0);
    CHECK(r2.first == r2.second);
}

/* --- Table: cursor ---------------------------------------------------------- */

TEST(table_cursor_on_an_empty_table_stays_invalid) {
    TestEntries entries;
    ui::RecentEntriesColumns columns{{"Key", 6}, {"Count", 5}};
    ui::RecentEntriesTable<TestEntries> table{entries, columns};

    CHECK_EQ(table.selected_key(), TestEntry::invalid_key);

    /* Re-validating an empty table must not walk off begin()/end(). */
    table.advance(0);
    CHECK_EQ(table.selected_key(), TestEntry::invalid_key);

    table.advance(-1);
    table.advance(1);
    CHECK_EQ(table.selected_key(), TestEntry::invalid_key);
    CHECK(table.selected_entry() == nullptr);

    /* Upstream dereferences front()/back() here and crashes. All three keys
     * have to be declined so focus can move somewhere useful. */
    CHECK(!table.on_key(ui::KeyEvent::Up));
    CHECK(!table.on_key(ui::KeyEvent::Down));
    CHECK(!table.on_key(ui::KeyEvent::Select));

    /* The encoder is always consumed, but must not select anything. */
    CHECK(table.on_encoder(1));
    CHECK_EQ(table.selected_key(), TestEntry::invalid_key);
}

TEST(table_cursor_clamps_at_both_ends_of_a_full_table) {
    TestEntries entries;
    for (uint32_t i = 1; i <= 5; i++) ui::on_packet(entries, i);
    /* Order is 5,4,3,2,1. */

    ui::RecentEntriesColumns columns{{"Key", 6}};
    ui::RecentEntriesTable<TestEntries> table{entries, columns};

    table.advance(0);
    CHECK_EQ(table.selected_key(), 5u);

    /* At the top: Up is declined and the cursor does not move. */
    CHECK(!table.on_key(ui::KeyEvent::Up));
    table.advance(-1);
    CHECK_EQ(table.selected_key(), 5u);

    CHECK(table.on_key(ui::KeyEvent::Down));
    CHECK_EQ(table.selected_key(), 4u);

    cursor_to_bottom(table, 10);
    CHECK_EQ(table.selected_key(), 1u);

    /* At the bottom: Down is declined and advancing past the end stays put
     * rather than wrapping to the front. */
    CHECK(!table.on_key(ui::KeyEvent::Down));
    table.advance(1);
    CHECK_EQ(table.selected_key(), 1u);

    CHECK(table.selected_entry() != nullptr);
    CHECK_EQ(table.selected_entry()->key(), 1u);
}

TEST(table_cursor_recovers_when_its_entry_is_evicted) {
    TestEntries entries;
    for (uint32_t i = 1; i <= 4; i++) ui::on_packet(entries, i, 4);

    ui::RecentEntriesColumns columns{{"Key", 6}};
    ui::RecentEntriesTable<TestEntries> table{entries, columns};

    table.advance(0);
    cursor_to_bottom(table, 4);
    CHECK_EQ(table.selected_key(), 1u);

    /* Key 1 is the oldest, so a fifth key pushes it out. */
    ui::on_packet(entries, 9u, 4);
    CHECK(table.selected_entry() == nullptr);

    table.advance(0);
    CHECK_EQ(table.selected_key(), 9u);
    CHECK(table.selected_entry() != nullptr);
}

TEST(table_select_reports_the_entry_under_the_cursor) {
    TestEntries entries;
    ui::on_packet(entries, 11u);
    ui::on_packet(entries, 22u);

    ui::RecentEntriesColumns columns{{"Key", 6}};
    ui::RecentEntriesTable<TestEntries> table{entries, columns};

    uint32_t reported = 0;
    int calls = 0;
    table.on_select = [&](const TestEntry& e) {
        reported = e.key();
        calls++;
    };

    table.advance(0);
    CHECK(table.on_key(ui::KeyEvent::Select));
    CHECK_EQ(calls, 1);
    CHECK_EQ(reported, 22u);

    table.advance(1);
    CHECK(table.on_key(ui::KeyEvent::Select));
    CHECK_EQ(calls, 2);
    CHECK_EQ(reported, 11u);
}

TEST(table_selected_key_can_be_set_directly) {
    TestEntries entries;
    for (uint32_t i = 1; i <= 3; i++) ui::on_packet(entries, i);

    ui::RecentEntriesColumns columns{{"Key", 6}};
    ui::RecentEntriesTable<TestEntries> table{entries, columns};

    table.set_selected_key(2u);
    CHECK(table.selected_entry() != nullptr);
    CHECK_EQ(table.selected_entry()->key(), 2u);

    /* Setting a key that is not present leaves the cursor dangling until it is
     * re-validated. */
    table.set_selected_key(77u);
    CHECK(table.selected_entry() == nullptr);
    table.advance(0);
    CHECK_EQ(table.selected_key(), 3u);
}

/* --- Columns and view layout ------------------------------------------------ */

TEST(columns_hold_names_and_widths) {
    ui::RecentEntriesColumns columns{{"MMSI", 9}, {"Name", 0}, {"Cnt", 3}};

    CHECK_EQ(columns.size(), size_t{3});
    CHECK_STR_EQ(columns.at(0).first, "MMSI");
    CHECK_EQ(columns.at(0).second, size_t{9});

    columns.set(0, "ID", 4);
    CHECK_STR_EQ(columns.at(0).first, "ID");
    CHECK_EQ(columns.at(0).second, size_t{4});

    /* Out of range is ignored, not undefined. */
    columns.set(9, "nope", 1);
    CHECK_EQ(columns.size(), size_t{3});
}

TEST(view_gives_the_leftover_width_to_the_first_zero_width_column) {
    /* 240 px / 8 = 30 character cells. Fixed columns take 9 + 3, plus two
     * inter-column spaces, so 30 - 14 = 16 are left. */
    ui::RecentEntriesColumns columns{{"MMSI", 9}, {"Name", 0}, {"Cnt", 3}};
    TestEntries entries;

    ui::RecentEntriesView<TestEntries> view{columns, entries};

    CHECK_EQ(columns.at(0).second, size_t{9});
    CHECK_EQ(columns.at(1).second, size_t{16});
    CHECK_EQ(columns.at(2).second, size_t{3});

    /* The header takes 16 px; the table gets the rest. */
    view.set_parent_rect({0, 0, 240, 288});
    CHECK_EQ(view.table().parent_rect().top(), 16);
    CHECK_EQ(view.table().parent_rect().height(), 272);
}

TEST(view_only_expands_the_first_zero_width_column) {
    ui::RecentEntriesColumns columns{{"A", 0}, {"B", 0}};
    TestEntries entries;

    ui::RecentEntriesView<TestEntries> view{columns, entries};
    (void)view;

    /* 30 cells, no fixed widths, one inter-column space: 29 to the first. */
    CHECK_EQ(columns.at(0).second, size_t{29});
    CHECK_EQ(columns.at(1).second, size_t{0});
}

TEST(view_forwards_selection_from_its_table) {
    TestEntries entries;
    ui::on_packet(entries, 5u);

    ui::RecentEntriesColumns columns{{"Key", 6}};
    ui::RecentEntriesView<TestEntries> view{columns, entries};

    uint32_t reported = 0;
    view.on_select = [&](const TestEntry& e) { reported = e.key(); };

    view.table().advance(0);
    CHECK(view.table().on_key(ui::KeyEvent::Select));
    CHECK_EQ(reported, 5u);
}

/* --- EntryTextEdit --------------------------------------------------------------- */

TEST(text_edit_inserts_at_the_cursor_and_respects_max_length) {
    std::string s = "abc";
    ui::EntryTextEdit edit{s, 5, {0, 0}, 10};

    /* The cursor starts at the end of the existing text. */
    CHECK_EQ(edit.cursor(), 3u);

    edit.set_cursor(99);
    CHECK_EQ(edit.cursor(), 3u);  // clamped to the length

    edit.set_cursor(1);
    edit.char_add('X');
    CHECK_STR_EQ(s, "aXbc");
    CHECK_EQ(edit.cursor(), 2u);

    edit.char_add('Y');
    CHECK_STR_EQ(s, "aXYbc");

    /* At max_length nothing more goes in. */
    edit.char_add('Z');
    CHECK_STR_EQ(s, "aXYbc");
    CHECK_EQ(edit.cursor(), 3u);
}

TEST(text_edit_max_length_never_shrinks_the_callers_string) {
    std::string s = "abcdefgh";
    ui::EntryTextEdit edit{s, 3, {0, 0}};

    /* Upstream takes max(max_length, str.length()) so an over-long initial
     * value is editable rather than immediately frozen. */
    CHECK_EQ(edit.max_length(), size_t{8});
}

TEST(text_edit_delete_removes_before_the_cursor) {
    std::string s = "abc";
    ui::EntryTextEdit edit{s, 10, {0, 0}};

    edit.char_delete();
    CHECK_STR_EQ(s, "ab");
    CHECK_EQ(edit.cursor(), 2u);

    edit.set_cursor(0);
    edit.char_delete();
    CHECK_STR_EQ(s, "ab");  // nothing before position 0
    CHECK_EQ(edit.cursor(), 0u);
}

TEST(text_edit_overwrite_mode_replaces_and_stops_at_the_end) {
    std::string s = "abc";
    ui::EntryTextEdit edit{s, 10, {0, 0}};

    edit.set_overwrite_mode();
    CHECK(!edit.insert_mode());

    edit.set_cursor(0);
    edit.char_add('Q');
    CHECK_STR_EQ(s, "Qbc");
    CHECK_EQ(edit.cursor(), 1u);

    /* Past the end there is nothing to overwrite. */
    edit.set_cursor(3);
    edit.char_add('W');
    CHECK_STR_EQ(s, "Qbc");

    edit.set_insert_mode();
    edit.char_add('W');
    CHECK_STR_EQ(s, "QbcW");
}

TEST(text_edit_encoder_wraps_the_cursor) {
    std::string s = "abc";
    ui::EntryTextEdit edit{s, 10, {0, 0}};

    CHECK(edit.on_encoder(+1));
    CHECK_EQ(edit.cursor(), 0u);  // past the end wraps to the start

    CHECK(edit.on_encoder(-1));
    CHECK_EQ(edit.cursor(), 3u);  // before the start wraps to the end
}

TEST(text_edit_arrow_keys_move_within_the_text) {
    std::string s = "ab";
    ui::EntryTextEdit edit{s, 10, {0, 0}};

    CHECK(edit.on_key(ui::KeyEvent::Left));
    CHECK_EQ(edit.cursor(), 1u);
    CHECK(edit.on_key(ui::KeyEvent::Left));
    CHECK_EQ(edit.cursor(), 0u);

    /* Declined at the ends so the focus manager can move focus away. */
    CHECK(!edit.on_key(ui::KeyEvent::Left));

    CHECK(edit.on_key(ui::KeyEvent::Right));
    CHECK(edit.on_key(ui::KeyEvent::Right));
    CHECK_EQ(edit.cursor(), 2u);
    CHECK(!edit.on_key(ui::KeyEvent::Right));
}

TEST(text_edit_back_deletes_until_the_field_is_empty) {
    std::string s = "hi";
    ui::EntryTextEdit edit{s, 10, {0, 0}};

    /* The window layer folds Backspace into KeyEvent::Back. */
    CHECK(edit.on_key(ui::KeyEvent::Back));
    CHECK_STR_EQ(s, "h");
    CHECK(edit.on_key(ui::KeyEvent::Back));
    CHECK_STR_EQ(s, "");

    /* Empty: declined, so Esc still closes the view. */
    CHECK(!edit.on_key(ui::KeyEvent::Back));
}

TEST(text_edit_keyboard_accepts_printable_and_backspace_only) {
    std::string s;
    ui::EntryTextEdit edit{s, 8, {0, 0}};

    CHECK(edit.on_keyboard(' '));   // 0x20, the low end of printable
    CHECK(edit.on_keyboard('~'));   // 0x7e, the high end
    CHECK_STR_EQ(s, " ~");

    CHECK(edit.on_keyboard(8));  // backspace
    CHECK_STR_EQ(s, " ");

    /* Control codes and anything above ASCII are declined. */
    CHECK(!edit.on_keyboard(0x1f));
    CHECK(!edit.on_keyboard(0x7f));
    CHECK(!edit.on_keyboard(0xe9));
    CHECK_STR_EQ(s, " ");
}

/* --- AlphanumView ----------------------------------------------------------- */

TEST(alphanum_accepts_a_character_and_a_backspace) {
    ui::NavigationView nav{{0, 0, 240, 304}};
    std::string s;
    ui::AlphanumView view{nav, s, 8, ENTER_KEYBOARD_MODE_ALPHA};

    CHECK(view.on_keyboard('h'));
    CHECK(view.on_keyboard('i'));
    CHECK_STR_EQ(s, "hi");
    CHECK_STR_EQ(view.value(), "hi");

    CHECK(view.on_keyboard(8));
    CHECK_STR_EQ(s, "h");

    CHECK(!view.on_keyboard(0x03));
    CHECK_STR_EQ(s, "h");
}

TEST(alphanum_stops_at_max_length_and_at_an_empty_field) {
    ui::NavigationView nav{{0, 0, 240, 304}};
    std::string s;
    ui::AlphanumView view{nav, s, 4, ENTER_KEYBOARD_MODE_ALPHA};

    for (int i = 0; i < 20; i++) view.on_keyboard('x');
    CHECK_EQ(s.length(), size_t{4});
    CHECK_STR_EQ(s, "xxxx");

    for (int i = 0; i < 20; i++) view.on_keyboard(8);
    CHECK_STR_EQ(s, "");

    /* Backspacing an empty field is a no-op, not an underflow. */
    CHECK(view.on_keyboard(8));
    CHECK_STR_EQ(s, "");
}

TEST(alphanum_enter_mode_selects_the_right_key_set) {
    ui::NavigationView nav{{0, 0, 240, 304}};
    std::string s;

    ui::AlphanumView alpha{nav, s, 8, ENTER_KEYBOARD_MODE_ALPHA};
    CHECK_EQ(alpha.mode(), 0u);

    ui::AlphanumView digits{nav, s, 8, ENTER_KEYBOARD_MODE_DIGITS};
    CHECK_EQ(digits.mode(), 1u);

    /* Symbols are the shifted digit set, so the key set is still 1. */
    ui::AlphanumView symbols{nav, s, 8, ENTER_KEYBOARD_MODE_SYMBOLS};
    CHECK_EQ(symbols.mode(), 1u);

    ui::AlphanumView hex{nav, s, 8, ENTER_KEYBOARD_MODE_HEX};
    CHECK_EQ(hex.mode(), 2u);

    /* An out-of-range mode falls back to letters rather than reading past the
     * end of the key set table. */
    ui::AlphanumView bogus{nav, s, 8, 200};
    CHECK_EQ(bogus.mode(), 0u);
}

TEST(alphanum_cursor_position_controls_where_text_lands) {
    ui::NavigationView nav{{0, 0, 240, 304}};
    std::string s = "ac";
    ui::AlphanumView view{nav, s, 8, ENTER_KEYBOARD_MODE_ALPHA};

    view.set_cursor(1);
    CHECK(view.on_keyboard('b'));
    CHECK_STR_EQ(s, "abc");
}

TEST(text_prompt_pushes_an_entry_view_and_reports_on_confirm) {
    ui::NavigationView nav{{0, 0, 240, 304}};
    /* A base view, so the prompt is not the root — the root is never popped. */
    nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    CHECK(nav.service());
    CHECK_EQ(nav.depth(), size_t{1});

    std::string s = "ab";
    std::string reported;
    int calls = 0;

    ui::text_prompt(nav, s, 8, ENTER_KEYBOARD_MODE_ALPHA, [&](std::string& v) {
        reported = v;
        calls++;
    });

    CHECK(nav.service());
    CHECK_EQ(nav.depth(), size_t{2});

    auto* view = dynamic_cast<ui::AlphanumView*>(nav.top());
    CHECK(view != nullptr);
    if (view == nullptr) return;

    CHECK_STR_EQ(view->title(), "Text entry");
    CHECK(view->on_keyboard('c'));
    CHECK_STR_EQ(s, "abc");

    view->confirm();
    CHECK_EQ(calls, 1);
    CHECK_STR_EQ(reported, "abc");

    CHECK(nav.service());
    CHECK_EQ(nav.depth(), size_t{1});
}

/* --- Modal dialog ----------------------------------------------------------- */

TEST(wrap_message_splits_on_newlines) {
    const auto lines = ui::wrap_message("one\ntwo\nthree", 30);
    CHECK_EQ(lines.size(), size_t{3});
    CHECK_STR_EQ(lines[0], "one");
    CHECK_STR_EQ(lines[1], "two");
    CHECK_STR_EQ(lines[2], "three");
}

TEST(wrap_message_handles_empty_and_trailing_newlines) {
    const auto empty = ui::wrap_message("", 30);
    CHECK_EQ(empty.size(), size_t{1});
    CHECK_STR_EQ(empty[0], "");

    const auto trailing = ui::wrap_message("a\n", 30);
    CHECK_EQ(trailing.size(), size_t{2});
    CHECK_STR_EQ(trailing[0], "a");
    CHECK_STR_EQ(trailing[1], "");

    /* CRLF text must not leave a stray carriage return glyph. */
    const auto crlf = ui::wrap_message("a\r\nb", 30);
    CHECK_EQ(crlf.size(), size_t{2});
    CHECK_STR_EQ(crlf[0], "a");
    CHECK_STR_EQ(crlf[1], "b");
}

TEST(wrap_message_breaks_at_spaces_and_mid_word_when_it_must) {
    const auto words = ui::wrap_message("one two three", 7);
    CHECK_EQ(words.size(), size_t{2});
    CHECK_STR_EQ(words[0], "one two");
    CHECK_STR_EQ(words[1], "three");

    /* A word longer than the dialog has to be cut. */
    const auto longword = ui::wrap_message("abcdefghij", 4);
    CHECK_EQ(longword.size(), size_t{3});
    CHECK_STR_EQ(longword[0], "abcd");
    CHECK_STR_EQ(longword[1], "efgh");
    CHECK_STR_EQ(longword[2], "ij");

    /* Zero width means "do not wrap". */
    const auto unwrapped = ui::wrap_message("a very long line indeed", 0);
    CHECK_EQ(unwrapped.size(), size_t{1});
}

TEST(modal_info_pops_itself_and_reports_ok) {
    ui::NavigationView nav{{0, 0, 240, 304}};
    nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    nav.service();

    int calls = 0;
    bool answer = false;
    ui::display_modal(nav, "Error", "It broke", ui::INFO, [&](bool v) {
        calls++;
        answer = v;
    });
    nav.service();

    CHECK_EQ(nav.depth(), size_t{2});
    auto* modal = dynamic_cast<ui::ModalMessageView*>(nav.top());
    CHECK(modal != nullptr);
    if (modal == nullptr) return;

    CHECK_STR_EQ(modal->title(), "Error");
    CHECK_STR_EQ(modal->message(), "It broke");
    CHECK_EQ(static_cast<int>(modal->type()), static_cast<int>(ui::INFO));

    modal->choose(true);
    nav.service();

    CHECK_EQ(calls, 1);
    CHECK(answer);
    CHECK_EQ(nav.depth(), size_t{1});
}

TEST(modal_yesno_reports_both_answers) {
    ui::NavigationView nav{{0, 0, 240, 304}};
    nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    nav.service();

    bool answered = false;
    bool answer = true;
    ui::display_modal(nav, "Delete?", "Erase the capture?", ui::YESNO, [&](bool v) {
        answered = true;
        answer = v;
    });
    nav.service();

    auto* modal = dynamic_cast<ui::ModalMessageView*>(nav.top());
    CHECK(modal != nullptr);
    if (modal == nullptr) return;

    modal->choose(false);
    nav.service();

    CHECK(answered);
    CHECK(!answer);
    CHECK_EQ(nav.depth(), size_t{1});
}

TEST(modal_abort_pops_the_view_underneath_as_well) {
    ui::NavigationView nav{{0, 0, 240, 304}};
    nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));  // root
    nav.service();
    nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));  // the failing app
    nav.service();
    CHECK_EQ(nav.depth(), size_t{2});

    ui::display_modal(nav, "Error", "RF transmit disabled.\nApplication closed.",
                      ui::ABORT, nullptr);
    nav.service();
    CHECK_EQ(nav.depth(), size_t{3});

    auto* modal = dynamic_cast<ui::ModalMessageView*>(nav.top());
    CHECK(modal != nullptr);
    if (modal == nullptr) return;

    modal->choose(true);
    nav.service();

    /* Both the modal and the app it was reporting on are gone. */
    CHECK_EQ(nav.depth(), size_t{1});
}

TEST(modal_without_a_navigation_stack_is_a_no_op) {
    /* app::globals().nav is null in the test binary; the convenience overload
     * must not dereference it. */
    ui::display_modal("Title", "Message");
    ui::display_modal("Title", "Message", ui::YESNO, nullptr);
}

TEST(modal_buttons_sit_inside_the_view) {
    ui::NavigationView nav{{0, 0, 240, 304}};
    ui::ModalMessageView modal{nav, "T", "M", ui::YESNO, nullptr};

    modal.set_parent_rect({0, 0, 240, 304});

    for (const auto* child : modal.children()) {
        const auto r = child->parent_rect();
        CHECK(r.top() >= 0);
        CHECK(r.bottom() <= 304);
        CHECK(r.left() >= 0);
        CHECK(r.right() <= 240);
    }
}
