/*
 * mayhem-b200 — ACARS receiver.
 *
 * See ui_acars_rx.hpp for the port notes and the three documented departures
 * from upstream (character bit order, CRC coverage, demodulator).
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2018 Furrtek
 * Copyright (C) 2023 Bernd Herzog
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_acars_rx.hpp"

#include "../core/file_path.hpp"
#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"

#include <algorithm>
#include <cmath>

namespace app {

/* The protocol layer — parity, CRC, frame parse and the block framing state
 * machine — is inline in ui_acars_rx.hpp so it can be linked and tested
 * without the view. Only the view lives here. */

/* --- View ------------------------------------------------------------------ */

namespace {

/* AM-detected audio rate. 24 kHz is ten samples per ACARS bit exactly, which
 * makes the one-bit correlator delay an integer number of samples. */
constexpr double kAcarsAudioRate = 24000.0;

/* Sampling rate the app asks the receiver for: 2.4 Msps divides by 100 to land
 * on kAcarsAudioRate exactly, and is inside every B200's range. */
constexpr double kAcarsCaptureRate = 2'400'000.0;

/* Samples pulled from the wideband tap per UI frame. */
constexpr size_t kPullSamples = 4096;

}  // namespace

AcarsRxView::AcarsRxView()
    : receiver_{*globals().receiver} {
    add_children({&labels_,
                  &field_frequency_,
                  &field_gain_,
                  &check_log_,
                  &text_counts_,
                  &text_status_,
                  &text_tap_,
                  &console_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    field_frequency_.set_value(kAcarsDefaultFrequency, false);
    field_frequency_.set_step_index(4); /* 5 kHz — ACARS channels are 25 kHz */
    field_frequency_.on_change = [this](uint64_t hz) {
        receiver_.set_target_frequency(hz);
        rebuild_chain();
    };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    check_log_.set_value(false);
    check_log_.on_select = [this](ui::Checkbox&, bool v) {
        logging_ = v;
        if (!v) {
            log_.flush();
            return;
        }
        if (!log_.is_open()) {
            const std::string dir = core::data_directory() + "/LOGS";
            core::ensure_directory(dir);
            log_.open(dir + "/ACARS.TXT", std::ios::out | std::ios::app);
            if (!log_.is_open()) {
                console_.writeln(STR_COLOR_RED "cannot open LOGS/ACARS.TXT");
                logging_ = false;
                check_log_.set_value(false);
            }
        }
    };

    decoder_.decoder().on_block = [this](const AcarsBlock& b) { this->on_block(b); };

    text_tap_.set(STR_COLOR_YELLOW "Tap: wideband snapshot only");

    console_.writeln(STR_COLOR_LIGHT_GREY "ACARS 2400bd MSK, AM channel.");
    console_.writeln(STR_COLOR_LIGHT_GREY "Decoder runs on the wideband tap;");
    console_.writeln(STR_COLOR_LIGHT_GREY "see the note below the counters.");
}

AcarsRxView::~AcarsRxView() {
    if (log_.is_open()) log_.close();
}

void AcarsRxView::on_show() {
    View::on_show();
    field_frequency_.focus();

    receiver_.set_target_frequency(field_frequency_.value());
    receiver_.set_sampling_rate(kAcarsCaptureRate);
    /* AM is the right analogue mode for the loudspeaker too — ACARS is an
     * amplitude-modulated MSK subcarrier, so the operator hears the bursts. */
    receiver_.set_mode(radio::ReceiverModel::Mode::AMAudio);
  /* Data decoder: no speaker monitor. It reads its own tap; the
     * demodulated audio would be modem tones nobody needs. */
    receiver_.set_audio_monitor(false);
    receiver_.set_am_configuration(radio::ReceiverModel::AmConfig::DSB9k);
    if (!receiver_.running()) receiver_.start();

    rebuild_chain();
}

void AcarsRxView::on_hide() {
    if (log_.is_open()) log_.flush();
    View::on_hide();
}

void AcarsRxView::rebuild_chain() {
    const double rate = receiver_.sampling_rate();
    if (!(rate > 0.0)) return;

    const double lo = globals().radio ? globals().radio->rx_frequency() : 0.0;
    const double offset = static_cast<double>(receiver_.target_frequency()) - lo;

    configured_rate_ = rate;
    configured_lo_ = lo;

    /* Mixing by -offset brings the wanted channel from +offset down to DC, the
     * same arithmetic ReceiverModel::retune_if_needed() does for the audio
     * path. The wideband tap is taken before that mixer, so it has to be
     * repeated here. */
    nco_.set_frequency(-offset, rate);

    /* Two decimation stages, as the firmware does, so neither filter needs a
     * silly number of taps: a wide first stage then a narrow channel filter. */
    size_t total = static_cast<size_t>(std::lround(rate / kAcarsAudioRate));
    if (total < 1) total = 1;

    size_t d1 = 1;
    for (size_t candidate = 1; candidate * candidate <= total; candidate++) {
        if (total % candidate == 0) d1 = candidate;
    }
    const size_t d2 = total / d1;

    const double mid_rate = rate / static_cast<double>(d1);
    channel_rate_ = mid_rate / static_cast<double>(d2);

    /* Stage 1: keep everything that will survive stage 2, no more. */
    decim1_.configure(dsp::design_lowpass(mid_rate * 0.4, mid_rate * 0.2, rate, 60.0), d1);
    /* Stage 2: the ACARS channel itself. The MSK spectrum reaches roughly
     * +/-3.6 kHz around the carrier (2400 Hz tone plus its first sideband). */
    decim2_.configure(dsp::design_lowpass(5000.0, 3000.0, mid_rate, 60.0), d2);

    decoder_.configure(static_cast<float>(channel_rate_));
    samples_seen_ = 0;
}

void AcarsRxView::pump_samples() {
    if (!receiver_.take_spectrum_samples(raw_, kPullSamples)) return;
    if (raw_.empty()) return;

    nco_.mix(raw_.data(), raw_.data(), raw_.size());

    stage1_.clear();
    decim1_.process(raw_.data(), raw_.size(), stage1_);
    if (stage1_.empty()) return;

    channel_.clear();
    decim2_.process(stage1_.data(), stage1_.size(), channel_);
    if (channel_.empty()) return;

    samples_seen_ += channel_.size();
    decoder_.process_baseband(channel_.data(), channel_.size());
}

void AcarsRxView::on_block(const AcarsBlock& block) {
    blocks_++;
    const AcarsDecoded decoded = acars_decode(block.raw());
    if (decoded.crc_ok) crc_ok_++;

    const std::string stamp = to_string_datetime_now();
    const std::string line = stamp + " " + acars_format_line(decoded);
    console_.writeln(decoded.crc_ok ? (STR_COLOR_GREEN + line) : (STR_COLOR_YELLOW + line));
    if (!decoded.txt.empty()) console_.writeln("  " + decoded.txt);

    log_line(stamp + ": " + acars_format(decoded));
}

void AcarsRxView::log_line(const std::string& line) {
    if (!logging_ || !log_.is_open()) return;
    log_ << line << '\n';
    log_.flush();
}

void AcarsRxView::update_status() {
    text_counts_.set("blk " + to_string_dec_uint(blocks_) +
                     " ok " + to_string_dec_uint(crc_ok_));

    const auto& dec = decoder_.decoder();
    text_status_.set(std::string{"State "} + acars_state_name(dec.state()) +
                     "  len " + to_string_dec_uint(dec.message_length()) +
                     "  perr " + to_string_dec_uint(dec.parity_errors()));

    /* Honesty: the only sample tap ReceiverModel publishes is a most-recent-
     * block wideband snapshot, so the bit stream this decoder sees has gaps.
     * Show the duty cycle rather than pretending the decoder is fed properly. */
    if (channel_rate_ > 0.0 && frame_counter_ > 0) {
        const double expected = channel_rate_ * static_cast<double>(frame_counter_) / 60.0;
        const double duty = (expected > 0.0)
                                ? 100.0 * static_cast<double>(samples_seen_) / expected
                                : 0.0;
        text_tap_.set(STR_COLOR_YELLOW "Wideband tap, " +
                      to_string_dec_uint(static_cast<uint64_t>(std::lround(std::min(duty, 999.0)))) +
                      "% of samples");
    }
}

void AcarsRxView::on_frame_sync() {
    View::on_frame_sync();

    frame_counter_++;

    /* The LO moves when the receiver decides the channel has drifted too far
     * from the middle of the captured band; the mixer here has to follow it. */
    const double lo = globals().radio ? globals().radio->rx_frequency() : 0.0;
    if (lo != configured_lo_ || receiver_.sampling_rate() != configured_rate_) rebuild_chain();

    pump_samples();

    if ((frame_counter_ % 10) == 0) update_status();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream's tile is an aircraft silhouette in orange, menu_location RX.
 * bitmap_icon_adsb is the closest aircraft icon in bitmaps.hpp. */
const app::Registrar reg_acars_rx{{"acars_rx", "ACARS", app::Category::Receive,
                                   ui::Color::orange(), &ui::bitmap_icon_adsb,
                                   [] { return std::make_unique<app::AcarsRxView>(); }}};
}  // namespace
