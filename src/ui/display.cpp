/*
 * mayhem-b200 — host software framebuffer implementing the ILI9341 drawing API.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original API/semantics)
 * Copyright (C) 2026 mayhem-b200 contributors
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

#include "display.hpp"

#include <algorithm>
#include <cstdlib>

namespace host {

Display display;

Display::Display()
    : fb_(static_cast<size_t>(ui::screen_width) * ui::screen_height, 0),
      scroll_state{0, 0, static_cast<ui::Dim>(ui::screen_height), 0} {
}

void Display::init() {
    std::lock_guard<std::mutex> g{mutex_};
    std::fill(fb_.begin(), fb_.end(), 0);
    scroll_enabled_ = false;
    scroll_state = {0, 0, static_cast<ui::Dim>(ui::screen_height), 0};
    damaged_ = true;
}

void Display::shutdown() {}
void Display::sleep(bool) {}
void Display::wake(bool) {}

ui::Rect Display::clip(ui::Rect r) const {
    return r.intersect({0, 0, width(), height()});
}

void Display::fill_rectangle(ui::Rect r, const ui::Color c) {
    const auto cr = clip(r);
    if (cr.is_empty()) return;

    std::lock_guard<std::mutex> g{mutex_};
    for (int y = cr.top(); y < cr.bottom(); y++) {
        uint16_t* dst = row(y) + cr.left();
        std::fill(dst, dst + cr.width(), c.v);
    }
    damaged_ = true;
}

void Display::draw_pixel(const ui::Point p, const ui::Color color) {
    if (p.x() < 0 || p.y() < 0 || p.x() >= width() || p.y() >= height()) return;

    std::lock_guard<std::mutex> g{mutex_};
    row(p.y())[p.x()] = color.v;
    damaged_ = true;
}

void Display::draw_line(const ui::Point start, const ui::Point end, const ui::Color color) {
    /* Integer Bresenham, matching the firmware's line rasterisation. */
    int x0 = start.x(), y0 = start.y();
    const int x1 = end.x(), y1 = end.y();

    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int sx = (x0 < x1) ? 1 : -1;
    const int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        draw_pixel({x0, y0}, color);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void Display::fill_circle(const ui::Point center,
                          const ui::Dim radius,
                          const ui::Color foreground,
                          const ui::Color background) {
    const int r2 = radius * radius;
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            const bool inside = (dx * dx + dy * dy) <= r2;
            if (inside)
                draw_pixel({center.x() + dx, center.y() + dy}, foreground);
            else if (background.v != ui::Color::magenta().v)
                draw_pixel({center.x() + dx, center.y() + dy}, background);
        }
    }
}

void Display::draw_bitmap(const ui::Point p,
                          const ui::Size size,
                          const uint8_t* const pixels,
                          const ui::Color foreground,
                          const ui::Color background,
                          uint8_t zoom_level) {
    /* Bit layout is the firmware's: index = y*w + x, LSB-first within each byte.
     * A magenta background means "transparent" — same convention as upstream. */
    const bool transparent = (background.v == ui::Color::magenta().v);
    if (zoom_level < 1) zoom_level = 1;

    if (zoom_level == 1) {
        std::lock_guard<std::mutex> g{mutex_};
        for (int y = 0; y < size.height(); y++) {
            const int py = p.y() + y;
            if (py < 0 || py >= height()) continue;
            uint16_t* dst = row(py);
            for (int x = 0; x < size.width(); x++) {
                const int px = p.x() + x;
                if (px < 0 || px >= width()) continue;
                const size_t i = static_cast<size_t>(y) * size.width() + x;
                const bool on = (pixels[i >> 3] & (1U << (i & 0x7))) != 0;
                if (on)
                    dst[px] = foreground.v;
                else if (!transparent)
                    dst[px] = background.v;
            }
        }
        damaged_ = true;
        return;
    }

    for (int y = 0; y < size.height(); y++) {
        for (int x = 0; x < size.width(); x++) {
            const size_t i = static_cast<size_t>(y) * size.width() + x;
            const bool on = (pixels[i >> 3] & (1U << (i & 0x7))) != 0;
            if (on || !transparent) {
                fill_rectangle({p.x() + x * zoom_level, p.y() + y * zoom_level,
                                zoom_level, zoom_level},
                               on ? foreground : background);
            }
        }
    }
}

void Display::draw_glyph(const ui::Point p,
                         const ui::Glyph& glyph,
                         const ui::Color foreground,
                         const ui::Color background,
                         uint8_t zoom_level) {
    draw_bitmap(p, glyph.size(), glyph.pixels(), foreground, background, zoom_level);
}

void Display::draw_pixels(const ui::Rect r, const ui::Color* const colors, const size_t count) {
    if (r.is_empty() || colors == nullptr) return;

    std::lock_guard<std::mutex> g{mutex_};
    size_t i = 0;
    for (int y = r.top(); y < r.bottom() && i < count; y++) {
        for (int x = r.left(); x < r.right() && i < count; x++, i++) {
            if (x < 0 || y < 0 || x >= width() || y >= height()) continue;
            row(y)[x] = colors[i].v;
        }
    }
    damaged_ = true;
}

void Display::read_pixels(const ui::Rect r, ui::ColorRGB888* const colors, const size_t count) {
    if (r.is_empty() || colors == nullptr) return;

    std::lock_guard<std::mutex> g{mutex_};
    size_t i = 0;
    for (int y = r.top(); y < r.bottom() && i < count; y++) {
        for (int x = r.left(); x < r.right() && i < count; x++, i++) {
            if (x < 0 || y < 0 || x >= width() || y >= height()) {
                colors[i] = {0, 0, 0};
                continue;
            }
            ui::Color c{row(y)[x]};
            colors[i] = {c.r(), c.g(), c.b()};
        }
    }
}

void Display::render_line(const ui::Point p, const uint16_t count, const ui::Color* line_buffer) {
    if (line_buffer == nullptr || count == 0) return;
    if (p.y() < 0 || p.y() >= height()) return;

    std::lock_guard<std::mutex> g{mutex_};
    uint16_t* dst = row(p.y());
    for (uint16_t i = 0; i < count; i++) {
        const int x = p.x() + i;
        if (x < 0 || x >= width()) continue;
        dst[x] = line_buffer[i].v;
    }
    damaged_ = true;
}

void Display::render_box(const ui::Point p, const ui::Size s, const ui::Color* line_buffer) {
    draw_pixels({p, s}, line_buffer, static_cast<size_t>(s.width()) * s.height());
}

/* --- Vertical scroll region -------------------------------------------------
 * Identical arithmetic to lcd::ILI9341 so ported waterfall code behaves the
 * same. Only the effect differs: the panel remapped at scan-out time, we remap
 * at composite time. */

void Display::scroll_set_area(const ui::Coord top_y, const ui::Coord bottom_y) {
    std::lock_guard<std::mutex> g{mutex_};
    scroll_state.top_area = top_y;
    scroll_state.bottom_area = static_cast<ui::Coord>(height() - bottom_y);
    scroll_state.height = static_cast<ui::Dim>(bottom_y - top_y);
    if (scroll_state.height <= 0) scroll_state.height = 1;
    scroll_state.current_position = 0;
    scroll_enabled_ = true;
    damaged_ = true;
}

ui::Coord Display::scroll_set_position(const ui::Coord position) {
    std::lock_guard<std::mutex> g{mutex_};
    if (scroll_state.height <= 0) return 0;
    scroll_state.current_position = static_cast<ui::Coord>(position % scroll_state.height);
    damaged_ = true;
    return static_cast<ui::Coord>(scroll_state.top_area + scroll_state.current_position);
}

ui::Coord Display::scroll(const int32_t delta) {
    ui::Coord pos;
    {
        std::lock_guard<std::mutex> g{mutex_};
        if (scroll_state.height <= 0) return 0;
        pos = static_cast<ui::Coord>(scroll_state.current_position + scroll_state.height - delta);
    }
    return scroll_set_position(pos);
}

ui::Coord Display::scroll_area_y(const ui::Coord y) const {
    std::lock_guard<std::mutex> g{mutex_};
    if (scroll_state.height <= 0) return y;
    const auto wrapped_y = (scroll_state.current_position + y) % scroll_state.height;
    return static_cast<ui::Coord>(wrapped_y + scroll_state.top_area);
}

void Display::scroll_disable() {
    std::lock_guard<std::mutex> g{mutex_};
    scroll_enabled_ = false;
    scroll_state = {0, 0, static_cast<ui::Dim>(height()), 0};
    damaged_ = true;
}

/* --- Compositing ----------------------------------------------------------- */

void Display::composite_bgra(uint32_t* out) const {
    std::lock_guard<std::mutex> guard{mutex_};

    const int w = width();
    const int h = height();

    for (int vy = 0; vy < h; vy++) {
        /* Map visual row -> framebuffer row through the scroll region. Rows in
         * the fixed top/bottom areas map 1:1. */
        int fy = vy;
        if (scroll_enabled_ && scroll_state.height > 0) {
            const int top = scroll_state.top_area;
            const int sh = scroll_state.height;
            if (vy >= top && vy < top + sh) {
                const int k = vy - top;
                fy = top + ((scroll_state.current_position + k) % sh);
            }
        }

        const uint16_t* src = row(fy);
        uint32_t* dst = out + static_cast<size_t>(vy) * w;
        for (int x = 0; x < w; x++) {
            const uint16_t v = src[x];
            /* RGB565 -> RGB888 with the low bits replicated so full-scale
             * channels reach 0xFF rather than 0xF8. */
            const uint32_t r5 = (v >> 11) & 0x1F;
            const uint32_t g6 = (v >> 5) & 0x3F;
            const uint32_t b5 = v & 0x1F;
            const uint32_t r = (r5 << 3) | (r5 >> 2);
            const uint32_t g = (g6 << 2) | (g6 >> 4);
            const uint32_t b = (b5 << 3) | (b5 >> 2);
            dst[x] = (r << 16) | (g << 8) | b;
        }
    }
}

bool Display::take_damage() {
    std::lock_guard<std::mutex> g{mutex_};
    const bool d = damaged_;
    damaged_ = false;
    return d;
}

}  // namespace host
