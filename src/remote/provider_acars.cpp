/*
 * mayhem-b200 — web portal panel provider for ACARS, and the console reader
 * the four text-decoder providers share.
 *
 * ACARS, POCSAG, FLEX and Tetra all publish their decode as a scrolling log
 * rather than a table: each one writes finished lines into its own ui::Console
 * and keeps them nowhere else. This file publishes the ACARS console as a
 * PanelKind::Console panel, and defines console_data_from(), which
 * provider_pocsag.cpp, provider_flex.cpp and provider_tetra.cpp call for
 * exactly the same job.
 *
 * One reader rather than four copies: the line cap, the newest-last order and
 * the colour-escape handling below are the parts that have to be identical
 * across the four panels, and four copies of them would be four chances to
 * drift. It is declared, not headered, in the other three files — the same way
 * tests/test_provider_*.cpp already reach the table adapters.
 *
 * Nothing here decodes anything: every string published is one the app itself
 * wrote with Console::writeln(). The one edit to each app is a const
 * console() accessor in the style of AprsTableView::entries()
 * (src/apps/ui_aprs_rx.hpp), because the widget is otherwise private.
 *
 * THREADING: a provider is only ever called from AppBridge::refresh(), i.e. on
 * the UI thread, so walking the view stack and reading the console here races
 * nothing — see app_bridge.hpp's file header.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/app_context.hpp"
#include "../apps/ui_acars_rx.hpp"
#include "../apps/ui_navigation.hpp"
#include "../ui/ui_widget.hpp"

#include <cstddef>
#include <string>

namespace remote {
namespace {

/* How much scrollback the portal serves. The four consoles cap themselves at
 * ui::Console's 256-line default today, so this never bites — it is the
 * portal's own bound, so that an app raising set_max_lines() cannot turn a
 * 700 ms poll into an unbounded response body. The tail is kept, because the
 * newest lines are the ones an operator is watching. */
constexpr size_t kMaxConsoleLines = 500;

/* The apps colour their console lines with STR_COLOR_* escapes (src/ui/ui.hpp:
 * 0x1B followed by one palette byte), which Painter::draw_string consumes as an
 * attribute rather than drawing. On the wire those bytes are neither text nor
 * layout, so they come out here — the same thing provider_epirb.cpp does to the
 * cells it publishes. What is lost is only the colour; POCSAG's "CRC ERROR" and
 * ACARS's CRC verdict are words as well as colours, so a bad frame stays
 * legible as bad.
 *
 * The pair does NOT always survive into the buffer intact, which is why this is
 * not provider_epirb.cpp's "drop 0x1B and whatever follows it". Console::write
 * inspects every byte before storing it, and two palette values collide with
 * the two it treats specially:
 *
 *   - STR_COLOR_GREEN is 0x1B 0x0A, and 0x0A is '\n': the escape lead is stored
 *     and the palette byte ends the line. The buffer gets a line holding just
 *     "\x1B", and the text lands on the line after it.
 *   - STR_COLOR_MAGENTA is 0x1B 0x0D, and 0x0D is '\r': write() discards it, so
 *     the escape lead is followed immediately by the first character of real
 *     text ("\x1B" "1200bps ...").
 *
 * So the byte after an 0x1B is dropped only when it is a C0 control character,
 * which every palette value is (0x00..0x10) and no legible text is. Dropping it
 * unconditionally would eat the '1' from POCSAG's "1200bps" — the device screen
 * does exactly that, because draw_string has no such test, but reproducing a
 * display bug is not the same as agreeing with the app, and quietly deleting a
 * decoded character is the worse of the two.
 *
 * `escape_pending` carries across the line boundary for the one case that spans
 * lines: write() hard-wraps at the console's column count without knowing about
 * escapes, so an escape written mid-line (POCSAG's numeric-detect branch does
 * that) can land with its 0x1B as the last byte of one line and its palette
 * byte as the first byte of the next. The same C0 test settles it there.
 *
 * Control characters that are NOT part of an escape are left alone: a decoded
 * ACARS text field is raw payload bytes and may legitimately contain STX/ETX.
 * JSON escapes them as \u00XX and the browser decides what to do with them. */
std::string strip_color_escapes(const std::string& line, bool& escape_pending) {
    std::string out;
    out.reserve(line.size());
    for (const char ch : line) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == 0x1B) {
            escape_pending = true;
            continue;
        }
        if (escape_pending) {
            escape_pending = false;
            if (c < 0x20) continue; /* the palette byte */
        }
        out.push_back(ch);
    }
    return out;
}

}  // namespace

/* The console's committed lines, oldest first, plus the partial line still
 * being accumulated — see ui::Console::pending() for why that one matters.
 *
 * No timestamp and no line number is added. ConsoleData carries flat strings
 * and the console records neither, so anything of that kind here would be
 * invented; positional order is the only ordering that exists, and it is what
 * is published.
 *
 * A line that held nothing but a colour escape comes out as an empty string
 * rather than being dropped. The console genuinely has a line there and the
 * device paints a blank one (see the STR_COLOR_GREEN case above), so removing
 * it would make the browser's log and the screen disagree about what was
 * written. */
ConsoleData console_data_from(const ui::Console& console) {
    const auto& lines = console.lines();
    const std::string& pending = console.pending();
    const bool has_pending = !pending.empty();

    const size_t total = lines.size() + (has_pending ? size_t{1} : size_t{0});
    const size_t skip = (total > kMaxConsoleLines) ? (total - kMaxConsoleLines) : 0;

    ConsoleData out;
    out.lines.reserve(total - skip);

    /* The escape state is rebuilt from the start of the buffer, not from the
     * first published line: a line dropped by the cap can be the one holding
     * the 0x1B whose palette byte begins the line after it. */
    bool escape_pending = false;
    for (size_t i = 0; i < lines.size(); i++) {
        std::string text = strip_color_escapes(lines[i], escape_pending);
        if (i >= skip) out.lines.push_back(std::move(text));
    }
    if (has_pending) out.lines.push_back(strip_color_escapes(pending, escape_pending));

    return out;
}

namespace {

/* The provider is handed nav->top(). AcarsRxView pushes nothing of its own, but
 * the frequency field's step menu and anything the navigation bar puts up sit
 * above it, so walk down for the view that owns the console rather than letting
 * the browser go blank the moment someone touches the device. */
app::AcarsRxView* find_acars_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::AcarsRxView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::AcarsRxView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

PanelData acars_panel(ui::View& view) {
    PanelData panel;

    app::AcarsRxView* app_view = find_acars_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "ACARS is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::Console;
    panel.console = console_data_from(app_view->console());
    return panel;
}

const ProviderRegistrar reg_acars_rx{"acars_rx", acars_panel};

}  // namespace
}  // namespace remote
