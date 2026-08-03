/*
 * mayhem-b200 — FLEX pager receiver (view implementation).
 *
 * The decoder is header-inline in ui_flex_rx.hpp so the tests can exercise it
 * without the UI. This file holds the console formatting and the view.
 *
 * Copyright (C) 2012-2014 Elias Oenal (multimon-ng demod_flex.c)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_flex_rx.hpp"

#include "../core/string_format.hpp"
#include "../radio/receiver_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_navigation.hpp"

#include <memory>

namespace app {

/* A US FLEX paging channel; upstream's default for this app. */
constexpr uint64_t kDefaultFrequency = 931'740'000ull;

/* FLEX runs up to 3200 symbols/s, so the decoder wants materially more than
 * POCSAG's 24 kHz. Upstream is stuck at 24 kHz by its fixed decimation chain;
 * the host is not. */
constexpr double kTargetAudioRate = 48'000.0;

/* Carson's rule for +/-4.8 kHz deviation at 3200 symbols/s gives ~16 kHz
 * occupied, so half of that either side. */
constexpr double kChannelCutoffHz = 8'000.0;

FlexRxView::FlexRxView()
    : receiver_{globals().receiver} {
    add_children({&field_frequency_, &field_gain_, &text_status1_, &text_status2_, &console_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    if (receiver_) {
        receiver_->set_mode(radio::ReceiverModel::Mode::NarrowbandFMAudio);
        receiver_->set_nfm_configuration(radio::ReceiverModel::NfmConfig::Wide16k);
        if (receiver_->target_frequency() == 0) receiver_->set_target_frequency(kDefaultFrequency);
        field_frequency_.set_value(receiver_->target_frequency(), false);
        field_gain_.set_value(static_cast<int32_t>(receiver_->gain()), false);
    } else {
        field_frequency_.set_value(kDefaultFrequency, false);
    }

    field_frequency_.on_change = [this](uint64_t hz) {
        if (receiver_) receiver_->set_target_frequency(hz);
    };
    field_gain_.on_change = [this](int32_t db) {
        if (receiver_) receiver_->set_gain(db);
    };

    decoder_.set_packet_handler([this](const flex::FlexPacket& p) { handle_packet(p); });

    console_.writeln(STR_COLOR_LIGHT_GREY "FLEX RX ready.");
    console_.writeln(STR_COLOR_DARK_GREY "No radio attached: decode is untested on air.");
}

FlexRxView::~FlexRxView() = default;

void FlexRxView::on_show() {
    View::on_show();
    field_frequency_.focus();
    if (receiver_ && !receiver_->running()) receiver_->start();
    reconfigure_dsp();
}

void FlexRxView::reconfigure_dsp() {
    if (!receiver_) return;

    const double rate = receiver_->sampling_rate();
    if (rate <= 0.0) return;

    if (rate != configured_rate_) {
        front_end_.set_deviation(4'800.0);
        front_end_.configure(rate, kTargetAudioRate, kChannelCutoffHz);
        decoder_.configure(static_cast<float>(front_end_.audio_rate()));
        configured_rate_ = rate;
    }

    if (auto* r = globals().radio) {
        const double offset = static_cast<double>(receiver_->target_frequency()) - r->rx_frequency();
        front_end_.set_offset(offset);
    }
}

void FlexRxView::handle_packet(const flex::FlexPacket& pkt) {
    ++packet_count_;

    /* Row 1: cycle/frame, speed, polarity, plus the clock if a BIW has told
     * us one. */
    {
        std::string s1 = "C" + to_string_dec_uint(pkt.cycle) + "/F" +
                         to_string_dec_uint(pkt.frame) + " " + to_string_dec_uint(pkt.bitrate) +
                         " " + (pkt.is_inverted ? "I" : "N");
        if (!status_time_.empty()) s1 += "  " + status_time_;
        if (!status_tz_.empty()) s1 += " " + status_tz_;
        text_status1_.set(s1);
    }

    /* Row 2 is built from the Block Information Words. */
    if (pkt.type == 9) {
        switch (pkt.biw_field) {
            case 0: /* SSID1 */
                status_lid_ = pkt.biw_v1;
                status_cz_ = pkt.biw_v2;
                break;
            case 2: { /* time */
                const uint32_t seconds = (pkt.biw_v3 * 75u) / 10u;
                status_time_ = to_string_dec_uint(pkt.biw_v1, 2, '0') + ":" +
                               to_string_dec_uint(pkt.biw_v2, 2, '0') + ":" +
                               to_string_dec_uint(seconds, 2, '0');
                break;
            }
            case 5: { /* system info, carries the timezone */
                if (pkt.biw_v1 == 4 || pkt.biw_v1 == 5) {
                    const int ofs = flex::timezone_offset_minutes(pkt.biw_v2 & 0x1Fu);
                    const int hrs = ofs / 60;
                    const int mins = (ofs < 0 ? -ofs : ofs) % 60;
                    status_tz_ = std::string("UTC") + (ofs >= 0 ? "+" : "") +
                                 to_string_dec_int(hrs);
                    if (mins != 0) status_tz_ += ":" + to_string_dec_int(mins, 2, '0');
                }
                break;
            }
            case 7: /* SSID2 */
                status_cc_ = pkt.biw_v1;
                break;
            default:
                break;
        }

        std::string s2;
        if (status_lid_) s2 += "LID:" + to_string_dec_uint(status_lid_);
        if (status_cz_) s2 += " CZ:" + to_string_dec_uint(status_cz_);
        if (status_cc_) s2 += " CC:" + to_string_dec_uint(status_cc_);
        if (pkt.fiw_roaming) s2 += " R";
        text_status2_.set(s2);
    }

    /* The console skips BIW frames, tone-only pages and the reserved short
     * message subtype, exactly as upstream does. */
    bool skip = (pkt.type == 9 || pkt.type == 2);
    if (pkt.type == 8 && pkt.function == 3) skip = true;
    if (!skip) console_.writeln(flex::format_packet_line(pkt));
}

void FlexRxView::on_frame_sync() {
    View::on_frame_sync();

    if (!receiver_) return;

    reconfigure_dsp();

    /* HONEST LIMITATION, the same one the POCSAG app has:
     * take_spectrum_samples() is the wideband tap and it is a sliding
     * snapshot, not a queue. A FLEX frame is 1.875 s long and its symbol
     * clock has to stay locked across the whole of it, so a decoder fed
     * 4096-sample fragments a few times a second cannot hold sync on real
     * traffic. What this needs is a gap-free channel tap — samples after the
     * receiver's NCO and channel filter, drained through a ring buffer.
     * ReceiverModel does not expose one. The decode chain below is correct
     * and tested against synthetic frames; only the sample source is
     * inadequate. */
    if (!receiver_->take_spectrum_samples(iq_, 4096)) return;

    front_end_.process(iq_.data(), iq_.size(), audio_);
    if (!audio_.empty()) decoder_.process(audio_.data(), audio_.size());

    if ((++frame_counter_ % 30) == 0 && packet_count_ == 0) {
        text_status1_.set(decoder_.demod.locked ? "Locked, no frames yet" : "No signal");
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* No FLEX-specific bitmap exists in bitmaps.hpp; the pager icon is the honest
 * closest match, and it is what the paging apps share. */
const app::Registrar reg_flex_rx{{"flex_rx", "FLEX RX", app::Category::Receive,
                                  ui::Color::green(), &ui::bitmap_icon_pocsag,
                                  [] { return std::make_unique<app::FlexRxView>(); }}};
}  // namespace
