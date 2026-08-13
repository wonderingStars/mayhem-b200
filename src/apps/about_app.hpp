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
 * redesigned around the same work. */
constexpr const char* kVersion = "0.10.0";

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
