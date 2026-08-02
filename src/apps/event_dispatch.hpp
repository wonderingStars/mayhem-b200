/*
 * mayhem-b200 — input routing.
 *
 * Mirrors the firmware's EventDispatcher: keys go to the focused widget first
 * and fall back to directional focus movement, touches hit-test the widget tree
 * depth-first, and the encoder reaches the focused widget then the view.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_EVENT_DISPATCH_H__
#define __MB200_EVENT_DISPATCH_H__

#include "ui.hpp"
#include "ui_widget.hpp"
#include "window.hpp"

namespace app {

class EventDispatcher {
   public:
    EventDispatcher(ui::Widget& root, ui::Context& context);

    /* Routes one input event. Returns false if the app should exit. */
    bool dispatch(const host::Event& event);

    /* Called when Back is pressed and nothing else consumed it. */
    std::function<void()> on_back{};

   private:
    void dispatch_key(ui::KeyEvent key);
    void dispatch_encoder(ui::EncoderEvent delta);
    void dispatch_touch(const ui::TouchEvent& event);
    void dispatch_character(ui::KeyboardEvent c);

    /* Depth-first search for the deepest visible widget that accepts the touch. */
    static bool touch_widget(ui::Widget* widget, const ui::TouchEvent& event);

    ui::Widget& root_;
    ui::Context& context_;

    /* The widget that took a touch Start, so Move/End reach the same one even
     * if the pointer leaves its bounds. */
    ui::Widget* touch_capture_{nullptr};
};

}  // namespace app

#endif /*__MB200_EVENT_DISPATCH_H__*/
