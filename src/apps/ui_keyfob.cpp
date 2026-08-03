/*
 * mayhem-b200 — Key fob TX implementation.
 *
 * Copyright (C) 2023 Bernd Herzog (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_keyfob.hpp"

#include "../dsp/modulate.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "string_format.hpp"

#include <cstddef>

namespace app {

namespace {

/* 1013.21 us per OOK symbol on air (upstream subaru_samples_per_bit). */
constexpr float kSymbolRateHz = 1.0f / 0.00101321f;
constexpr float kSampleRateHz = 500000.0f;
constexpr uint32_t kRepeats = 4;
constexpr uint32_t kPauseSymbols = 200;

std::vector<uint8_t> pack_ook_bits(const std::string& s) {
    std::vector<uint8_t> bytes((s.size() + 7) / 8, 0u);
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] != '0')
            bytes[i >> 3] = static_cast<uint8_t>(bytes[i >> 3] | (0x80u >> (i & 7u)));
    return bytes;
}

/* Appends the payload of one frame: each of the 80 bits, MSB first, as "10"
 * for a 1 and "01" for a 0. */
void append_payload(std::string& out, const SubaruFrame& frame) {
    for (size_t i = 0; i < (10 * 8); ++i) {
        const bool bit = (frame[i >> 3] & (1u << (7 - (i & 7u)))) != 0;
        out += bit ? "10" : "01";
    }
}

}  // namespace

uint8_t subaru_checksum(const SubaruFrame& frame) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < 9; ++i) {
        checksum ^= static_cast<uint8_t>(frame[i] & 0x0Fu);
        checksum ^= static_cast<uint8_t>((frame[i] >> 4) & 0x0Fu);
    }
    checksum ^= static_cast<uint8_t>((frame[9] >> 4) & 0x0Fu);
    checksum++;
    checksum &= 0x0Fu;
    return checksum;
}

bool subaru_is_valid(const SubaruFrame& frame) {
    if (frame[0] != 0x55) return false;
    return subaru_checksum(frame) == (frame[9] & 0x0Fu);
}

void subaru_set_command(SubaruFrame& frame, uint8_t command) {
    frame[5] = static_cast<uint8_t>((frame[5] & 0xF0u) | (command & 0x0Fu));
    frame[6] = static_cast<uint8_t>((frame[6] & 0xF0u) | (command & 0x0Fu));
}

SubaruFrame subaru_build_frame(uint64_t half_a, uint64_t half_b) {
    SubaruFrame frame{};
    uint64_t p = half_a;
    for (size_t i = 0; i < 5; ++i) {
        frame[4 - i] = static_cast<uint8_t>(p & 0xFFu);
        p >>= 8;
    }
    p = half_b;
    for (size_t i = 0; i < 5; ++i) {
        frame[9 - i] = static_cast<uint8_t>(p & 0xFFu);
        p >>= 8;
    }
    frame[9] = static_cast<uint8_t>((frame[9] & 0xF0u) | subaru_checksum(frame));
    return frame;
}

std::string keyfob_encode_bitstream(const SubaruFrame& frame) {
    std::string out;
    out.reserve(256 + 4 + 160 + 8 + 160);

    for (size_t i = 0; i < 128; ++i) out += "01";  // preamble
    out += "0000";                                 // 4x space
    append_payload(out, frame);                    // payload
    out += "00000000";                             // 8x space
    append_payload(out, frame);                    // payload again
    return out;
}

/* --- View ------------------------------------------------------------------ */

KeyfobView::KeyfobView() {
    add_children({&labels_, &field_make_, &field_command_, &field_freq_,
                  &field_payload_a_, &field_payload_b_, &console_, &button_tx_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_freq_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                              static_cast<uint64_t>(caps.tx_freq.max));
    }
    field_freq_.set_value(433'920'000, false);

    frame_ = SubaruFrame{};
    frame_[0] = 0x55;
    subaru_set_command(frame_, 1);
    fields_from_frame();

    field_make_.set_selected_index(0, false);

    field_command_.on_change = [this](size_t, int32_t value) {
        frame_from_fields();
        subaru_set_command(frame_, static_cast<uint8_t>(value));
        fields_from_frame();
    };
    field_payload_a_.on_change = [this](ui::SymField&) { frame_from_fields(); };
    field_payload_b_.on_change = [this](ui::SymField&) { frame_from_fields(); };
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
    console_.writeln(STR_COLOR_YELLOW "Car-remote spoof.");
    console_.writeln(STR_COLOR_YELLOW "TX is illegal almost everywhere.");
    console_.writeln(STR_COLOR_LIGHT_GREY "RF needs a USRP B200.");
}

KeyfobView::~KeyfobView() {
    if (transmitting_) stop_tx();
}

void KeyfobView::focus() {
    button_tx_.focus();
}

void KeyfobView::frame_from_fields() {
    const SubaruFrame f = subaru_build_frame(field_payload_a_.to_integer(),
                                             field_payload_b_.to_integer());
    frame_ = f;
}

void KeyfobView::fields_from_frame() {
    for (size_t i = 0; i < 5; ++i) {
        field_payload_a_.set_offset(i << 1, frame_[i] >> 4);
        field_payload_a_.set_offset((i << 1) + 1, frame_[i] & 0x0Fu);
    }
    for (size_t i = 0; i < 5; ++i) {
        field_payload_b_.set_offset(i << 1, frame_[5 + i] >> 4);
        field_payload_b_.set_offset((i << 1) + 1, frame_[5 + i] & 0x0Fu);
    }
}

void KeyfobView::start_tx() {
    auto* tx = globals().transmitter;
    if (!tx) {
        console_.writeln(STR_COLOR_RED "No transmitter wired.");
        return;
    }

    frame_from_fields();
    frame_[9] = static_cast<uint8_t>((frame_[9] & 0xF0u) | subaru_checksum(frame_));
    fields_from_frame();

    const std::string bitstream = keyfob_encode_bitstream(frame_);
    const auto bits = pack_ook_bits(bitstream);

    dsp::OokKeyer keyer;
    keyer.configure(kSampleRateHz, kSymbolRateHz);
    keyer.set_data(bits.data(), bitstream.size());
    keyer.set_repeat(kRepeats, kPauseSymbols);

    tx_iq_.assign(keyer.total_samples(), std::complex<float>{0.0f, 0.0f});
    if (!tx_iq_.empty())
        keyer.process(tx_iq_.data(), tx_iq_.size());
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

    transmitting_ = true;
    button_tx_.set_text("Stop");
    console_.writeln("TX " + to_string_dec_uint(kRepeats) + "x");
}

void KeyfobView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    transmitting_ = false;
    button_tx_.set_text("Start");
}

void KeyfobView::on_frame_sync() {
    View::on_frame_sync();
    if (!transmitting_) return;
    if (tx_pos_.load() >= tx_iq_.size()) {
        console_.writeln(STR_COLOR_GREEN "Done.");
        stop_tx();
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_keyfob{{
    "keyfob", "Keyfob", app::Category::Transmit,
    ui::Color::orange(), nullptr,
    [] { return std::make_unique<app::KeyfobView>(); }}};
}  // namespace
