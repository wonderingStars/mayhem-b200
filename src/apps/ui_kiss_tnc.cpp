/*
 * mayhem-b200 — KISS TNC (AX.25 / APRS, 1200-baud AFSK).
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek (ax25)
 * Copyright (C) 2024 Sarah Rose (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_kiss_tnc.hpp"

#include "../core/string_format.hpp"
#include "../dsp/demod_digital.hpp"  /* afsk_modulate */
#include "../radio/transmitter_model.hpp"
#include "app_context.hpp"

#include <cctype>

namespace app {
namespace kiss {

/* --- KISS codec ----------------------------------------------------------- */

std::vector<uint8_t> kiss_encode(const uint8_t* data, size_t len, uint8_t command) {
    std::vector<uint8_t> out;
    out.reserve(len + 4);
    out.push_back(FEND);
    out.push_back(command);
    for (size_t i = 0; i < len; i++) {
        if (data[i] == FEND) {
            out.push_back(FESC);
            out.push_back(TFEND);
        } else if (data[i] == FESC) {
            out.push_back(FESC);
            out.push_back(TFESC);
        } else {
            out.push_back(data[i]);
        }
    }
    out.push_back(FEND);
    return out;
}

std::vector<uint8_t> kiss_encode(const std::vector<uint8_t>& data, uint8_t command) {
    return kiss_encode(data.data(), data.size(), command);
}

void KissDecoder::reset() {
    state_ = State::Idle;
    buf_.clear();
}

void KissDecoder::emit() {
    if (!buf_.empty() && on_frame_) on_frame_(buf_);
}

void KissDecoder::feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        const uint8_t b = data[i];
        switch (state_) {
            case State::Idle:
                if (b == FEND) state_ = State::Cmd;
                break;
            case State::Cmd:
                if (b == FEND) break;  /* back-to-back FEND */
                if (b == 0x00) {
                    buf_.clear();
                    state_ = State::Data;
                } else {
                    state_ = State::Idle;  /* non-data command, ignore */
                }
                break;
            case State::Data:
                if (b == FEND) {
                    if (!buf_.empty()) emit();
                    buf_.clear();
                    state_ = State::Cmd;
                } else if (b == FESC) {
                    state_ = State::Esc;
                } else {
                    buf_.push_back(b);
                }
                break;
            case State::Esc:
                if (b == TFEND)
                    buf_.push_back(FEND);
                else if (b == TFESC)
                    buf_.push_back(FESC);
                state_ = State::Data;
                break;
        }
    }
}

/* --- AX.25 ---------------------------------------------------------------- */

uint16_t ax25_fcs(const uint8_t* data, size_t len) {
    dsp::Crc<16, true, true> crc{0x1021, 0xFFFF, 0xFFFF};
    crc.process_bytes(data, len);
    return static_cast<uint16_t>(crc.checksum());
}

void AX25Frame::begin() {
    bits_.clear();
    current_bit_ = 0;
    ones_ = 0;
    crc_.reset();
}

void AX25Frame::nrzi_add_bit(uint32_t bit) {
    if (!bit) current_bit_ ^= 1;  /* data 0 flips the level, data 1 holds it */
    bits_.push_back(current_bit_);
}

void AX25Frame::add_byte(uint8_t byte, bool is_flag, bool is_data) {
    if (is_data) crc_.process_byte(byte);
    for (uint32_t i = 0; i < 8; i++) {
        const bool bit = (byte >> i) & 1;  /* least significant bit first */
        nrzi_add_bit(bit ? 1u : 0u);
        if (bit) {
            ones_++;
            if (ones_ == 5 && !is_flag) {
                nrzi_add_bit(0);  /* HDLC stuff bit */
                ones_ = 0;
            }
        } else {
            ones_ = 0;
        }
    }
}

void AX25Frame::add_checksum() {
    const uint16_t fcs = static_cast<uint16_t>(crc_.checksum());
    add_byte(static_cast<uint8_t>(fcs & 0xFF), false, false);
    add_byte(static_cast<uint8_t>((fcs >> 8) & 0xFF), false, false);
}

void AX25Frame::make_frame_from_raw(const uint8_t* data, size_t len) {
    begin();
    for (int i = 0; i < 4; i++) add_flag();
    for (size_t i = 0; i < len; i++) add_data(data[i]);
    add_checksum();
    add_flag();
    add_flag();
}

void AX25Frame::make_ui_frame(const uint8_t* address, uint8_t control, uint8_t protocol,
                              const std::string& info) {
    begin();
    for (int i = 0; i < 4; i++) add_flag();

    /* Extended address field: every octet shifted left one, the final octet's
     * LSB set to mark the end of the address (upstream make_extended_field). */
    for (size_t i = 0; i < 14; i++) {
        const uint8_t shifted = static_cast<uint8_t>(address[i] << 1);
        add_data(i == 13 ? (shifted | 1) : shifted);
    }
    add_data(control);
    add_data(protocol);
    for (char c : info) add_data(static_cast<uint8_t>(c));
    add_checksum();
    add_flag();
    add_flag();
}

std::vector<uint8_t> AX25Frame::bytes() const {
    std::vector<uint8_t> out((bits_.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits_.size(); i++)
        if (bits_[i] & 1) out[i >> 3] |= static_cast<uint8_t>(0x80u >> (i & 7));
    return out;
}

std::vector<uint8_t> make_address(const std::string& dest, uint8_t dest_ssid,
                                  const std::string& source, uint8_t source_ssid) {
    std::vector<uint8_t> a(14, ' ');
    for (size_t i = 0; i < 6; i++)
        a[i] = (i < dest.size()) ? static_cast<uint8_t>(std::toupper(dest[i])) : ' ';
    a[6] = static_cast<uint8_t>(0x30 | (dest_ssid & 0x0F));
    for (size_t i = 0; i < 6; i++)
        a[7 + i] = (i < source.size()) ? static_cast<uint8_t>(std::toupper(source[i])) : ' ';
    a[13] = static_cast<uint8_t>(0x30 | (source_ssid & 0x0F));
    return a;
}

/* --- View ----------------------------------------------------------------- */

KissTncView::KissTncView()
    : transmitter_{globals().transmitter} {
    add_children({&labels_, &field_frequency_, &field_source_, &field_dest_, &field_info_,
                  &button_tx_, &text_status_, &console_, &notes_});

    console_.enable_scrolling(true);

    field_frequency_.set_step_index(3);  /* 100 kHz */
    field_frequency_.set_value(144'390'000ULL, false);  /* NA APRS */

    button_tx_.on_select = [this](ui::Button&) {
        if (transmitting_)
            stop_tx();
        else
            start_tx();
    };
}

KissTncView::~KissTncView() {
    stop_tx();
}

void KissTncView::on_show() {
    ui::View::on_show();
    button_tx_.focus();
    console_.writeln(STR_COLOR_LIGHT_GREY "KISS over USB serial on hardware.");
    console_.writeln(STR_COLOR_LIGHT_GREY "No host bridge here: manual TX only.");
    console_.writeln(STR_COLOR_LIGHT_GREY "1200 baud Bell-202 AFSK / AX.25.");
}

void KissTncView::on_hide() {
    stop_tx();
    ui::View::on_hide();
}

void KissTncView::start_tx() {
    if (!transmitter_) {
        console_.writeln("No transmitter (needs B200).");
        return;
    }

    auto addr = make_address(field_dest_.get_text(), 0, field_source_.get_text(), 0);
    AX25Frame frame;
    frame.make_ui_frame(addr.data(), 0x03, 0xF0, field_info_.get_text());
    const auto& bits = frame.bits();
    if (bits.empty()) {
        text_status_.set("Empty frame");
        return;
    }

    auto audio = dsp::afsk_modulate(bits, kAudioRate, kMarkHz, kSpaceHz, kBaud, 0.8f);
    if (audio.empty()) {
        text_status_.set("Nothing to send");
        return;
    }

    audio_ring_.clear();
    transmitter_->set_mode(radio::TransmitterModel::Mode::NarrowbandFM);
    transmitter_->set_deviation(kDeviationHz);
    transmitter_->set_target_frequency(field_frequency_.value());
    transmitter_->set_audio_source([this](float* out, size_t n) {
        return audio_ring_.read(out, n);
    });
    audio_ring_.write(audio.data(), audio.size());

    if (!transmitter_->start()) {
        console_.writeln("TX start failed (needs B200).");
        transmitter_->set_audio_source(nullptr);
        return;
    }

    transmitting_ = true;
    button_tx_.set_text("Stop");
    text_status_.set("TX AX.25 UI frame (" + to_string_dec_uint(bits.size()) + " bits)");
}

void KissTncView::stop_tx() {
    if (transmitter_) {
        transmitter_->stop();
        transmitter_->set_audio_source(nullptr);
    }
    audio_ring_.clear();
    transmitting_ = false;
    button_tx_.set_text("TX");
    text_status_.set("Idle");
}

void KissTncView::on_frame_sync() {
    ui::View::on_frame_sync();
    if (!transmitting_) return;
    if (audio_ring_.empty()) stop_tx();
}

}  // namespace kiss
}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_kiss_tnc{{"kiss_tnc", "KISS TNC", app::Category::Transceiver,
                                   ui::Color::cyan(), &ui::bitmap_icon_aprs,
                                   [] { return std::make_unique<app::kiss::KissTncView>(); }}};
}  // namespace
