/*
 * mayhem-b200 — web portal panel provider for NOAA APT.
 *
 * Publishes the picture NoaaAptRxView has already decoded, so a browser can
 * show it at whatever size the operator's screen allows instead of at the
 * device's 240 pixels. Nothing here demodulates, syncs or scales anything: the
 * AM envelope detection, the pixel clock, the sync-A correlation and the
 * greyscale mapping all happened in src/apps/ui_noaaapt_rx.cpp long before this
 * file sees a pixel.
 *
 * WHAT IS AND IS NOT PUBLISHED — read this before "fixing" the resolution.
 * An APT line is 2080 samples wide. The app keeps exactly two things: the ONE
 * line currently being assembled (line_, overwritten twice a second), and the
 * ScanCanvas preview it draws, which is 240 columns wide because
 * noaaapt::preview_column() throws away seven of every eight samples on the way
 * in. The full-width line is written only to the BMP capture on disk, and only
 * while the operator has capture running. There is therefore NO full-resolution
 * image in memory to publish, and this file does not invent one — it publishes
 * the 240-wide preview and says so in the payload's `note`. Widening it would
 * mean changing what the app itself keeps, which is an app-logic change.
 *
 * REV. Contract 4's `rev` must increment only when the picture actually
 * changed, because every bump costs the browser a fresh ~170 kB fetch. The app
 * exposes no line counter, so the change signal is a content hash of the
 * canvas: cheap (one pass over 240*232 uint16s), and truthful in both
 * directions — a decoder that has stopped producing lines does not bump, and a
 * canvas that was cleared and redrawn identically does not either.
 *
 * THREADING: a provider is only ever called from AppBridge::refresh(), i.e. on
 * the UI thread, so walking the view tree, hashing the canvas and holding the
 * function-local statics below all race nothing — see app_bridge.hpp's file
 * header.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/app_context.hpp"
#include "../apps/ui_navigation.hpp"
#include "../apps/ui_noaaapt_rx.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace remote {

/* Exposed rather than left file-local so tests/test_provider_media_geo.cpp can
 * drive the conversion with a canvas it filled itself. The three image
 * providers share it because they all read the same thing — a framebuffer of
 * ui::Color — and a second copy of this logic is a second place for the
 * geometry and the blank-detection to drift. */
ImageData image_data_from_pixels(const std::vector<ui::Color>& pixels,
                                 uint32_t width,
                                 uint32_t height,
                                 ImageRevCounter& rev_counter,
                                 std::string app_name,
                                 std::string resolution_note) {
    /* ui::Color is a single uint16_t (src/ui/ui.hpp); the helpers in
     * app_data.hpp are declared on uint16_t so that header stays independent of
     * src/ui. Checked rather than assumed. */
    static_assert(sizeof(ui::Color) == sizeof(uint16_t),
                  "ui::Color must be a bare RGB565 word for the cast below");

    ImageData img;
    img.app_name = std::move(app_name);

    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (width == 0 || height == 0 || pixels.size() != expected) {
        /* The widget's rect and its framebuffer disagree, which can only be a
         * bug in this file's geometry. Say nothing rather than serve a picture
         * that would render as diagonal garbage. */
        img.note = "Preview geometry is unavailable.";
        return img;
    }

    const auto* raw = reinterpret_cast<const uint16_t*>(pixels.data());

    if (rgb565_is_blank(raw, pixels.size())) {
        /* rev stays 0 and no pixels are sent: the canvas is in the state its
         * constructor (and clear()) leaves it in, so there is nothing decoded.
         * A black frame is emphatically not published as an image. */
        img.width = width;
        img.height = height;
        img.note = "No image decoded yet.";
        return img;
    }

    img.width = width;
    img.height = height;
    img.rev = rev_counter.observe(rgb565_content_hash(raw, pixels.size()));
    rgb565_to_rgb888(raw, pixels.size(), img.rgb);
    img.note = std::move(resolution_note);
    return img;
}

namespace {

/* The provider is handed nav->top(). NoaaAptRxView pushes nothing of its own
 * today, but the provider must not depend on that: anything the operator puts
 * on top of it has to leave the panel intact rather than blanking the browser.
 * Same walk as provider_aprs.cpp's find_aprs_view(). */
app::NoaaAptRxView* find_apt_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::NoaaAptRxView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::NoaaAptRxView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

PanelData apt_panel(ui::View& view) {
    PanelData panel;

    app::NoaaAptRxView* app_view = find_apt_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "NOAA APT is not the open app.";
        return panel;
    }

    /* UI-thread-only state, exactly like app_bridge.cpp's SpectrumScratch: a
     * function-local static rather than a member, because it is purely an
     * implementation detail of this one provider. */
    static ImageRevCounter rev_counter;

    const auto& canvas = app_view->canvas();
    const int rows = canvas.rows();
    const uint32_t height = (rows > 0) ? static_cast<uint32_t>(rows) : 0u;
    const uint32_t width =
        (height > 0 && canvas.pixels().size() % height == 0)
            ? static_cast<uint32_t>(canvas.pixels().size() / height)
            : 0u;

    panel.kind = PanelKind::Image;
    panel.image = image_data_from_pixels(
        canvas.pixels(), width, height, rev_counter, "NOAA APT",
        "Screen preview, not a full-resolution APT image: the decoder keeps one "
        "column of the preview per eight of the 2080 samples in a line (the app's "
        "own preview_column). Full-width lines go only to the BMP capture on disk, "
        "and only while capture is running.");
    return panel;
}

const ProviderRegistrar reg_noaaapt_rx{"noaaapt_rx", apt_panel};

}  // namespace
}  // namespace remote
