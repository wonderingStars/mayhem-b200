/*
 * mayhem-b200 — SSTV receiver view.
 *
 * See ui_sstvrx.hpp for the pipeline and the list of deviations from
 * firmware/baseband/proc_sstvrx.cpp.
 *
 * SAMPLE TAP — READ THIS BEFORE TRUSTING A DECODE.
 * Upstream's decoder is handed a gapless 48 kHz stream by the M4's baseband
 * thread. The host ReceiverModel exposes no continuous channel tap: the only
 * raw-sample accessor is take_spectrum_samples(), which returns a snapshot of
 * the most recent 4096 samples for the waterfall. This app mixes and filters
 * that snapshot down to ~48 kHz itself, which is sample-accurate but not
 * continuous — at 2.4 Msps a snapshot is 1.7 ms and the UI asks for one about
 * every 16 ms, so most of the air time is never seen. An SSTV frame is 1-2
 * minutes of unbroken tone timing, so live pictures will not build from RF
 * through this tap.
 *
 * The tap that would work is a continuous post-channel-filter complex stream
 * from ReceiverModel (a ring buffer written by its DSP thread at the channel
 * rate). That is a ReceiverModel change, out of scope here; it is stated on
 * screen rather than hidden. The decoder itself is exercised end to end against
 * synthesised SSTV audio in tests/test_sstvrx.cpp.
 *
 * Copyright (C) 2025 StarVore Labs (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_sstvrx.hpp"

#include "../core/file_path.hpp"
#include "../core/string_format.hpp"
#include "../radio/receiver_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "display.hpp"
#include "ui_painter.hpp"

#include <algorithm>

namespace app {

namespace {

constexpr size_t kSnapshotSamples = 4096;

/* Upstream runs its SSTV chain at 48 kHz ("Keep 48kHz audio for better pixel
 * resolution" in proc_sstvrx.cpp) rather than the 24 kHz its other RX
 * processors use. */
constexpr double kTargetChannelRate = 48'000.0;

/* Default: 14.230 MHz, the 20 m SSTV calling frequency. Upstream defaults to
 * 145.800 MHz (the ISS downlink); both are conventions, and the HF one matches
 * this app's placement among the HF modes. */
constexpr uint64_t kDefaultFrequency = 14'230'000;

}  // namespace

/* --- SstvImage ------------------------------------------------------------ */

SstvImage::SstvImage(ui::Rect parent_rect)
    : ui::Widget{parent_rect} {
    width_ = parent_rect.width();
    height_ = parent_rect.height();
    if (width_ < 1) width_ = 1;
    if (height_ < 1) height_ = 1;
    pixels_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_),
                   ui::Color::black());
}

void SstvImage::clear() {
    std::fill(pixels_.begin(), pixels_.end(), ui::Color::black());
    set_dirty();
}

void SstvImage::set_line(uint16_t line, const uint8_t* rgb) {
    if (rgb == nullptr || height_ <= 0) return;

    /* Upstream wraps at the bottom of its drawing area rather than scrolling. */
    const int row = static_cast<int>(line % static_cast<uint16_t>(height_));
    ui::Color* dst = &pixels_[static_cast<size_t>(row) * static_cast<size_t>(width_)];

    for (int x = 0; x < width_; x++) {
        const int src = (x * static_cast<int>(sstv::kPixelsPerLine)) / width_;
        const uint8_t* px = rgb + static_cast<size_t>(src) * 3;
        dst[x] = ui::Color(px[0], px[1], px[2]);
    }
    set_dirty();
}

void SstvImage::paint(ui::Painter& painter) {
    (void)painter;
    const auto r = screen_rect();
    host::display.draw_pixels(r, pixels_.data(), pixels_.size());
}

/* --- SstvRxView ----------------------------------------------------------- */

SstvRxView::SstvRxView() {
    add_children({&labels_,
                  &field_frequency_,
                  &step_view_,
                  &options_mode_,
                  &field_gain_,
                  &field_phase_,
                  &field_slant_,
                  &check_auto_vis_,
                  &text_status_,
                  &button_start_,
                  &text_path_,
                  &image_});

    auto& g = globals();

    if (auto* r = g.radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    if (auto* rx = g.receiver) {
        const uint64_t f = rx->target_frequency();
        field_frequency_.set_value(f != 0 ? f : kDefaultFrequency, false);
        field_gain_.set_value(static_cast<int32_t>(rx->gain()), false);
    } else {
        field_frequency_.set_value(kDefaultFrequency, false);
    }
    field_frequency_.set_step_index(2); /* 100 Hz; SSTV tuning is fussy */

    field_frequency_.on_change = [this](uint64_t hz) {
        if (auto* rx = globals().receiver) rx->set_target_frequency(hz);
    };
    field_gain_.on_change = [this](int32_t db) {
        if (auto* rx = globals().receiver) rx->set_gain(db);
    };

    /* Upstream populates this from sstv_modes[] and preselects Scottie 2. */
    ui::OptionsField::options_t mode_options;
    for (size_t i = 0; i < sstv::kModeCount; i++)
        mode_options.emplace_back(sstv::kModes[i].name, static_cast<int32_t>(i));
    options_mode_.set_options(std::move(mode_options));
    options_mode_.on_change = [this](size_t index, int32_t) {
        if (const auto* m = sstv::mode_by_index(index)) {
            decoder_.set_mode(m);
            update_status();
        }
    };
    options_mode_.set_selected_index(1, false);
    decoder_.set_mode(sstv::mode_by_index(1));

    check_auto_vis_.set_value(true);
    check_auto_vis_.on_select = [this](ui::Checkbox&, bool value) {
        decoder_.set_auto_vis(value);
    };

    field_phase_.set_value(0, false);
    field_phase_.on_change = [this](int32_t v) {
        decoder_.line_decoder().set_phase_offset(static_cast<int16_t>(v));
    };
    field_slant_.set_value(0, false);
    field_slant_.on_change = [this](int32_t v) {
        decoder_.line_decoder().set_slant(static_cast<int16_t>(v));
    };

    button_start_.on_select = [this](ui::Button&) { on_start_stop(); };

    decoder_.on_line = [this](uint16_t line, const uint8_t* rgb) { handle_line(line, rgb); };
    decoder_.on_mode_detected = [this](const sstv::Mode& m, uint8_t vis) {
        detected_ = std::string{m.name} + " (VIS " + to_string_dec_uint(vis) + ")";
        for (size_t i = 0; i < sstv::kModeCount; i++) {
            if (&sstv::kModes[i] == &m) {
                options_mode_.set_selected_index(i, false);
                break;
            }
        }
        update_status();
    };

    update_status();
}

SstvRxView::~SstvRxView() {
    bmp_.close();
}

void SstvRxView::on_show() {
    View::on_show();
    field_frequency_.focus();
    if (auto* rx = globals().receiver) {
        if (!rx->running()) rx->start();
        rebuild_chain();
    }
}

void SstvRxView::on_hide() {
    stop_receiving();
    View::on_hide();
}

void SstvRxView::rebuild_chain() {
    auto* rx = globals().receiver;
    if (!rx) {
        chain_valid_ = false;
        return;
    }

    const double rate = rx->sampling_rate();
    if (rate <= 0.0) {
        chain_valid_ = false;
        return;
    }

    front_end_.configure(rate, kTargetChannelRate);
    configured_input_rate_ = rate;

    const auto* mode = decoder_.mode();
    decoder_.configure(static_cast<float>(front_end_.output_rate()));
    if (mode) decoder_.set_mode(mode);
    decoder_.set_auto_vis(check_auto_vis_.value());
    decoder_.line_decoder().set_phase_offset(static_cast<int16_t>(field_phase_.value()));
    decoder_.line_decoder().set_slant(static_cast<int16_t>(field_slant_.value()));

    chain_valid_ = true;
}

void SstvRxView::on_start_stop() {
    if (receiving_)
        stop_receiving();
    else
        start_receiving();
}

void SstvRxView::start_receiving() {
    rebuild_chain();
    decoder_.reset();
    image_.clear();
    last_line_ = 0;
    file_line_ = 0;
    detected_.clear();

    /* Upstream writes to /SSTV/RX on the SD card; the host equivalent lives
     * under the app's data directory. */
    const std::string dir = core::data_directory() + "/SSTV/RX";
    bmp_.close();
    if (core::ensure_directory(dir)) {
        image_path_ = dir + "/SSTV_" + to_string_timestamp_now() + ".bmp";
        if (!bmp_.create(image_path_, sstv::kPixelsPerLine, 1)) {
            bmp_.close();
            image_path_.clear();
        }
    } else {
        image_path_.clear();
    }

    text_path_.set(image_path_.empty() ? "no file" : "-> SSTV/RX");
    button_start_.set_text("Stop RX");
    receiving_ = true;
    update_status();
}

void SstvRxView::stop_receiving() {
    if (!receiving_) return;
    receiving_ = false;
    bmp_.close();
    button_start_.set_text("Start RX");
    update_status();
}

void SstvRxView::handle_line(uint16_t line, const uint8_t* rgb) {
    last_line_ = line;
    image_.set_line(line, rgb);

    if (!bmp_.is_loaded()) return;

    /* Upstream appends every completed line to the file in arrival order and
     * grows the image as it goes, so a partial reception is still a valid BMP. */
    if (bmp_.get_real_height() <= file_line_) {
        if (!bmp_.expand_y(file_line_ + 1)) return;
    }
    if (!bmp_.seek(0, file_line_)) return;

    for (uint16_t x = 0; x < sstv::kPixelsPerLine; x++) {
        const uint8_t* px = rgb + static_cast<size_t>(x) * 3;
        if (!bmp_.write_next_px(ui::Color(px[0], px[1], px[2]))) return;
    }
    file_line_++;
}

void SstvRxView::update_status() {
    std::string s;
    if (!receiving_) {
        s = "Stopped  ";
    } else {
        s = "Line " + to_string_dec_uint(last_line_) + "/" +
            to_string_dec_uint(decoder_.line_decoder().lines()) + "  ";
    }
    if (!detected_.empty())
        s += detected_;
    else if (const auto* m = decoder_.mode())
        s += m->name;
    text_status_.set(s);
}

void SstvRxView::on_frame_sync() {
    View::on_frame_sync();

    auto* rx = globals().receiver;
    auto* radio = globals().radio;
    if (!rx || !radio) return;

    if (!chain_valid_ || rx->sampling_rate() != configured_input_rate_) rebuild_chain();
    if (!chain_valid_ || !receiving_) return;

    if (!rx->take_spectrum_samples(raw_, kSnapshotSamples)) return;

    const double offset = static_cast<double>(rx->target_frequency()) - radio->rx_frequency();
    front_end_.set_offset(offset);

    front_end_.process(raw_, channel_);
    if (channel_.empty()) return;

    decoder_.process(channel_.data(), channel_.size());

    frame_counter_++;
    if ((frame_counter_ % 15) == 0) update_status();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream's application_information_t: app_name "SSTV RX", icon_color yellow,
 * menu_location app_location_t::RX. bitmaps.hpp has no picture/camera glyph, so
 * this takes the generic tile rather than borrowing a misleading one. */
const app::Registrar reg_sstvrx{{"sstvrx", "SSTV RX", app::Category::Receive,
                                 ui::Color::yellow(), nullptr,
                                 [] { return std::make_unique<app::SstvRxView>(); }}};
}  // namespace
