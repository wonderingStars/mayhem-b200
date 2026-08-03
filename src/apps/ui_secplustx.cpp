/*
 * mayhem-b200 — Security+ TX implementation.
 *
 * The encoder half is a verbatim port of Clayton Smith's secplus (GPL-3.0)
 * as it appears in firmware/application/external/secplustx/secplustx.cpp.
 *
 * Copyright (C) 2022 Clayton Smith (secplus)
 * Copyright (C) 2026 Synray (original app)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui_secplustx.hpp"

#include "../dsp/modulate.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "string_format.hpp"

#include <cstddef>

namespace app {

/* --- Security+ 2.0 encoder (secplus, verbatim) ----------------------------- */

namespace {

void v2_calc_parity(const uint64_t fixed, uint32_t* data) {
    uint32_t parity = (fixed >> 32) & 0xf;

    *data &= 0xffff0fff;
    for (int8_t offset = 0; offset < 32; offset += 4)
        parity ^= ((*data >> offset) & 0xf);
    *data |= (parity << 12);
}

void encode_v2_rolling(const uint32_t rolling, uint32_t* rolling_halves) {
    uint32_t rolling_reversed = 0;

    for (int8_t i = 0; i < 28; i++)
        rolling_reversed |= ((rolling >> i) & 1) << (28 - i - 1);

    rolling_halves[0] = 0;
    rolling_halves[1] = 0;

    for (int8_t half = 0; half < 2; half++) {
        for (int8_t i = 0; i < 8; i += 2) {
            rolling_halves[half] |= rolling_reversed % 3 << i;
            rolling_reversed /= 3;
        }
    }

    for (int8_t half = 0; half < 2; half++) {
        for (int8_t i = 10; i < 18; i += 2) {
            rolling_halves[half] |= rolling_reversed % 3 << i;
            rolling_reversed /= 3;
        }
    }

    rolling_halves[0] |= (rolling_reversed % 3) << 8;
    rolling_reversed /= 3;

    rolling_halves[1] |= (rolling_reversed % 3) << 8;
}

const int8_t ORDER[16] = {9, 33, 6, -1, 24, 18, 36, -1,
                          24, 36, 6, -1, -1, -1, -1, -1};
const int8_t INVERT[16] = {6, 2, 1, -1, 7, 5, 3, -1,
                           4, 0, 5, -1, -1, -1, -1, -1};

void v2_scramble(const uint32_t* parts, const uint8_t frame_type, uint8_t* packet_half) {
    const int8_t order = ORDER[packet_half[0] >> 4];
    const int8_t invert = INVERT[packet_half[0] & 0xf];
    uint8_t out_offset = 10;
    int8_t end;
    uint32_t parts_permuted[3];

    end = (frame_type == 0 ? 5 : 8);
    for (int8_t i = 1; i < end; i++)
        packet_half[i] = 0;

    parts_permuted[0] = (invert & 4) ? ~parts[(order >> 4) & 3] : parts[(order >> 4) & 3];
    parts_permuted[1] = (invert & 2) ? ~parts[(order >> 2) & 3] : parts[(order >> 2) & 3];
    parts_permuted[2] = (invert & 1) ? ~parts[order & 3] : parts[order & 3];

    end = (frame_type == 0 ? 8 : 0);
    for (int8_t i = 18 - 1; i >= end; i--) {
        packet_half[out_offset >> 3] |= ((parts_permuted[0] >> i) & 1) << (7 - (out_offset % 8));
        out_offset++;
        packet_half[out_offset >> 3] |= ((parts_permuted[1] >> i) & 1) << (7 - (out_offset % 8));
        out_offset++;
        packet_half[out_offset >> 3] |= ((parts_permuted[2] >> i) & 1) << (7 - (out_offset % 8));
        out_offset++;
    }
}

void encode_v2_half_parts(const uint32_t rolling, const uint32_t fixed, const uint16_t data,
                          const uint8_t frame_type, uint8_t* packet_half) {
    uint32_t parts[3];

    parts[0] = ((fixed >> 10) << 8) | (data >> 8);
    parts[1] = ((fixed & 0x3ff) << 8) | (data & 0xff);
    parts[2] = rolling;

    packet_half[0] = (uint8_t)rolling;

    v2_scramble(parts, frame_type, packet_half);
}

int8_t v2_check_limits(const uint32_t rolling, const uint64_t fixed) {
    if ((rolling >> 28) != 0) return -1;
    if ((fixed >> 40) != 0) return -1;
    return 0;
}

void encode_v2_half(const uint32_t rolling, const uint32_t fixed, const uint16_t data,
                    const uint8_t frame_type, uint8_t* packet_half) {
    encode_v2_half_parts(rolling, fixed, data, frame_type, packet_half);

    packet_half[1] |= (packet_half[0] & 0x3) << 6;
    packet_half[0] >>= 2;
    packet_half[0] |= (frame_type << 6);
}

/* --- OOK framing ----------------------------------------------------------- */

/* 250 us per OOK symbol on air (upstream OOK_SAMPLERATE / 4000). */
constexpr float kSymbolRateHz = 4000.0f;
constexpr float kSampleRateHz = 500000.0f;
constexpr uint32_t kRepeats = 3;

void mc_append(std::string& out, uint32_t x, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        const bool bit = ((x >> (size - 1 - i)) & 1) != 0;
        out += bit ? "01" : "10";
    }
}

std::vector<uint8_t> pack_ook_bits(const std::string& s) {
    std::vector<uint8_t> bytes((s.size() + 7) / 8, 0u);
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] != '0')
            bytes[i >> 3] = static_cast<uint8_t>(bytes[i >> 3] | (0x80u >> (i & 7u)));
    return bytes;
}

}  // namespace

int8_t secplus_encode_v2(uint32_t rolling, uint64_t fixed, uint32_t data,
                         uint8_t frame_type, uint8_t* packet1, uint8_t* packet2) {
    const int8_t err = v2_check_limits(rolling, fixed);
    if (err < 0) return err;

    uint32_t rolling_halves[2];
    encode_v2_rolling(rolling, rolling_halves);
    v2_calc_parity(fixed, &data);

    encode_v2_half(rolling_halves[0], static_cast<uint32_t>(fixed >> 20),
                   static_cast<uint16_t>(data >> 16), frame_type, packet1);
    encode_v2_half(rolling_halves[1], static_cast<uint32_t>(fixed & 0xfffff),
                   static_cast<uint16_t>(data & 0xffff), frame_type, packet2);

    return 0;
}

std::string secplus_encode_packet(uint8_t indicator, const uint8_t* packet, bool has_data) {
    std::string out;
    mc_append(out, 0b0000000000000000'1111u, 20);  // preamble
    mc_append(out, indicator, 2);
    const uint8_t n = has_data ? 8 : 5;
    for (uint8_t b = 0; b < n; ++b) mc_append(out, packet[b], 8);
    out.append(24, '0');  // blank
    return out;
}

std::string secplus_encode_bitstream(uint32_t rolling, uint64_t fixed, uint32_t data,
                                     bool has_data) {
    uint8_t packet1[8]{};
    uint8_t packet2[8]{};
    if (secplus_encode_v2(rolling, fixed, data, has_data ? 1 : 0, packet1, packet2) < 0)
        return {};

    std::string out = secplus_encode_packet(0b00, packet1, has_data);
    out += secplus_encode_packet(0b01, packet2, has_data);
    return out;
}

/* --- View ------------------------------------------------------------------ */

SecplusTXView::SecplusTXView() {
    add_children({&labels_, &field_fixed_, &field_rolling_, &check_data_, &field_data_,
                  &field_freq_, &check_learn_, &console_, &button_tx_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_freq_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                              static_cast<uint64_t>(caps.tx_freq.max));
    }
    field_freq_.set_value(315'000'000, false);

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
    console_.writeln(STR_COLOR_YELLOW "Garage rolling-code spoof.");
    console_.writeln(STR_COLOR_YELLOW "TX is illegal in most places.");
    console_.writeln(STR_COLOR_LIGHT_GREY "RF needs a USRP B200.");
}

SecplusTXView::~SecplusTXView() {
    if (transmitting_) stop_tx();
}

void SecplusTXView::focus() {
    button_tx_.focus();
}

void SecplusTXView::start_tx() {
    auto* tx = globals().transmitter;
    if (!tx) {
        console_.writeln(STR_COLOR_RED "No transmitter wired.");
        return;
    }

    const uint32_t rolling = static_cast<uint32_t>(field_rolling_.to_integer());
    const uint64_t fixed = field_fixed_.to_integer();
    const uint32_t data = static_cast<uint32_t>(field_data_.to_integer());
    const bool has_data = check_data_.value();

    const std::string bitstream = secplus_encode_bitstream(rolling, fixed, data, has_data);
    if (bitstream.empty()) {
        console_.writeln(STR_COLOR_RED "Invalid fixed/rolling code.");
        return;
    }

    const auto bits = pack_ook_bits(bitstream);
    dsp::OokKeyer keyer;
    keyer.configure(kSampleRateHz, kSymbolRateHz);
    keyer.set_data(bits.data(), bitstream.size());
    keyer.set_repeat(kRepeats, 0);

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

void SecplusTXView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    transmitting_ = false;
    button_tx_.set_text("Start");

    /* Outside learn mode, advance the rolling code, as upstream does. */
    if (!check_learn_.value()) {
        const uint32_t rolling = (static_cast<uint32_t>(field_rolling_.to_integer()) + 1) & 0x0FFFFFFFu;
        field_rolling_.set_value(rolling);
    }
}

void SecplusTXView::on_frame_sync() {
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
const app::Registrar reg_secplustx{{
    "secplustx", "Security+", app::Category::Transmit,
    ui::Color::yellow(), nullptr,
    [] { return std::make_unique<app::SecplusTXView>(); }}};
}  // namespace
