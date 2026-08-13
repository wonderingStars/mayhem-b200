/*
 * mayhem-b200 — PortaPack Mayhem for the Ettus USRP B200
 *
 * Host software framebuffer standing in for the PortaPack's ILI9341 LCD.
 * The public API is deliberately identical to lcd::ILI9341 so that drawing code
 * ported from the firmware compiles unchanged, including the vertical-scroll
 * region semantics the waterfall depends on.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original API)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __HOST_DISPLAY_H__
#define __HOST_DISPLAY_H__

#include "ui.hpp"
#include "ui_text.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace host {

/* Software framebuffer with the ILI9341's drawing surface API.
 *
 * Pixels are stored RGB565 exactly as the panel would hold them, so ported code
 * that reasons about ui::Color bit layout stays correct. The window layer
 * (see window.hpp) converts to RGB888 once per frame when compositing.
 *
 * Thread safety: DSP threads never draw. All drawing happens on the UI thread.
 * The compositor runs on the UI thread too, but takes the same lock so a future
 * background blit stays safe. */
class Display {
   public:
    Display();

    Display(const Display&) = delete;
    Display(Display&&) = delete;
    void operator=(const Display&) = delete;

    /* --- Lifecycle (no-ops on host; kept so ported code can call them) --- */
    void init();
    void shutdown();
    void sleep(bool hw_sleep = true);
    void wake(bool hw_sleep = true);

    /* --- Geometry --- */
    ui::Dim width() const { return static_cast<ui::Dim>(ui::screen_width); }
    ui::Dim height() const { return static_cast<ui::Dim>(ui::screen_height); }
    ui::Rect screen_rect() const { return {0, 0, width(), height()}; }

    /* --- Primitives --- */
    void fill_rectangle(ui::Rect r, const ui::Color c);
    /* The firmware unrolled this loop by 8 to keep up with the LCD bus. Here it
     * is just an alias; kept so ported call sites compile. */
    void fill_rectangle_unrolled8(ui::Rect r, const ui::Color c) { fill_rectangle(r, c); }

    void draw_pixel(const ui::Point p, const ui::Color color);
    void draw_line(const ui::Point start, const ui::Point end, const ui::Color color);
    void fill_circle(const ui::Point center,
                     const ui::Dim radius,
                     const ui::Color foreground,
                     const ui::Color background);

    void draw_bitmap(const ui::Point p,
                     const ui::Size size,
                     const uint8_t* const data,
                     const ui::Color foreground,
                     const ui::Color background,
                     uint8_t zoom_level = 1);

    void draw_glyph(const ui::Point p,
                    const ui::Glyph& glyph,
                    const ui::Color foreground,
                    const ui::Color background,
                    uint8_t zoom_level = 1);

    /* Writes `count` pixels row-major into rect `r`. */
    void draw_pixels(const ui::Rect r, const ui::Color* const colors, const size_t count);
    void read_pixels(const ui::Rect r, ui::ColorRGB888* const colors, const size_t count);

    template <size_t N>
    void draw_pixels(const ui::Rect r, const std::array<ui::Color, N>& colors) {
        draw_pixels(r, colors.data(), colors.size());
    }
    void draw_pixels(const ui::Rect r, const std::vector<ui::Color>& colors) {
        draw_pixels(r, colors.data(), colors.size());
    }
    template <size_t N>
    void read_pixels(const ui::Rect r, std::array<ui::ColorRGB888, N>& colors) {
        read_pixels(r, colors.data(), colors.size());
    }
    void read_pixels(const ui::Rect r, std::vector<ui::ColorRGB888>& colors) {
        read_pixels(r, colors.data(), colors.size());
    }

    /* One horizontal run of pixels — the waterfall's hot path. */
    void render_line(const ui::Point p, const uint16_t count, const ui::Color* line_buffer);
    void render_box(const ui::Point p, const ui::Size s, const ui::Color* line_buffer);

    /* --- Vertical scrolling ---
     * Mirrors the ILI9341's hardware scroll: the framebuffer contents do not
     * move, only the line the panel starts reading from. `scroll_area_y()` maps
     * a visual offset within the scroll region to the framebuffer row to write,
     * and the compositor applies the inverse when blitting. */
    void scroll_set_area(const ui::Coord top_y, const ui::Coord bottom_y);
    void scroll_disable();
    ui::Coord scroll_set_position(const ui::Coord position);
    ui::Coord scroll(const int32_t delta);
    ui::Coord scroll_area_y(const ui::Coord y) const;

    /* --- Compositing (called by the window layer) ---
     * Expands the RGB565 framebuffer into a caller-supplied 0x00RRGGBB buffer of
     * width()*height() entries, applying the active scroll mapping. */
    void composite_bgra(uint32_t* out) const;

    /* Same visual result as composite_bgra(), into a width()*height() buffer of
     * the framebuffer's own RGB565 words. The web portal streams these verbatim
     * (see the /api/screen frame layout in remote/remote_server.hpp), and going
     * through composite_bgra() would cost an RGB565 -> RGB888 -> RGB565 round
     * trip per frame. read_pixels() cannot stand in for this: it reads raw rows
     * without the scroll mapping, so the waterfall region of every spectrum-style
     * app would tear. */
    void composite_rgb565(uint16_t* out) const;

    /* True if anything has been drawn since the last composite. Lets the window
     * layer skip redundant blits. */
    bool take_damage();

    /* Monotonic count of damage events. take_damage() is destructive and
     * therefore single-consumer — the window layer owns it, and a second reader
     * would silently stop the window repainting. This is the non-destructive
     * form: a second consumer (the portal's frame capture) remembers the last
     * value it saw and compares, consuming nothing. Wraps at 2^32 like any
     * counter; the comparison callers do is equality, which wrapping does not
     * break. */
    uint32_t damage_generation() const { return damage_generation_.load(std::memory_order_relaxed); }

    void mark_damaged() {
        damaged_ = true;
        damage_generation_.fetch_add(1, std::memory_order_relaxed);
    }

    std::mutex& lock() { return mutex_; }

   private:
    /* Raises both damage signals at once. Every drawing primitive calls this
     * instead of assigning damaged_ directly, so a new consumer can never be
     * added by touching only one of the two. Callers already hold mutex_;
     * damage_generation_ is atomic anyway because mark_damaged() is the one
     * entry point that does not. */
    void mark_damaged_locked() {
        damaged_ = true;
        damage_generation_.fetch_add(1, std::memory_order_relaxed);
    }

    /* Row of the framebuffer, no scroll translation. Bounds-checked by callers. */
    uint16_t* row(int y) { return &fb_[static_cast<size_t>(y) * ui::screen_width]; }
    const uint16_t* row(int y) const { return &fb_[static_cast<size_t>(y) * ui::screen_width]; }

    /* Clips r to the screen. Returns an empty rect if fully off-screen. */
    ui::Rect clip(ui::Rect r) const;

    struct scroll_t {
        ui::Coord top_area;
        ui::Coord bottom_area;
        ui::Dim height;
        ui::Coord current_position;
    };

    std::vector<uint16_t> fb_;
    scroll_t scroll_state;
    bool scroll_enabled_{false};
    bool damaged_{true};
    /* Starts at 1, not 0, so it agrees with damaged_{true}: a consumer whose
     * "last seen" starts at 0 correctly reads the initial state as damaged. */
    std::atomic<uint32_t> damage_generation_{1};
    mutable std::mutex mutex_;
};

/* The single display instance, mirroring the firmware's portapack::display. */
extern Display display;

}  // namespace host

#endif /*__HOST_DISPLAY_H__*/
