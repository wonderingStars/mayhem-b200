/*
 * mayhem-b200 — Bluetooth Low Energy advertising transmitter.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2023 TJ Baginski (original app / baseband)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_ble_tx.hpp"

#include "../core/string_format.hpp"
#include "../dsp/modulate.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"

#include <cctype>
#include <cstring>

namespace app {
namespace ble_tx {

/* --- Encoder -------------------------------------------------------------- */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<uint8_t> hex_to_bits(std::string_view hex, bool flip, size_t octet_limit) {
    /* Keep only hex characters, the way convert_hex_to_bit counts them. */
    std::string clean;
    for (char c : hex)
        if (hex_nibble(c) >= 0) clean += c;

    const size_t num_hex = clean.size();
    if (num_hex < 2 || (num_hex % 2) != 0) return {};
    if (num_hex > octet_limit * 2) return {};

    if (flip) {
        /* Reverse octet order (upstream stream_flip). */
        std::string flipped;
        flipped.resize(num_hex);
        for (size_t i = 0; i < num_hex; i += 2) {
            flipped[num_hex - i - 2] = clean[i];
            flipped[num_hex - i - 1] = clean[i + 1];
        }
        clean.swap(flipped);
    }

    std::vector<uint8_t> bits;
    bits.reserve(num_hex * 4);
    for (size_t i = 0; i < num_hex; i += 2) {
        const int n = (hex_nibble(clean[i]) << 4) | hex_nibble(clean[i + 1]);
        for (int b = 0; b < 8; b++)
            bits.push_back(static_cast<uint8_t>((n >> b) & 1));  /* LSB first */
    }
    return bits;
}

std::array<uint8_t, 16> adv_pdu_header(PduType type, int txadd, int rxadd, int payload_len) {
    std::array<uint8_t, 16> h{};
    /* 4-bit PDU type, laid out bit0..bit3 as fill_adv_pdu_header does. */
    uint8_t t0 = 0, t1 = 0, t2 = 0, t3 = 0;
    switch (type) {
        case PduType::ADV_IND:         t3 = 0; t2 = 0; t1 = 0; t0 = 0; break;
        case PduType::ADV_DIRECT_IND:  t3 = 0; t2 = 0; t1 = 0; t0 = 1; break;
        case PduType::ADV_NONCONN_IND: t3 = 0; t2 = 0; t1 = 1; t0 = 0; break;
        case PduType::SCAN_REQ:        t3 = 0; t2 = 0; t1 = 1; t0 = 1; break;
        case PduType::SCAN_RSP:        t3 = 0; t2 = 1; t1 = 0; t0 = 0; rxadd = 1; break;
        case PduType::CONNECT_REQ:     t3 = 0; t2 = 1; t1 = 0; t0 = 1; break;
        case PduType::ADV_SCAN_IND:    t3 = 0; t2 = 1; t1 = 1; t0 = 0; break;
    }
    h[0] = t0;
    h[1] = t1;
    h[2] = t2;
    h[3] = t3;
    h[4] = 0;
    h[5] = 0;
    h[6] = static_cast<uint8_t>(txadd & 1);
    h[7] = static_cast<uint8_t>(rxadd & 1);
    for (int i = 0; i < 6; i++)
        h[8 + i] = static_cast<uint8_t>((payload_len >> i) & 1);
    h[14] = 0;
    h[15] = 0;
    return h;
}

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

std::vector<uint8_t> whitening_sequence(int channel, size_t nbits) {
    return whiten(std::vector<uint8_t>(nbits, 0), channel);
}

std::array<uint8_t, 24> crc24(const std::vector<uint8_t>& pdu_bits, uint32_t init) {
    uint8_t bs[24];
    /* Preset from `init`, three octets, each least-significant-bit first — the
     * same order convert_hex_to_bit produces from "555555". */
    for (int i = 0; i < 3; i++) {
        const uint8_t byte = static_cast<uint8_t>((init >> ((2 - i) * 8)) & 0xFF);
        for (int b = 0; b < 8; b++)
            bs[i * 8 + b] = static_cast<uint8_t>((byte >> b) & 1);
    }

    for (uint8_t in : pdu_bits) {
        const uint8_t nb = static_cast<uint8_t>((bs[23] ^ in) & 1);
        uint8_t u[24];
        u[0] = nb;
        u[1] = static_cast<uint8_t>((bs[0] ^ nb) & 1);
        u[2] = bs[1];
        u[3] = static_cast<uint8_t>((bs[2] ^ nb) & 1);
        u[4] = static_cast<uint8_t>((bs[3] ^ nb) & 1);
        u[5] = bs[4];
        u[6] = static_cast<uint8_t>((bs[5] ^ nb) & 1);
        u[7] = bs[6];
        u[8] = bs[7];
        u[9] = static_cast<uint8_t>((bs[8] ^ nb) & 1);
        u[10] = static_cast<uint8_t>((bs[9] ^ nb) & 1);
        for (int k = 11; k < 24; k++) u[k] = bs[k - 1];
        std::memcpy(bs, u, 24);
    }

    std::array<uint8_t, 24> result{};
    for (int i = 0; i < 24; i++) result[i] = bs[23 - i];
    return result;
}

std::vector<uint8_t> build_info_bits(const AdvPacket& pkt) {
    auto preamble = hex_to_bits(kPreambleHex, false, 1);
    auto aa = hex_to_bits(kAccessAddressHex, false, 4);
    auto mac = hex_to_bits(pkt.mac, true, 6);
    /* AdvData may be empty; upstream allows a header-only PDU only when AdvA is
     * present, so require the MAC and at least accept empty data. */
    std::vector<uint8_t> adv;
    if (!pkt.adv_data.empty())
        adv = hex_to_bits(pkt.adv_data, false, 31);

    if (preamble.size() != 8 || aa.size() != 32 || mac.size() != 48) return {};
    if (!pkt.adv_data.empty() && adv.empty()) return {};  /* invalid hex */

    /* payload_len is the PDU payload octet count: AdvA + AdvData. */
    const int payload_len = static_cast<int>((mac.size() + adv.size()) / 8);
    auto header = adv_pdu_header(pkt.type, pkt.txadd, pkt.rxadd, payload_len);

    std::vector<uint8_t> info;
    info.reserve(preamble.size() + aa.size() + 16 + mac.size() + adv.size());
    info.insert(info.end(), preamble.begin(), preamble.end());
    info.insert(info.end(), aa.begin(), aa.end());
    info.insert(info.end(), header.begin(), header.end());
    info.insert(info.end(), mac.begin(), mac.end());
    info.insert(info.end(), adv.begin(), adv.end());
    return info;
}

std::vector<uint8_t> build_phy_bits(const AdvPacket& pkt) {
    auto info = build_info_bits(pkt);
    if (info.empty()) return {};

    /* PDU = everything after preamble (8) + access address (32) = bit 40 on. */
    const size_t pdu_start = 40;
    std::vector<uint8_t> pdu(info.begin() + pdu_start, info.end());

    auto crc = crc24(pdu, kCrcInit);

    std::vector<uint8_t> pdu_and_crc = pdu;
    pdu_and_crc.insert(pdu_and_crc.end(), crc.begin(), crc.end());

    auto whitened = whiten(pdu_and_crc, pkt.channel);

    std::vector<uint8_t> phy;
    phy.reserve(pdu_start + whitened.size());
    phy.insert(phy.end(), info.begin(), info.begin() + pdu_start);  /* preamble + AA */
    phy.insert(phy.end(), whitened.begin(), whitened.end());
    return phy;
}

uint64_t channel_frequency(int channel) {
    switch (channel) {
        case 37: return 2'402'000'000ULL;
        case 38: return 2'426'000'000ULL;
        case 39: return 2'480'000'000ULL;
        default: break;
    }
    if (channel >= 0 && channel <= 10) return 2'404'000'000ULL + channel * 2'000'000ULL;
    if (channel >= 11 && channel <= 36) return 2'428'000'000ULL + (channel - 11) * 2'000'000ULL;
    return 2'402'000'000ULL;
}

/* --- View ----------------------------------------------------------------- */

static std::vector<uint8_t> pack_bits_msb(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> bytes((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); i++)
        if (bits[i] & 1) bytes[i >> 3] |= static_cast<uint8_t>(0x80u >> (i & 7));
    return bytes;
}

BleTxView::BleTxView()
    : transmitter_{globals().transmitter} {
    add_children({&labels_, &field_frequency_, &options_channel_, &options_type_,
                  &field_mac_, &field_data_, &field_gain_, &check_loop_, &button_tx_,
                  &text_status_, &console_, &notes_});

    console_.enable_scrolling(true);

    field_frequency_.set_step_index(5);  /* 1 MHz — BLE channel spacing */
    field_frequency_.set_value(channel_frequency(37), false);

    options_channel_.set_by_value(37, false);
    options_channel_.on_change = [this](size_t, int32_t v) {
        packet_.channel = static_cast<int>(v);
        field_frequency_.set_value(channel_frequency(packet_.channel), true);
        update_preview();
    };

    options_type_.set_by_value(0, false);
    options_type_.on_change = [this](size_t, int32_t v) {
        packet_.type = static_cast<PduType>(v);
        update_preview();
    };

    field_mac_.set_value(std::string_view{packet_.mac});
    field_mac_.on_change = [this](ui::SymField&) {
        packet_.mac = field_mac_.to_string();
        update_preview();
    };

    field_data_.set_value(std::string_view{packet_.adv_data});
    field_data_.on_change = [this](ui::SymField&) {
        packet_.adv_data = field_data_.to_string();
        update_preview();
    };

    field_gain_.set_value(30);

    button_tx_.on_select = [this](ui::Button&) {
        if (transmitting_)
            stop_tx();
        else
            start_tx();
    };

    update_preview();
}

BleTxView::~BleTxView() {
    stop_tx();
}

void BleTxView::on_show() {
    ui::View::on_show();
    button_tx_.focus();

    std::string range = "TX range: unknown (no radio)";
    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        if (caps.tx_freq.max > caps.tx_freq.min) {
            range = "TX " + to_string_short_freq(static_cast<uint64_t>(caps.tx_freq.min)) +
                    " - " + to_string_short_freq(static_cast<uint64_t>(caps.tx_freq.max));
        }
    }
    console_.writeln(range);
}

void BleTxView::on_hide() {
    stop_tx();
    ui::View::on_hide();
}

void BleTxView::build_packet() {
    packet_.mac = field_mac_.to_string();
    packet_.adv_data = field_data_.to_string();
    packet_.channel = static_cast<int>(options_channel_.selected_index_value());
    packet_.type = static_cast<PduType>(options_type_.selected_index_value());

    auto phy = build_phy_bits(packet_);
    burst_.clear();
    if (phy.empty()) return;

    dsp::FskKeyer keyer;
    keyer.configure(static_cast<float>(kSampleRate), kSymbolRate, kDeviation);
    keyer.set_gaussian(kGaussianBt, 4);
    keyer.set_repeat(1, 0);
    const auto packed = pack_bits_msb(phy);
    keyer.set_data(packed.data(), phy.size());

    std::complex<float> buf[512];
    for (int guard = 0; guard < 100000; guard++) {
        const size_t n = keyer.process(buf, 512);
        if (n == 0) break;
        burst_.insert(burst_.end(), buf, buf + n);
    }

    /* ~1 ms of inter-packet silence. */
    burst_.insert(burst_.end(), static_cast<size_t>(kSampleRate / 1000.0),
                  std::complex<float>{0.0f, 0.0f});
}

void BleTxView::update_preview() {
    auto info = build_info_bits(packet_);
    if (info.empty()) {
        text_status_.set("Invalid MAC/Data hex");
        return;
    }
    auto phy = build_phy_bits(packet_);
    const size_t pdu_bits = info.size() - 40;  /* header + AdvA + AdvData */
    text_status_.set("PDU " + to_string_dec_uint(pdu_bits / 8) + "B  phy " +
                     to_string_dec_uint(phy.size() / 8) + "B");
}

void BleTxView::start_tx() {
    if (!transmitter_) {
        console_.writeln("No transmitter (needs B200).");
        return;
    }
    build_packet();
    if (burst_.empty()) {
        text_status_.set("Invalid MAC/Data hex");
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

    /* Prime the ring before the radio starts pulling. */
    ring_.write(burst_.data(), burst_.size());

    if (!transmitter_->start()) {
        console_.writeln("TX start failed (needs B200).");
        transmitter_->set_iq_source(nullptr);
        return;
    }

    transmitting_ = true;
    button_tx_.set_text("Stop");
    text_status_.set(check_loop_.value() ? "Transmitting (loop)" : "Transmitting");
    console_.writeln("TX ch " + to_string_dec_uint(packet_.channel));
}

void BleTxView::stop_tx() {
    if (transmitter_) {
        transmitter_->stop();
        transmitter_->set_iq_source(nullptr);
    }
    ring_.clear();
    transmitting_ = false;
    button_tx_.set_text("TX");
    text_status_.set("Idle");
}

void BleTxView::on_frame_sync() {
    ui::View::on_frame_sync();
    if (!transmitting_ || burst_.empty()) return;

    /* Keep the ring topped up while transmitting: loop keeps refilling; a
     * single shot stops once the last burst has drained. */
    if (ring_.space() >= burst_.size()) {
        if (check_loop_.value()) {
            ring_.write(burst_.data(), burst_.size());
        } else if (ring_.empty()) {
            stop_tx();
        }
    }
}

}  // namespace ble_tx
}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_ble_tx{{"bletx", "BLE TX", app::Category::Transmit,
                                 ui::Color::orange(), &ui::bitmap_icon_btle,
                                 [] { return std::make_unique<app::ble_tx::BleTxView>(); }}};
}  // namespace
