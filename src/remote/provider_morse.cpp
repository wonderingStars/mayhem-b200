/*
 * mayhem-b200 — web portal panel provider for the Morse (CW) decoder.
 *
 * Publishes the running decode of MorseRadioView so the browser can show the
 * received text, WPM and tone in a native panel — and, paired with the
 * POST /api/morse/transmit endpoint (remote_server.cpp), give a text-to-Morse
 * transmit interface. This file adds no decoding: every value is read straight
 * off the accessors the app already exposes.
 *
 * THREADING: a provider is only ever called from AppBridge::refresh(), i.e. on
 * the UI thread, the same thread MorseRadioView updates its decode on, so the
 * reads here race nothing — see app_bridge.hpp's file header.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/app_context.hpp"
#include "../apps/ui_morse_radio.hpp"
#include "../apps/ui_navigation.hpp"

namespace remote {
namespace {

/* MorseRadioView stays on the nav stack underneath any sub-view it pushes, so
 * walk down for it rather than letting the panel go blank the moment the
 * operator opens a child page — the same convention the other providers use. */
app::MorseRadioView* find_morse_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::MorseRadioView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::MorseRadioView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

PanelData morse_panel(ui::View& view) {
    PanelData panel;

    app::MorseRadioView* app_view = find_morse_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "Morse is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::Morse;
    MorseData& out = panel.morse;
    out.decoded_text = app_view->decoded_text();
    out.wpm = app_view->decoded_wpm();       /* 0 => omitted on the wire */
    out.tone_hz = app_view->decoded_tone_hz();
    out.receiving = app_view->receiving();
    return panel;
}

const ProviderRegistrar reg_morse{"morseradio", PanelKind::Morse, morse_panel};

}  // namespace
}  // namespace remote
