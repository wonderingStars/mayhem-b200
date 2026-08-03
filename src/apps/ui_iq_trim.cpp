/*
 * mayhem-b200 — IQ Trim.
 *
 * Copyright (C) 2023 Kyle Reed
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_iq_trim.hpp"

#include "../core/file_path.hpp"
#include "../core/fs_utils.hpp"
#include "../core/string_format.hpp"
#include "app_context.hpp"
#include "theme.hpp"
#include "ui_modal.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

/* ========================================================================== */
/*  Pure trim logic                                                            */
/* ========================================================================== */
namespace iqtrim {

void PowerBuckets::add(size_t index, uint32_t power) {
    if (index >= size) return;
    auto& b = p[index];
    /* Host guard: upstream's count is a uint8_t; once it saturates at 255 the
     * post-increment wraps to 0 and the next divide is UB on x86. A well-formed
     * capture profiles ~samples_per_bucket (10) samples per bucket, so this only
     * bites a degenerate tiny file, where freezing the average is the safe act. */
    if (b.count == 255) return;

    const uint64_t weighted = static_cast<uint64_t>(b.power) * b.count;
    b.count++;
    b.power = static_cast<uint32_t>((power + weighted) / b.count);
}

namespace {

/* Profiles a capture whose samples are interleaved pairs of the integer type S
 * (int16_t for C16, int8_t for C8). Power is computed on the raw stored
 * integers, exactly as upstream's power()/iq_max() do. */
template <typename S>
std::optional<CaptureInfo> profile_typed(
    const std::string& path,
    PowerBuckets& buckets,
    uint8_t samples_per_bucket) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;

    f.seekg(0, std::ios::end);
    const std::streamoff end_pos = f.tellg();
    if (end_pos < 0) return std::nullopt;

    const uint8_t sample_size = static_cast<uint8_t>(2 * sizeof(S));
    CaptureInfo info{};
    info.file_size = static_cast<uint64_t>(end_pos);
    info.sample_count = info.file_size / sample_size;
    info.sample_size = sample_size;

    if (buckets.size == 0 || info.sample_count == 0)
        return info;  // Nothing to profile, but the metadata is still valid.

    const uint64_t profile_samples =
        static_cast<uint64_t>(buckets.size) * samples_per_bucket;
    uint64_t sample_interval =
        profile_samples ? info.sample_count / profile_samples : 0;
    /* Host guard: a file smaller than profile_samples gives interval 0, which
     * upstream would loop on forever (sample_index never advances). Stepping by
     * one instead profiles every sample of a small file — strictly better. */
    if (sample_interval == 0) sample_interval = 1;

    const uint64_t bucket_width =
        std::max<uint64_t>(1, info.sample_count / buckets.size);

    uint64_t sample_index = 0;
    S iq[2];

    while (true) {
        f.clear();
        f.seekg(static_cast<std::streamoff>(sample_index * sample_size));
        f.read(reinterpret_cast<char*>(iq), sample_size);
        if (f.gcount() != static_cast<std::streamsize>(sample_size))
            break;  // EOF or short tail.

        const int32_t re = iq[0];
        const int32_t im = iq[1];

        const uint32_t sample_iq =
            static_cast<uint32_t>(std::max(std::abs(re), std::abs(im)));
        if (sample_iq > info.max_iq) info.max_iq = sample_iq;

        const uint32_t mag_squared = static_cast<uint32_t>(re * re + im * im);
        if (mag_squared > info.max_power) info.max_power = mag_squared;

        buckets.add(static_cast<size_t>(sample_index / bucket_width), mag_squared);
        sample_index += sample_interval;
    }

    return info;
}

}  // namespace

std::optional<CaptureInfo> profile_capture(
    const std::string& path,
    PowerBuckets& buckets,
    uint8_t samples_per_bucket) {
    switch (core::iq_format_from_path(path)) {
        case core::IqFormat::C16:
            return profile_typed<int16_t>(path, buckets, samples_per_bucket);
        case core::IqFormat::C8:
            return profile_typed<int8_t>(path, buckets, samples_per_bucket);
        default:
            return std::nullopt;
    }
}

TrimRange compute_trim_range(
    const CaptureInfo& info,
    const PowerBuckets& buckets,
    uint8_t cutoff_percent) {
    bool has_start = false;
    size_t start_bucket = 0;
    size_t end_bucket = 0;

    const uint32_t power_cutoff = static_cast<uint32_t>(
        cutoff_percent * static_cast<uint64_t>(info.max_power) / 100);

    for (size_t i = 0; i < buckets.size; ++i) {
        const uint32_t power = buckets.p[i].power;
        if (power > power_cutoff) {
            if (has_start) {
                end_bucket = i;
            } else {
                start_bucket = i;
                end_bucket = i;
                has_start = true;
            }
        }
    }

    const uint64_t samples_per_bucket =
        buckets.size ? info.sample_count / buckets.size : 0;

    if (!has_start) {
        /* Deviation from upstream, which returns {0, samples_per_bucket}: report
         * an empty range so the caller can say "no active region" and refuse to
         * trim, rather than silently keeping the first bucket. */
        return {0, 0, info.sample_size, false};
    }

    /* end is the first bucket *after* the last with signal. */
    ++end_bucket;
    return {start_bucket * samples_per_bucket,
            end_bucket * samples_per_bucket,
            info.sample_size,
            true};
}

void amplify_iq_buffer(
    uint8_t* buffer,
    uint32_t length,
    uint32_t amplification,
    uint8_t sample_size) {
    /* Deviation from upstream: application/iq_trim.cpp computes
     *   mult_count = length / sample_size / 2
     * which amplifies only a quarter of each block and corrupts the rest. We
     * amplify every integer component in the buffer. Clamp rails (±0x7FFF /
     * ±0x7F) match upstream. */
    switch (sample_size) {
        case sizeof(int16_t) * 2: {  // C16
            int16_t* ptr = reinterpret_cast<int16_t*>(buffer);
            const uint32_t count = length / sizeof(int16_t);
            for (uint32_t i = 0; i < count; ++i) {
                int32_t val = static_cast<int32_t>(ptr[i]) *
                              static_cast<int32_t>(amplification);
                if (val > 0x7FFF)
                    val = 0x7FFF;
                else if (val < -0x7FFF)
                    val = -0x7FFF;
                ptr[i] = static_cast<int16_t>(val);
            }
            break;
        }
        case sizeof(int8_t) * 2: {  // C8
            int8_t* ptr = reinterpret_cast<int8_t*>(buffer);
            const uint32_t count = length / sizeof(int8_t);
            for (uint32_t i = 0; i < count; ++i) {
                int32_t val = static_cast<int32_t>(ptr[i]) *
                              static_cast<int32_t>(amplification);
                if (val > 0x7F)
                    val = 0x7F;
                else if (val < -0x7F)
                    val = -0x7F;
                ptr[i] = static_cast<int8_t>(val);
            }
            break;
        }
        default:
            break;
    }
}

bool trim_capture_with_range(
    const std::string& path,
    const TrimRange& range,
    const std::function<void(uint8_t)>& on_progress,
    uint32_t amplification) {
    if (range.sample_size == 0 || range.start_sample >= range.end_sample)
        return false;

    const uint8_t sample_size = range.sample_size;
    const uint64_t start_byte = range.start_sample * sample_size;
    const uint64_t end_byte = range.end_sample * sample_size;
    const uint64_t length = end_byte - start_byte;

    const std::string temp_path = path + "-tmp";

    std::ifstream src(path, std::ios::binary);
    if (!src) return false;

    std::ofstream dst(temp_path, std::ios::binary | std::ios::trunc);
    if (!dst) return false;

    src.seekg(static_cast<std::streamoff>(start_byte));
    if (!src) return false;

    constexpr size_t buffer_size = 16384;
    std::vector<uint8_t> buffer(buffer_size);

    uint64_t processed = 0;
    const uint64_t report_interval = (length / 20) ? (length / 20) : 1;
    uint64_t next_report = report_interval;

    while (processed < length) {
        const uint64_t remaining = length - processed;
        const size_t to_read =
            static_cast<size_t>(std::min<uint64_t>(buffer_size, remaining));

        src.read(reinterpret_cast<char*>(buffer.data()),
                 static_cast<std::streamsize>(to_read));
        const std::streamsize got = src.gcount();
        if (got <= 0) break;  // Unexpected EOF; keep what we have.

        if (amplification > 1)
            amplify_iq_buffer(buffer.data(), static_cast<uint32_t>(got),
                              amplification, sample_size);

        dst.write(reinterpret_cast<const char*>(buffer.data()), got);
        if (!dst) return false;

        processed += static_cast<uint64_t>(got);

        if (on_progress && processed >= next_report) {
            on_progress(static_cast<uint8_t>(100 * processed / length));
            next_report += report_interval;
        }
    }

    src.close();
    dst.close();
    if (!dst) return false;

    /* Replace the original with the trimmed copy, as upstream does. */
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) return false;
    std::filesystem::rename(temp_path, path, ec);
    if (ec) return false;

    return true;
}

}  // namespace iqtrim

/* ========================================================================== */
/*  The app view                                                               */
/*                                                                             */
/*  Guarded so a focused unit-test build (-DMB200_IQ_TRIM_LOGIC_ONLY) can link */
/*  the real iqtrim:: logic above without pulling in the whole UI/registry. In */
/*  the normal build the macro is undefined and the full file compiles.        */
/* ========================================================================== */
#ifndef MB200_IQ_TRIM_LOGIC_ONLY
namespace app {

IQTrimView::IQTrimView() {
    power_buckets_.resize(ui::screen_width);

    add_children({
        &labels_,
        &field_file_,
        &field_start_,
        &field_end_,
        &text_samples_,
        &text_max_,
        &field_cutoff_,
        &field_amplify_,
        &text_status_,
        &button_trim_,
    });

    text_samples_.set_style(ui::Theme::getInstance()->fg_light);
    text_max_.set_style(ui::Theme::getInstance()->fg_light);
    text_status_.set_style(ui::Theme::getInstance()->fg_light);

    field_file_.on_change = [this](size_t index, int32_t) {
        if (index < files_.size()) open_file(files_[index]);
    };

    field_start_.on_change = [this](int32_t v) {
        if (field_end_.value() < v) field_end_.set_value(v, false);
        set_dirty();
    };
    field_end_.on_change = [this](int32_t v) {
        if (field_start_.value() > v) field_start_.set_value(v, false);
        set_dirty();
    };

    field_cutoff_.set_value(7);  // 7% of max is a good default (upstream).
    field_cutoff_.on_change = [this](int32_t) {
        compute_range();
        refresh_ui();
    };

    field_amplify_.set_value(1);  // 1x = no amplification.
    field_amplify_.on_change = [this](int32_t) { refresh_ui(); };

    button_trim_.on_select = [this](ui::Button&) {
        if (do_trim()) open_file(path_);  // Re-profile the trimmed file.
    };
}

IQTrimView::IQTrimView(std::string path)
    : IQTrimView() {
    path_ = std::move(path);
}

void IQTrimView::on_show() {
    View::on_show();
    rescan_files();
    field_file_.focus();
}

void IQTrimView::focus() {
    field_file_.focus();
}

void IQTrimView::rescan_files() {
    files_.clear();

    const std::string dir = core::captures_directory();
    std::vector<core::DirEntry> entries;
    core::ListOptions opts;
    opts.extensions = {".C16", ".C8"};
    opts.include_directories = false;
    core::list_directory(dir, entries, opts);

    ui::OptionsField::options_t options;
    for (const auto& e : entries) {
        files_.push_back(core::path_join(dir, e.name));
        options.emplace_back(truncate(e.name, 28),
                             static_cast<int32_t>(options.size()));
    }

    if (files_.empty()) {
        field_file_.set_options({{"(no captures)", 0}});
        info_.reset();
        path_.clear();
        set_status("No .C16/.C8 in CAPTURES", true);
        set_dirty();
        return;
    }

    field_file_.set_options(std::move(options));

    /* If constructed with an explicit path that is in the list, select it;
     * otherwise load whatever is highlighted (first item on a fresh scan). */
    size_t index = field_file_.selected_index();
    if (!path_.empty()) {
        for (size_t i = 0; i < files_.size(); ++i) {
            if (files_[i] == path_) {
                index = i;
                break;
            }
        }
        field_file_.set_selected_index(index, false);
    }
    if (index >= files_.size()) index = 0;

    open_file(files_[index]);
}

void IQTrimView::open_file(const std::string& path) {
    path_ = path;
    profile_current();
    if (info_) {
        compute_range();
        refresh_ui();
    }
    set_dirty();
}

void IQTrimView::profile_current() {
    power_buckets_.assign(ui::screen_width, {});
    iqtrim::PowerBuckets buckets{power_buckets_.data(), power_buckets_.size()};

    set_status("Reading capture...");
    info_ = iqtrim::profile_capture(path_, buckets);

    if (!info_)
        set_status("Cannot read capture", true);
    else
        set_status("");
}

void IQTrimView::compute_range() {
    if (!info_) return;

    iqtrim::PowerBuckets buckets{power_buckets_.data(), power_buckets_.size()};
    const auto range = iqtrim::compute_trim_range(
        *info_, buckets, static_cast<uint8_t>(field_cutoff_.value()));

    update_range_controls(range);
    if (!range.has_signal) set_status("No active region found");
}

void IQTrimView::update_range_controls(const iqtrim::TrimRange& range) {
    const int32_t max_range =
        info_ ? static_cast<int32_t>(
                    std::min<uint64_t>(info_->sample_count, INT32_MAX))
              : 0;
    int32_t step =
        (info_ && ui::screen_width)
            ? static_cast<int32_t>(info_->sample_count / ui::screen_width)
            : 0;
    if (step < 1) step = 1;

    field_start_.set_range(0, max_range);
    field_start_.set_step(step);
    field_end_.set_range(0, max_range);
    field_end_.set_step(step);

    field_start_.set_value(static_cast<int32_t>(range.start_sample), false);
    field_end_.set_value(static_cast<int32_t>(range.end_sample), false);
}

void IQTrimView::refresh_ui() {
    if (!info_) return;

    text_samples_.set(to_string_dec_uint(info_->sample_count));

    /* Peak power after amplification. Power is I*I + Q*Q, so amplifying each
     * component by A scales it by A*A (upstream displays A^4, which overstates
     * it; A*A is the physical value). */
    const uint64_t amp = static_cast<uint64_t>(field_amplify_.value());
    const uint64_t power_scale = amp * amp;
    text_max_.set(to_string_dec_uint(static_cast<uint64_t>(info_->max_power) * power_scale));

    /* Red when the chosen amplification would clip the peak sample. */
    const uint32_t clipping_limit = (info_->sample_size == sizeof(int8_t) * 2) ? 0x80 : 0x8000;
    if (static_cast<uint64_t>(field_amplify_.value()) * info_->max_iq > clipping_limit)
        text_max_.set_style(ui::Theme::getInstance()->fg_red);
    else
        text_max_.set_style(ui::Theme::getInstance()->fg_light);

    set_dirty();
}

bool IQTrimView::do_trim() {
    if (!info_) {
        ui::display_modal("Error", "Open a file first.");
        return false;
    }

    iqtrim::TrimRange range{
        static_cast<uint64_t>(field_start_.value()),
        static_cast<uint64_t>(field_end_.value()),
        info_->sample_size,
        true};

    if (range.start_sample >= range.end_sample) {
        ui::display_modal("Error", "Invalid trimming range.");
        return false;
    }

    set_status("Trimming capture...");
    const bool ok = iqtrim::trim_capture_with_range(
        path_, range, nullptr, static_cast<uint32_t>(field_amplify_.value()));

    if (!ok) {
        set_status("Trimming failed", true);
        ui::display_modal("Error", "Trimming failed.");
        return false;
    }

    set_status("Trim complete");
    return true;
}

void IQTrimView::set_status(const std::string& message, bool error) {
    text_status_.set_style(error ? ui::Theme::getInstance()->fg_red
                                 : ui::Theme::getInstance()->fg_light);
    text_status_.set(message);
}

void IQTrimView::paint(ui::Painter& painter) {
    View::paint(painter);  // Clears the background.

    if (!info_ || info_->sample_count == 0) return;

    const ui::Point origin = screen_rect().location();
    const ui::Point strip{origin.x(), origin.y() + 64};
    const int strip_height = 32;

    const uint32_t power_cutoff = static_cast<uint32_t>(
        field_cutoff_.value() * static_cast<uint64_t>(info_->max_power) / 100);

    for (size_t i = 0; i < power_buckets_.size(); ++i) {
        const uint32_t power = power_buckets_[i].power;
        uint8_t amp = 0;
        if (power > power_cutoff && info_->max_power > 0)
            amp = static_cast<uint8_t>((255ULL * power) / info_->max_power);

        painter.draw_vline(strip + ui::Point{static_cast<int>(i), 0},
                           strip_height, ui::Color(amp, amp, amp));
    }

    const int start_x = static_cast<int>(
        static_cast<uint64_t>(ui::screen_width) * field_start_.value() / info_->sample_count);
    const int end_x = static_cast<int>(
        static_cast<uint64_t>(ui::screen_width) * field_end_.value() / info_->sample_count);

    painter.draw_vline(strip + ui::Point{start_x, 0}, strip_height, ui::Color::dark_green());
    painter.draw_vline(strip + ui::Point{end_x, 0}, strip_height, ui::Color::dark_red());
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_iqtrim{{"iqtrim", "IQ Trim", app::Category::Utilities,
                                 ui::Color::orange(), &ui::bitmap_icon_trim,
                                 [] { return std::make_unique<app::IQTrimView>(); }}};
}  // namespace

#endif  // MB200_IQ_TRIM_LOGIC_ONLY
