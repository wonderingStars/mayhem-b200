/*
 * mayhem-b200 — Coaster / Syscall pager TX implementation.
 *
 * Copyright (C) 2023 Bernd Herzog (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_coasterp.hpp"

#include "../dsp/demod_digital.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "string_format.hpp"

#include <cstddef>

namespace app {

namespace {

/* 1000 baud, +/-5 kHz deviation (upstream set_fsk_data(19*8, 2280000/1000,
 * 5000, 32)). */
constexpr float kBaudHz = 1000.0f;
constexpr float kDeviationHz = 5000.0f;
constexpr float kSampleRateHz = 500000.0f;

}  // namespace

std::array<uint8_t, 19> coasterp_build_frame(uint64_t data) {
    std::array<uint8_t, 19> frame{};
    for (uint8_t c = 0; c < 8; ++c) frame[c] = 0x55;  // preamble
    frame[8] = 0x2D;                                  // sync
    frame[9] = 0xD4;
    frame[10] = 8;  // data length
    for (size_t i = 0; i < 8; ++i)
        frame[11 + i] = static_cast<uint8_t>((data >> ((7 - i) * 8)) & 0xFFu);  // big-endian
    return frame;
}

std::vector<uint8_t> coasterp_frame_bits(const std::array<uint8_t, 19>& frame) {
    std::vector<uint8_t> bits;
    bits.reserve(frame.size() * 8);
    for (uint8_t byte : frame)
        for (int b = 7; b >= 0; --b)
            bits.push_back(static_cast<uint8_t>((byte >> b) & 1u));
    return bits;
}

/* --- View ------------------------------------------------------------------ */

CoasterPagerView::CoasterPagerView() {
    add_children({&labels_, &sym_data_, &field_freq_, &check_scan_, &console_, &button_tx_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_freq_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                              static_cast<uint64_t>(caps.tx_freq.max));
    }
    field_freq_.set_value(433'920'000, false);
    sym_data_.set_value(static_cast<uint64_t>(0x44013B30303034BCull));

    field_freq_.on_change = [](uint64_t f) {
        if (auto* tx = globals().transmitter) tx->set_target_frequency(f);
    };

    button_tx_.on_select = [this](ui::Button&) {
        if (transmitting_)
            stop_tx();
        else
            start_tx();
    };

    console_.enable_scrolling(true);
    console_.writeln(STR_COLOR_YELLOW "Restaurant pager buzzer.");
    console_.writeln(STR_COLOR_YELLOW "TX misuses a licensed band.");
    console_.writeln(STR_COLOR_LIGHT_GREY "RF needs a USRP B200.");
}

CoasterPagerView::~CoasterPagerView() {
    if (transmitting_) stop_tx();
}

void CoasterPagerView::focus() {
    button_tx_.focus();
}

void CoasterPagerView::start_tx() {
    auto* tx = globals().transmitter;
    if (!tx) {
        console_.writeln(STR_COLOR_RED "No transmitter wired.");
        return;
    }

    const auto frame = coasterp_build_frame(sym_data_.to_integer());
    const auto bits = coasterp_frame_bits(frame);
    tx_iq_ = dsp::fsk_modulate(bits, kSampleRateHz, kBaudHz, kDeviationHz);
    tx_pos_.store(0);

    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(kSampleRateHz);
    tx->set_target_frequency(field_freq_.value());
    tx->set_iq_source([this](std::complex<float>* out, size_t n) -> size_t {
        const size_t pos = tx_pos_.load();
        const size_t remaining = (pos < tx_iq_.size()) ? (tx_iq_.size() - pos) : 0;
        const size_t take = (n < remaining) ? n : remaining;
        for (size_t i = 0; i < take; ++i) out[i] = tx_iq_[pos + i];
        tx_pos_.store(pos + take);
        return take;
    });

    if (!tx->start()) {
        console_.writeln(STR_COLOR_RED "TX start failed (no B200).");
        tx->set_iq_source(nullptr);
        return;
    }

    scanning_ = check_scan_.value();
    transmitting_ = true;
    button_tx_.set_text("Stop");
    console_.writeln("TX " + to_string_hex(sym_data_.to_integer(), 16));
}

void CoasterPagerView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    transmitting_ = false;
    button_tx_.set_text("Start");
}

void CoasterPagerView::on_frame_sync() {
    View::on_frame_sync();
    if (!transmitting_) return;
    if (tx_pos_.load() < tx_iq_.size()) return;

    if (scanning_) {
        /* Increment the low 16 bits (the address) and send again. */
        uint64_t data = sym_data_.to_integer();
        const uint16_t address = static_cast<uint16_t>((data & 0xFFFFu) + 1);
        data = (data & 0xFFFFFFFFFFFF0000ull) | address;
        sym_data_.set_value(data);
        start_tx();
    } else {
        console_.writeln(STR_COLOR_GREEN "Done.");
        stop_tx();
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_coasterp{{
    "coasterp", "BurgerPgr", app::Category::Transmit,
    ui::Color::yellow(), nullptr,
    [] { return std::make_unique<app::CoasterPagerView>(); }}};
}  // namespace
