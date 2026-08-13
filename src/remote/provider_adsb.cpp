/*
 * mayhem-b200 — web portal panel provider for ADS-B RX.
 *
 * Publishes the aircraft AdsbRxView's AircraftTracker is already tracking, so
 * the browser can draw a live traffic map. This file adds no decoding of any
 * kind: every value below is read straight off an AircraftRecentEntry that
 * src/apps/ui_adsb_rx.cpp filled in, and the app is not modified at all —
 * AdsbRxView::tracker() is already public because the views the app pushes on
 * top of itself need it.
 *
 * It lives here rather than in the app's own .cpp so that portal concerns stay
 * out of the ported app; ProviderRegistrar (app_bridge.hpp) is designed for
 * exactly this and self-registers at static-init time, so nothing else has to
 * know this file exists.
 *
 * THREADING: a provider is only ever called from AppBridge::refresh(), i.e.
 * on the UI thread, so reading the tracker here races nothing — see
 * app_bridge.hpp's file header.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/ui_adsb_rx.hpp"
#include "../apps/ui_navigation.hpp"
#include "../apps/app_context.hpp"

#include <cstdio>
#include <string>

namespace remote {
namespace {

/* Upstream's five age buckets, as the strings the browser dims rows by. */
const char* state_name(app::ADSBAgeState s) {
    switch (s) {
        case app::ADSBAgeState::Current: return "current";
        case app::ADSBAgeState::Recent: return "recent";
        case app::ADSBAgeState::Old: return "old";
        case app::ADSBAgeState::Expired: return "expired";
        case app::ADSBAgeState::Invalid: break;
    }
    return "invalid";
}

/* The tracker stores the ICAO address as a number and also keeps the app's own
 * formatted string; prefer the latter so the web view and the 240x320 screen
 * can never disagree, and fall back to formatting it only if it is empty. */
std::string icao_string(const app::AircraftRecentEntry& e) {
    if (!e.icao_str.empty()) return e.icao_str;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06X", static_cast<unsigned>(e.ICAO_address));
    return std::string{buf};
}

/* ADS-B identity messages carry the callsign in a fixed 8-character field,
 * space-padded ("EXS93H  "). The device screen draws it into a fixed-width
 * cell so the padding is invisible there, but on the wire it would make every
 * client handle it: string compares, search boxes and sorts would all see the
 * trailing spaces. Trim once, here. */
std::string trimmed(std::string s) {
    const auto last = s.find_last_not_of(' ');
    if (last == std::string::npos) return {};
    s.erase(last + 1);
    const auto first = s.find_first_not_of(' ');
    return s.substr(first);
}

/* The provider is handed nav->top(), but the tracker lives on AdsbRxView,
 * which stays on the stack underneath the details and map views the operator
 * can push on top of it locally. Walking down finds it wherever it is, so the
 * browser's view does not go blank when someone touches the device. */
app::AdsbRxView* find_adsb_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::AdsbRxView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::AdsbRxView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

PanelData adsb_panel(ui::View& view) {
    PanelData panel;

    app::AdsbRxView* app_view = find_adsb_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "ADS-B RX is not the open app.";
        return panel;
    }

    panel.kind = PanelKind::Adsb;
    AdsbData& out = panel.adsb;

    const auto& tracker = app_view->tracker();
    out.frames_seen = tracker.frames_seen();
    out.frames_accepted = tracker.frames_accepted();

    /* The app has no notion of where the receiver is — there is no GPS on a
     * B200 and nothing in the ported UI asks for a position — so home is left
     * unset and the browser asks the operator for one (and remembers it).
     * Reporting a fabricated origin here would put every range ring and
     * bearing quietly, plausibly wrong. */
    out.home_valid = false;

    for (const auto& e : tracker.entries()) {
        AdsbAircraft a;
        a.icao = icao_string(e);
        a.callsign = trimmed(e.callsign);
        a.info = e.info_string;
        a.state = state_name(e.state);

        a.pos_valid = e.pos.pos_valid;
        if (a.pos_valid) {
            a.lat = e.pos.latitude;
            a.lon = e.pos.longitude;
        }

        a.altitude_valid = e.pos.alt_valid;
        if (a.altitude_valid) a.altitude_ft = e.pos.altitude;

        /* Surface-position frames (ME types 5..8) are not decoded by this port
         * — SURFACE_POS_L/H are declared but never used — so there is no
         * trustworthy on-ground indication to publish. It stays false rather
         * than being guessed from a zero altitude, which is exactly the
         * inference decode_ac13_altitude() deliberately refuses to make. */
        a.on_ground = false;

        a.velocity_valid = e.velo.valid;
        if (a.velocity_valid) {
            /* get_ground_speed() applies the app's own IAS-to-ground-speed
             * correction, so the web view and the device agree. */
            a.speed_kt = e.get_ground_speed();
            a.heading_deg = e.velo.heading;
            a.vertical_rate_fpm = e.velo.v_rate;
        }

        a.squawk = e.sqwk;
        a.messages = e.hits;
        a.age_s = e.age;
        /* The same number the device's Amp column shows (upstream's amp >> 9),
         * so the portal's Sig column and the screen agree. */
        a.amp = e.amp_display();

        out.aircraft.push_back(std::move(a));
    }

    return panel;
}

const ProviderRegistrar reg_adsb{"adsbrx", PanelKind::Adsb, adsb_panel};

}  // namespace
}  // namespace remote
