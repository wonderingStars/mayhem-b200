/*
 * mayhem-b200 — web portal panel provider for Search.
 *
 * Publishes the wideband energy scanner's results list as a generic table
 * panel: the same three columns SearchView shows on the 240x320 screen, in the
 * same order, with the same per-cell formatting. Nothing here scans, detects or
 * resolves anything — the slice sweep, the mean/threshold detector and the
 * lock/release state machine all ran in src/apps/ui_search.cpp long before this
 * file sees an entry, and every value below is read straight off a
 * SearchRecentEntry that SearchView::do_detection() already filled in.
 *
 * The app's only concession to this file is SearchView::entries(), one public
 * const accessor in the style of AprsTableView::entries(); the container and
 * the RecentEntriesView that owns the table are otherwise private, so there is
 * no other way to reach the list from outside the view.
 *
 * It lives here rather than in the app's own .cpp so that portal concerns stay
 * out of the ported app; ProviderRegistrar (app_bridge.hpp) is designed for
 * exactly this and self-registers at static-init time, so nothing else has to
 * know this file exists.
 *
 * THREADING: a provider is only ever called from AppBridge::refresh(), i.e.
 * on the UI thread, so walking the view tree and reading the entries list here
 * races nothing — see app_bridge.hpp's file header.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/app_context.hpp"
#include "../apps/ui_navigation.hpp"
#include "../apps/ui_search.hpp"
#include "../core/string_format.hpp"

#include <string>
#include <vector>

namespace remote {
namespace {

/* SearchView's own columns_ initializer, repeated (src/apps/ui_search.hpp).
 * It cannot be read off the running view — the RecentEntriesColumns object and
 * the RecentEntriesView holding it are private members — so the list is
 * duplicated here and pinned by a test, rather than being free to drift.
 * The declared widths are not carried: RecentEntriesView's constructor mutates
 * them at run time (it resolves the zero-width Frequency column to whatever is
 * left of the 30-character line), which is a layout decision about a fixed
 * screen and means nothing to a browser table. */
ui::RecentEntriesColumns search_columns() {
    return ui::RecentEntriesColumns{{"Frequency", 0}, {"Time", 8}, {"Duration", 8}};
}

/* One row, laid out to agree cell-for-cell with the line SearchView's own
 * on_draw paints:
 *
 *   to_string_short_freq(entry.frequency) | entry.time | format_duration(...)
 *
 * each resize()d out to its column width. Two kinds of padding are deliberately
 * not copied, both being the fixed-width screen's alignment rather than part of
 * any value: those three resize() calls, and the leading spaces
 * to_string_short_freq itself emits (it right-justifies the MHz part in four
 * columns, so 100 MHz comes back as " 100.0000"). A browser table lays its own
 * cells out and would only have to strip both again. The four decimal places
 * are not alignment — they are the resolution the app resolved the hit to — and
 * stay.
 *
 * Time is empty until the entry is locked (SearchRecentEntry::time defaults to
 * an empty string and only do_detection()'s Lock branch sets it), and an entry
 * whose Lock row was evicted from the 64-entry list before its Release can be
 * re-created with no time at all. That cell stays empty rather than being
 * given an invented timestamp — the device draws eight blanks there for the
 * same reason.
 *
 * Duration is emitted even at zero. It is a genuine counter: Detector advances
 * it every ~100 ms for as long as the hit stays locked, so 0 means "locked, no
 * tenth of a second elapsed yet", which is what the device shows too. */
std::vector<std::string> search_row(const app::SearchRecentEntry& e) {
    return {
        trim(to_string_short_freq(e.frequency)),
        e.time,
        app::search::format_duration(e.duration),
    };
}

/* The provider is handed nav->top(). SearchView pushes nothing on top of
 * itself today (upstream's FrequencySaveView is not ported), but the operator
 * can still end up looking at something else the navigation stack put there,
 * and SearchView stays underneath — so walk down for it rather than
 * static_cast'ing the top blindly (app_bridge.hpp). */
app::SearchView* find_search_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::SearchView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::SearchView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

}  // namespace

/* Exposed rather than left in the anonymous namespace above so that
 * tests/test_provider_nrf_search.cpp can drive it with entries a real
 * SearchView produced. The formatting is the part that has to keep agreeing
 * with the device screen, and that is only worth asserting against the app's
 * own output. */
TableData search_table_data(const app::SearchRecentEntries& entries) {
    return table_data_from_entries(search_columns(), entries, search_row);
}

namespace {

PanelData search_panel(ui::View& view) {
    PanelData panel;

    app::SearchView* app_view = find_search_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "Search is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::Table;
    panel.table = search_table_data(app_view->entries());
    return panel;
}

/* The id src/apps/ui_search.cpp registers with app::Registrar. A wrong id here
 * registers a provider that never fires and leaves the portal showing the
 * placeholder card, which is why a test drives this end to end. */
const ProviderRegistrar reg_search{"search", search_panel};

}  // namespace
}  // namespace remote
