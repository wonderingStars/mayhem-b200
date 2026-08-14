/*
 * mayhem-b200 — Win32 window hosting the 240x320 framebuffer.
 *
 * Replaces the PortaPack's physical panel, five-way switch, rotary encoder and
 * resistive touch screen with a desktop window, the keyboard and the mouse.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version. See LICENSE.GPL-2.0-or-later.
 */

#ifndef __HOST_WINDOW_H__
#define __HOST_WINDOW_H__

#include "ui.hpp"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace host {

/* One input event, in the same vocabulary the firmware's EventDispatcher uses. */
struct Event {
    enum class Type : uint8_t {
        Key,       /* five-way switch / Back / DFU */
        Encoder,   /* rotary encoder detents, signed */
        Touch,     /* touch panel */
        Character, /* printable character, for text entry */
        Quit,      /* window closed */
    };

    Type type{Type::Quit};
    ui::KeyEvent key{ui::KeyEvent::Select};
    ui::EncoderEvent encoder{0};
    ui::TouchEvent touch{};
    ui::KeyboardEvent character{0};
};

/* Keyboard/mouse mapping (also printed by --help and shown in the About app):
 *
 *   Arrow keys .............. five-way switch: Up / Down / Left / Right
 *   Enter, Space ............ Select
 *   Escape, Backspace ....... Back
 *   Mouse wheel ............. rotary encoder
 *   PageUp / PageDown ....... rotary encoder, one detent
 *   Left mouse button ....... touch panel (press / drag / release)
 *   Printable keys .......... text entry
 *   F11 ..................... cycle window scale (1x - 4x)
 */
class Window {
   public:
    Window();
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /* Creates the window. `scale` is the integer pixel magnification.
     *
     * `hidden` creates it but never shows it: the message queue, the DIB and
     * the framebuffer all still work, so the portal keeps getting frames (it
     * composites from the software framebuffer, not from this window), but
     * nothing appears on screen. Used by the launcher for browser-only
     * operation. A hidden window receives no keyboard or mouse input — it
     * cannot be focused — so local control is remote-only in that mode, and
     * the tray icon becomes the only way to quit or bring the window back. */
    bool create(const std::string& title, int scale = 2, bool hidden = false);
    void destroy();

    /* Show or hide the window after creation (the tray menu drives this). */
    void set_visible(bool visible);
    bool visible() const { return visible_; }

    /* Pumps pending Win32 messages. Returns false once the window is closed. */
    bool pump();

    /* Copies the display framebuffer to the window if anything changed.
     * `force` blits even when the display reports no damage. */
    void present(bool force = false);

    /* Pops the next queued input event. Returns false when the queue is empty. */
    bool poll_event(Event& out);

    void set_title(const std::string& title);

    int scale() const { return scale_; }
    void set_scale(int scale);

    bool closed() const { return closed_; }

    /* Public only so the window procedure, which lives at file scope in
     * window.cpp, can reach it. Not part of the intended API. */
    struct Impl;

   private:
    Impl* impl_{nullptr};
    int scale_{2};
    bool closed_{false};
    bool visible_{true};
};

}  // namespace host

#endif /*__HOST_WINDOW_H__*/
