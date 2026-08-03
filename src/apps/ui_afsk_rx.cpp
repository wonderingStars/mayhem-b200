/*
 * mayhem-b200 — AFSK receive terminal.
 *
 * See ui_afsk_rx.hpp for the port notes and the two documented departures.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2023 Bernd Herzog
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_afsk_rx.hpp"

#include "../core/file_path.hpp"
#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"

#include <algorithm>
#include <cmath>

namespace app {

/* The modem presets, the word framer and the word-to-character transform are
 * inline in ui_afsk_rx.hpp so they can be linked and tested without the view.
 * Only the view lives here. */

/* --- View ------------------------------------------------------------------ */

namespace {

/* 24 kHz is upstream's audio rate (3.072 MHz / 8 / 8 / 2) and comfortably above
 * twice the highest modem tone in the preset table. */
constexpr double kAfskAudioRate = 24000.0;

/* 2.4 Msps divides by 100 onto kAfskAudioRate exactly. */
constexpr double kAfskCaptureRate = 2'400'000.0;

/* Half the 11 kHz channel filter upstream uses. */
constexpr double kAfskChannelCutoff = 5500.0;

constexpr size_t kPullSamples = 4096;

/* Upstream's default: LCR reception on Bell 202. */
constexpr uint64_t kAfskDefaultFrequency = 467'225'500ull;

}  // namespace

AfskRxView::AfskRxView()
    : receiver_{*globals().receiver} {
    add_children({&labels_,
                  &field_frequency_,
                  &field_gain_,
                  &options_preset_,
                  &check_log_,
                  &field_baudrate_,
                  &field_mark_,
                  &field_space_,
                  &options_data_bits_,
                  &options_parity_,
                  &options_stop_bits_,
                  &options_bit_order_,
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

    ui::OptionsField::options_t presets;
    presets.reserve(kAfskModemDefCount);
    for (size_t i = 0; i < kAfskModemDefCount; i++)
        presets.emplace_back(kAfskModemDefs[i].name, static_cast<int32_t>(i));
    options_preset_.set_options(std::move(presets));

    field_frequency_.set_value(kAfskDefaultFrequency, false);
    /* Upstream sets a 100 Hz step for this app. */
    field_frequency_.set_step_index(2);
    field_frequency_.on_change = [this](uint64_t hz) {
        receiver_.set_target_frequency(hz);
        rebuild_chain();
    };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    options_preset_.on_change = [this](size_t index, int32_t) { apply_preset(index); };

    field_baudrate_.on_change = [this](int32_t) { reconfigure_decoder(); };
    field_mark_.on_change = [this](int32_t) { reconfigure_decoder(); };
    field_space_.on_change = [this](int32_t) { reconfigure_decoder(); };

    options_data_bits_.on_change = [this](size_t, int32_t v) {
        format_.data_bits = static_cast<uint8_t>(v);
        reconfigure_decoder();
    };
    options_parity_.on_change = [this](size_t, int32_t v) {
        format_.parity = static_cast<AfskSerialParity>(v);
        reconfigure_decoder();
    };
    options_stop_bits_.on_change = [this](size_t, int32_t v) {
        format_.stop_bits = static_cast<uint8_t>(v);
    };
    options_bit_order_.on_change = [this](size_t, int32_t v) {
        format_.bit_order = static_cast<AfskSerialBitOrder>(v);
    };

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
            log_.open(dir + "/AFSK.TXT", std::ios::out | std::ios::app);
            if (!log_.is_open()) {
                console_.writeln(STR_COLOR_RED "cannot open LOGS/AFSK.TXT");
                logging_ = false;
                check_log_.set_value(false);
            }
        }
    };

    decoder_.framer().on_word = [this](uint32_t v) { this->on_word(v); };

    /* Upstream auto-configures Bell 202 with a 7E1 LSB-first serial format. */
    format_ = AfskSerialFormat{7, AfskSerialParity::Even, 1, AfskSerialBitOrder::LsbFirst};
    options_data_bits_.set_by_value(7, false);
    options_parity_.set_by_value(static_cast<int32_t>(AfskSerialParity::Even), false);
    options_stop_bits_.set_by_value(1, false);
    options_bit_order_.set_by_value(static_cast<int32_t>(AfskSerialBitOrder::LsbFirst), false);
    options_preset_.set_selected_index(0, false);
    apply_preset(0);

    text_tap_.set(STR_COLOR_YELLOW "Tap: wideband snapshot only");
}

AfskRxView::~AfskRxView() {
    if (log_.is_open()) log_.close();
}

void AfskRxView::apply_preset(size_t index) {
    if (index >= kAfskModemDefCount) return;
    const auto& def = kAfskModemDefs[index];
    field_baudrate_.set_value(static_cast<int32_t>(def.baudrate), false);
    field_mark_.set_value(static_cast<int32_t>(def.mark_freq), false);
    field_space_.set_value(static_cast<int32_t>(def.space_freq), false);
    reconfigure_decoder();
}

void AfskRxView::reconfigure_decoder() {
    const double rate = (channel_rate_ > 0.0) ? channel_rate_ : kAfskAudioRate;
    decoder_.configure(static_cast<float>(rate),
                       static_cast<float>(field_mark_.value()),
                       static_cast<float>(field_space_.value()),
                       static_cast<float>(field_baudrate_.value()),
                       format_.word_length());
    /* configure() rebuilds the framer, which drops the callback. */
    decoder_.framer().on_word = [this](uint32_t v) { this->on_word(v); };
    prev_value_ = 0;
}

void AfskRxView::on_show() {
    View::on_show();
    field_frequency_.focus();

    receiver_.set_target_frequency(field_frequency_.value());
    receiver_.set_sampling_rate(kAfskCaptureRate);
    receiver_.set_mode(radio::ReceiverModel::Mode::NarrowbandFMAudio);
    receiver_.set_nfm_configuration(radio::ReceiverModel::NfmConfig::Medium11k);
    if (!receiver_.running()) receiver_.start();

    rebuild_chain();
}

void AfskRxView::on_hide() {
    if (log_.is_open()) log_.flush();
    View::on_hide();
}

void AfskRxView::rebuild_chain() {
    const double rate = receiver_.sampling_rate();
    if (!(rate > 0.0)) return;

    const double lo = globals().radio ? globals().radio->rx_frequency() : 0.0;
    const double offset = static_cast<double>(receiver_.target_frequency()) - lo;

    configured_rate_ = rate;
    configured_lo_ = lo;

    /* The wideband tap is taken before ReceiverModel's own mixer, so the
     * channel has to be brought down to DC here. */
    nco_.set_frequency(-offset, rate);

    size_t total = static_cast<size_t>(std::lround(rate / kAfskAudioRate));
    if (total < 1) total = 1;

    size_t d1 = 1;
    for (size_t candidate = 1; candidate * candidate <= total; candidate++) {
        if (total % candidate == 0) d1 = candidate;
    }
    const size_t d2 = total / d1;

    const double mid_rate = rate / static_cast<double>(d1);
    channel_rate_ = mid_rate / static_cast<double>(d2);

    decim1_.configure(dsp::design_lowpass(mid_rate * 0.4, mid_rate * 0.2, rate, 60.0), d1);
    decim2_.configure(dsp::design_lowpass(kAfskChannelCutoff, 2500.0, mid_rate, 60.0), d2);

    samples_seen_ = 0;
    reconfigure_decoder();
}

void AfskRxView::pump_samples() {
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

/* Port of AFSKRxView::on_data() for is_data == true. */
void AfskRxView::on_word(uint32_t value) {
    words_++;

    const uint32_t character = afsk_deframe_word(value,
                                                 format_.data_bits,
                                                 format_.has_parity(),
                                                 format_.bit_order);

    std::string str_console = "\x1B";
    str_console += static_cast<char>((console_color_ & 3) + 9);
    const std::string str_byte = afsk_format_byte(character);
    str_console += str_byte;

    console_.write(str_console);

    if (logging_ && log_.is_open()) str_log_ += str_byte;

    if ((character != 0x7F) && (prev_value_ == 0x7F)) {
        /* Message split. */
        console_.writeln("");
        console_color_++;

        if (logging_ && log_.is_open()) {
            log_ << str_log_ << '\n';
            log_.flush();
            str_log_.clear();
        }
    }
    prev_value_ = character;
}

void AfskRxView::update_status() {
    text_status_.set(to_string_dec_uint(words_) + " wd");

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

void AfskRxView::on_frame_sync() {
    View::on_frame_sync();

    frame_counter_++;

    const double lo = globals().radio ? globals().radio->rx_frequency() : 0.0;
    if (lo != configured_lo_ || receiver_.sampling_rate() != configured_rate_) rebuild_chain();

    pump_samples();

    if ((frame_counter_ % 10) == 0) update_status();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream's tile is a modem/keyboard glyph in yellow, menu_location RX.
 * Nothing in bitmaps.hpp depicts a modem, so this takes the generic tile rather
 * than borrowing a misleading icon (porting contract, "Registering an app"). */
const app::Registrar reg_afsk_rx{{"afsk_rx", "AFSK RX", app::Category::Receive,
                                  ui::Color::yellow(), nullptr,
                                  [] { return std::make_unique<app::AfskRxView>(); }}};
}  // namespace
