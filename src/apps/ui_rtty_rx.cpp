/*
 * mayhem-b200 — RTTY receiver view.
 *
 * See ui_rtty_rx.hpp for the pipeline description and the list of deviations
 * from firmware/baseband/proc_rtty_rx.cpp.
 *
 * SAMPLE TAP — READ THIS BEFORE TRUSTING A DECODE.
 * Upstream's decoder is fed a gapless 24 kHz stream by the M4's baseband
 * thread. The host ReceiverModel currently exposes only two taps: the audio it
 * sends to waveOut (already demodulated by the *analogue* chain, and resampled)
 * and take_spectrum_samples(), which hands back a snapshot of the most recent
 * 4096 raw samples for the waterfall. Neither is a continuous channel stream.
 *
 * This app uses the wideband snapshot and mixes/filters down to ~24 kHz itself,
 * which is correct sample-for-sample but *not* continuous: at 2.4 Msps a
 * snapshot is 1.7 ms of signal and the UI asks for one about every 16 ms, so
 * roughly 90% of the air time is never seen. A 45.45 baud character is 154 ms
 * long, so in practice characters will not complete from live RF.
 *
 * The ideal tap would be a continuous post-channel-filter complex stream from
 * ReceiverModel — i.e. a ring buffer written by the DSP thread at the channel
 * rate, alongside the existing audio path. That is a ReceiverModel change and
 * so out of scope for this app; it is called out here and on screen rather than
 * papered over. The decoder itself is exercised end to end against synthesised
 * 2FSK in tests/test_rtty_rx.cpp.
 *
 * Copyright (C) 2026 HTotoo (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_rtty_rx.hpp"

#include "../core/string_format.hpp"
#include "../radio/receiver_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"

#include <algorithm>

namespace app {

namespace {

/* One snapshot's worth. ReceiverModel keeps 4096 raw samples. */
constexpr size_t kSnapshotSamples = 4096;

/* Channel rate the decoder wants; upstream's demod runs at exactly this. */
constexpr double kTargetChannelRate = 24'000.0;

/* Control characters ITA2 can produce that would paint as garbage glyphs.
 * Newline and carriage return are handled by the Console itself. */
bool printable_for_console(char c) {
    return c == '\n' || c == '\r' || (static_cast<unsigned char>(c) >= 0x20);
}

}  // namespace

RttyRxView::RttyRxView() {
    add_children({&labels_,
                  &field_frequency_,
                  &step_view_,
                  &options_baud_,
                  &field_gain_,
                  &text_status_,
                  &notes_,
                  &console_});

    console_.enable_scrolling(true);

    auto& g = globals();

    if (auto* r = g.radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    if (auto* rx = g.receiver) {
        /* Upstream steps in 100 Hz; a 170 Hz shift needs that resolution. */
        field_frequency_.set_step_index(2);
        field_frequency_.set_value(rx->target_frequency(), false);
        field_gain_.set_value(static_cast<int32_t>(rx->gain()), false);
    }

    field_frequency_.on_change = [this](uint64_t hz) {
        if (auto* rx = globals().receiver) rx->set_target_frequency(hz);
    };

    field_gain_.on_change = [this](int32_t db) {
        if (auto* rx = globals().receiver) rx->set_gain(db);
    };

    options_baud_.on_change = [this](size_t, int32_t value) {
        baud_centi_ = static_cast<uint16_t>(value);
        demod_.configure(static_cast<float>(front_end_.output_rate()), baud_centi_);
        update_status();
    };
    /* Upstream defaults to Auto; set_by_value with trigger fires the same
     * configure path the M4 gets from set_rtty_config(0, 170). */
    options_baud_.set_by_value(0, true);
}

RttyRxView::~RttyRxView() = default;

void RttyRxView::on_show() {
    View::on_show();
    field_frequency_.focus();

    auto& g = globals();
    if (g.receiver) {
        if (!g.receiver->running()) g.receiver->start();
        rebuild_chain();
    } else {
        console_.writeln(STR_COLOR_YELLOW "No receiver: decoder idle.");
    }
}

void RttyRxView::rebuild_chain() {
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
    demod_.configure(static_cast<float>(front_end_.output_rate()), baud_centi_);
    chain_valid_ = true;
    update_status();
}

void RttyRxView::update_status() {
    const uint16_t centi = demod_.estimated_baud_centi();
    std::string s = "Baud ";
    if (centi == 0) {
        s += "--";
    } else {
        s += to_string_dec_uint(centi / 100);
        const uint16_t frac = static_cast<uint16_t>(centi % 100);
        if (frac != 0) s += "." + to_string_dec_uint(frac, 2, '0');
    }
    s += "  Shift 170  ";
    s += demod_.squelched() ? STR_COLOR_DARK_GREY "SQL" : STR_COLOR_GREEN "SIG";
    text_status_.set(s);
}

void RttyRxView::on_frame_sync() {
    View::on_frame_sync();

    auto* rx = globals().receiver;
    auto* radio = globals().radio;
    if (!rx || !radio) return;

    if (!chain_valid_ || rx->sampling_rate() != configured_input_rate_) rebuild_chain();
    if (!chain_valid_) return;

    if (!rx->take_spectrum_samples(raw_, kSnapshotSamples)) return;

    /* The snapshot sits at the LO, not at the tuned frequency: ReceiverModel
     * moves the LO only when the target would leave the middle 80% of the band
     * and makes up the rest with its own NCO. Reproduce that offset here. */
    const double offset = static_cast<double>(rx->target_frequency()) - radio->rx_frequency();
    front_end_.set_offset(offset);

    front_end_.process(raw_, channel_);
    if (channel_.empty()) return;

    decoded_.clear();
    demod_.process(channel_.data(), channel_.size(), decoded_);

    if (!decoded_.empty()) {
        std::string shown;
        shown.reserve(decoded_.size());
        for (char c : decoded_)
            if (printable_for_console(c)) shown.push_back(c);
        if (!shown.empty()) console_.write(shown);
    }

    frame_counter_++;
    if ((frame_counter_ % 15) == 0) update_status();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream's application_information_t: app_name "RTTY", icon_color yellow,
 * menu_location app_location_t::RX. No bitmap in bitmaps.hpp depicts a
 * teleprinter or a Baudot tape, so this takes the generic tile rather than
 * borrowing a misleading icon. */
const app::Registrar reg_rtty_rx{{"rtty_rx", "RTTY", app::Category::Receive,
                                  ui::Color::yellow(), nullptr,
                                  [] { return std::make_unique<app::RttyRxView>(); }}};
}  // namespace
