/*
 * mayhem-b200 — about / help screen.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_ABOUT_APP_H__
#define __MB200_ABOUT_APP_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <string>

namespace app {

/* Project version, shown here and in the window title.
 * 0.9.0: full Mayhem app suite ported (~100 apps across all categories),
 * software-complete and tested, not yet verified against a physical B200.
 * 0.9.1: ADS-B frame amplitude is referred to the measured noise floor and
 * carried as a float, so the Amp column and the portal's Sig column report a
 * real signal strength on a B200 instead of zero.
 * 0.9.2: the web portal streams the 240x320 framebuffer (GET /api/screen) and
 * routes input back (POST /api/input), so every app is operable from a
 * browser rather than only the handful with a structured panel.
 * 0.9.3: the portal now reads which app is open off the navigation stack
 * instead of remembering the last launch, so navigating with the keys no
 * longer serves one app's panel under another app's title; GET /api/panel
 * honours ?have_image_rev=N; and a bare GET /api/screen keeps working past
 * frame 2^31.
 * 0.10.0: the receiver's sample tap is a gapless ring buffer rather than a
 * destructive snapshot, and the rate is chosen from the attached radio's
 * reported capabilities instead of being fixed at the HackRF's. Measured on
 * live 1090 MHz traffic in paired alternating windows: 0.485 -> 22.88 accepted
 * ADS-B frames/sec, 2-3 -> 7-11 aircraft tracked. The web portal is
 * redesigned around the same work.
 * 0.11.0: a dropped sdrlink connection recovers instead of ending the
 * session. The control link and the IQ stream both re-establish themselves
 * with backed-off retries, the session's tuning is replayed onto the new
 * connection, and GET /api/status carries a "link" field so a link that is
 * coming back is distinguishable from one that has given up.
 * 0.11.1: a keepalive ping, so an IDLE client notices a dead link at all.
 * Verified against a live sdrlink server and a B200: without it, killing the
 * server left the client reporting "connected" indefinitely.
 * 0.11.2: GET /api/status carries can_transmit, from the attached radio's
 * DeviceCaps.has_tx, so the portal can lock the transmit apps on a
 * receive-only SDR instead of offering ~28 apps that cannot work. Sent only
 * while a device is open: with nothing attached there is no honest answer.
 * 0.11.3: rebuild the RX streamer when UHD moves the B200's master clock.
 * Changing the sample rate while streaming re-initialises the radio under the
 * streamer already handed out, and the receiver then goes silently deaf --
 * which is why an app opened directly from another app could fail to
 * receive.
 * 0.11.4: hardware-gated regression tests (MB200_HW_TESTS=1) run the
 * clock-move and transmit paths against a real USRP; first hardware evidence
 * the TX plumbing works. set_rx_rate keeps caps().master_clock_rate honest on
 * every call. Test-suite hardening: no fixture collides across concurrent
 * processes, no detached thread can dangle a stack frame, and scheduling
 * assumptions under load became guarantees.
 * 0.12.0: the capability round. GET /api/apps carries panel_kind so the grid
 * badges apps with native views; analog RX bandwidth follows the chosen rate
 * and gain is validated against the attached radio's caps (capability_policy,
 * beside rate_policy); FPV Detect's retune storm and frozen level are fixed
 * (first reading now ~1 s, was 10-15 s); the sdrlink reconnect replays gain
 * in dB rather than Hz; and the USRP RX thread survives the app-switch race
 * (start-command retry across streamer generations, plus a starvation
 * watchdog that re-kicks a stream the hardware accepted but never fed). */
constexpr const char* kVersion = "0.12.0";

class AboutView : public ui::View {
   public:
    AboutView();

    std::string title() const override { return "About"; }

    void on_show() override;

   private:
    ui::Console console_{{0, 4, 240, 272}};
    ui::Button button_done_{{72, 280, 96, 24}, "Back"};
};

}  // namespace app

#endif /*__MB200_ABOUT_APP_H__*/
