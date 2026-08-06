/*
 * mayhem-b200 — web portal panel provider for ERT (utility meter) RX.
 *
 * Publishes the meter list ErtView already keeps, as the same five columns its
 * RecentEntriesView shows on the 240x320 screen, in the same order, with the
 * same per-cell formatting. Nothing here decodes anything: every value is read
 * off an ert::RecentEntry that src/apps/ui_ert.cpp filled in, and every cell is
 * produced by the app's own ert::format_* functions — the Manchester decode,
 * the SCM BCH / CCITT checks and the field extraction all happened long before
 * this file sees an entry.
 *
 * The one edit to the app is ErtView::entries(), a const accessor in the same
 * style as AprsTableView::entries() (src/apps/ui_aprs_rx.hpp), because the
 * container is otherwise private and a provider handed a ui::View& cannot reach
 * it. It is read-only and nothing here mutates the app.
 *
 * It lives here rather than in the app's own .cpp so that portal concerns stay
 * out of the ported app; ProviderRegistrar (app_bridge.hpp) is designed for
 * exactly this and self-registers at static-init time, so nothing else has to
 * know this file exists.
 *
 * THREADING: a provider is only ever called from AppBridge::refresh(), i.e. on
 * the UI thread, so walking the view stack and reading the list here races
 * nothing — see app_bridge.hpp's file header.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/app_context.hpp"
#include "../apps/ui_ert.hpp"
#include "../apps/ui_navigation.hpp"
#include "../core/string_format.hpp"

#include <string>
#include <vector>

namespace remote {
namespace {

/* ErtView's own columns_ initializer, repeated (src/apps/ui_ert.hpp). It cannot
 * be read off the running view — the RecentEntriesColumns object is a private
 * member — so the list is duplicated here and pinned by a test, rather than
 * being free to drift. The widths are the app's declared ones;
 * RecentEntriesView resolves the zero-width ID column to whatever is left on
 * screen, which is a layout decision TableData does not carry.
 *
 * "Ty" is the commodity type (electricity/gas/water), which is what the app
 * draws in that column — not the SCM/SCM+/IDM frame type, which the app uses
 * only to pick the tamper format and never shows. "Ct" is the received count. */
ui::RecentEntriesColumns ert_columns() {
    return ui::RecentEntriesColumns{
        {"ID", 0}, {"Ty", 2}, {"Consumpt", 8}, {"Tamp", 4}, {"Ct", 2}};
}

/* One row, laid out to agree cell-for-cell with the line ErtView's own on_draw
 * paints (src/apps/ui_ert.cpp), and built with the app's own ert::format_*
 * functions so the two cannot drift apart.
 *
 * What is deliberately not copied is the fixed-width screen's alignment, and
 * only that: ert::format_id/format_consumption/format_commodity_type right-
 * justify into 10/8/2 columns with spaces, format_tamper_flags_scm leads with a
 * space to fill the 4-wide Tamp column, and the received count is right-
 * justified in three. A browser table lays its own cells out and would only
 * have to strip that padding again. The zero padding inside a hex tamper field
 * is not alignment — it is how the app renders a 16-bit value — so it stays,
 * and so does the "++" ceiling the app shows past 99 receptions.
 *
 * An entry that exists but has never been updated by a packet — received_count
 * of zero — has a default-constructed consumption and tamper field, which are
 * absences and not readings: they publish as empty cells rather than as a
 * meter reading of 0 and a tamper word of 0000. The count itself is a genuine
 * counter and a real zero, so it is published. */
std::vector<std::string> ert_row(const app::ert::RecentEntry& e) {
    const bool heard = (e.received_count > 0);

    std::string tamper;
    if (heard) {
        tamper = (e.packet_type == app::ert::PacketType::SCM)
                     ? trim(app::ert::format_tamper_flags_scm(e.last_tamper_flags))
                     : app::ert::format_tamper_flags(e.last_tamper_flags);
    }

    return {
        trim(app::ert::format_id(e.id)),
        trim(app::ert::format_commodity_type(e.commodity_type)),
        heard ? trim(app::ert::format_consumption(e.last_consumption)) : std::string{},
        tamper,
        (e.received_count > 99)
            ? std::string{"++"}
            : trim(to_string_dec_uint(static_cast<uint64_t>(e.received_count), 3)),
    };
}

/* The provider is handed nav->top(). ErtView pushes nothing of its own today,
 * but the frequency field's step menu and anything the navigation bar puts up
 * sit above it, so walk down for the view that owns the list rather than
 * letting the browser go blank the moment someone touches the device. */
app::ErtView* find_ert_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::ErtView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::ErtView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

}  // namespace

/* Exposed rather than left in the anonymous namespace above so that
 * tests/test_provider_epirb_ert.cpp can drive it with entries a real
 * ert::Packet produced. The formatting is the part that has to keep agreeing
 * with the device screen, and that is only worth asserting against the app's
 * own output. */
TableData ert_table_data(const app::ert::RecentEntries& entries) {
    return table_data_from_entries(ert_columns(), entries, ert_row);
}

namespace {

PanelData ert_panel(ui::View& view) {
    PanelData panel;

    app::ErtView* app_view = find_ert_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "ERT Meter is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::Table;
    panel.table = ert_table_data(app_view->entries());
    return panel;
}

const ProviderRegistrar reg_ert{"ert", ert_panel};

}  // namespace
}  // namespace remote
