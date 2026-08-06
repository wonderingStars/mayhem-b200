/*
 * mayhem-b200 — web portal panel provider for SubCar (car key-fob RX).
 *
 * Publishes SubCarView's recent-entries table as a generic table panel: the
 * same three columns the app draws on the 240x320 screen, in the same order,
 * with the same per-cell text. Every value is read straight off a
 * subcar::RecentEntry that src/apps/ui_subcar.cpp already filled in — the OOK/FM
 * slicing, the pulse extraction and all eleven protocol state machines ran long
 * before this file sees an entry, and none of it is repeated or re-derived here.
 *
 * The app itself gains exactly one thing: SubCarView::entries(), a const
 * accessor for the list it already holds, because the container is a private
 * member and a provider is handed only a ui::View&. That is the same accessor
 * AprsTableView and AdsbRxView already expose for the same reason.
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
#include "../apps/ui_subcar.hpp"
#include "../core/string_format.hpp"

#include <string>
#include <vector>

namespace remote {
namespace {

/* SubCarView's own columns_ initializer, repeated (src/apps/ui_subcar.hpp).
 * It cannot be read back off a running view — the RecentEntriesColumns object
 * is held only by private members of the view, its table and its header — so
 * the list is duplicated here and pinned by a test rather than being free to
 * drift. The widths are the app's declared ones; RecentEntriesView resolves the
 * zero-width Type column to whatever is left on screen, which is a layout
 * decision TableData does not carry. */
ui::RecentEntriesColumns subcar_columns() {
    return ui::RecentEntriesColumns{{"Type", 0}, {"Bits", 4}, {"Age", 3}};
}

/* One row, laid out to agree cell-for-cell with the line SubCarView's own
 * on_draw paints. The Type cell really does carry both the protocol name and
 * the 64-bit payload's low eight hex digits — that is what the app draws into
 * that column, and the browser showing anything narrower would be showing a
 * different table.
 *
 * Three pieces of that line are deliberately not copied, because they are the
 * fixed-width screen's alignment rather than part of any value: the Type text
 * is padded (and, on a narrow screen, truncated) to the resolved column width,
 * and the bit and age counts are right-justified in five and four characters.
 * A browser table lays its own cells out and would only have to strip all three
 * again.
 *
 * Bits and Age are genuine counters, so a zero in either is a real reading and
 * is published as one: a freshly decoded frame is 0 seconds old, and that is
 * exactly what the device shows. */
std::vector<std::string> subcar_row(const app::subcar::RecentEntry& e) {
    return {
        std::string{app::subcar::sensor_type_name(e.sensor_type)} + " " +
            to_string_hex(e.data, 8),
        to_string_dec_uint(e.bits),
        to_string_dec_uint(e.age),
    };
}

/* The provider is handed nav->top(), which is the SubCarDetailView whenever the
 * operator has opened a decoded frame on the device. SubCarView stays on the
 * stack underneath it, so walk down for it rather than letting the browser's
 * view go blank the moment someone touches the device. */
app::SubCarView* find_subcar_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::SubCarView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::SubCarView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

}  // namespace

/* Exposed rather than left in the anonymous namespace above so that
 * tests/test_provider_subcar_tt_wx.cpp can drive it with entries the app's own
 * decoders produced. The formatting is the part that has to keep agreeing with
 * the device screen, and that is only worth asserting against the app's own
 * output. */
TableData subcar_table_data(const app::subcar::RecentEntries& entries) {
    return table_data_from_entries(subcar_columns(), entries, subcar_row);
}

namespace {

PanelData subcar_panel(ui::View& view) {
    PanelData panel;

    app::SubCarView* app_view = find_subcar_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "SubCar is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::Table;
    panel.table = subcar_table_data(app_view->entries());
    return panel;
}

/* The id is the one src/apps/ui_subcar.cpp registers with app::Registrar. Get
 * it wrong and the provider is never reached: the app keeps showing the portal's
 * placeholder card and nothing anywhere reports an error. */
const ProviderRegistrar reg_subcar{"subcarrx", subcar_panel};

}  // namespace
}  // namespace remote
