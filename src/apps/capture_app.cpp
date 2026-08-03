/*
 * mayhem-b200 — IQ capture to disk.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "capture_app.hpp"

#include "../core/file_path.hpp"
#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

namespace app {

CaptureView::CaptureView()
    : receiver_{*globals().receiver} {
    add_children({&labels_, &field_frequency_, &options_rate_, &field_gain_,
                  &button_record_, &button_back_, &console_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    field_frequency_.set_value(receiver_.target_frequency(), false);
    field_frequency_.on_change = [this](uint64_t hz) { receiver_.set_target_frequency(hz); };

    options_rate_.set_by_nearest_value(static_cast<int32_t>(receiver_.sampling_rate()), false);
    options_rate_.on_change = [this](size_t, int32_t value) {
        /* Changing the rate mid-recording would make the file's metadata a lie. */
        if (receiver_.capturing()) return;
        receiver_.set_sampling_rate(static_cast<double>(value));
    };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    button_record_.on_select = [this](ui::Button&) { toggle_capture(); };
    button_back_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

CaptureView::~CaptureView() {
    /* Never leave a recording running after the view is gone — the user has no
     * way to stop it from anywhere else. */
    receiver_.stop_capture();
}

void CaptureView::on_show() {
    View::on_show();
    button_record_.focus();

    if (!receiver_.running()) receiver_.start();
    refresh_status();
}

void CaptureView::toggle_capture() {
    if (receiver_.capturing()) {
        receiver_.stop_capture();
        button_record_.set_text("Record");
        refresh_status();
        return;
    }

    const auto directory = core::captures_directory();
    if (!core::ensure_directory(directory)) {
        console_.writeln(STR_COLOR_RED "Cannot create capture folder");
        return;
    }

    const std::string stem = directory + "/MB200_" + to_string_timestamp_now();

    if (!receiver_.start_capture(stem)) {
        console_.writeln(STR_COLOR_RED "Capture failed:");
        console_.writeln(truncate(receiver_.capture_error(), 29));
        return;
    }

    button_record_.set_text("Stop");
    refresh_status();
}

void CaptureView::refresh_status() {
    console_.clear();
    console_.enable_scrolling(false);

    const bool active = receiver_.capturing();

    console_.writeln(active ? STR_COLOR_RED "RECORDING" : STR_COLOR_LIGHT_GREY "Idle");
    console_.writeln("");

    if (!receiver_.capture_path().empty()) {
        console_.writeln("File:");
        /* Show the tail of the path; the console is 30 columns wide. */
        const auto& path = receiver_.capture_path();
        console_.writeln(path.size() > 29 ? path.substr(path.size() - 29) : path);
        console_.writeln("");

        const uint64_t bytes = receiver_.captured_bytes();
        console_.writeln("Size: " + to_string_file_size(bytes));

        const double rate = receiver_.sampling_rate();
        if (rate > 0.0) {
            const double seconds = static_cast<double>(bytes) / (rate * 4.0);
            console_.writeln("Time: " + to_string_decimal(static_cast<float>(seconds), 1) + " s");
        }
    }

    console_.writeln("");
    console_.writeln(STR_COLOR_LIGHT_GREY "Format: .C16 int16 IQ");
    console_.writeln(STR_COLOR_LIGHT_GREY "Sidecar: .TXT (Mayhem)");

    const double rate = receiver_.sampling_rate();
    console_.writeln(STR_COLOR_LIGHT_GREY "Disk rate: " +
                     to_string_decimal(static_cast<float>(rate * 4.0 / 1e6), 1) + " MB/s");
}

void CaptureView::on_frame_sync() {
    View::on_frame_sync();

    const bool active = receiver_.capturing();

    /* The recorder stops itself if the disk fails; reflect that in the button. */
    if (active != last_capturing_) {
        last_capturing_ = active;
        button_record_.set_text(active ? "Stop" : "Record");
        refresh_status();
    }

    if (active && (++frame_counter_ % 15) == 0) refresh_status();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_capture{{"capture", "Capture", app::Category::Home,
                                  ui::Color::red(), &ui::bitmap_icon_capture,
                                  [] { return std::make_unique<app::CaptureView>(); }}};
}  // namespace
