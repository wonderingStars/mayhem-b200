/*
 * mayhem-b200 — web portal panel provider for SSTV RX.
 *
 * Publishes the picture SstvRxView has already decoded, so a browser can show
 * it larger than the device's 240 pixels. Nothing here demodulates: the VIS
 * detection, the mode timing, the sync and the per-line colour reconstruction
 * all ran in src/apps/ui_sstvrx.cpp before this file sees a pixel.
 *
 * WHAT IS AND IS NOT PUBLISHED. An SSTV line is 320 pixels wide
 * (sstv::kPixelsPerLine) and the decoder produces it in full, but SstvImage
 * scales it down to the 240-pixel widget on the way in (SstvImage::set_line)
 * and wraps at the bottom of the widget rather than scrolling, so a mode taller
 * than the widget overwrites its own top. Only the BMP capture on disk keeps
 * the 320-wide lines. There is no full-resolution image in memory to publish,
 * so this file publishes what the app really holds — the 240-wide widget — and
 * says so in `note`. See provider_apt.cpp's header for the long form.
 *
 * THREADING: a provider is only ever called from AppBridge::refresh(), i.e. on
 * the UI thread — see app_bridge.hpp's file header.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/app_context.hpp"
#include "../apps/ui_navigation.hpp"
#include "../apps/ui_sstvrx.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace remote {

/* Defined in provider_apt.cpp; shared so the three image providers cannot
 * drift apart on geometry checks, blank detection or rev bookkeeping. */
ImageData image_data_from_pixels(const std::vector<ui::Color>& pixels,
                                 uint32_t width,
                                 uint32_t height,
                                 ImageRevCounter& rev_counter,
                                 std::string app_name,
                                 std::string resolution_note);

namespace {

/* SstvRxView pushes nothing of its own today, but the provider must not depend
 * on that — see provider_aprs.cpp's find_aprs_view(). */
app::SstvRxView* find_sstv_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::SstvRxView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::SstvRxView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

PanelData sstv_panel(ui::View& view) {
    PanelData panel;

    app::SstvRxView* app_view = find_sstv_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "SSTV RX is not the open app.";
        return panel;
    }

    /* UI-thread-only state; see provider_apt.cpp. */
    static ImageRevCounter rev_counter;

    /* SstvImage sizes its framebuffer from the rect it was constructed with and
     * never resizes, so the widget rect is the picture's geometry.
     * image_data_from_pixels() cross-checks the two against each other. */
    const auto& image = app_view->image();
    const auto rect = image.parent_rect();
    const uint32_t width = (rect.width() > 0) ? static_cast<uint32_t>(rect.width()) : 0u;
    const uint32_t height = (rect.height() > 0) ? static_cast<uint32_t>(rect.height()) : 0u;

    panel.kind = PanelKind::Image;
    panel.image = image_data_from_pixels(
        image.pixels(), width, height, rev_counter, "SSTV RX",
        "Screen preview, not the full-resolution picture: each 320-pixel line is "
        "scaled onto the 240-wide widget the app draws, and a mode taller than "
        "the widget wraps onto its own top rows. Full-width lines go only to the "
        "BMP capture on disk.");
    return panel;
}

const ProviderRegistrar reg_sstvrx{"sstvrx", sstv_panel};

}  // namespace
}  // namespace remote
