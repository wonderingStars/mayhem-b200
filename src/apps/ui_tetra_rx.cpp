/*
 * mayhem-b200 — TETRA downlink receiver (view).
 *
 * The decoder itself lives in the header so it can be linked — and tested —
 * without the UI. This file is the on-screen app.
 *
 * Copyright (C) 2026 PortaPack Mayhem contributors (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_tetra_rx.hpp"

#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"

#include <cmath>

namespace app {

using Result = tetra::ChannelDecoder::Result;

TetraRxView::TetraRxView()
    : receiver_{*globals().receiver} {
    add_children({&field_frequency_,
                  &step_view_,
                  &field_gain_,
                  &text_mcc_,
                  &text_mnc_,
                  &text_ts_,
                  &text_fn_,
                  &text_bcc_,
                  &text_enc_,
                  &text_la_,
                  &text_pdu_,
                  &text_debug_,
                  &text_tap_note_,
                  &console_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    field_frequency_.set_value(receiver_.target_frequency(), false);
    field_frequency_.on_change = [this](uint64_t hz) { receiver_.set_target_frequency(hz); };

    /* Upstream field_frequency.set_step(25000) — TETRA carriers are on a
     * 25 kHz raster. */
    for (size_t i = 0; i < ui::FrequencyField::step_count; ++i) {
        if (ui::FrequencyField::steps[i] == 25'000) {
            field_frequency_.set_step_index(i);
            break;
        }
    }

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    demod_.configure(static_cast<float>(channel_rate_));
    demod_.set_sync_handler([this](const tetra::SyncBurst& b) { this->on_sync_burst(b); });
    demod_.set_dnb_handler([this](const tetra::NormalBurst& b) { this->on_dnb_burst(b); });

    /* Upstream runs the front end at 3.072 Msps with a 1.75 MHz analog filter.
     * SpectrumAnalysis mode is used here because the app wants raw baseband,
     * not audio. */
    receiver_.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
    receiver_.set_squelch_level(0);
    receiver_.set_sampling_rate(3'072'000.0);
    if (auto* r = globals().radio) r->set_rx_bandwidth(1'750'000.0);

    text_tap_note_.set(STR_COLOR_YELLOW "Snapshot tap: bursts may be lost");
    console_.writeln("TETRA downlink decoder ready.");
    console_.writeln("Decoding is unverified without RF.");
}

TetraRxView::~TetraRxView() = default;

void TetraRxView::rebuild_channel_filter() {
    const double input_rate = receiver_.sampling_rate();
    if (input_rate <= 0.0) return;
    if (std::fabs(input_rate - filter_input_rate_) < 1.0) return;

    /* Decimate to as close to upstream's 48 kHz channel rate as an integer
     * factor allows, then tell the demodulator the rate it actually got. */
    size_t decimation = static_cast<size_t>(
        std::lround(input_rate / static_cast<double>(tetra::kTetraChannelRate)));
    if (decimation < 1) decimation = 1;

    channel_rate_ = input_rate / static_cast<double>(decimation);

    /* TETRA's pi/4-DQPSK at 18 kBd occupies 25 kHz; a 12.5 kHz-ish cutoff with
     * a 6 kHz transition keeps the adjacent carrier out. */
    auto taps = dsp::design_lowpass(12'500.0, 6'000.0, input_rate, 60.0);
    channel_filter_.configure(std::move(taps), decimation);
    filter_input_rate_ = input_rate;

    demod_.configure(static_cast<float>(channel_rate_));
}

void TetraRxView::on_show() {
    ui::View::on_show();
    field_frequency_.focus();
    if (!receiver_.running()) receiver_.start();
    rebuild_channel_filter();
}

void TetraRxView::on_hide() {
    ui::View::on_hide();
}

void TetraRxView::on_frame_sync() {
    ui::View::on_frame_sync();

    rebuild_channel_filter();

    /* See the header: this is a snapshot of the most recent wideband block,
     * not a gap-free stream, so burst sync only survives inside one snapshot.
     * The pipeline below is what a continuous channel tap would drive. */
    if (receiver_.take_spectrum_samples(samples_, 32768) && !samples_.empty()) {
        channel_.clear();
        channel_filter_.process(samples_.data(), samples_.size(), channel_);
        if (!channel_.empty()) demod_.process(channel_.data(), channel_.size());
    }

    if (counters_dirty_) {
        update_counters();
        counters_dirty_ = false;
    }
}

void TetraRxView::update_counters() {
    text_debug_.set("Syn: " + to_string_dec_uint(sync_count_) +
                    ", V: " + to_string_dec_uint(valid_count_) +
                    ", H: " + to_string_dec_uint(h_valid_count_) +
                    ", E:" + to_string_dec_uint(last_sync_errors_));
}

void TetraRxView::on_sync_burst(const tetra::SyncBurst& burst) {
    sync_count_++;
    const auto result = decoder_.decode_burst(burst);
    last_sync_errors_ = result.sync_errors;

    if (result.type == Result::Type::Sync || result.type == Result::Type::SyncFull) {
        valid_count_++;

        text_mcc_.set("MCC: " + to_string_dec_uint(result.mcc));
        text_mnc_.set("MNC: " + to_string_dec_uint(result.mnc));
        text_bcc_.set("BCC: " + to_string_dec_uint(result.bcc));
        text_ts_.set("TS:  " + to_string_dec_uint(result.timeslot));
        text_fn_.set("FN:  " + to_string_dec_uint(result.frame_number));
        text_enc_.set(std::string{"ENC: "} +
                      ((result.encryption != 0) ? "Encrypted" : "Clear"));

        if (result.type == Result::Type::SyncFull) {
            h_valid_count_++;
            if (result.la != 0xFFFF) text_la_.set("LA:  " + to_string_dec_uint(result.la));
            text_pdu_.set("PDU: " + tetra::ChannelDecoder::pdu_name(result.pdu_type));
        }
    }

    counters_dirty_ = true;
}

void TetraRxView::on_dnb_burst(const tetra::NormalBurst& burst) {
    dnb_count_++;
    const auto result = decoder_.decode_dnb(burst);
    if (!result.is_ok || result.type != Result::Type::Dnb) return;

    if (result.la != 0xFFFF) text_la_.set("LA:  " + to_string_dec_uint(result.la));
    text_pdu_.set("PDU: " + tetra::ChannelDecoder::pdu_name(result.pdu_type));
    text_enc_.set(std::string{"ENC: "} + ((result.encryption != 0) ? "Encrypted" : "Clear"));

    if (result.cmce_type != 0xFF) {
        console_.writeln(tetra::ChannelDecoder::cmce_name(result.cmce_type));
        if (result.call_id != 0xFFFF) {
            console_.writeln("ID:" + to_string_dec_uint(result.call_id) +
                             " | SSI:" + to_string_dec_uint(result.calling_ssi));
        }
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream main.cpp: app_location_t::RX. No stock bitmap depicts a trunked
 * radio network, so a generic tile (doc/PORTING.md). */
const app::Registrar reg_tetra_rx{{"tetra_rx", "Tetra RX", app::Category::Receive,
                                   ui::Color::green(), nullptr,
                                   [] { return std::make_unique<app::TetraRxView>(); }}};
}  // namespace
