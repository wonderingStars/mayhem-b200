/*
 * mayhem-b200 — POCSAG receiver (view implementation).
 *
 * The protocol and baseband layers are header-inline in pocsag_app.hpp so the
 * tests can exercise them without dragging in the UI. This file is the view.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2023 Kyle Reed
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "pocsag_app.hpp"

#include "../core/string_format.hpp"
#include "../radio/receiver_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_navigation.hpp"

#include <cmath>
#include <memory>

namespace app {

using namespace pocsag;

/* Where UK/EU POCSAG paging traffic mostly sits; the same default the
 * firmware's POCSAGAppView uses. */
constexpr uint64_t kDefaultFrequency = 466'175'000ull;

/* Target audio rate for the decoder. Upstream's baseband is fixed at 24 kHz,
 * which leaves only 10 samples per bit at 2400 bps — measured on synthetic
 * signals, that loses every batch from a transmitter running 0.05% slow. The
 * host has no such constraint, so it asks for twice the rate; the decimation
 * must be an integer so what it actually gets is the nearest achievable. */
constexpr double kTargetAudioRate = 48'000.0;

/* Half-bandwidth of the POCSAG channel. Carson's rule for +/-4.5 kHz
 * deviation at 2400 baud gives ~13.8 kHz occupied, so +/-7 kHz. */
constexpr double kChannelCutoffHz = 7'000.0;

/* --- settings view -------------------------------------------------------- */

PocsagSettingsView::PocsagSettingsView(PocsagSettings& settings)
    : settings_{settings} {
    add_children({&labels_,
                  &opt_baud_rate_,
                  &check_small_font_,
                  &check_hide_bad_,
                  &check_hide_addr_only_,
                  &check_numeric_detect_,
                  &opt_filter_mode_,
                  &field_filter_address_,
                  &button_save_});

    opt_baud_rate_.set_by_value(settings_.baud_rate, false);
    check_small_font_.set_value(settings_.enable_small_font);
    check_hide_bad_.set_value(settings_.hide_bad_data);
    check_hide_addr_only_.set_value(settings_.hide_addr_only);
    check_numeric_detect_.set_value(settings_.enable_numeric_detect);
    opt_filter_mode_.set_by_value(settings_.filter_mode, false);
    field_filter_address_.set_value(static_cast<int32_t>(settings_.filter_address), false);

    button_save_.on_select = [this](ui::Button&) {
        settings_.enable_small_font = check_small_font_.value();
        settings_.hide_bad_data = check_hide_bad_.value();
        settings_.hide_addr_only = check_hide_addr_only_.value();
        settings_.enable_numeric_detect = check_numeric_detect_.value();
        settings_.filter_mode = static_cast<uint8_t>(opt_filter_mode_.selected_index_value());
        settings_.filter_address = static_cast<uint32_t>(field_filter_address_.value());
        settings_.baud_rate = opt_baud_rate_.selected_index_value();
        if (auto* nav = globals().nav) nav->pop();
    };
}

void PocsagSettingsView::on_show() {
    View::on_show();
    button_save_.focus();
}

/* --- main view ------------------------------------------------------------ */

PocsagAppView::PocsagAppView()
    : receiver_{globals().receiver} {
    add_children({&field_frequency_,
                  &field_gain_,
                  &field_volume_,
                  &text_status_,
                  &text_counters_,
                  &button_filter_last_,
                  &button_config_,
                  &console_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    if (receiver_) {
        /* POCSAG is narrowband FM; the app owns the tuning but the demod chain
         * it needs is its own — the decoder taps the raw band separately. The
         * NFM mode is kept only for that tap's channel setup; the speaker
         * monitor is off, because a pager decoder's product is the decoded
         * text, not the FSK warble (operator request 2026-08-14, "only sounds
         * from the ones that need it"). */
        receiver_->set_mode(radio::ReceiverModel::Mode::NarrowbandFMAudio);
        receiver_->set_audio_monitor(false);
        receiver_->set_nfm_configuration(radio::ReceiverModel::NfmConfig::Medium11k);
        if (receiver_->target_frequency() == 0) receiver_->set_target_frequency(kDefaultFrequency);

        field_frequency_.set_value(receiver_->target_frequency(), false);
        field_gain_.set_value(static_cast<int32_t>(receiver_->gain()), false);
        field_volume_.set_value(receiver_->volume(), false);
    } else {
        field_frequency_.set_value(kDefaultFrequency, false);
    }

    field_frequency_.on_change = [this](uint64_t hz) {
        if (receiver_) receiver_->set_target_frequency(hz);
    };
    field_gain_.on_change = [this](int32_t db) {
        if (receiver_) receiver_->set_gain(db);
    };
    field_volume_.on_change = [this](int32_t v) {
        if (receiver_) receiver_->set_volume(static_cast<uint8_t>(v));
    };

    button_filter_last_.on_select = [this](ui::Button&) {
        if (settings_.filter_mode == FILTER_NONE) settings_.filter_mode = FILTER_DROP;
        settings_.filter_address = last_address_;
        refresh_ui();
    };

    button_config_.on_select = [this](ui::Button&) {
        if (auto* nav = globals().nav) nav->push_new<PocsagSettingsView>(settings_);
    };

    decoder_.set_packet_handler([this](const POCSAGPacket& p) { handle_packet(p); });

    console_.writeln(STR_COLOR_LIGHT_GREY "POCSAG RX ready.");
    console_.writeln(STR_COLOR_DARK_GREY "No radio attached: decode is untested on air.");

    refresh_ui();
}

PocsagAppView::~PocsagAppView() = default;

void PocsagAppView::on_show() {
    View::on_show();
    field_frequency_.focus();
    if (receiver_ && !receiver_->running()) receiver_->start();
    reconfigure_dsp();
}

void PocsagAppView::refresh_ui() {
    const char* btn = "Filter Last";
    switch (settings_.filter_mode) {
        case FILTER_DROP:
            btn = "Ignore Last";
            break;
        case FILTER_KEEP:
            btn = "Keep Last";
            break;
        default:
            break;
    }
    button_filter_last_.set_text(btn);

    /* The baud selection is a decoder parameter, so a change means a
     * reconfigure. */
    decoder_.configure(static_cast<float>(front_end_.audio_rate() > 0.0
                                              ? front_end_.audio_rate()
                                              : kTargetAudioRate),
                       static_cast<int8_t>(settings_.baud_rate));
}

void PocsagAppView::reconfigure_dsp() {
    if (!receiver_) return;

    const double rate = receiver_->sampling_rate();
    if (rate <= 0.0) return;

    if (rate != configured_rate_) {
        front_end_.set_deviation(4'500.0);
        front_end_.configure(rate, kTargetAudioRate, kChannelCutoffHz);
        decoder_.configure(static_cast<float>(front_end_.audio_rate()),
                           static_cast<int8_t>(settings_.baud_rate));
        configured_rate_ = rate;
    }

    /* Bring the tuned channel to DC. The spectrum tap is taken before the
     * receiver's own NCO, so the offset has to be applied here. */
    if (auto* r = globals().radio) {
        const double offset = static_cast<double>(receiver_->target_frequency()) - r->rx_frequency();
        front_end_.set_offset(offset);
    }
}

bool PocsagAppView::ignore_address(uint32_t address) const {
    switch (settings_.filter_mode) {
        case FILTER_DROP:
            return address == settings_.filter_address;
        case FILTER_KEEP:
            return address != settings_.filter_address;
        default:
            return false;
    }
}

std::string PocsagAppView::format_decoded(const std::string& prefix) const {
    std::string type_str;
    const bool numeric_detect = settings_.enable_numeric_detect;
    if (numeric_detect && state_.detected == DET_NUMERIC)
        type_str = STR_COLOR_GREEN "n";
    else if (numeric_detect && state_.detected == DET_ALPHA)
        type_str = STR_COLOR_LIGHT_GREY "a";
    else if (state_.out_type == OUT_ADDRESS)
        type_str = STR_COLOR_DARK_YELLOW "t";

    std::string info = STR_COLOR_LIGHT_GREY + prefix;
    info += STR_COLOR_WHITE " #" + to_string_dec_uint(state_.address);
    info += " F" + to_string_dec_uint(state_.function);
    if (!type_str.empty()) info += " " + type_str;
    return info;
}

void PocsagAppView::handle_decoded(const std::string& prefix) {
    const bool bad_data = state_.errors >= 3;

    if (bad_data && settings_.hide_bad_data) {
        console_.writeln(STR_COLOR_MAGENTA + prefix + " Too many decode errors.");
        last_address_ = 0;
        return;
    }

    if (ignore_address(state_.address)) {
        console_.writeln(STR_COLOR_CYAN + prefix + " Ignored: " +
                         to_string_dec_uint(state_.address));
        last_address_ = state_.address;
        return;
    }

    const std::string info = format_decoded(prefix);
    const bool numeric_detect = settings_.enable_numeric_detect;

    if (state_.out_type == OUT_ADDRESS) {
        last_address_ = state_.address;
        if (!settings_.hide_addr_only) console_.writeln(info);
        return;
    }

    if (state_.out_type != OUT_MESSAGE) return;

    if (state_.new_message) {
        last_address_ = state_.address;
        console_.writeln(info);

        if (numeric_detect && state_.detected == DET_NUMERIC && state_.numeric_len > 0) {
            console_.writeln(STR_COLOR_GREEN +
                             std::string(state_.numeric_buf, state_.numeric_len));
        }
        console_.write(state_.output);
    } else {
        /* Continuation of a message that began in an earlier batch. */
        if (numeric_detect && state_.detected == DET_NUMERIC && state_.numeric_len > 0) {
            console_.write(STR_COLOR_GREEN +
                           std::string(state_.numeric_buf, state_.numeric_len));
        }
        console_.write(state_.output);
    }
}

void PocsagAppView::handle_packet(const POCSAGPacket& packet) {
    /* Round the reported bit rate to the nearest 50, as upstream does, so a
     * jittery clock estimate does not flicker the display. */
    constexpr uint32_t round_val = 50;
    const uint32_t bitrate_rounded =
        round_val * ((packet.bitrate() + (round_val / 2)) / round_val);
    const std::string prefix = to_string_dec_uint(bitrate_rounded) + "bps";

    ++packet_count_;

    if (packet.flag() != FLAG_NORMAL) {
        console_.writeln(STR_COLOR_RED + prefix + " CRC ERROR: " + flag_str(packet.flag()));
        last_address_ = 0;
        return;
    }

    state_.codeword_index = 0;
    state_.errors = 0;
    current_bitrate_ = packet.bitrate();
    current_inverted_ = packet.inverted();

    /* A batch can hold several pages; decode_batch returns true while more
     * remains. */
    while (pocsag_decode_batch(packet, state_)) handle_decoded(prefix);

    /* The remainder — unless the decoder is still holding an address that
     * arrived at the very end of the batch, in which case we cannot yet tell
     * whether it is tone-only or has a message in the next batch. */
    if (state_.mode != STATE_HAVE_ADDRESS) handle_decoded(prefix);
}

void PocsagAppView::on_frame_sync() {
    View::on_frame_sync();

    if (!receiver_) return;

    reconfigure_dsp();

    /* Pull whatever the receiver has captured and push it through the decode
     * chain.
     *
     * HONEST LIMITATION: take_spectrum_samples() is the *wideband* tap and it
     * is a sliding snapshot, not a queue — at 2.4 Msps it holds 4096 samples
     * (1.7 ms) and the DSP thread overwrites it far faster than the UI runs.
     * POCSAG needs a continuous bit stream, so on real hardware this drops
     * most of the signal and only complete batches that happen to fall inside
     * one snapshot will decode. What this decoder actually wants is a
     * gap-free channel tap: samples after the receiver's NCO and channel
     * filter, delivered through a ring buffer the app can drain. ReceiverModel
     * does not expose one yet. The decode chain below is correct and tested
     * against synthetic signals; only the sample source is inadequate. */
    if (!receiver_->take_spectrum_samples(iq_, 4096)) return;

    front_end_.process(iq_.data(), iq_.size(), audio_);
    if (!audio_.empty()) decoder_.process(audio_.data(), audio_.size());

    if ((++frame_counter_ % 10) == 0) {
        std::string s = to_string_dec_uint(packet_count_) + " pkts  ";
        s += decoder_.baud_rate() ? (to_string_dec_uint(decoder_.baud_rate()) + "bps") : "----";
        s += decoder_.has_sync() ? "  SYNC" : "  ....";
        s += "  frames " + to_string_dec_uint(decoder_.current_frames());
        text_counters_.set(s);
        text_status_.set(current_inverted_ ? "INV" : "");
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_pocsag{{"pocsag", "POCSAG RX", app::Category::Receive,
                                 ui::Color::green(), &ui::bitmap_icon_pocsag,
                                 [] { return std::make_unique<app::PocsagAppView>(); }}};
}  // namespace
