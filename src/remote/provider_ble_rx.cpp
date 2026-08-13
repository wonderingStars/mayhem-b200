/*
 * mayhem-b200 — web portal panel provider for BLE RX.
 *
 * Publishes the advertiser list BleRxView is already keeping as a generic
 * table panel: the same three columns its RecentEntriesTable shows on the
 * 240x320 screen, in the same order, with the same per-cell formatting. Every
 * value below is read straight off a BleRecentEntry that src/apps/ui_ble_rx.cpp
 * already filled in — the access-address correlation, the dewhitening and the
 * CRC-24 check all happened long before this file sees an entry — and nothing
 * here re-derives any of it.
 *
 * The one change to the app is BleRxView::entries(), a const accessor added so
 * this file can read `entries_` without the app having to know the portal
 * exists. It lives here rather than in ui_ble_rx.cpp for the same reason:
 * ProviderRegistrar (app_bridge.hpp) self-registers at static-init time, so
 * nothing else has to know this file exists.
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
#include "../apps/ui_ble_rx.hpp"
#include "../apps/ui_navigation.hpp"
#include "../core/string_format.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace remote {
namespace {

/* BleRxView's own columns_ initializer, repeated (src/apps/ui_ble_rx.hpp).
 * It cannot be read off the running view — the RecentEntriesColumns object is
 * a private member the table only holds a reference to — so the list is
 * duplicated here and pinned by a test, rather than being free to drift. The
 * widths are the app's declared ones; resolving the zero-width Data column to
 * whatever is left on screen is a layout decision TableData does not carry. */
ui::RecentEntriesColumns ble_columns() {
    return ui::RecentEntriesColumns{{"MAC", 13}, {"T", 2}, {"Data", 0}};
}

/* The advertiser address as the app's on_draw paints it: the six octets in
 * display order (the order BleRxView's decoder already put them in when it
 * built Packet::mac), colon separated, two uppercase hex digits each. */
std::string mac_string(uint64_t mac) {
    std::string s;
    for (int i = 5; i >= 0; i--) {
        s += to_string_hex(static_cast<uint8_t>((mac >> (i * 8)) & 0xFF), 2);
        if (i) s += ':';
    }
    return s;
}

/* One row, laid out to agree cell-for-cell with the line BleRxView's own
 * on_draw paints (ui_ble_rx.cpp).
 *
 * One piece of that line is deliberately not copied, because it is the
 * fixed-width screen's alignment rather than part of any value: the whole line
 * is padded out to the character width of the row. A browser table lays its
 * own cells out and would only have to strip it again.
 *
 * Anything an advertiser has not sent stays empty. A PDU whose payload is the
 * six AdvA octets and nothing else — a bare ADV_NONCONN_IND or a SCAN_REQ —
 * carries no AdvData at all, and its Data cell is blank here exactly as it is
 * on the device; it is emphatically not a run of zero bytes. The MAC and the
 * PDU type are not optional: an entry exists only because a frame with that
 * address passed the CRC-24 check, and the type is the header nibble that came
 * with it. */
std::vector<std::string> ble_row(const app::ble_rx::BleRecentEntry& e) {
    return {
        mac_string(e.mac),
        /* The app draws the numeric ADV_PDU_TYPE rather than its name, so that
         * is what goes on the wire; Packet::type_string() is only used by the
         * decoder's own callers. */
        to_string_dec_uint(e.type, 1),
        e.data_hex,
    };
}

/* The provider is handed nav->top(). BleRxView pushes nothing of its own
 * today, but the operator can reach this app with anything already on the
 * stack beneath it, and a later detail page would push over it — so walk down
 * for the view that owns the data rather than casting the top blindly. */
app::ble_rx::BleRxView* find_ble_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::ble_rx::BleRxView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::ble_rx::BleRxView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

}  // namespace

/* Exposed rather than left in the anonymous namespace above so that
 * tests/test_provider_ais_ble.cpp can drive it with entries built from a real
 * ble_rx::Packet. The formatting is the part that has to keep agreeing with
 * the device screen, and that is only worth asserting against the app's own
 * output. */
TableData ble_table_data(const app::ble_rx::BleRecentEntries& entries) {
    return table_data_from_entries(ble_columns(), entries, ble_row);
}

namespace {

PanelData ble_panel(ui::View& view) {
    PanelData panel;

    app::ble_rx::BleRxView* app_view = find_ble_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "BLE RX is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::Table;
    panel.table = ble_table_data(app_view->entries());
    return panel;
}

const ProviderRegistrar reg_ble_rx{"blerx", PanelKind::Table, ble_panel};

}  // namespace
}  // namespace remote
