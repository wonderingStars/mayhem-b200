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
 * software-complete and tested, not yet verified against a physical B200. */
constexpr const char* kVersion = "0.9.0";

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
