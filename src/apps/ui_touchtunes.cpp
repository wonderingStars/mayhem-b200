/*
 * mayhem-b200 — TouchTunes jukebox remote (OOK, 433.92 MHz).
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2022 NotPike (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_touchtunes.hpp"

#include "../core/string_format.hpp"
#include "../dsp/modulate.hpp"
#include "../radio/transmitter_model.hpp"
#include "app_context.hpp"

namespace app {
namespace touchtunes {

/* --- Encoder -------------------------------------------------------------- */

/* button_codes[], upstream ui_touchtunes.hpp (code then complement on air). */
static const uint8_t kButtonCodes[kButtonCount] = {
    0x32, 0x78, 0x70, 0x60, 0xCA, 0x20, 0xF2, 0xA0, 0x84, 0x44, 0xC4, 0x30,
    0x80, 0xB0, 0xF0, 0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8, 0x18,
    0x98, 0x58, 0xD0, 0x90, 0xC0, 0x50, 0x10, 0x40};

static const char* const kButtonNames[kButtonCount] = {
    "Pause", "On/Off", "P1", "P2", "P3", "F1", "Up", "F2", "Left", "OK",
    "Right", "F3", "Down", "F4", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "Music/Karaoke", "0", "Lock/Queue", "Z1 Vol+", "Z2 Vol+", "Z3 Vol+",
    "Z1 Vol-", "Z2 Vol-", "Z3 Vol-"};

uint8_t button_code(size_t index) {
    return (index < kButtonCount) ? kButtonCodes[index] : 0;
}

const char* button_name(size_t index) {
    return (index < kButtonCount) ? kButtonNames[index] : "?";
}

uint32_t frame_word(uint32_t pin, size_t button_index) {
    uint32_t frame = kSyncWord;
    /* PIN, least significant bit first (upstream shifts left, ORs bit 0..7). */
    for (uint32_t bit = 0; bit < 8; bit++) {
        frame <<= 1;
        if (pin & (1u << bit)) frame |= 1;
    }
    const uint8_t code = button_code(button_index);
    frame <<= 16;
    frame |= (static_cast<uint32_t>(code) << 8);
    frame |= static_cast<uint32_t>(code ^ 0xFF);
    return frame;
}

std::string build_fragments(uint32_t pin, size_t button_index) {
    uint32_t v = frame_word(pin, button_index);
    std::string fragments;
    for (uint32_t bit = 0; bit < (8 + 8 + 16); bit++) {
        fragments += (v & 0x80000000UL) ? "1000" : "10";
        v <<= 1;
    }
    return std::string("111111111111111100000000") + fragments + "1000";
}

/* --- View ----------------------------------------------------------------- */

static std::vector<uint8_t> fragments_to_bytes(const std::string& frag) {
    std::vector<uint8_t> bytes((frag.size() + 7) / 8, 0);
    for (size_t i = 0; i < frag.size(); i++)
        if (frag[i] == '1') bytes[i >> 3] |= static_cast<uint8_t>(0x80u >> (i & 7));
    return bytes;
}

TouchTunesView::TouchTunesView()
    : transmitter_{globals().transmitter} {
    add_children({&labels_, &field_frequency_, &field_pin_, &options_button_,
                  &field_gain_, &button_tx_, &text_status_, &console_, &notes_});

    console_.enable_scrolling(true);

    field_frequency_.set_step_index(3);  /* 100 kHz */
    field_frequency_.set_value(433'920'000ULL, false);

    field_pin_.set_value(0);
    field_gain_.set_value(40);
    options_button_.set_by_value(1, false);  /* On/Off */

    button_tx_.on_select = [this](ui::Button&) {
        if (transmitting_)
            stop_tx();
        else
            start_tx();
    };
}

TouchTunesView::~TouchTunesView() {
    stop_tx();
}

void TouchTunesView::on_show() {
    ui::View::on_show();
    button_tx_.focus();
    console_.writeln(STR_COLOR_LIGHT_GREY "OOK 433.92 MHz, 1766 baud.");
    console_.writeln(STR_COLOR_LIGHT_GREY "RF output needs a USRP B200.");
}

void TouchTunesView::on_hide() {
    stop_tx();
    ui::View::on_hide();
}

void TouchTunesView::start_tx() {
    if (!transmitter_) {
        console_.writeln("No transmitter (needs B200).");
        return;
    }

    const uint32_t pin = static_cast<uint32_t>(field_pin_.value());
    const size_t idx = static_cast<size_t>(options_button_.selected_index_value());
    const std::string frag = build_fragments(pin, idx);

    dsp::OokKeyer keyer;
    keyer.configure(static_cast<float>(kSampleRate), kBaud);
    keyer.set_repeat(kRepeats, 100);  /* upstream repeat 4, 100-symbol pause */
    const auto bytes = fragments_to_bytes(frag);
    keyer.set_data(bytes.data(), frag.size());

    burst_.assign(keyer.total_samples(), std::complex<float>{0.0f, 0.0f});
    if (!burst_.empty()) keyer.process(burst_.data(), burst_.size());

    if (burst_.empty()) {
        text_status_.set("Nothing to send");
        return;
    }

    ring_.clear();
    transmitter_->set_mode(radio::TransmitterModel::Mode::Raw);
    transmitter_->set_sampling_rate(kSampleRate);
    transmitter_->set_target_frequency(field_frequency_.value());
    transmitter_->set_gain(static_cast<double>(field_gain_.value()));
    transmitter_->set_iq_source([this](std::complex<float>* out, size_t n) {
        return ring_.read(out, n);
    });
    ring_.write(burst_.data(), burst_.size());

    if (!transmitter_->start()) {
        console_.writeln("TX start failed (needs B200).");
        transmitter_->set_iq_source(nullptr);
        return;
    }

    transmitting_ = true;
    button_tx_.set_text("Stop");
    text_status_.set("TX " + std::string(button_name(idx)) + " pin " + to_string_dec_uint(pin));
    console_.writeln("TX: " + std::string(button_name(idx)));
}

void TouchTunesView::stop_tx() {
    if (transmitter_) {
        transmitter_->stop();
        transmitter_->set_iq_source(nullptr);
    }
    ring_.clear();
    transmitting_ = false;
    button_tx_.set_text("TX");
    text_status_.set("Idle");
}

void TouchTunesView::on_frame_sync() {
    ui::View::on_frame_sync();
    if (!transmitting_) return;
    /* A single burst (already carries its own repeats); stop once it drains. */
    if (ring_.empty()) stop_tx();
}

}  // namespace touchtunes
}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_touchtunes{{"touchtune", "TouchTunes", app::Category::Transmit,
                                     ui::Color::orange(), &ui::bitmap_icon_touchtunes,
                                     [] { return std::make_unique<app::touchtunes::TouchTunesView>(); }}};
}  // namespace
