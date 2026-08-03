/*
 * mayhem-b200 — Morse TX (CW / MCW keyer).
 *
 * Copyright (C) 2015 Jared Boone / 2016 Furrtek (original app + encoder)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_morse_tx.hpp"

#include "../core/string_format.hpp"
#include "../dsp/modulate.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_alphanum.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <cmath>

namespace app {

using namespace morse_tx;

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
}  // namespace

MorseTxView::MorseTxView() {
    add_children({&labels_, &text_warning_, &field_speed_, &field_tone_,
                  &options_mode_, &check_loop_, &text_duration_,
                  &field_frequency_, &button_message_, &text_message_,
                  &button_tx_, &text_status_});

    text_warning_.set(STR_COLOR_YELLOW "Licensed/amateur use only");

    uint64_t f_min = 70'000'000, f_max = 6'000'000'000ull;
    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        if (caps.tx_freq.max > caps.tx_freq.min) {
            f_min = static_cast<uint64_t>(caps.tx_freq.min);
            f_max = static_cast<uint64_t>(caps.tx_freq.max);
        }
    }
    field_frequency_.set_range(f_min, f_max);
    uint64_t f0 = 14'060'000;  /* the CW QRP calling frequency */
    if (auto* tx = globals().transmitter) {
        const uint64_t t = tx->target_frequency();
        if (t >= f_min && t <= f_max) f0 = t;
    }
    field_frequency_.set_value(f0, false);
    field_frequency_.on_change = [this](uint64_t f) {
        if (auto* tx = globals().transmitter) tx->set_target_frequency(f);
    };

    field_speed_.set_value(15, false);
    field_tone_.set_value(700, false);
    options_mode_.set_selected_index(0, false);  /* CW */

    field_speed_.on_change = [this](int32_t) { update_duration(); };
    options_mode_.on_change = [this](size_t, int32_t) { update_duration(); };
    check_loop_.on_select = [this](ui::Checkbox&, bool v) { loop_ = v; };

    button_message_.on_select = [this](ui::Button&) {
        auto* nav = globals().nav;
        if (!nav) return;
        ui::text_prompt(*nav, message_, 64, ENTER_KEYBOARD_MODE_ALPHA,
                        [this](std::string& v) {
                            message_ = v;
                            text_message_.set(message_);
                            update_duration();
                        });
    };

    button_tx_.on_select = [this](ui::Button&) {
        if (transmitting_)
            stop_tx();
        else
            start_tx();
    };

    text_message_.set(message_);
    update_duration();
}

MorseTxView::~MorseTxView() {
    stop_tx();
}

void MorseTxView::focus() {
    button_message_.focus();
}

void MorseTxView::on_hide() {
    stop_tx();
    View::on_hide();
}

void MorseTxView::update_duration() {
    std::vector<uint8_t> symbols;
    const size_t n = morse_encode(message_, symbols);
    if (n == 0) {
        text_duration_.set(STR_COLOR_RED "too long/empty");
        return;
    }
    const uint32_t units = morse_time_units(symbols);
    const uint32_t unit_ms = morse_time_unit_ms(static_cast<uint32_t>(field_speed_.value()));
    const double secs = static_cast<double>(units) * unit_ms / 1000.0;
    text_duration_.set(to_string_decimal(static_cast<float>(secs), 1) + " s");
}

void MorseTxView::build_waveform() {
    waveform_.clear();

    std::vector<uint8_t> symbols;
    if (morse_encode(message_, symbols) == 0) return;

    const std::vector<uint8_t> onoff = morse_expand_units(symbols);
    if (onoff.empty()) return;

    const uint32_t unit_ms = morse_time_unit_ms(static_cast<uint32_t>(field_speed_.value()));
    if (unit_ms == 0) return;
    const double units_per_sec = 1000.0 / static_cast<double>(unit_ms);

    if (options_mode_.selected_index_value() == 0) {
        /* CW (A1A): key the carrier on/off. */
        const auto packed = pack_bits_msb(onoff);
        dsp::OokKeyer keyer;
        keyer.configure(static_cast<float>(kSampleRate),
                        static_cast<float>(units_per_sec));
        keyer.set_data(packed.data(), onoff.size());

        waveform_.resize(keyer.total_samples());
        size_t got = 0;
        while (got < waveform_.size() && !keyer.done())
            got += keyer.process(waveform_.data() + got, waveform_.size() - got);
        waveform_.resize(got);
    } else {
        /* MCW/FM: gate a sidetone and frequency-modulate it (carrier stays on). */
        const double spu = kSampleRate / units_per_sec;  /* samples per unit */
        std::vector<float> audio;
        audio.reserve(static_cast<size_t>(spu * onoff.size()) + onoff.size());

        const double tone = static_cast<double>(field_tone_.value());
        const double inc = tone / kSampleRate;  /* cycles per sample */
        double phase = 0.0;
        double boundary = 0.0;
        double sample_index = 0.0;

        for (uint8_t on : onoff) {
            boundary += spu;
            while (sample_index < boundary) {
                audio.push_back(on ? static_cast<float>(std::sin(kTwoPi * phase)) : 0.0f);
                phase += inc;
                if (phase >= 1.0) phase -= 1.0;
                sample_index += 1.0;
            }
        }

        dsp::FmModulator fm;
        fm.configure(static_cast<float>(kSampleRate), static_cast<float>(kFmDeviation));
        fm.process(audio.data(), audio.size(), waveform_);
    }
}

void MorseTxView::start_tx() {
    if (message_.empty()) {
        ui::display_modal("Empty", "Set a message first.");
        return;
    }

    build_waveform();
    if (waveform_.empty()) {
        ui::display_modal("Error", "Nothing to send\n(message too long?).");
        return;
    }

    auto* tx = globals().transmitter;
    if (!tx) {
        text_status_.set(STR_COLOR_YELLOW "No transmitter (needs B200).");
        return;
    }

    play_pos_.store(0);
    /* Both CW and MCW are pre-rendered to complex baseband above, so the
     * transmitter streams them verbatim in Raw mode. */
    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(kSampleRate);
    tx->set_target_frequency(field_frequency_.value());
    tx->set_iq_source([this](dsp::cfloat* out, size_t n) -> size_t {
        const size_t total = waveform_.size();
        if (total == 0) return 0;
        size_t written = 0;
        while (written < n) {
            size_t pos = play_pos_.load();
            if (pos >= total) {
                if (!loop_) break;
                pos = 0;
                play_pos_.store(0);
            }
            size_t avail = std::min(n - written, total - pos);
            std::copy(waveform_.begin() + static_cast<std::ptrdiff_t>(pos),
                      waveform_.begin() + static_cast<std::ptrdiff_t>(pos + avail),
                      out + written);
            written += avail;
            play_pos_.store(pos + avail);
        }
        return written;
    });

    if (!tx->start()) {
        tx->set_iq_source(nullptr);
        text_status_.set(STR_COLOR_YELLOW "TX start failed (needs B200).");
        return;
    }

    transmitting_.store(true);
    button_tx_.set_text("Stop TX");
    const double secs = static_cast<double>(waveform_.size()) / kSampleRate;
    text_status_.set(STR_COLOR_GREEN "Sending " +
                     to_string_decimal(static_cast<float>(secs), 1) + "s");
}

void MorseTxView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    if (transmitting_.exchange(false)) {
        button_tx_.set_text("Start TX");
        text_status_.set("Idle");
    }
}

void MorseTxView::on_frame_sync() {
    View::on_frame_sync();
    if (!transmitting_.load()) return;
    if (!loop_ && play_pos_.load() >= waveform_.size()) {
        stop_tx();
        text_status_.set(STR_COLOR_LIGHT_GREY "Sent.");
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream menu_location TX, iconColor green. Functional on the host: it builds a
 * correct CW/MCW waveform and streams it. RF only radiates with a USRP B200
 * attached, so hardware_limited stays false. */
const app::Registrar reg_morse_tx{{
    "morse_tx", "Morse TX", app::Category::Transmit,
    ui::Color::green(), &ui::bitmap_icon_microphone,
    [] { return std::make_unique<app::MorseTxView>(); }}};
}  // namespace
