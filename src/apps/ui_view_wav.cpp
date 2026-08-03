/*
 * mayhem-b200 — WAV waveform viewer implementation.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_view_wav.hpp"

#include "../core/file_path.hpp"
#include "../core/fs_utils.hpp"
#include "../core/string_format.hpp"
#include "app_context.hpp"
#include "spectrum_color_lut.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace app {

using namespace ui;

/* --- WavFilePickerView ----------------------------------------------------- */

WavFilePickerView::WavFilePickerView() {
    add_children({&header_, &menu_, &empty_note_, &button_back_});

    directory_ = core::captures_directory();
    core::ensure_directory(directory_);
    header_.set("WAV in " + core::filename(directory_) + ":");

    std::vector<core::DirEntry> entries;
    core::ListOptions options{".WAV"};
    options.include_directories = false;
    core::list_directory(directory_, entries, options);

    if (entries.empty()) {
        empty_note_.set("No .WAV files found in\n" + directory_);
        menu_.hidden(true);
    } else {
        empty_note_.hidden(true);
        for (const auto& e : entries) {
            const std::string full = core::path_join(directory_, e.name);
            menu_.add_item({e.name,
                            Theme::getInstance()->fg_light->foreground,
                            [this, full]() {
                                if (on_pick) on_pick(full);
                                if (auto* nav = globals().nav) nav->pop();
                            }});
        }
    }

    button_back_.on_select = [](Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void WavFilePickerView::on_show() {
    View::on_show();
    focus();
}

void WavFilePickerView::focus() {
    if (menu_.item_count() > 0)
        menu_.focus();
    else
        button_back_.focus();
}

/* --- WavViewerView --------------------------------------------------------- */

WavViewerView::WavViewerView() {
    waveform_buffer_.assign(kColumns, 0);
    overview_peaks_.assign(kColumns, 0);
    waveform_.set_data(waveform_buffer_.data());
    waveform_.set_length(kColumns);

    add_children({&labels_,
                  &text_filename_,
                  &text_samplerate_,
                  &text_title_,
                  &text_duration_,
                  &button_open_,
                  &waveform_,
                  &field_pos_seconds_,
                  &field_pos_ms_,
                  &field_scale_,
                  &field_pos_samples_,
                  &field_cursor_a_,
                  &field_cursor_b_,
                  &text_delta_,
                  &text_status_,
                  &text_note_});

    text_note_.set(STR_COLOR_LIGHT_GREY "Viewer only: no audio on B200");
    set_status("Open an 8/16-bit mono WAV");

    button_open_.on_select = [this](Button&) { open_picker(); };

    field_scale_.on_change = [this](int32_t v) { update_scale(v); };
    field_pos_seconds_.on_change = [this](int32_t) { on_pos_time_changed(); };
    field_pos_ms_.on_change = [this](int32_t) { on_pos_time_changed(); };
    field_pos_samples_.on_change = [this](int32_t) { on_pos_sample_changed(); };

    field_cursor_a_.on_change = [this](int32_t v) {
        waveform_.set_cursor(0, static_cast<int16_t>(v));
        refresh_measurements();
    };
    field_cursor_b_.on_change = [this](int32_t v) {
        waveform_.set_cursor(1, static_cast<int16_t>(v));
        refresh_measurements();
    };
}

void WavViewerView::open_picker() {
    auto picker = std::make_unique<WavFilePickerView>();
    picker->on_pick = [this](const std::string& path) { load(path); };
    if (auto* nav = globals().nav) nav->push(std::move(picker));
}

bool WavViewerView::load(const std::string& path) {
    reader_.close();

    if (!reader_.open(path)) {
        set_status(STR_COLOR_RED "File read error");
        return false;
    }

    if (reader_.channels() != 1 ||
        (reader_.bits_per_sample() != 8 && reader_.bits_per_sample() != 16)) {
        set_status(STR_COLOR_RED "Need 8/16-bit mono WAV");
        reader_.close();
        return false;
    }

    const uint32_t total = reader_.sample_count();
    const size_t n = std::min<size_t>(total, kMaxSamples);
    samples_.assign(n, 0);
    reader_.data_seek(0);
    const size_t got = reader_.read_samples(samples_.data(), n);
    samples_.resize(got);

    overview_peaks_.assign(kColumns, 0);
    wav_view_detail::peak_columns(samples_.data(), samples_.size(),
                                  overview_peaks_.data(), kColumns);

    text_filename_.set(core::filename(path));
    text_samplerate_.set(to_string_dec_uint(reader_.sample_rate()) + "Hz " +
                         to_string_dec_uint(reader_.bits_per_sample()) + "-bit mono");
    text_title_.set(reader_.title());
    /* ms_duration is milliseconds; the host unit_prefix table is
     * {p,n,u,m,_,k,M}, so milli is index 3 (upstream's index 2 predates the
     * added pico slot). */
    text_duration_.set(unit_auto_scale(reader_.ms_duration(), 3, 3) + "s");

    loaded_ = true;
    reset_controls();
    update_scale(1);

    if (total > kMaxSamples)
        set_status(STR_COLOR_YELLOW "Long file: truncated view");
    else
        set_status(STR_COLOR_GREEN "Loaded");

    field_pos_seconds_.focus();
    set_dirty();
    return true;
}

void WavViewerView::reset_controls() {
    const int32_t sample_count = static_cast<int32_t>(samples_.size());
    const uint32_t ms = reader_.ms_duration();

    field_scale_.set_value(1, false);
    field_scale_.set_range(
        1, std::max<int32_t>(1, std::min<int32_t>(99999, sample_count / static_cast<int32_t>(kColumns))));

    field_pos_seconds_.set_value(0, false);
    field_pos_seconds_.set_range(0, static_cast<int32_t>(ms / 1000));

    field_pos_ms_.set_value(0, false);
    field_pos_ms_.set_range(0, (ms < 1000) ? static_cast<int32_t>(ms % 1000) : 999);

    field_pos_samples_.set_value(0, false);
    field_pos_samples_.set_range(0, sample_count > 0 ? sample_count - 1 : 0);

    field_cursor_a_.set_value(0, false);
    field_cursor_b_.set_value(0, false);
    waveform_.set_cursor(0, 0);
    waveform_.set_cursor(1, 0);

    position_ = 0;
}

void WavViewerView::update_scale(int32_t new_scale) {
    scale_ = new_scale < 1 ? 1 : new_scale;
    ns_per_pixel_ = wav_view_detail::ns_per_pixel(reader_.sample_rate(), scale_);
    refresh_waveform();
    refresh_measurements();
}

void WavViewerView::refresh_waveform() {
    if (!loaded_) return;

    for (size_t i = 0; i < kColumns; i++) {
        const uint64_t idx = position_ + static_cast<uint64_t>(i) * scale_;
        waveform_buffer_[i] = (idx < samples_.size()) ? samples_[idx] : 0;
    }
    waveform_.set_dirty();
    set_dirty();
}

void WavViewerView::refresh_measurements() {
    const int32_t a = field_cursor_a_.value();
    const int32_t b = field_cursor_b_.value();
    const uint64_t span_ns = ns_per_pixel_ * static_cast<uint64_t>(std::abs(b - a));

    if (span_ns) {
        /* base_unit 1 = nano on the host prefix table (see load()). */
        text_delta_.set(unit_auto_scale(static_cast<double>(span_ns), 1, 3) + "s (" +
                        to_string_dec_uint(1000000000ULL / span_ns) + "Hz)");
    } else {
        text_delta_.set("0ns ?Hz");
    }
}

void WavViewerView::on_pos_time_changed() {
    if (reader_.sample_rate() == 0) return;

    position_ = (static_cast<uint64_t>(field_pos_seconds_.value()) * 1000 +
                 field_pos_ms_.value()) *
                reader_.sample_rate() / 1000;

    if (!updating_position_) {
        updating_position_ = true;
        field_pos_samples_.set_value(static_cast<int32_t>(position_), false);
        updating_position_ = false;
    }
    refresh_waveform();
}

void WavViewerView::on_pos_sample_changed() {
    if (reader_.sample_rate() == 0) return;

    position_ = static_cast<uint64_t>(field_pos_samples_.value());

    if (!updating_position_) {
        updating_position_ = true;
        field_pos_seconds_.set_value(
            static_cast<int32_t>(position_ / reader_.sample_rate()), false);
        field_pos_ms_.set_value(
            static_cast<int32_t>((position_ * 1000ULL / reader_.sample_rate()) % 1000), false);
        updating_position_ = false;
    }
    refresh_waveform();
}

void WavViewerView::set_status(const std::string& s) {
    text_status_.set(s);
    set_dirty();
}

void WavViewerView::paint(Painter& painter) {
    painter.fill_rectangle(screen_rect(), Theme::getInstance()->bg_darkest->background);

    /* Waveform window frame (upstream draws these two rules). */
    painter.draw_hline({0, 70}, 240, Theme::getInstance()->bg_medium->background);
    painter.draw_hline({0, 137}, 240, Theme::getInstance()->bg_medium->background);

    /* Whole-file peak-envelope overview, coloured through the spectrum LUT. */
    for (size_t i = 0; i < kColumns; i++) {
        const uint8_t idx = wav_view_detail::peak_to_lut_index(overview_peaks_[i]);
        painter.draw_vline({static_cast<int>(i), 146}, 8, spectrum_rgb2_lut[idx]);
    }
}

void WavViewerView::on_show() {
    View::on_show();
    button_open_.focus();
    set_dirty();
}

void WavViewerView::focus() {
    button_open_.focus();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_wav_view{{
    "wav_view",
    "WAV Viewer",
    app::Category::Utilities,
    ui::Color::green(),
    &ui::bitmap_icon_speaker,
    [] { return std::make_unique<app::WavViewerView>(); },
}};
}  // namespace
