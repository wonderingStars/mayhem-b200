/*
 * mayhem-b200 — web portal panel provider for WeFax.
 *
 * Publishes the picture WeFaxRxView has already decoded, so a browser can show
 * it larger than the device's 240 pixels. Nothing here demodulates or scales
 * anything: the SSB demod, the discriminator, the compressor and the pixel
 * clock all ran in src/apps/ui_wefax_rx.cpp long before this file sees a pixel.
 *
 * WHAT IS AND IS NOT PUBLISHED. WeFax is the same shape as NOAA APT and the
 * same caveat applies: the app keeps the ONE line currently being assembled
 * plus a 240-wide ScanCanvas preview, because wefax::preview_column() decimates
 * on the way in. Full-width lines exist only in the BMP capture on disk, and
 * only while capture is running. There is no full-resolution image in memory to
 * publish, so this file publishes the preview and says so in `note` rather than
 * inventing detail. See provider_apt.cpp's header for the long form.
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
#include "../apps/ui_wefax_rx.hpp"

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

/* WeFaxRxView pushes nothing of its own today, but the provider must not depend
 * on that — see provider_aprs.cpp's find_aprs_view(). */
app::WeFaxRxView* find_wefax_view(ui::View& top) {
    if (auto* v = dynamic_cast<app::WeFaxRxView*>(&top)) return v;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return nullptr;
    for (size_t i = 0; i < ctx.nav->depth(); i++) {
        if (auto* v = dynamic_cast<app::WeFaxRxView*>(ctx.nav->at_depth(i))) return v;
    }
    return nullptr;
}

PanelData wefax_panel(ui::View& view) {
    PanelData panel;

    app::WeFaxRxView* app_view = find_wefax_view(view);
    if (app_view == nullptr) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "WeFax is not the open app.";
        return panel;
    }

    /* UI-thread-only state; see provider_apt.cpp. */
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
        canvas.pixels(), width, height, rev_counter, "WeFax",
        "Screen preview, not a full-resolution fax: the decoder decimates each "
        "line onto the 240-column canvas it draws (the app's own "
        "preview_column). Full-width lines go only to the BMP capture on disk, "
        "and only while capture is running.");
    return panel;
}

const ProviderRegistrar reg_wefax_rx{"wefax_rx", wefax_panel};

}  // namespace
}  // namespace remote
