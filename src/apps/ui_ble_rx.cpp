/*
 * mayhem-b200 — Bluetooth Low Energy advertising receiver.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2020 Shao
 * Copyright (C) 2023 TJ Baginski (original app / baseband)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_ble_rx.hpp"

#include "../core/string_format.hpp"
#include "../radio/receiver_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_painter.hpp"

#include <cmath>
#include <cstring>
#include <utility>

namespace app {
namespace ble_rx {

/* --- CRC (proc_btlerx table engine) --------------------------------------- */

static const uint32_t kCrcTable[256] = {
    0x000000, 0x01b4c0, 0x036980, 0x02dd40, 0x06d300, 0x0767c0, 0x05ba80, 0x040e40,
    0x0da600, 0x0c12c0, 0x0ecf80, 0x0f7b40, 0x0b7500, 0x0ac1c0, 0x081c80, 0x09a840,
    0x1b4c00, 0x1af8c0, 0x182580, 0x199140, 0x1d9f00, 0x1c2bc0, 0x1ef680, 0x1f4240,
    0x16ea00, 0x175ec0, 0x158380, 0x143740, 0x103900, 0x118dc0, 0x135080, 0x12e440,
    0x369800, 0x372cc0, 0x35f180, 0x344540, 0x304b00, 0x31ffc0, 0x332280, 0x329640,
    0x3b3e00, 0x3a8ac0, 0x385780, 0x39e340, 0x3ded00, 0x3c59c0, 0x3e8480, 0x3f3040,
    0x2dd400, 0x2c60c0, 0x2ebd80, 0x2f0940, 0x2b0700, 0x2ab3c0, 0x286e80, 0x29da40,
    0x207200, 0x21c6c0, 0x231b80, 0x22af40, 0x26a100, 0x2715c0, 0x25c880, 0x247c40,
    0x6d3000, 0x6c84c0, 0x6e5980, 0x6fed40, 0x6be300, 0x6a57c0, 0x688a80, 0x693e40,
    0x609600, 0x6122c0, 0x63ff80, 0x624b40, 0x664500, 0x67f1c0, 0x652c80, 0x649840,
    0x767c00, 0x77c8c0, 0x751580, 0x74a140, 0x70af00, 0x711bc0, 0x73c680, 0x727240,
    0x7bda00, 0x7a6ec0, 0x78b380, 0x790740, 0x7d0900, 0x7cbdc0, 0x7e6080, 0x7fd440,
    0x5ba800, 0x5a1cc0, 0x58c180, 0x597540, 0x5d7b00, 0x5ccfc0, 0x5e1280, 0x5fa640,
    0x560e00, 0x57bac0, 0x556780, 0x54d340, 0x50dd00, 0x5169c0, 0x53b480, 0x520040,
    0x40e400, 0x4150c0, 0x438d80, 0x423940, 0x463700, 0x4783c0, 0x455e80, 0x44ea40,
    0x4d4200, 0x4cf6c0, 0x4e2b80, 0x4f9f40, 0x4b9100, 0x4a25c0, 0x48f880, 0x494c40,
    0xda6000, 0xdbd4c0, 0xd90980, 0xd8bd40, 0xdcb300, 0xdd07c0, 0xdfda80, 0xde6e40,
    0xd7c600, 0xd672c0, 0xd4af80, 0xd51b40, 0xd11500, 0xd0a1c0, 0xd27c80, 0xd3c840,
    0xc12c00, 0xc098c0, 0xc24580, 0xc3f140, 0xc7ff00, 0xc64bc0, 0xc49680, 0xc52240,
    0xcc8a00, 0xcd3ec0, 0xcfe380, 0xce5740, 0xca5900, 0xcbedc0, 0xc93080, 0xc88440,
    0xecf800, 0xed4cc0, 0xef9180, 0xee2540, 0xea2b00, 0xeb9fc0, 0xe94280, 0xe8f640,
    0xe15e00, 0xe0eac0, 0xe23780, 0xe38340, 0xe78d00, 0xe639c0, 0xe4e480, 0xe55040,
    0xf7b400, 0xf600c0, 0xf4dd80, 0xf56940, 0xf16700, 0xf0d3c0, 0xf20e80, 0xf3ba40,
    0xfa1200, 0xfba6c0, 0xf97b80, 0xf8cf40, 0xfcc100, 0xfd75c0, 0xffa880, 0xfe1c40,
    0xb75000, 0xb6e4c0, 0xb43980, 0xb58d40, 0xb18300, 0xb037c0, 0xb2ea80, 0xb35e40,
    0xbaf600, 0xbb42c0, 0xb99f80, 0xb82b40, 0xbc2500, 0xbd91c0, 0xbf4c80, 0xbef840,
    0xac1c00, 0xada8c0, 0xaf7580, 0xaec140, 0xaacf00, 0xab7bc0, 0xa9a680, 0xa81240,
    0xa1ba00, 0xa00ec0, 0xa2d380, 0xa36740, 0xa76900, 0xa6ddc0, 0xa40080, 0xa5b440,
    0x81c800, 0x807cc0, 0x82a180, 0x831540, 0x871b00, 0x86afc0, 0x847280, 0x85c640,
    0x8c6e00, 0x8ddac0, 0x8f0780, 0x8eb340, 0x8abd00, 0x8b09c0, 0x89d480, 0x886040,
    0x9a8400, 0x9b30c0, 0x99ed80, 0x985940, 0x9c5700, 0x9de3c0, 0x9f3e80, 0x9e8a40,
    0x972200, 0x9696c0, 0x944b80, 0x95ff40, 0x91f100, 0x9045c0, 0x929880, 0x932c40};

uint32_t crc24(const uint8_t* data, size_t length, uint32_t init) {
    uint32_t crc = init;
    for (size_t i = 0; i < length; i++) {
        const uint32_t idx = (crc ^ data[i]) & 0xFF;
        crc = (kCrcTable[idx] ^ (crc >> 8)) & 0xFFFFFF;
    }
    return crc & 0xFFFFFF;
}

uint32_t crc_init_reorder(uint32_t crc_init) {
    uint32_t tmp = crc_init;
    uint32_t input = tmp & 0xFF;
    tmp >>= 8;
    input = (input << 8) | (tmp & 0xFF);
    tmp >>= 8;
    input = (input << 8) | (tmp & 0xFF);
    input <<= 1;
    uint32_t out = 0;
    for (int i = 0; i < 24; i++) {
        input >>= 1;
        out = (out << 1) | (input & 1);
    }
    return out;
}

/* --- Whitening (proc_ble_tx scramble, self-inverse) ----------------------- */

std::vector<uint8_t> whiten(const std::vector<uint8_t>& bits, int channel) {
    uint8_t bs[7];
    bs[0] = 1;
    bs[1] = static_cast<uint8_t>((channel >> 5) & 1);
    bs[2] = static_cast<uint8_t>((channel >> 4) & 1);
    bs[3] = static_cast<uint8_t>((channel >> 3) & 1);
    bs[4] = static_cast<uint8_t>((channel >> 2) & 1);
    bs[5] = static_cast<uint8_t>((channel >> 1) & 1);
    bs[6] = static_cast<uint8_t>((channel >> 0) & 1);

    std::vector<uint8_t> out(bits.size());
    for (size_t i = 0; i < bits.size(); i++) {
        out[i] = static_cast<uint8_t>((bs[6] ^ bits[i]) & 1);
        uint8_t u[7];
        u[0] = bs[6];
        u[1] = bs[0];
        u[2] = bs[1];
        u[3] = bs[2];
        u[4] = static_cast<uint8_t>((bs[3] ^ bs[6]) & 1);
        u[5] = bs[4];
        u[6] = bs[5];
        std::memcpy(bs, u, 7);
    }
    return out;
}

/* --- Packet --------------------------------------------------------------- */

uint64_t Packet::mac_key() const {
    uint64_t k = 0;
    for (size_t i = 0; i < 6; i++) k = (k << 8) | mac[i];
    return k;
}

std::string Packet::mac_string() const {
    std::string s;
    for (size_t i = 0; i < 6; i++) {
        if (i) s += ':';
        s += to_string_hex(mac[i], 2);
    }
    return s;
}

std::string Packet::data_string() const {
    std::string s;
    for (size_t i = 0; i < data.size(); i++) {
        if (i) s += ' ';
        s += to_string_hex(data[i], 2);
    }
    return s;
}

const char* Packet::type_string() const {
    switch (type) {
        case 0: return "ADV_IND";
        case 1: return "ADV_DIRECT";
        case 2: return "ADV_NONCONN";
        case 3: return "SCAN_REQ";
        case 4: return "SCAN_RSP";
        case 5: return "CONNECT_REQ";
        case 6: return "ADV_SCAN";
        default: return "?";
    }
}

/* --- Decoder -------------------------------------------------------------- */

static float phase_diff(const dsp::cfloat& a, const dsp::cfloat& b) {
    const float dI = b.real() * a.real() + b.imag() * a.imag();
    const float dQ = b.imag() * a.real() - b.real() * a.imag();
    return std::atan2(dQ, dI);
}

void Decoder::configure(size_t samples_per_symbol, int channel) {
    sps_ = (samples_per_symbol > 0) ? samples_per_symbol : 1;
    channel_ = channel;
    reset();
}

float Decoder::symbol_sum(const std::vector<float>& diffs, size_t pos) const {
    float sum = 0.0f;
    for (size_t k = 0; k < sps_; k++) sum += diffs[pos + k];
    return sum;
}

bool Decoder::try_parse(const std::vector<float>& diffs, size_t header_start) {
    const size_t max_bits = (kHeaderBytes + kMaxPayloadBytes + kCrcBytes) * 8;

    std::vector<uint8_t> raw;
    raw.reserve(max_bits);
    for (size_t s = 0; s < max_bits; s++) {
        const size_t pos = header_start + s * sps_;
        if (pos + sps_ > diffs.size()) break;
        raw.push_back(symbol_sum(diffs, pos) > 0.0f ? uint8_t{1} : uint8_t{0});
    }
    if (raw.size() < kHeaderBytes * 8 + kCrcBytes * 8) return false;

    auto dewhitened = whiten(raw, channel_);

    /* Pack least-significant-bit first into bytes. */
    std::vector<uint8_t> bytes(dewhitened.size() / 8, 0);
    for (size_t i = 0; i < bytes.size() * 8; i++)
        if (dewhitened[i] & 1) bytes[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));

    if (bytes.size() < kHeaderBytes) return false;
    const uint8_t type = static_cast<uint8_t>(bytes[0] & 0x0F);
    const uint8_t payload_len = static_cast<uint8_t>(bytes[1] & 0x3F);

    if (type >= 7) return false;                          /* reserved PDU type */
    if (payload_len < 6 || payload_len > kMaxPayloadBytes) return false;

    const size_t body_len = static_cast<size_t>(payload_len) + kHeaderBytes;
    if (bytes.size() < body_len + kCrcBytes) return false;

    const uint32_t computed = crc24(bytes.data(), body_len, crc_init_reorder(kCrcInit));
    const uint32_t received = static_cast<uint32_t>(bytes[body_len]) |
                              (static_cast<uint32_t>(bytes[body_len + 1]) << 8) |
                              (static_cast<uint32_t>(bytes[body_len + 2]) << 16);
    if (computed != received) return false;

    Packet pkt{};
    pkt.type = type;
    pkt.payload_len = payload_len;
    for (size_t i = 0; i < 6; i++) pkt.mac[i] = bytes[7 - i];  /* rb_buf[7..2] */
    const size_t data_len = static_cast<size_t>(payload_len) - 6;
    pkt.data.assign(bytes.begin() + 8, bytes.begin() + 8 + data_len);
    pkt.crc = received;
    pkt.computed_crc = computed;
    pkt.channel = channel_;

    packets_decoded_++;
    if (on_packet_) on_packet_(pkt);
    return true;
}

void Decoder::process(const dsp::cfloat* in, size_t count) {
    if (count < 2) return;

    std::vector<float> diffs(count - 1);
    for (size_t i = 0; i + 1 < count; i++) diffs[i] = phase_diff(in[i], in[i + 1]);

    /* Try every symbol phase so the burst need not be symbol-aligned. On a
     * cleanly oversampled signal several adjacent phases decode the same access
     * address, so a success is suppressed when it lands within one byte of an
     * earlier one — that is the same physical packet, not a new one. */
    std::vector<size_t> emitted;
    for (size_t phase = 0; phase < sps_; phase++) {
        uint32_t reg = 0;
        size_t symbols = 0;
        for (size_t pos = phase; pos + sps_ <= diffs.size(); pos += sps_) {
            const uint8_t bit = symbol_sum(diffs, pos) > 0.0f ? 1u : 0u;
            reg = (reg >> 1) | (static_cast<uint32_t>(bit) << 31);
            symbols++;
            if (symbols >= 32 && reg == kAccessAddress) {
                const size_t header_start = pos + sps_;
                bool duplicate = false;
                for (size_t h : emitted) {
                    const size_t d = (header_start > h) ? header_start - h : h - header_start;
                    if (d < sps_ * 8) { duplicate = true; break; }
                }
                if (!duplicate && try_parse(diffs, header_start))
                    emitted.push_back(header_start);
            }
        }
    }
}

/* --- View ----------------------------------------------------------------- */

BleRxView::BleRxView()
    : receiver_{*globals().receiver} {
    add_children({&labels_, &field_frequency_, &options_channel_, &button_startstop_,
                  &text_status_, &text_range_, &table_, &notes_});

    table_.set_parent_rect({0, 80, 240, 184});
    table_.on_draw = [](const BleRecentEntry& entry, const ui::Rect& target_rect,
                        ui::Painter& painter, const ui::Style& style,
                        ui::RecentEntriesColumns&) {
        std::string mac;
        for (int i = 5; i >= 0; i--) {
            mac += to_string_hex(static_cast<uint8_t>((entry.mac >> (i * 8)) & 0xFF), 2);
            if (i) mac += ':';
        }
        std::string line = mac + " " + to_string_dec_uint(entry.type, 1) + " " + entry.data_hex;
        const size_t width = static_cast<size_t>(target_rect.width() / 8);
        line.resize(width, ' ');
        painter.draw_string(target_rect.location(), style, line);
    };

    options_channel_.set_by_value(37, false);
    options_channel_.on_change = [this](size_t, int32_t v) {
        channel_ = static_cast<int>(v);
        field_frequency_.set_value(
            v == 37 ? 2'402'000'000ULL : (v == 38 ? 2'426'000'000ULL : 2'480'000'000ULL), true);
        if (running_) rebuild_front_end();
    };

    button_startstop_.on_select = [this](ui::Button&) {
        if (running_) {
            running_ = false;
            button_startstop_.set_text("Start");
        } else {
            receiver_.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
            if (!receiver_.running()) receiver_.start();
            rebuild_front_end();
            running_ = true;
            button_startstop_.set_text("Stop");
        }
    };

    field_frequency_.set_step_index(5);  /* 1 MHz */
    field_frequency_.set_value(2'402'000'000ULL, false);
    field_frequency_.on_change = [this](uint64_t hz) { receiver_.set_target_frequency(hz); };
}

BleRxView::~BleRxView() = default;

void BleRxView::on_show() {
    ui::View::on_show();
    button_startstop_.focus();

    std::string range = "RX range: unknown (no radio)";
    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        if (caps.rx_freq.max > caps.rx_freq.min) {
            range = "RX " + to_string_short_freq(static_cast<uint64_t>(caps.rx_freq.min)) +
                    " - " + to_string_short_freq(static_cast<uint64_t>(caps.rx_freq.max));
        }
    }
    text_range_.set(range);
}

void BleRxView::on_hide() {
    running_ = false;
    button_startstop_.set_text("Start");
    ui::View::on_hide();
}

void BleRxView::rebuild_front_end() {
    const double input_rate = receiver_.sampling_rate();
    if (input_rate <= 0.0) return;

    /* BLE advertising: 1 Msym/s GFSK, ±250 kHz. Decode at 2 samples/symbol. */
    const double bit_rate = 1'000'000.0;
    const double wanted = bit_rate * 2.0;
    size_t decim = static_cast<size_t>(std::floor(input_rate / wanted));
    if (decim < 1) decim = 1;
    decimation_ = decim;
    const double demod_rate = input_rate / static_cast<double>(decim);
    samples_per_symbol_ = static_cast<size_t>(std::lround(demod_rate / bit_rate));
    if (samples_per_symbol_ < 1) samples_per_symbol_ = 1;

    const double deviation = 250'000.0;
    const double channel_bw = 2.0 * deviation + bit_rate;  /* ~1.5 MHz */
    channel_filter_.configure(
        dsp::design_lowpass(channel_bw, channel_bw * 0.5, input_rate, 50.0), decim);

    decoder_.configure(samples_per_symbol_, channel_);

    text_status_.set("sps " + to_string_dec_uint(samples_per_symbol_) + "  rate " +
                     to_string_dec_uint(static_cast<uint64_t>(demod_rate / 1000.0)) + " kHz");
    configured_input_rate_ = input_rate;
}

void BleRxView::on_packet(const Packet& packet) {
    packets_++;
    auto& entry = ui::on_packet(entries_, packet.mac_key(), 32);
    entry.count++;
    entry.type = packet.type;
    entry.payload_len = packet.payload_len;
    entry.data_hex = packet.data_string();
    table_.set_dirty();
}

void BleRxView::on_frame_sync() {
    ui::View::on_frame_sync();
    if (!running_) return;
    if (receiver_.sampling_rate() != configured_input_rate_) rebuild_front_end();

    /* SAMPLE TAP — see the header note: the wideband spectrum tap is not
     * contiguous, so a burst straddling a block is lost. The decode pipeline is
     * proved on synthesised GFSK in tests/test_ble_rx.cpp. */
    if (!receiver_.take_spectrum_samples(samples_, 4096)) return;
    if (samples_.empty()) return;

    channel_filter_.process(samples_.data(), samples_.size(), chan_out_);
    if (chan_out_.empty()) return;

    decoder_.set_channel(channel_);
    decoder_.set_on_packet([this](const Packet& p) { this->on_packet(p); });
    decoder_.process(chan_out_.data(), chan_out_.size());
}

}  // namespace ble_rx
}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_ble_rx{{"blerx", "BLE RX", app::Category::Receive,
                                 ui::Color::green(), &ui::bitmap_icon_btle,
                                 [] { return std::make_unique<app::ble_rx::BleRxView>(); }}};
}  // namespace
