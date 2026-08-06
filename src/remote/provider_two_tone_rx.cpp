/*
 * mayhem-b200 — web portal panel provider for Two-Tone (QCII paging) RX.
 *
 * Publishes TwoToneRxView's pair log as a generic table panel: the one column
 * the app draws on the 240x320 screen, carrying the very string the app already
 * built. The Goertzel tone measurement, the A/B sequencing and the
 * "1153.4Hz 1000ms" formatting all happened in src/apps/ui_two_tone_rx.cpp
 * before this file sees an entry; nothing here re-formats or re-derives any of
 * it, so the browser and the device cannot disagree.
 *
 * The app itself gains exactly one thing: TwoToneRxView::entries(), a const
 * accessor for the log it already holds, because the container is a private
 * member and a provider is handed only a ui::View&. That is the same accessor
 * AprsTableView and AdsbRxView already expose for the same reason.
 *
 * It lives here rather than in the app's own .cpp so that portal concerns stay
 * out of the ported app; ProviderRegistrar (app_bridge.hpp) is designed for
 * exactly this and self-registers at static-init time, so nothing else has to
 * know this file exists.
 *
 * NOT PUBLISHED: the live one-line status (TwoToneSequencer::status(), painted
 * into text_status_) and the tap description, both of which are private ui::Text
 * widgets. A table panel has nowhere to put them, and reaching them would need
 * more of the app opened up than publishing the log does.
 *
 * THREADING: a provider is only ever called from AppBridge::refresh(), i.e.
 * on the UI thread, so walking the view tree and reading the log here races
 * nothing — see app_bridge.hpp's file header.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/app_context.hpp"
#include "../apps/ui_navigation.hpp"
#include "../apps/ui_two_tone_rx.hpp"

#include <string>
#include <vector>

namespace remote {
namespace {

/* TwoToneRxView's own log_columns_ initializer, repeated
 * (src/apps/ui_two_tone_rx.hpp). It cannot be read back off a running view —
 * the RecentEntriesColumns object is a private member — so the list is
 * duplicated here and pinned by a test rather than being free to drift. The
 * declared width of zero is the app's "take the rest of the line", which is a
 * layout decision TableData does not carry. */
ui::RecentEntriesColumns two_tone_columns() {
    return ui::RecentEntriesColumns{{"A tone / B tone", 0}};
}

/* One row. The app's on_draw paints entry.line and nothing else, so that string
 * is the cell; the only thing dropped is the padding out to the table's pixel
 * width, which is screen alignment.
 *
 * An entry whose pair has not been filled in has an empty line, and it stays
 * empty here — a logged row with no tones is not "0Hz 0ms". */
std::vector<std::string> two_tone_row(const app::TwoToneLogEntry& e) {
    return {e.line};
}

/* The provider is handed nav->top(). TwoToneRxView pushes nothing of its own
 * today, but the operator can still be somewhere above it (the portal's own
 * back control, a future detail view), so walk down for the app rather than
 * letting the browser's view go blank. */
app::TwoToneRxView* find_two_tone_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::TwoToneRxView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::TwoToneRxView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

}  // namespace

/* Exposed rather than left in the anonymous namespace above so that
 * tests/test_provider_subcar_tt_wx.cpp can drive it with entries the app's own
 * logging produced. */
TableData two_tone_table_data(const app::TwoToneLogEntries& entries) {
    return table_data_from_entries(two_tone_columns(), entries, two_tone_row);
}

namespace {

PanelData two_tone_panel(ui::View& view) {
    PanelData panel;

    app::TwoToneRxView* app_view = find_two_tone_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "2-Tone RX is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::Table;
    panel.table = two_tone_table_data(app_view->entries());
    return panel;
}

/* The id is the one src/apps/ui_two_tone_rx.cpp registers with app::Registrar.
 * Get it wrong and the provider is never reached: the app keeps showing the
 * portal's placeholder card and nothing anywhere reports an error. */
const ProviderRegistrar reg_two_tone_rx{"two_tone_rx", two_tone_panel};

}  // namespace
}  // namespace remote
