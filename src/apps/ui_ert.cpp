/*
 * mayhem-b200 — ERT utility meter receiver (view).
 *
 * The decoders live in ui_ert.hpp so the tests can reach them without the UI;
 * this file is the ui::View subclass and the app registration.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2023 Mark Thompson (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_ert.hpp"

#include "../core/string_format.hpp"
#include "../radio/receiver_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_navigation.hpp"

#include <memory>

namespace app {

/* Where the 900 MHz ERT channels sit. Upstream's RxRadioState starts at
 * 911.6 MHz with a 2.5 MHz channel and a 4.194304 MHz baseband. */
namespace {
constexpr uint64_t kErtDefaultFrequency = 911'600'000;
constexpr double kErtSampleRate = 4'194'304.0;
}  // namespace

ErtView::ErtView() {
    add_children({&field_frequency_, &step_view_, &field_gain_, &text_status_,
                  &text_note_, &recent_view_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    /* Upstream sets a 1 MHz step for this app. */
    field_frequency_.set_step_index(10);

    if (auto* rx = globals().receiver) {
        const uint64_t f = rx->target_frequency();
        field_frequency_.set_value((f == 0) ? kErtDefaultFrequency : f, false);
        field_gain_.set_value(static_cast<int32_t>(rx->gain()), false);
    } else {
        field_frequency_.set_value(kErtDefaultFrequency, false);
    }

    field_frequency_.on_change = [this](uint64_t hz) {
        if (auto* rx = globals().receiver) rx->set_target_frequency(hz);
        /* A retune invalidates every partially-built frame. */
        decoder_.reset();
    };

    field_gain_.on_change = [](int32_t db) {
        if (auto* rx = globals().receiver) rx->set_gain(db);
    };

    /* Row renderer, laid out as upstream's RecentEntriesTable specialization. */
    recent_view_.table().on_draw = [](const ert::RecentEntry& entry,
                                      const ui::Rect& target_rect,
                                      ui::Painter& painter,
                                      const ui::Style& style,
                                      ui::RecentEntriesColumns& columns) {
        std::string lid = ert::format_id(entry.id);
        lid.resize(columns.at(0).second, ' ');

        std::string line = lid + " " + ert::format_commodity_type(entry.commodity_type) +
                           " " + ert::format_consumption(entry.last_consumption) + " ";

        line += (entry.packet_type == ert::PacketType::SCM)
                    ? ert::format_tamper_flags_scm(entry.last_tamper_flags)
                    : ert::format_tamper_flags(entry.last_tamper_flags);

        line += (entry.received_count > 99)
                    ? " ++"
                    : to_string_dec_uint(entry.received_count, 3);

        const auto chars = static_cast<size_t>(target_rect.width() / 8);
        line.resize(chars, ' ');
        painter.draw_string(target_rect.location(), style, line);
    };

    decoder_.set_packet_handler(
        [this](ert::PacketType type, const std::vector<uint8_t>& chips) {
            on_frame(type, chips);
        });

    update_status();
}

void ErtView::set_parent_rect(const ui::Rect new_parent_rect) {
    View::set_parent_rect(new_parent_rect);
    recent_view_.set_parent_rect({0, header_height, new_parent_rect.width(),
                                  new_parent_rect.height() - header_height});
}

void ErtView::on_show() {
    View::on_show();
    field_frequency_.focus();

    if (auto* rx = globals().receiver) {
        rx->set_target_frequency(field_frequency_.value());
        rx->set_sampling_rate(kErtSampleRate);
        rx->set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
        if (!rx->running()) rx->start();
        decoder_.configure(rx->sampling_rate());
    }
    update_status();
}

void ErtView::on_frame(const ert::PacketType type, const std::vector<uint8_t>& chips) {
    candidates_++;

    const ert::Packet packet{type, chips};
    if (!packet.crc_ok()) return;

    decoded_++;

    auto& entry = ui::on_packet(recent_,
                                ert::Key{packet.id(), packet.commodity_type()});
    entry.update(packet);
    recent_view_.table().set_dirty();
}

void ErtView::update_status() {
    auto* rx = globals().receiver;
    if (rx == nullptr) {
        text_status_.set("no radio");
        text_note_.set("No receiver: decode idle.");
        return;
    }

    text_status_.set(to_string_dec_uint(decoded_, 4) + "/" +
                     to_string_dec_uint(candidates_, 4));

    /* Honest about the tap: take_spectrum_samples() hands back the tail of the
     * most recent DSP block and drops the rest, so frames that straddle a gap
     * are lost. See the header for the tap this app actually wants. */
    text_note_.set(rx->running() ? "SCM/SCM+/IDM. Tap is gapped."
                                 : "Receiver stopped.");
}

void ErtView::on_frame_sync() {
    View::on_frame_sync();

    auto* rx = globals().receiver;
    if (rx == nullptr) return;

    decoder_.configure(rx->sampling_rate());

    if (rx->take_spectrum_samples(samples_, 65536)) {
        decoder_.process(samples_);
    }

    frame_counter_++;
    if ((frame_counter_ % 30) == 0) update_status();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream ships a bespoke meter-dial bitmap for this app; nothing in
 * bitmaps.hpp depicts one, so per the porting contract this takes the generic
 * tile rather than borrowing a misleading icon. */
const app::Registrar reg_ert{{"ert", "ERT Meter", app::Category::Receive,
                              ui::Color::green(), nullptr,
                              [] { return std::make_unique<app::ErtView>(); }}};
}  // namespace
