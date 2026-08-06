/*
 * mayhem-b200 — web portal panel provider for NRF RX.
 *
 * Publishes the nRF24L01+ (Enhanced ShockBurst) decoder's results list as a
 * generic table panel: the same three columns NrfRxView shows on the 240x320
 * screen, in the same order, with the same per-cell formatting. Nothing here
 * decodes anything — the preamble correlation, the bit slicing and the CRC
 * check all happened in src/apps/ui_nrf_rx.cpp long before this file sees an
 * entry, and every value below is read straight off a NrfRecentEntry that
 * NrfRxView::on_packet() already filled in.
 *
 * The app's only concession to this file is NrfRxView::entries(), one public
 * const accessor in the style of AprsTableView::entries(); the container and
 * the table widget are otherwise private, so there is no other way to reach
 * the list from outside the view.
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
#include "../apps/ui_nrf_rx.hpp"
#include "../core/string_format.hpp"

#include <string>
#include <vector>

namespace remote {
namespace {

/* NrfRxView's own columns_ initializer, repeated (src/apps/ui_nrf_rx.hpp).
 * It cannot be read off the running view — both the RecentEntriesColumns
 * object and the table that owns it are private members — so the list is
 * duplicated here and pinned by a test, rather than being free to drift.
 * The widths are the app's declared ones; they describe a fixed-width screen
 * and TableData does not carry them. */
ui::RecentEntriesColumns nrf_columns() {
    return ui::RecentEntriesColumns{{"Address", 12}, {"Len", 4}, {"Payload", 0}};
}

/* One row, laid out to agree cell-for-cell with the line NrfRxView's own
 * on_draw paints:
 *
 *   to_string_hex(entry.address, 10) + " " +
 *   to_string_dec_uint(entry.payload_length, 3) + " " + entry.payload_hex
 *
 * Two pieces of that line are deliberately not copied, because they are the
 * fixed-width screen's alignment rather than part of any value: the payload
 * length is right-justified in three characters, and the whole line is then
 * padded out to the table's pixel width. A browser table lays its own cells
 * out and would only have to strip both again. The address's ten hex digits
 * are NOT alignment — an ESB address is five bytes, so all ten digits are
 * significant — and stay.
 *
 * The Payload cell is whatever Packet::payload_string() produced: space-
 * separated hex bytes, and empty for a zero-length payload. A zero-length ESB
 * payload is legal and real, so an empty cell there means "this frame carried
 * no payload", which is exactly what the device shows. */
std::vector<std::string> nrf_row(const app::NrfRecentEntry& e) {
    return {
        to_string_hex(e.address, 10),
        to_string_dec_uint(e.payload_length),
        e.payload_hex,
    };
}

/* The provider is handed nav->top(). NrfRxView pushes nothing on top of itself
 * today, but the operator can still end up looking at something else the app
 * or the navigation stack put there, and NrfRxView stays underneath — so walk
 * down for it rather than static_cast'ing the top blindly (app_bridge.hpp). */
app::NrfRxView* find_nrf_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::NrfRxView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::NrfRxView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

}  // namespace

/* Exposed rather than left in the anonymous namespace above so that
 * tests/test_provider_nrf_search.cpp can drive it with entries a real
 * NrfRxView produced. The formatting is the part that has to keep agreeing
 * with the device screen, and that is only worth asserting against the app's
 * own output. */
TableData nrf_table_data(const app::NrfRecentEntries& entries) {
    return table_data_from_entries(nrf_columns(), entries, nrf_row);
}

namespace {

PanelData nrf_panel(ui::View& view) {
    PanelData panel;

    app::NrfRxView* app_view = find_nrf_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "NRF RX is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::Table;
    panel.table = nrf_table_data(app_view->entries());
    return panel;
}

/* The id src/apps/ui_nrf_rx.cpp registers with app::Registrar. A wrong id here
 * registers a provider that never fires and leaves the portal showing the
 * placeholder card, which is why a test drives this end to end. */
const ProviderRegistrar reg_nrf_rx{"nrf_rx", nrf_panel};

}  // namespace
}  // namespace remote
