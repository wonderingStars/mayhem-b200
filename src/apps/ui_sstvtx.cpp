/*
 * mayhem-b200 — SSTV transmitter (view).
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_sstvtx.hpp"

#include "../audio/audio_out.hpp"
#include "../core/bmp_file.hpp"
#include "../core/file_path.hpp"
#include "../core/fs_utils.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "display.hpp"
#include "theme.hpp"

#include <algorithm>
#include <memory>

namespace app {

/* --- SstvTxImage ----------------------------------------------------------- */

SstvTxImage::SstvTxImage(ui::Rect parent_rect)
    : ui::Widget{parent_rect} {
    width_ = parent_rect.width();
    height_ = parent_rect.height();
    if (width_ < 1) width_ = 1;
    if (height_ < 1) height_ = 1;
    pixels_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_),
                   ui::Color::black());
}

void SstvTxImage::clear() {
    std::fill(pixels_.begin(), pixels_.end(), ui::Color::black());
    set_dirty();
}

void SstvTxImage::set_image(const uint8_t* rgb) {
    if (rgb == nullptr) return;
    /* Nearest-neighbour downscale of the 320x256 source into the widget. */
    for (int y = 0; y < height_; y++) {
        const int src_y = (y * static_cast<int>(sstvtx::kLinesPerImage)) / height_;
        for (int x = 0; x < width_; x++) {
            const int src_x =
                (x * static_cast<int>(sstvtx::kPixelsPerLine)) / width_;
            const size_t si =
                (static_cast<size_t>(src_y) * sstvtx::kPixelsPerLine + src_x) * 3;
            pixels_[static_cast<size_t>(y) * width_ + x] =
                ui::Color(rgb[si + 0], rgb[si + 1], rgb[si + 2]);
        }
    }
    set_dirty();
}

void SstvTxImage::paint(ui::Painter&) {
    const auto r = screen_rect();
    host::display.draw_pixels(r, pixels_.data(), pixels_.size());
}

/* --- SstvTxView ------------------------------------------------------------ */

SstvTxView::SstvTxView() {
    add_children({&labels_,
                  &options_bitmaps_,
                  &options_modes_,
                  &field_frequency_,
                  &field_gain_,
                  &button_start_,
                  &progressbar_,
                  &text_status_,
                  &preview_});

    image_rgb_.assign(sstvtx::kImageBytes, 0);

    sstv_dir_ = core::data_directory() + "/SSTV";

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.tx_gain.min),
                              static_cast<int32_t>(caps.tx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                                   static_cast<uint64_t>(caps.tx_freq.max));
    }

    if (auto* tx = globals().transmitter) {
        const uint64_t f = tx->target_frequency();
        field_frequency_.set_value(f, false);
        field_gain_.set_value(static_cast<int32_t>(tx->gain()), false);
    }
    field_frequency_.set_step_index(2);  /* 100 Hz */
    field_frequency_.on_change = [this](uint64_t hz) {
        if (auto* tx = globals().transmitter) tx->set_target_frequency(hz);
    };
    field_gain_.on_change = [this](int32_t db) {
        if (auto* tx = globals().transmitter) tx->set_gain(db);
    };

    /* Mode list, from the ported table; preselect Martin 1. */
    ui::OptionsField::options_t mode_options;
    for (size_t i = 0; i < sstvtx::kModeCount; i++)
        mode_options.emplace_back(sstvtx::kModes[i].name, static_cast<int32_t>(i));
    options_modes_.set_options(std::move(mode_options));
    options_modes_.on_change = [this](size_t index, int32_t) { set_mode(index); };

    options_bitmaps_.on_change = [this](size_t index, int32_t) {
        if (transmitting_) return;
        load_bitmap(index);
    };

    button_start_.on_select = [this](ui::Button&) {
        if (transmitting_)
            stop_tx();
        else
            start_tx();
    };

    progressbar_.set_max(1000);
    progressbar_.set_value(0);

    populate_bitmaps();

    options_modes_.set_selected_index(3, false);  /* Martin 1 */
    set_mode(3);
}

SstvTxView::~SstvTxView() {
    stop_tx();
}

void SstvTxView::populate_bitmaps() {
    bitmap_names_.clear();
    core::ensure_directory(sstv_dir_);

    std::vector<core::DirEntry> entries;
    core::ListOptions opts{".bmp"};
    opts.include_directories = false;
    core::list_directory(sstv_dir_, entries, opts);

    ui::OptionsField::options_t opt;
    for (const auto& e : entries) {
        /* Accept only exactly-320x256 24-bit BMPs, as upstream does. */
        core::BmpFile bmp;
        if (!bmp.open(core::path_join(sstv_dir_, e.name)))
            continue;
        if (bmp.get_width() == sstvtx::kPixelsPerLine &&
            bmp.get_real_height() == sstvtx::kLinesPerImage &&
            bmp.bits_per_pixel() == 24) {
            bitmap_names_.push_back(e.name);
            opt.emplace_back(e.name.substr(0, 20), static_cast<int32_t>(opt.size()));
        }
        bmp.close();
    }

    options_bitmaps_.set_options(opt);

    if (bitmap_names_.empty()) {
        image_loaded_ = false;
        preview_.clear();
        set_status("No 320x256 BMP in SSTV dir");
        button_start_.set_focusable(false);
    } else {
        button_start_.set_focusable(true);
        options_bitmaps_.set_selected_index(0, false);
        load_bitmap(0);
    }
}

bool SstvTxView::load_bitmap(size_t index) {
    if (index >= bitmap_names_.size()) {
        image_loaded_ = false;
        return false;
    }

    core::BmpFile bmp;
    if (!bmp.open(core::path_join(sstv_dir_, bitmap_names_[index]))) {
        image_loaded_ = false;
        set_status("Cannot open BMP");
        return false;
    }

    if (bmp.get_width() != sstvtx::kPixelsPerLine ||
        bmp.get_real_height() != sstvtx::kLinesPerImage) {
        image_loaded_ = false;
        set_status("BMP must be 320x256");
        return false;
    }

    /* Read every row top-to-bottom into the RGB buffer. BmpFile yields RGB565
     * ui::Color, so per-channel luma is quantised to 5/6 bits here — documented
     * in the header. */
    std::vector<ui::Color> row(sstvtx::kPixelsPerLine);
    for (uint32_t y = 0; y < sstvtx::kLinesPerImage; y++) {
        if (!bmp.seek(0, y)) break;
        if (!bmp.read_next_px_cnt(row.data(), sstvtx::kPixelsPerLine, false)) break;
        uint8_t* dst = image_rgb_.data() +
                       static_cast<size_t>(y) * sstvtx::kPixelsPerLine * 3;
        for (uint32_t x = 0; x < sstvtx::kPixelsPerLine; x++) {
            ui::Color c = row[x];
            dst[x * 3 + 0] = c.r();
            dst[x * 3 + 1] = c.g();
            dst[x * 3 + 2] = c.b();
        }
    }
    bmp.close();

    image_loaded_ = true;
    preview_.set_image(image_rgb_.data());
    encoder_.set_image(image_rgb_.data(), image_rgb_.size());
    set_status(bitmap_names_[index]);
    return true;
}

void SstvTxView::set_mode(size_t index) {
    if (index >= sstvtx::kModeCount) index = 0;
    mode_index_ = index;
    encoder_.configure(sstvtx::kModes[index], audio::sample_rate);
    if (image_loaded_)
        encoder_.set_image(image_rgb_.data(), image_rgb_.size());
    progressbar_.set_value(0);
}

void SstvTxView::start_tx() {
    if (transmitting_) return;

    if (!image_loaded_) {
        set_status("Load a 320x256 BMP first");
        return;
    }

    auto* tx = globals().transmitter;
    if (!tx) {
        set_status("No transmitter (needs B200)");
        return;
    }

    /* Rebuild the whole tone plan on this (UI) thread before the DSP thread
     * starts pulling audio, so no large allocation happens inside the source
     * callback. */
    encoder_.configure(sstvtx::kModes[mode_index_], audio::sample_rate);
    encoder_.set_image(image_rgb_.data(), image_rgb_.size());
    encoder_.begin();

    /* SSTV is an audio-domain FM signal. The transmit chain's FM modulator
     * plays proc_sstvtx's FM stage; NFM at the widest deviation (~5 kHz) is the
     * closest match to upstream's fixed ~4.5 kHz peak deviation. */
    tx->set_mode(radio::TransmitterModel::Mode::NarrowbandFM);
    tx->set_nfm_configuration(radio::TransmitterModel::NfmConfig::Wide16k);
    tx->set_audio_gain(1.0f);
    tx->set_target_frequency(field_frequency_.value());
    tx->set_gain(static_cast<double>(field_gain_.value()));
    tx->set_audio_source([this](float* out, size_t n) {
        return encoder_.fill(out, n);
    });

    if (!tx->start()) {
        tx->set_audio_source({});
        set_status("TX start failed (needs B200)");
        return;
    }

    transmitting_ = true;
    options_bitmaps_.set_focusable(false);
    options_modes_.set_focusable(false);
    button_start_.set_text("Stop");
    button_start_.set_dirty();
    set_status("Transmitting...");
}

void SstvTxView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_audio_source({});
    }
    if (!transmitting_) return;

    transmitting_ = false;
    options_bitmaps_.set_focusable(true);
    options_modes_.set_focusable(true);
    button_start_.set_text("Start");
    button_start_.set_dirty();
    progressbar_.set_value(0);
    set_status(image_loaded_ ? "Ready" : "Load a BMP");
}

void SstvTxView::set_status(const std::string& s) {
    text_status_.set(s);
}

void SstvTxView::focus() {
    if (bitmap_names_.empty())
        options_modes_.focus();
    else
        options_bitmaps_.focus();
}

void SstvTxView::on_show() {
    View::on_show();
    /* Directory contents may have changed since construction. */
    if (!transmitting_) populate_bitmaps();
}

void SstvTxView::on_hide() {
    stop_tx();
    View::on_hide();
}

void SstvTxView::on_frame_sync() {
    View::on_frame_sync();
    if (!transmitting_) return;

    progressbar_.set_value(
        static_cast<uint32_t>(encoder_.progress() * 1000.0));

    if (encoder_.done()) {
        stop_tx();
        set_status("Done");
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream application_information_t: app_name "SSTV", icon_color green,
 * menu_location app_location_t::TX. bitmaps.hpp has no picture glyph, so this
 * takes the generic tile rather than a misleading one, as the receiver does. */
const app::Registrar reg_sstvtx{{"sstvtx", "SSTV TX", app::Category::Transmit,
                                 ui::Color::green(), nullptr,
                                 [] { return std::make_unique<app::SstvTxView>(); }}};
}  // namespace
