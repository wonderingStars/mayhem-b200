/*
 * mayhem-b200 — tests for the web portal console panels of the four text
 * decoders: POCSAG, ACARS, FLEX and Tetra (src/remote/provider_pocsag.cpp,
 * provider_acars.cpp, provider_flex.cpp, provider_tetra.cpp).
 *
 * Two halves, as with the table providers:
 *
 *   - remote::console_data_from() is driven directly with a ui::Console fed the
 *     way the apps feed theirs. That is where the whole contract lives: order,
 *     the 500-line cap, the partial line, and what happens to the STR_COLOR_*
 *     escapes the apps prefix their lines with. A ui::Console is used rather
 *     than a hand-built vector on purpose — write()'s wrapping and its
 *     '\n'/'\r' handling are exactly what mangles those escapes, and a fake
 *     would hide it.
 *   - The four providers are driven end to end through AppBridge —
 *     request_launch(), drain_launch_queue(), NavigationView::service(),
 *     refresh(), panel_json(). That is the only path that sets the bridge's
 *     current app id, it is the path the HTTP handler reads, and it is the one
 *     that catches a wrong app id: with the wrong id the provider is never
 *     reached, the panel comes back as "screen", and the portal silently keeps
 *     showing its placeholder card.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_context.hpp"
#include "audio_out.hpp"
#include "receiver_model.hpp"
#include "remote/app_bridge.hpp"
#include "remote/app_data.hpp"
#include "ui.hpp"
#include "ui_navigation.hpp"
#include "ui_widget.hpp"
#include "usrp_radio.hpp"

#include <cstddef>
#include <memory>
#include <string>

using remote::ConsoleData;

/* Defined in src/remote/provider_acars.cpp; see the comment there for why the
 * four providers share one reader rather than carrying four copies of the cap,
 * the ordering and the escape handling. */
namespace remote {
ConsoleData console_data_from(const ui::Console& console);
}  // namespace remote

namespace {

/* The geometry every one of the four apps gives its console is 240 wide, so
 * ui::Console::columns() is 240 / font::fixed_8x16.char_width() = 30. Several
 * tests below depend on that wrap point, so it is stated once here. */
constexpr int kConsoleColumns = 30;

ui::Console make_console() { return ui::Console{{0, 0, 240, 240}}; }

}  // namespace

/* ===========================================================================
 * The console reader
 * ========================================================================= */

TEST(console_panel_with_nothing_written_yields_no_lines) {
    /* A decoder that has heard nothing publishes an empty array. Not one empty
     * string, not a "waiting for data" line — the portal is told the log is
     * empty and says so itself. */
    ui::Console console = make_console();

    const ConsoleData d = remote::console_data_from(console);
    CHECK_EQ(d.lines.size(), size_t{0});
}

TEST(console_panel_publishes_lines_oldest_first) {
    /* Positional order is the only ordering a ui::Console has: it keeps a
     * deque and pushes to the back, so the newest line is last. No sequence
     * number and no timestamp is added on the way out — the console records
     * neither, so either would be invented. */
    ui::Console console = make_console();
    console.writeln("first");
    console.writeln("second");
    console.writeln("third");

    const ConsoleData d = remote::console_data_from(console);
    CHECK_EQ(d.lines.size(), size_t{3});
    if (d.lines.size() < 3) return;

    CHECK_STR_EQ(d.lines[0], "first");
    CHECK_STR_EQ(d.lines[1], "second");
    CHECK_STR_EQ(d.lines[2], "third");
}

TEST(console_panel_strips_a_well_formed_colour_escape) {
    /* STR_COLOR_LIGHT_GREY is 0x1B 0x07 — neither byte collides with write()'s
     * '\n'/'\r' handling, so the pair reaches the buffer intact and both bytes
     * come off. This is the shape of every banner line the four apps write. */
    ui::Console console = make_console();
    console.writeln(STR_COLOR_LIGHT_GREY "POCSAG RX ready.");

    const ConsoleData d = remote::console_data_from(console);
    CHECK_EQ(d.lines.size(), size_t{1});
    if (d.lines.empty()) return;

    CHECK_STR_EQ(d.lines[0], "POCSAG RX ready.");
}

TEST(console_panel_keeps_the_first_character_after_a_magenta_escape) {
    /* The regression this file exists to pin. STR_COLOR_MAGENTA is 0x1B 0x0D,
     * and Console::write() discards 0x0D as a carriage return, so what reaches
     * the buffer is the escape lead followed immediately by real text. A reader
     * that drops "0x1B and whatever follows" eats the '1' of "1200bps" — which
     * is precisely what the device's own draw_string does, and is a display bug
     * rather than something to reproduce on the wire. */
    ui::Console console = make_console();
    console.writeln(STR_COLOR_MAGENTA "1200bps errors");

    /* The raw buffer first, so the reason lives in the test and not only in the
     * comment above it: what is stored is the escape lead with the text hard up
     * against it, no palette byte in between. */
    CHECK_EQ(console.lines().size(), size_t{1});
    if (console.lines().empty()) return;
    CHECK_STR_EQ(console.lines()[0], std::string{"\x1B"} + "1200bps errors");

    const ConsoleData d = remote::console_data_from(console);
    CHECK_EQ(d.lines.size(), size_t{1});
    if (d.lines.empty()) return;

    CHECK_STR_EQ(d.lines[0], "1200bps errors");
}

TEST(console_panel_keeps_the_blank_line_a_green_escape_creates) {
    /* STR_COLOR_GREEN is 0x1B 0x0A, and 0x0A is '\n': write() stores the escape
     * lead and then ends the line on the palette byte, so the buffer really
     * does hold a line of its own before the text. The device paints a blank
     * line there; dropping it here would make the browser's log and the screen
     * disagree about what was written. */
    ui::Console console = make_console();
    console.writeln(STR_COLOR_GREEN "12345");

    /* The raw buffer: two lines, the first holding nothing but the escape
     * lead. */
    CHECK_EQ(console.lines().size(), size_t{2});
    if (console.lines().size() < 2) return;
    CHECK_STR_EQ(console.lines()[0], "\x1B");
    CHECK_STR_EQ(console.lines()[1], "12345");

    const ConsoleData d = remote::console_data_from(console);
    CHECK_EQ(d.lines.size(), size_t{2});
    if (d.lines.size() < 2) return;

    CHECK_STR_EQ(d.lines[0], "");
    CHECK_STR_EQ(d.lines[1], "12345");
}

TEST(console_panel_publishes_the_partial_line_still_being_written) {
    /* POCSAG's message body arrives through Console::write(), not writeln():
     * a short page therefore sits in pending_ until the next line is started.
     * A reader that only walked the committed deque would show the address
     * line and silently lose the message under it. */
    ui::Console console = make_console();
    console.writeln("1200bps ADDR: 1234567");
    console.write("CALL 555 1234");

    const ConsoleData d = remote::console_data_from(console);
    CHECK_EQ(d.lines.size(), size_t{2});
    if (d.lines.size() < 2) return;

    CHECK_STR_EQ(d.lines[0], "1200bps ADDR: 1234567");
    CHECK_STR_EQ(d.lines[1], "CALL 555 1234");
}

TEST(console_panel_strips_an_escape_split_across_the_wrap_point) {
    /* Console::write() hard-wraps at the column count without knowing about
     * escapes, so an escape written mid-line can leave its 0x1B as the last
     * byte of one line and its palette byte as the first byte of the next.
     * Filling the line to one short of the wrap puts the escape exactly on the
     * boundary. Neither byte may reach the browser. */
    ui::Console console = make_console();
    console.write(std::string(static_cast<size_t>(kConsoleColumns) - 1, 'a'));
    console.write(STR_COLOR_LIGHT_GREY "bc");

    const ConsoleData d = remote::console_data_from(console);
    CHECK_EQ(d.lines.size(), size_t{2});
    if (d.lines.size() < 2) return;

    CHECK_STR_EQ(d.lines[0], std::string(static_cast<size_t>(kConsoleColumns) - 1, 'a'));
    CHECK_STR_EQ(d.lines[1], "bc");
}

TEST(console_panel_leaves_text_control_characters_alone) {
    /* An ACARS text field is raw payload bytes and can legitimately carry
     * STX/ETX. Only the byte immediately after an 0x1B is treated as a palette
     * value; everything else is the decoder's own output and is published as
     * it stands, for the JSON writer to escape as \u00XX. */
    ui::Console console = make_console();
    console.writeln(std::string{"\x02"} + "MSG" + std::string{"\x03"});

    const ConsoleData d = remote::console_data_from(console);
    CHECK_EQ(d.lines.size(), size_t{1});
    if (d.lines.empty()) return;

    CHECK_STR_EQ(d.lines[0], std::string{"\x02"} + "MSG" + std::string{"\x03"});
}

TEST(console_panel_caps_at_five_hundred_lines_and_keeps_the_newest) {
    /* The four apps leave ui::Console at its 256-line default, so the cap does
     * not bite today; it is the portal's own bound against an app raising
     * set_max_lines() and turning a 700 ms poll into an unbounded body. The
     * tail is what is kept — the newest lines are the ones being watched. */
    ui::Console console = make_console();
    console.set_max_lines(1000);
    for (int i = 0; i < 600; i++) console.writeln("L" + std::to_string(i));

    const ConsoleData d = remote::console_data_from(console);
    CHECK_EQ(d.lines.size(), size_t{500});
    if (d.lines.size() != 500) return;

    CHECK_STR_EQ(d.lines[0], "L100");
    CHECK_STR_EQ(d.lines[499], "L599");
}

TEST(console_panel_counts_the_partial_line_against_the_cap) {
    /* The boundary: 500 committed lines plus a partial one is 501 published
     * lines' worth of content, and the oldest committed line is what gives way.
     * The partial line is never the one dropped — it is the newest thing the
     * decoder has produced. */
    ui::Console console = make_console();
    console.set_max_lines(1000);
    for (int i = 0; i < 500; i++) console.writeln("L" + std::to_string(i));
    console.write("partial");

    const ConsoleData d = remote::console_data_from(console);
    CHECK_EQ(d.lines.size(), size_t{500});
    if (d.lines.size() != 500) return;

    CHECK_STR_EQ(d.lines[0], "L1");
    CHECK_STR_EQ(d.lines[499], "partial");
}

/* ===========================================================================
 * The four providers, end to end through AppBridge
 * ========================================================================= */

namespace {

/* All four views bind globals().receiver in their constructors (two of them by
 * reference), so a ReceiverModel has to exist before the app registry's factory
 * can build one. Nothing here opens a device: on_show() calls receiver.start(),
 * which fails at start_rx() on a closed radio and returns false without
 * spawning a DSP thread, so each view comes up with only the banner lines its
 * constructor wrote — which is exactly the state under test. Tears the globals
 * back down so later tests see them as they were. */
struct ProviderHarness {
    radio::UsrpRadio radio{};
    audio::AudioOut audio{};
    radio::ReceiverModel receiver{radio, audio};
    ui::NavigationView nav{{0, 0, 240, 304}};

    radio::RadioDevice* saved_radio{app::globals().radio};
    radio::ReceiverModel* saved_receiver{app::globals().receiver};
    ui::NavigationView* saved_nav{app::globals().nav};

    ProviderHarness() {
        app::globals().radio = &radio;
        app::globals().receiver = &receiver;
        app::globals().nav = &nav;

        nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304})); /* root */
        nav.service();
    }

    ~ProviderHarness() {
        /* AppBridge is a process-global singleton, so a test that leaves it
         * believing one of these apps is open hands that state to every later
         * test in the binary — and to every -count= rerun. Clearing it needs
         * the nav still wired up, so it happens before the globals go back. */
        remote::AppBridge::instance().request_home();
        remote::AppBridge::instance().drain_launch_queue();
        nav.service();

        app::globals().radio = saved_radio;
        app::globals().receiver = saved_receiver;
        app::globals().nav = saved_nav;
    }

    /* Puts an app on the stack the way the portal does. */
    void launch(const std::string& app_id) {
        remote::AppBridge::instance().request_launch(app_id);
        remote::AppBridge::instance().drain_launch_queue();
        nav.service();
    }
};

bool panel_contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/* Three of the four apps write their banner with a STR_COLOR_* escape, so a
 * panel that still carried one would show up here: the JSON writer has no
 * literal form for 0x1B and escapes it as a six-character u-sequence, which is
 * a string no decoded page can contain. */
void check_no_escapes_on_the_wire(const std::string& panel) {
    CHECK(!panel_contains(panel, "\\u001b"));
}

/* One app, opened the way the portal opens it. `first_line_prefix` is the start
 * of the first banner line the app's constructor wrote, which pins three things
 * at once: the provider ran, it read the right console, and the colour escape
 * came off the front. */
void check_console_panel(const std::string& app_id, const std::string& first_line_prefix) {
    ProviderHarness h;
    h.launch(app_id);
    CHECK_EQ(h.nav.depth(), size_t{2});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"app_id\":\"" + app_id + "\""));
    CHECK(panel_contains(panel, "\"panel_kind\":\"console\""));
    /* Never the placeholder: that is what a wrong app id looks like. */
    CHECK(!panel_contains(panel, "\"panel_kind\":\"screen\""));
    CHECK(panel_contains(panel, "\"lines\":[\"" + first_line_prefix));
    check_no_escapes_on_the_wire(panel);
}

/* Any view pushed on top reproduces the condition the provider has to cope
 * with: it is handed that view, and the app's own view is only reachable by
 * walking NavigationView::at_depth() down the stack. On the device that is a
 * settings page (POCSAG) or a frequency-step menu (Tetra); the browser must not
 * go blank while the operator uses one. */
void check_console_panel_survives_a_sub_view(const std::string& app_id) {
    ProviderHarness h;
    h.launch(app_id);

    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{3});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"panel_kind\":\"console\""));
    CHECK(panel_contains(panel, "\"lines\":["));
}

/* Popped on the device rather than through request_home() — the path a remote
 * key press takes, which never goes near the launch queue.
 *
 * UPDATED for AppBridge::refresh() deriving the current app from the
 * navigation stack. This used to assert the provider's own "X is not the open
 * app." fallback, because the bridge went on believing the app was current
 * after its view was gone and asked its provider anyway. That belief was the
 * bug: the same staleness served one app's panel under another app's title
 * once the operator navigated with the keys instead of the launch route. With
 * the app popped, the truthful answer is Home, so that is what is pinned here
 * — plus the id and title, which are what actually went stale. The fallback
 * is still in every provider as a guard; the bridge simply no longer asks a
 * provider about an app that is not on the stack. */
void check_console_panel_is_honest_when_the_app_is_gone(const std::string& app_id) {
    ProviderHarness h;
    h.launch(app_id);

    h.nav.pop_to_root();
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{1});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();
    const std::string current = remote::AppBridge::instance().current_app_json();

    CHECK(panel_contains(panel, "\"panel_kind\":\"screen\""));
    CHECK(panel_contains(panel, "Home -- no app is open."));
    CHECK(!panel_contains(panel, "\"lines\""));
    CHECK(panel_contains(panel, "\"app_id\":\"\""));
    CHECK(panel_contains(current, "\"id\":\"\""));
    CHECK(panel_contains(current, "\"title\":\"Home\""));
}

}  // namespace

TEST(pocsag_panel_provider_publishes_a_console_when_the_app_is_open) {
    /* The id has to be the one src/apps/pocsag_app.cpp registers ("pocsag"). */
    check_console_panel("pocsag", "POCSAG RX ready.\"");
}

TEST(pocsag_panel_provider_survives_the_operator_opening_the_config_page) {
    check_console_panel_survives_a_sub_view("pocsag");
}

TEST(pocsag_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    check_console_panel_is_honest_when_the_app_is_gone("pocsag");
}

TEST(acars_panel_provider_publishes_a_console_when_the_app_is_open) {
    /* The id has to be the one src/apps/ui_acars_rx.cpp registers ("acars_rx").
     * The banner is 29 characters plus a two-byte escape, which is past the
     * console's 30-column wrap, so only its start is asserted. */
    check_console_panel("acars_rx", "ACARS 2400bd MSK, AM channel");
}

TEST(acars_panel_provider_survives_the_operator_drilling_into_a_sub_view) {
    check_console_panel_survives_a_sub_view("acars_rx");
}

TEST(acars_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    check_console_panel_is_honest_when_the_app_is_gone("acars_rx");
}

TEST(flex_panel_provider_publishes_a_console_when_the_app_is_open) {
    /* The id has to be the one src/apps/ui_flex_rx.cpp registers ("flex_rx"). */
    check_console_panel("flex_rx", "FLEX RX ready.\"");
}

TEST(flex_panel_provider_survives_the_operator_drilling_into_a_sub_view) {
    check_console_panel_survives_a_sub_view("flex_rx");
}

TEST(flex_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    check_console_panel_is_honest_when_the_app_is_gone("flex_rx");
}

TEST(tetra_panel_provider_publishes_a_console_when_the_app_is_open) {
    /* The id has to be the one src/apps/ui_tetra_rx.cpp registers ("tetra_rx").
     * Tetra's banner carries no colour escape at all, so this one also checks
     * that a plain line is published untouched. */
    check_console_panel("tetra_rx", "TETRA downlink decoder ready.\"");
}

TEST(tetra_panel_provider_survives_the_operator_opening_the_step_menu) {
    check_console_panel_survives_a_sub_view("tetra_rx");
}

TEST(tetra_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    check_console_panel_is_honest_when_the_app_is_gone("tetra_rx");
}
