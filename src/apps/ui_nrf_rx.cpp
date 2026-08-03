/*
 * mayhem-b200 — Nordic nRF24L01+ (Enhanced ShockBurst) receiver.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2020 Shao (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_nrf_rx.hpp"

#include "../core/string_format.hpp"
#include "../dsp/protocol.hpp"
#include "../radio/receiver_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_painter.hpp"

#include <cmath>
#include <utility>

namespace app {
namespace nrf {

/* --- CRC ------------------------------------------------------------------ */

uint16_t crc16(const uint8_t* data, const size_t length, const uint16_t init) {
    dsp::Crc<16> crc{0x1021, init, 0x0000};
    crc.process_bytes(data, length);
    return static_cast<uint16_t>(crc.checksum());
}

uint16_t esb_crc(const uint64_t address, const uint16_t pcf9,
                 const uint8_t* payload, const size_t length) {
    dsp::Crc<16> crc{0x1021, 0xFFFF, 0x0000};
    crc.process_bits(static_cast<uint32_t>((address >> 8) & 0xFFFFFFFFULL), 32);
    crc.process_bits(static_cast<uint32_t>(address & 0xFFULL), 8);
    crc.process_bits(static_cast<uint32_t>(pcf9 & 0x1FF), 9);
    if (payload != nullptr) {
        for (size_t i = 0; i < length; i++) crc.process_byte(payload[i]);
    }
    return static_cast<uint16_t>(crc.checksum());
}

std::string Packet::address_string() const {
    std::string s;
    for (size_t i = 0; i < kAddressBytes; i++) {
        const auto byte = static_cast<uint8_t>((address >> ((kAddressBytes - 1 - i) * 8)) & 0xFF);
        s += to_string_hex(byte, 2);
    }
    return s;
}

std::string Packet::payload_string() const {
    std::string s;
    for (size_t i = 0; i < payload_length && i < kMaxPayloadBytes; i++) {
        if (i) s += ' ';
        s += to_string_hex(payload[i], 2);
    }
    return s;
}

/* --- Decoder -------------------------------------------------------------- */

void Decoder::configure(const size_t samples_per_bit, const float dc_limit) {
    samples_per_bit_ = (samples_per_bit > 0) ? samples_per_bit : 1;
    dc_limit_ = dc_limit;
    /* Big enough that a maximum-length frame fits inside one window. */
    ring_.assign(kMaxFrameBits * samples_per_bit_ + 1, 0.0f);
    reset();
}

void Decoder::reset() {
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    head_ = 0;
    filled_ = 0;
    /* Upstream primes skipSamples to the ring length so nothing is decoded
     * until the window holds real data. */
    skip_samples_ = static_cast<int>(ring_.size());
    last_threshold_ = 0.0f;
    packets_decoded_ = 0;
}

float Decoder::window_sample(const size_t offset) const {
    /* The window starts at the oldest sample, one past the newest. */
    const size_t n = ring_.size();
    return ring_[(head_ + 1 + offset) % n];
}

float Decoder::bit_sample(const size_t k) const {
    return window_sample(k * samples_per_bit_);
}

void Decoder::process(const float* samples, const size_t count) {
    for (size_t i = 0; i < count; i++) process_sample(samples[i]);
}

void Decoder::process_sample(const float sample) {
    if (ring_.empty()) return;

    head_ = (head_ + 1) % ring_.size();
    ring_[head_] = sample;
    if (filled_ < ring_.size()) filled_++;

    if (skip_samples_ > 0) {
        skip_samples_--;
        return;
    }
    attempt_decode();
}

void Decoder::attempt_decode() {
    const size_t sps = samples_per_bit_;

    /* Slicing threshold: the mean of the first eight bit periods, which for a
     * valid frame is the alternating preamble and therefore its midpoint. */
    float sum = 0.0f;
    const size_t threshold_samples = 8 * sps;
    for (size_t c = 0; c < threshold_samples; c++) sum += window_sample(c);
    const float threshold = sum / static_cast<float>(threshold_samples);
    last_threshold_ = threshold;

    if (!(std::fabs(threshold) < dc_limit_)) return;

    /* The preamble alternates, so exactly four of the eight bit-to-bit steps
     * fall in the direction set by the first address bit. */
    int transitions = 0;
    if (bit_sample(9) > threshold) {
        for (size_t c = 0; c < 8; c++)
            if (bit_sample(c) > bit_sample(c + 1)) transitions++;
    } else {
        for (size_t c = 0; c < 8; c++)
            if (bit_sample(c) < bit_sample(c + 1)) transitions++;
    }
    if (transitions != 4) return;

    const auto read_byte = [this](const size_t first_bit) {
        uint8_t byte = 0;
        for (size_t c = 0; c < 8; c++) {
            const bool bit = bit_sample(first_bit + c) > last_threshold_;
            byte = static_cast<uint8_t>(byte | (bit ? (1u << (7 - c)) : 0u));
        }
        return byte;
    };

    Packet packet{};

    /* Address: bits 8..47, first on-air byte is the most significant. */
    for (size_t t = 0; t < kAddressBytes; t++) {
        const uint8_t byte = read_byte(kPreambleBits + t * 8);
        packet.address |= static_cast<uint64_t>(byte) << ((kAddressBytes - 1 - t) * 8);
    }

    /* Packet control field: 9 bits starting at bit 48, taken as the top 9 of a
     * 16-bit read exactly as upstream does. */
    const uint16_t pcf16 = static_cast<uint16_t>((read_byte(48) << 8) | read_byte(56));
    const uint16_t pcf = static_cast<uint16_t>(pcf16 >> 7);
    packet.pcf = pcf;
    packet.payload_length = static_cast<uint8_t>(pcf >> 3);
    packet.pid = static_cast<uint8_t>((pcf >> 1) & 0x3);
    packet.no_ack = (pcf & 0x1) != 0;

    /* Upstream sets a "not detected" flag here but carries on reading, which
     * overruns its 50-byte CRC scratch for any length above 43. Bail out. */
    if (packet.payload_length > kMaxPayloadBytes) return;

    const size_t length = packet.payload_length;
    for (size_t t = 0; t < length; t++) {
        packet.payload[t] = read_byte(57 + t * 8);
    }

    /* CRC over the 49-bit header left-padded to seven bytes, then the payload;
     * init 0x3C18 absorbs the seven pad bits. */
    uint8_t packed[7 + kMaxPayloadBytes]{};
    uint64_t header = packet.address;
    header <<= kPcfBits;
    header |= pcf;
    for (size_t c = 0; c < 7; c++) {
        packed[c] = static_cast<uint8_t>((header >> ((6 - c) * 8)) & 0xFF);
    }
    for (size_t c = 0; c < length; c++) packed[c + 7] = packet.payload[c];
    packet.computed_crc = crc16(packed, 7 + length);

    /* Received CRC: the 16 bits after the payload. */
    const size_t crc_first_bit = kPreambleBits + kAddressBits + kPcfBits + length * 8;
    packet.crc = static_cast<uint16_t>((read_byte(crc_first_bit) << 8) |
                                       read_byte(crc_first_bit + 8));

    if (packet.crc != packet.computed_crc) return;

    packets_decoded_++;
    if (on_packet_) on_packet_(packet);

    /* Upstream's guard against re-detecting the same frame at the neighbouring
     * sample alignments. */
    skip_samples_ = 20;
}

}  // namespace nrf

/* --- View ----------------------------------------------------------------- */

NrfRxView::NrfRxView()
    : receiver_{*globals().receiver} {
    add_children({&labels_,
                  &field_frequency_,
                  &step_view_,
                  &options_bitrate_,
                  &button_startstop_,
                  &text_status_,
                  &text_range_,
                  &table_,
                  &notes_});

    table_.set_parent_rect({0, 80, 240, 164});
    table_.on_draw = [](const NrfRecentEntry& entry, const ui::Rect& target_rect,
                        ui::Painter& painter, const ui::Style& style,
                        ui::RecentEntriesColumns&) {
        std::string line = to_string_hex(entry.address, 10) + " " +
                           to_string_dec_uint(entry.payload_length, 3) + " " +
                           entry.payload_hex;
        const size_t width = static_cast<size_t>(target_rect.width() / 8);
        line.resize(width, ' ');
        painter.draw_string(target_rect.location(), style, line);
    };

    options_bitrate_.set_by_value(static_cast<int32_t>(bitrate_kbps_), false);
    options_bitrate_.on_change = [this](size_t, int32_t v) {
        bitrate_kbps_ = static_cast<uint32_t>(v);
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

    /* nRF24 channels are 1 MHz apart from 2400 MHz. */
    field_frequency_.set_step_index(5);
    field_frequency_.set_value(2'400'000'000ULL, false);
    field_frequency_.on_change = [this](uint64_t hz) { receiver_.set_target_frequency(hz); };
}

NrfRxView::~NrfRxView() = default;

void NrfRxView::on_show() {
    ui::View::on_show();
    button_startstop_.focus();

    /* Report the tuning range the device actually advertises rather than a
     * hard-coded figure. 2.4 GHz is inside a B200's range, not at its edge. */
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

void NrfRxView::on_hide() {
    running_ = false;
    button_startstop_.set_text("Start");
    ui::View::on_hide();
}

void NrfRxView::rebuild_front_end() {
    const double input_rate = receiver_.sampling_rate();
    if (input_rate <= 0.0) return;

    /* proc_nrfrx runs its correlator at four samples per bit. Pick the integer
     * decimation that gets closest to bitrate * 4 without going below it. */
    const double bit_rate = static_cast<double>(bitrate_kbps_) * 1000.0;
    const double wanted = bit_rate * 4.0;
    size_t decim = static_cast<size_t>(std::floor(input_rate / wanted));
    if (decim < 1) decim = 1;
    decimation_ = decim;
    demod_rate_ = static_cast<float>(input_rate / static_cast<double>(decim));

    const size_t sps = static_cast<size_t>(std::lround(demod_rate_ / bit_rate));

    /* nRF24 deviation: +/-160 kHz at 250 kbps, +/-320 kHz at 1 and 2 Mbps. */
    const double deviation = (bitrate_kbps_ <= 250) ? 160'000.0 : 320'000.0;
    const double channel_bw = deviation + bit_rate;

    channel_filter_.configure(
        dsp::design_lowpass(channel_bw, channel_bw * 0.5, input_rate, 50.0), decim);
    fm_.configure(demod_rate_, static_cast<float>(deviation));
    decoder_.configure(sps ? sps : 1);

    text_status_.set("sps " + to_string_dec_uint(sps) + "  rate " +
                     to_string_dec_uint(static_cast<uint64_t>(demod_rate_ / 1000.0f)) + " kHz");
    configured_input_rate_ = input_rate;
}

void NrfRxView::on_packet(const nrf::Packet& packet) {
    packets_++;
    auto& entry = ui::on_packet(entries_, packet.address, 32);
    entry.count++;
    entry.payload_length = packet.payload_length;
    entry.pid = packet.pid;
    entry.payload_hex = packet.payload_string();
    table_.set_dirty();
}

void NrfRxView::on_frame_sync() {
    ui::View::on_frame_sync();

    if (!running_) return;
    if (receiver_.sampling_rate() != configured_input_rate_) rebuild_front_end();

    /* SAMPLE TAP — the honest note.
     *
     * proc_nrfrx sees every baseband sample. take_spectrum_samples() hands back
     * only the most recent 4096-sample wideband block and clears the ready
     * flag, so consecutive reads are not contiguous. A 105-bit ESB frame is
     * ~420 samples at four samples per bit, so a frame that straddles a gap is
     * lost. The pipeline below is a faithful port and decodes correctly when
     * fed a continuous stream (proved on synthesised GFSK in
     * tests/test_nrf_rx.cpp), but sustained over-the-air capture needs a
     * lossless channel-rate tap on ReceiverModel that does not exist yet. */
    if (!receiver_.take_spectrum_samples(samples_, 4096)) return;
    if (samples_.empty()) return;

    channel_filter_.process(samples_.data(), samples_.size(), channel_);
    if (channel_.empty()) return;

    fm_.process(channel_.data(), channel_.size(), audio_);
    if (audio_.empty()) return;

    decoder_.set_on_packet([this](const nrf::Packet& p) { this->on_packet(p); });
    decoder_.process(audio_.data(), audio_.size());
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream: menu_location RX, icon_color yellow. Its own bitmap is a chip
 * outline; bitmaps.hpp has no equivalent, and BTLE's icon is the nearest
 * honest match for a 2.4 GHz packet decoder. */
const app::Registrar reg_nrf_rx{{"nrf_rx", "NRF", app::Category::Receive,
                                 ui::Color::yellow(), &ui::bitmap_icon_btle,
                                 [] { return std::make_unique<app::NrfRxView>(); }}};
}  // namespace
