/*
 * mayhem-b200 — SubCar: car key-fob (subcarrier) receiver (views).
 *
 * The protocol decoders and the pulse extractor live in the header so they can
 * be linked — and tested — without the UI. This file is the on-screen app.
 *
 * Copyright (C) 2026 HTotoo (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_subcar.hpp"

#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_navigation.hpp"

#include <cmath>

namespace app {

/* --- Detail view ---------------------------------------------------------- */

SubCarDetailView::SubCarDetailView(const subcar::RecentEntry& entry)
    : entry_{entry} {
    add_children({&labels_, &text_type_, &text_id_, &console_, &button_done_});

    button_done_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };

    const auto decoded = subcar::parse_frame(entry_.sensor_type, entry_.data, entry_.data2,
                                             entry_.bits);

    text_type_.set(subcar::sensor_type_name(entry_.sensor_type));
    text_id_.set("0x" + to_string_hex(decoded.serial, 8));

    if (entry_.bits > 0) console_.writeln("Bits: " + to_string_dec_uint(entry_.bits));
    if (!decoded.button.empty()) console_.writeln("Btn: " + decoded.button);
    if (decoded.count != subcar::kNoCount) {
        console_.writeln("Cnt: " + to_string_dec_uint(decoded.count));
    }
    if (entry_.data != 0) console_.writeln("Data : " + to_string_hex(entry_.data, 16));
    if (entry_.data2 != 0) console_.writeln("Data2: " + to_string_hex(entry_.data2, 16));
}

void SubCarDetailView::on_show() {
    ui::View::on_show();
    button_done_.focus();
}

/* --- Main view ------------------------------------------------------------ */

SubCarView::SubCarView()
    : receiver_{*globals().receiver} {
    add_children({&field_frequency_,
                  &step_view_,
                  &field_gain_,
                  &button_clear_,
                  &labels_,
                  &options_mode_,
                  &text_status_,
                  &recent_view_});

    recent_view_.set_parent_rect({0, 64, 240, 240});

    recent_view_.table().on_draw = [](const subcar::RecentEntry& entry, const ui::Rect& rect,
                                      ui::Painter& painter, const ui::Style& style,
                                      ui::RecentEntriesColumns& columns) {
        std::string line = subcar::sensor_type_name(entry.sensor_type);
        line += " " + to_string_hex(entry.data, 8);
        line.resize(columns.at(0).second, ' ');

        const std::string bits_str = to_string_dec_uint(entry.bits);
        const std::string age_str = to_string_dec_uint(entry.age);
        line += std::string(5 > bits_str.length() ? 5 - bits_str.length() : 0, ' ') + bits_str;
        line += std::string(4 > age_str.length() ? 4 - age_str.length() : 0, ' ') + age_str;

        line.resize(static_cast<size_t>(std::max(0, rect.width() / 8)), ' ');
        painter.draw_string(rect.location(), style, line);
    };

    recent_view_.on_select = [](const subcar::RecentEntry& entry) {
        if (auto* nav = globals().nav) nav->push_new<SubCarDetailView>(entry);
    };

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    if (receiver_.target_frequency() == 0) receiver_.set_target_frequency(433'920'000);
    field_frequency_.set_value(receiver_.target_frequency(), false);
    field_frequency_.on_change = [this](uint64_t hz) { receiver_.set_target_frequency(hz); };

    /* Upstream field_frequency.set_step(10000). */
    for (size_t i = 0; i < ui::FrequencyField::step_count; ++i) {
        if (ui::FrequencyField::steps[i] == 10'000) {
            field_frequency_.set_step_index(i);
            break;
        }
    }

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    button_clear_.on_select = [this](ui::Button&) {
        recent_.clear();
        recent_view_.set_dirty();
    };

    options_mode_.set_by_value(modulation_, false);
    options_mode_.on_change = [this](size_t, int32_t v) {
        modulation_ = static_cast<uint8_t>(v);
        extractor_.configure(static_cast<float>(channel_rate_), modulation_);
    };

    protocols_.set_callback([this](subcar::CarProtocol& proto) { this->on_decoded(proto); });
    extractor_.set_handler(
        [this](bool level, uint32_t duration_us) { protocols_.feed(level, duration_us); });

    /* Upstream: 4 Msps capture, AM demodulation, 433.92 MHz by default. */
    receiver_.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
    receiver_.set_squelch_level(0);
    receiver_.set_sampling_rate(4'000'000.0);
    if (auto* r = globals().radio) r->set_rx_bandwidth(1'750'000.0);

    text_status_.set(STR_COLOR_YELLOW "Snapshot tap: frames may be lost");
}

SubCarView::~SubCarView() = default;

void SubCarView::rebuild_channel_filter() {
    const double input_rate = receiver_.sampling_rate();
    if (input_rate <= 0.0) return;
    if (std::fabs(input_rate - filter_input_rate_) < 1.0) return;

    /* Upstream decimates 4 Msps by 8 to 500 kHz with a +-220 kHz filter. */
    size_t decimation = static_cast<size_t>(std::lround(input_rate / 500'000.0));
    if (decimation < 1) decimation = 1;
    channel_rate_ = input_rate / static_cast<double>(decimation);

    auto taps = dsp::design_lowpass(220'000.0, 80'000.0, input_rate, 60.0);
    channel_filter_.configure(std::move(taps), decimation);
    filter_input_rate_ = input_rate;

    extractor_.configure(static_cast<float>(channel_rate_), modulation_);
}

void SubCarView::on_show() {
    ui::View::on_show();
    field_frequency_.focus();
    if (!receiver_.running()) receiver_.start();
    rebuild_channel_filter();
}

void SubCarView::on_hide() {
    ui::View::on_hide();
}

void SubCarView::on_decoded(subcar::CarProtocol& proto) {
    const subcar::RecentEntry key{proto.sensor_type, proto.decode_data, proto.decode_data2,
                                  proto.data_count_bit};

    auto match = ui::find(recent_, key.key());
    if (match != std::end(recent_)) {
        match->reset_age();
        recent_.push_front(*match);
        recent_.erase(match);
    } else {
        recent_.push_front(key);
        ui::truncate_entries(recent_, 64);
    }
    recent_view_.set_dirty();
}

void SubCarView::on_frame_sync() {
    ui::View::on_frame_sync();

    rebuild_channel_filter();

    /* The wideband snapshot tap, not a continuous stream — a key-fob frame is
     * a few tens of milliseconds long, so most of them fall between snapshots.
     * IDEAL TAP: ReceiverModel::take_channel_samples() draining a gap-free
     * ring at the channel rate. The pipeline below is what that would drive. */
    if (receiver_.take_spectrum_samples(samples_, 65536) && !samples_.empty()) {
        channel_.clear();
        channel_filter_.process(samples_.data(), samples_.size(), channel_);
        if (!channel_.empty()) extractor_.process(channel_.data(), channel_.size());
    }

    /* Upstream ages entries once a second off the RTC tick. */
    frame_counter_++;
    if ((frame_counter_ % 60) == 0) {
        for (auto& entry : recent_) entry.inc_age(1);
        recent_view_.set_dirty();
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream main.cpp: app_location_t::RX. bitmap_icon_remote is the stock
 * key-fob / remote-control icon. */
const app::Registrar reg_subcar{{"subcarrx", "SubCar", app::Category::Receive,
                                 ui::Color::green(), &ui::bitmap_icon_remote,
                                 [] { return std::make_unique<app::SubCarView>(); }}};
}  // namespace
