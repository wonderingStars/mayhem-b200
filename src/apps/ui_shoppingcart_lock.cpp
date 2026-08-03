/*
 * mayhem-b200 — Shopping-cart wheel lock tone (host port).
 *
 * See ui_shoppingcart_lock.hpp for the protocol notes and the honesty/legal
 * statement.
 *
 * Copyright (C) 2023 RocketGod (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_shoppingcart_lock.hpp"

#include "../audio/audio_out.hpp"
#include "../core/string_format.hpp"
#include "app_context.hpp"

#include <algorithm>
#include <cmath>

namespace app {
namespace cartlock {

/* --- pure tone generation -------------------------------------------------- */

std::vector<float> generate_ook(uint8_t code, float carrier_hz,
                                float sample_rate_hz, float bit_seconds,
                                float amplitude) {
    const size_t spb = samples_per_bit(sample_rate_hz, bit_seconds);
    std::vector<float> out;
    out.reserve(spb * 8);

    const double phase_step = (sample_rate_hz > 0.0f)
                                  ? 2.0 * M_PI * static_cast<double>(carrier_hz) /
                                        static_cast<double>(sample_rate_hz)
                                  : 0.0;
    double phase = 0.0;

    for (size_t bit = 0; bit < 8; ++bit) {
        const bool on = code_bit(code, bit);
        for (size_t s = 0; s < spb; ++s) {
            const float sample =
                on ? amplitude * static_cast<float>(std::sin(phase)) : 0.0f;
            out.push_back(sample);
            phase += phase_step;
            if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
    }
    return out;
}

/* --- view ------------------------------------------------------------------ */

ShoppingCartLockView::ShoppingCartLockView() {
    add_children({&labels_, &field_carrier_, &options_code_,
                  &text_note1_, &text_note2_, &text_note3_, &text_legal_,
                  &console_,
                  &button_lock_, &button_unlock_, &button_stop_});

    field_carrier_.set_value(static_cast<int32_t>(kCarrierHz), false);
    options_code_.set_selected_index(0, false);

    console_.enable_scrolling(true);

    button_lock_.on_select = [this](ui::Button&) {
        const uint8_t code = (options_code_.selected_index() == 0) ? kLockCode
                                                                   : kLock2Code;
        begin("LOCK", code);
    };
    button_unlock_.on_select = [this](ui::Button&) {
        const uint8_t code =
            (options_code_.selected_index() == 0) ? kUnlockCode : kUnlock2Code;
        begin("UNLOCK", code);
    };
    button_stop_.on_select = [this](ui::Button&) { stop(); };

    log(STR_COLOR_LIGHT_GREY "Cart Lock: ~7.8kHz OOK tone.");
    log(STR_COLOR_LIGHT_GREY "Emitted via PC sound card,");
    log(STR_COLOR_LIGHT_GREY "not the B200 (RF floor 70MHz).");
    log(STR_COLOR_DARK_YELLOW "Legal: only on your own gear.");
    log(STR_COLOR_WHITE "Ready. Press Lock/Unlock.");
}

ShoppingCartLockView::~ShoppingCartLockView() {
    stop();
}

void ShoppingCartLockView::focus() {
    button_lock_.focus();
}

void ShoppingCartLockView::on_hide() {
    stop();
    View::on_hide();
}

void ShoppingCartLockView::begin(const char* what, uint8_t code) {
    stop();

    const float carrier = static_cast<float>(field_carrier_.value());
    waveform_ = generate_ook(code, carrier, static_cast<float>(audio::sample_rate),
                             kBitSeconds);
    play_pos_ = 0;

    auto* ao = globals().audio_out;
    if (!ao) {
        log(STR_COLOR_RED "No audio device.");
        return;
    }
    if (!ao->running()) {
        if (!ao->start()) {
            log(STR_COLOR_RED "Audio start failed:");
            log(ao->last_error());
            return;
        }
    }
    audio_started_ = true;
    ao->set_muted(false);
    ao->set_volume(99);
    active_ = true;

    log(std::string(STR_COLOR_GREEN ">> ") + what + " 0x" +
        to_string_hex(code, 2));
    log(STR_COLOR_LIGHT_GREY "Carrier " +
        to_string_dec_uint(static_cast<uint64_t>(carrier)) + "Hz, looping.");
    refill_audio();
}

void ShoppingCartLockView::stop() {
    if (active_) log(STR_COLOR_LIGHT_GREY "<< stopped");
    active_ = false;
    play_pos_ = 0;
    if (audio_started_) {
        if (auto* ao = globals().audio_out) ao->stop();
        audio_started_ = false;
    }
}

void ShoppingCartLockView::refill_audio() {
    auto* ao = globals().audio_out;
    if (!ao || waveform_.empty()) return;

    size_t space = ao->space();
    while (space > 0) {
        const size_t avail = waveform_.size() - play_pos_;
        const size_t chunk = std::min(space, avail);
        const size_t wrote = ao->write(waveform_.data() + play_pos_, chunk);
        play_pos_ += wrote;
        if (play_pos_ >= waveform_.size()) play_pos_ = 0;
        if (wrote < chunk) break;  /* ring full for now */
        space -= wrote;
    }
}

void ShoppingCartLockView::on_frame_sync() {
    View::on_frame_sync();
    if (active_) refill_audio();
}

void ShoppingCartLockView::log(std::string_view line) {
    console_.writeln(line);
}

}  // namespace cartlock
}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_shoppingcart_lock{{
    "shoppingcart_lock", "Cart Lock", app::Category::Utilities,
    ui::Color::yellow(), &ui::bitmap_icon_utilities,
    [] { return std::make_unique<app::cartlock::ShoppingCartLockView>(); },
    false /* generates + plays the tone on the host; coupling caveat on screen */}};
}  // namespace
