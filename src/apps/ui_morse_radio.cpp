/*
 * mayhem-b200 — Morse (CW) receiver.
 *
 * Copyright (C) 2025, 2026 Pezsma
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_morse_radio.hpp"

#include "../core/file_path.hpp"
#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace app {

namespace {

/* Upstream runs its baseband at 3.072 Msps and decimates to 12 kHz audio
 * (24 kHz in NFM). Keeping the same capture rate keeps the decimation an exact
 * power of two, which matters because ToneProcessor's block sizes and its
 * duration arithmetic are written against exactly those audio rates. */
constexpr double kCaptureRate = 3'072'000.0;

/* One poll's worth of wideband samples. The receiver's spectrum tap holds
 * 4096. */
constexpr size_t kTapSamples = 4096;

/* Channel filter passbands, from proc_morse.cpp's configure(): 2 kHz AM lowpass
 * for AM, 1.5 kHz for DSB/USB/LSB, and an 11 kHz channel for NFM whose
 * discriminator is set up for 5 kHz deviation. */
double channel_cutoff_hz(morse::Modulation m) {
    switch (m) {
        case morse::Modulation::FM:
            return 5000.0;
        case morse::Modulation::AM:
            return 2000.0;
        default:
            return 1500.0;
    }
}

std::ofstream& log_stream() {
    static std::ofstream f;
    return f;
}

}  // namespace

MorseRadioView::MorseRadioView()
    : receiver_{*globals().receiver} {
    add_children({&labels_,
                  &field_frequency_,
                  &field_gain_,
                  &text_level_,
                  &field_squelch_,
                  &options_mode_,
                  &text_speed_,
                  &text_tone_,
                  &text_last_,
                  &text_clip_,
                  &text_tap_,
                  &button_clear_,
                  &check_log_,
                  &console_});

    text_clip_.set_style(ui::Theme::getInstance()->fg_red);
    text_clip_.hidden(true);
    text_tap_.set_style(ui::Theme::getInstance()->fg_yellow);

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    /* Upstream sets a 100 Hz tuning step here. */
    field_frequency_.set_step_index(2);
    field_frequency_.set_value(receiver_.target_frequency(), false);
    field_frequency_.on_change = [this](uint64_t hz) {
        receiver_.set_target_frequency(hz);
        chain_valid_ = false;
    };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    field_squelch_.set_value(receiver_.squelch_level(), false);
    field_squelch_.on_change = [this](int32_t v) {
        receiver_.set_squelch_level(static_cast<uint8_t>(v));
        processor_.set_squelch_level(v);
    };

    options_mode_.on_change = [this](size_t, int32_t mode) { set_mode(mode); };

    button_clear_.on_select = [this](ui::Button&) {
        console_.clear(true);
        text_last_.set("");
        decoder_.resetLearning();
        accumulator_.reset();
    };

    check_log_.set_value(false);
    check_log_.on_select = [](ui::Checkbox&, bool enabled) {
        auto& f = log_stream();
        if (f.is_open()) f.close();
        if (!enabled) return;
        const std::string dir = core::data_directory() + "/LOGS";
        core::ensure_directory(dir);
        f.open(dir + "/MORSE_" + to_string_timestamp_now() + ".TXT",
               std::ios::out | std::ios::app);
    };

    options_mode_.set_selected_index(0, false);
    set_mode(MORSE_AM_CW);
}

MorseRadioView::~MorseRadioView() {
    if (log_stream().is_open()) log_stream().close();
}

void MorseRadioView::focus() {
    field_frequency_.focus();
}

void MorseRadioView::set_mode(int32_t mode) {
    mode_ = mode;

    /* The UI index is upstream's morse_modes, which its baseband casts straight
     * to ModulationMode — same numbering. */
    const auto modulation = static_cast<morse::Modulation>(static_cast<uint8_t>(mode));
    processor_.configure(modulation);
    processor_.set_squelch_level(field_squelch_.value());
    decoder_.resetLearning();
    accumulator_.reset();

    /* Put the receiver's own audio path in the matching mode so the operator
     * hears what the detector is looking at. */
    if (mode == MORSE_NFM) {
        receiver_.set_mode(radio::ReceiverModel::Mode::NarrowbandFMAudio);
        receiver_.set_nfm_configuration(radio::ReceiverModel::NfmConfig::Medium11k);
        field_squelch_.set_focusable(true);
    } else {
        receiver_.set_mode(radio::ReceiverModel::Mode::AMAudio);
        switch (mode) {
            case MORSE_AM_USB:
                receiver_.set_am_configuration(radio::ReceiverModel::AmConfig::USB);
                break;
            case MORSE_AM_LSB:
                receiver_.set_am_configuration(radio::ReceiverModel::AmConfig::LSB);
                break;
            case MORSE_AM_DSB:
                receiver_.set_am_configuration(radio::ReceiverModel::AmConfig::DSB6k);
                break;
            default:
                receiver_.set_am_configuration(radio::ReceiverModel::AmConfig::CW);
                break;
        }
        /* Upstream greys the squelch out in the AM modes: its AM path keeps the
         * squelch open regardless. */
        field_squelch_.set_focusable(false);
    }

    chain_valid_ = false;
    text_speed_.set("??");
    text_tone_.set("??");
}

void MorseRadioView::rebuild_chain() {
    const double input_rate = receiver_.sampling_rate();
    if (input_rate <= 0.0) return;

    const auto modulation = processor_.modulation();
    const double audio_rate_target = processor_.audio_rate_hz();

    size_t decimation = static_cast<size_t>(std::lround(input_rate / audio_rate_target));
    if (decimation < 1) decimation = 1;
    chain_decimation_ = decimation;
    chain_audio_rate_ = input_rate / static_cast<double>(decimation);

    const double cutoff = channel_cutoff_hz(modulation);
    /* Stop band starts at the decimated Nyquist so nothing folds into the
     * detector's band. */
    const double transition = std::max(500.0, chain_audio_rate_ * 0.5 - cutoff);
    channel_filter_.configure(dsp::design_lowpass(cutoff, transition, input_rate, 50.0),
                              decimation);

    switch (modulation) {
        case morse::Modulation::FM:
            fm_.configure(static_cast<float>(chain_audio_rate_), 5000.0f);
            break;
        case morse::Modulation::USB:
            ssb_.configure(static_cast<float>(chain_audio_rate_),
                           dsp::SsbDemod::Sideband::Upper, 63);
            break;
        case morse::Modulation::LSB:
            ssb_.configure(static_cast<float>(chain_audio_rate_),
                           dsp::SsbDemod::Sideband::Lower, 63);
            break;
        default:
            am_.configure(static_cast<float>(chain_audio_rate_));
            break;
    }

    chain_input_rate_ = input_rate;
    chain_offset_hz_ = 0.0;
    if (auto* r = globals().radio) {
        const double lo = r->rx_frequency();
        if (lo > 0.0) chain_offset_hz_ = static_cast<double>(receiver_.target_frequency()) - lo;
    }
    /* Mix the wanted channel down to DC: the tap is centred on the LO. */
    nco_.set_frequency(-chain_offset_hz_, input_rate);

    /* Honest duty-cycle figure for the banner. The spectrum tap is the only
     * sample source this view can reach, and it is block-sampled: one poll per
     * UI frame yields kTapSamples out of input_rate samples per second. */
    const double duty = (static_cast<double>(kTapSamples) * 60.0) / input_rate;
    const int pct = static_cast<int>(std::lround(std::min(1.0, duty) * 100.0));
    text_tap_.set("tap " + to_string_dec_uint(static_cast<uint64_t>(pct)) + "% - no chan tap");

    chain_valid_ = true;
}

void MorseRadioView::pump() {
    if (!chain_valid_) rebuild_chain();
    if (!chain_valid_) return;

    /* IDEAL TAP: a continuous stream of demodulated channel audio at 12 kHz
     * (24 kHz in NFM), i.e. what upstream's M4 hands its Goertzel. The host
     * ReceiverModel exposes only take_spectrum_samples(), a snapshot of the most
     * recent wideband block, so this view mixes and filters the channel itself
     * and gets whatever fraction of the signal the snapshot covers. Everything
     * downstream of here is upstream's pipeline and is correct when fed
     * contiguous audio; with the snapshot tap the element timings will be short
     * and intermittent. text_tap_ says so on screen. */
    if (!receiver_.take_spectrum_samples(raw_, kTapSamples)) return;
    if (raw_.empty()) return;

    mixed_.resize(raw_.size());
    nco_.mix(raw_.data(), mixed_.data(), raw_.size());

    channel_.clear();
    channel_filter_.process(mixed_.data(), mixed_.size(), channel_);
    if (channel_.empty()) return;

    audio_.clear();
    switch (processor_.modulation()) {
        case morse::Modulation::FM:
            fm_.process(channel_.data(), channel_.size(), audio_);
            break;
        case morse::Modulation::USB:
        case morse::Modulation::LSB:
            ssb_.process(channel_.data(), channel_.size(), audio_);
            break;
        default:
            am_.process(channel_.data(), channel_.size(), audio_);
            break;
    }
    if (audio_.empty()) return;

    durations_.clear();
    processor_.process_audio(audio_.data(), audio_.size(), durations_);

    text_clip_.hidden(!processor_.clipped());

    for (int32_t d : durations_) {
        const int32_t ms = accumulator_.process(d, decoder_.getInterWordThreshold());
        const auto result = decoder_.handleInput(ms);
        if (!result.isValid()) continue;

        quiet_frames_ = 0;
        write_char(result.text, result.confidence);
        text_speed_.set(to_string_dec_uint(decoder_.wpm()));
    }

    uint32_t tone_hz = 0;
    if (processor_.take_frequency_update(tone_hz)) update_tone_readout(tone_hz);
}

void MorseRadioView::write_char(const std::string& ch, double confidence) {
    if (ch.empty()) return;

    text_last_.set(decoder_.getLastSequence());

    /* Upstream's confidence colouring: white for a space or an unmatched
     * sequence, then red / yellow / green as confidence rises. */
    const char* color = STR_COLOR_WHITE;
    if (ch != " " && ch[0] != '{') {
        if (confidence < 0.8)
            color = STR_COLOR_RED;
        else if (confidence < 0.9)
            color = STR_COLOR_YELLOW;
        else
            color = STR_COLOR_GREEN;
    }

    console_.write(std::string{color} + ch);

    /* Plain-text mirror for the browser panel, bounded to the most recent
     * characters. '{...}' is the decoder's "unmatched sequence" marker; keep
     * it out of the clean text a reader sees. */
    if (ch[0] != '{') {
        decoded_history_ += ch;
        constexpr size_t kMaxHistory = 400;
        if (decoded_history_.size() > kMaxHistory)
            decoded_history_.erase(0, decoded_history_.size() - kMaxHistory);
    }

    auto& f = log_stream();
    if (f.is_open()) {
        f << ch;
        f.flush();
    }
}

void MorseRadioView::update_tone_readout(uint32_t hz) {
    last_tone_hz_ = hz; /* published to the browser panel; 0 means no lock */

    /* Port of on_freq(): clamp to the detector's 300..2300 Hz range and colour
     * by how close the tone is to the 400..1400 Hz sweet spot. */
    std::string prefix = " ";
    uint32_t freq = hz;
    if (freq < 301) {
        prefix = "<";
        freq = 300;
    }
    if (freq > 2299) {
        freq = 2300;
        prefix = ">";
    }

    if (freq < 400 || freq > 1400)
        text_tone_.set_style(ui::Theme::getInstance()->fg_red);
    else if (freq <= 580 || freq >= 1220)
        text_tone_.set_style(ui::Theme::getInstance()->fg_yellow);
    else
        text_tone_.set_style(ui::Theme::getInstance()->fg_green);

    text_tone_.set(prefix + to_string_dec_uint(freq));
}

void MorseRadioView::check_for_timeout() {
    /* Upstream resets the learned timing after 60 s of silence. on_frame_sync
     * runs at ~60 Hz, so 3600 quiet frames is the same minute. */
    quiet_frames_++;
    if (quiet_frames_ >= 3600) {
        decoder_.resetLearning();
        accumulator_.reset();
        quiet_frames_ = 0;
    }
}

void MorseRadioView::on_show() {
    View::on_show();

    receiver_.set_sampling_rate(kCaptureRate);
    chain_valid_ = false;
    if (!receiver_.running()) receiver_.start();

    field_frequency_.focus();
}

void MorseRadioView::on_hide() {
    View::on_hide();
}

void MorseRadioView::on_frame_sync() {
    View::on_frame_sync();
    frame_counter_++;

    pump();
    check_for_timeout();

    if ((frame_counter_ % 12) == 0) {
        const int level = static_cast<int>(std::lround(receiver_.channel_level_db()));
        text_level_.set(to_string_dec_int(level) + " dB");
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_morse_radio{{"morseradio", "Morse", app::Category::Receive,
                                      ui::Color::green(), &ui::bitmap_icon_speaker,
                                      [] { return std::make_unique<app::MorseRadioView>(); }}};
}  // namespace
