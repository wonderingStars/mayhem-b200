/*
 * mayhem-b200 — SAME / EAS alert header transmitter (AFSK).
 *
 * Copyright (C) 2024 HTotoo (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_same_tx.hpp"

#include "../core/string_format.hpp"
#include "../dsp/demod_digital.hpp"  /* afsk_modulate */
#include "../radio/transmitter_model.hpp"
#include "app_context.hpp"

#include <cstdio>

namespace app {
namespace same_tx {

/* --- Encoder -------------------------------------------------------------- */

static const char* const kOrg[] = {"WXR", "EAS", "CIV", "PEP"};
static const char* const kEvt[] = {
    "RWT", "RMT", "NPT", "NST", "NMT", "EAN", "EAT", "NIC", "ADR", "AVA", "AVW",
    "BZW", "CFW", "CFS", "DSW", "EQW", "EVI", "FFW", "FFS", "FFH", "FRW", "HLS",
    "HUW", "HUH", "SVR", "TOR"};

const char* org_code(size_t index) { return (index < 4) ? kOrg[index] : kOrg[0]; }
const char* evt_code(size_t index) { return (index < 26) ? kEvt[index] : kEvt[0]; }
size_t org_count() { return 4; }
size_t evt_count() { return 26; }

std::string build_message(size_t org_idx, size_t evt_idx, int ss, int ccc, int dh, int dm) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "ZCZC-%s-%s-%02d0%03d+%02d%02d-0010000-SAMETX--",
                  org_code(org_idx), evt_code(evt_idx), ss % 100, ccc % 1000,
                  dh % 100, dm % 100);
    return std::string(buf);
}

std::vector<uint8_t> build_bytes(const std::string& message) {
    std::vector<uint8_t> bytes;
    bytes.reserve(kPreambleCount + message.size());
    for (size_t i = 0; i < kPreambleCount; i++) bytes.push_back(kPreambleByte);
    for (char c : message) bytes.push_back(static_cast<uint8_t>(c));
    return bytes;
}

std::vector<uint8_t> build_bits(const std::string& message) {
    auto bytes = build_bytes(message);
    std::vector<uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (uint8_t b : bytes)
        for (int k = 0; k < 8; k++)
            bits.push_back(static_cast<uint8_t>((b >> k) & 1));  /* LSB first */
    return bits;
}

/* --- View ----------------------------------------------------------------- */

SameTxView::SameTxView()
    : transmitter_{globals().transmitter} {
    add_children({&labels_, &field_frequency_, &field_org_, &field_evt_, &text_evt_,
                  &field_state_, &field_county_, &field_dur_h_, &field_dur_m_,
                  &button_tx_, &text_msg_, &text_status_, &notes_});

    field_frequency_.set_step_index(4);  /* 25 kHz */
    field_frequency_.set_value(162'550'000ULL, false);

    field_org_.set_by_value(0, false);
    field_evt_.set_value(0);
    field_state_.set_value(39);   /* Ohio */
    field_county_.set_value(7);
    field_dur_h_.set_value(0);
    field_dur_m_.set_value(30);

    auto refresh = [this](int32_t) { update_preview(); };
    field_evt_.on_change = [this](int32_t) {
        text_evt_.set(evt_code(static_cast<size_t>(field_evt_.value())));
        update_preview();
    };
    field_org_.on_change = [this](size_t, int32_t) { update_preview(); };
    field_state_.on_change = refresh;
    field_county_.on_change = refresh;
    field_dur_h_.on_change = refresh;
    field_dur_m_.on_change = refresh;

    button_tx_.on_select = [this](ui::Button&) {
        if (transmitting_)
            stop_tx();
        else
            start_tx();
    };

    update_preview();
}

SameTxView::~SameTxView() {
    stop_tx();
}

void SameTxView::on_show() {
    ui::View::on_show();
    button_tx_.focus();
}

void SameTxView::on_hide() {
    stop_tx();
    ui::View::on_hide();
}

void SameTxView::update_preview() {
    const std::string msg = build_message(
        field_org_.selected_index(), static_cast<size_t>(field_evt_.value()),
        field_state_.value(), field_county_.value(),
        field_dur_h_.value(), field_dur_m_.value());
    text_msg_.set(msg);
}

void SameTxView::start_tx() {
    if (!transmitter_) {
        text_status_.set("No transmitter (needs B200)");
        return;
    }

    const std::string msg = build_message(
        field_org_.selected_index(), static_cast<size_t>(field_evt_.value()),
        field_state_.value(), field_county_.value(),
        field_dur_h_.value(), field_dur_m_.value());
    const auto bits = build_bits(msg);

    /* One AFSK burst, then a short gap; the whole thing SAME_REPEAT times. */
    auto burst = dsp::afsk_modulate(bits, kAudioRate, kMarkHz, kSpaceHz, kBaud, 0.8f);
    std::vector<float> audio;
    const size_t gap = static_cast<size_t>(kAudioRate);  /* 1 s between bursts */
    for (int r = 0; r < kRepeat; r++) {
        audio.insert(audio.end(), burst.begin(), burst.end());
        audio.insert(audio.end(), gap, 0.0f);
    }
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
        text_status_.set("TX start failed (needs B200)");
        transmitter_->set_audio_source(nullptr);
        return;
    }

    transmitting_ = true;
    button_tx_.set_text("Stop");
    text_status_.set("Transmitting SAME header");
}

void SameTxView::stop_tx() {
    if (transmitter_) {
        transmitter_->stop();
        transmitter_->set_audio_source(nullptr);
    }
    audio_ring_.clear();
    transmitting_ = false;
    button_tx_.set_text("TX");
    text_status_.set("Idle");
}

void SameTxView::on_frame_sync() {
    ui::View::on_frame_sync();
    if (!transmitting_) return;
    if (audio_ring_.empty()) stop_tx();
}

}  // namespace same_tx
}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_same_tx{{"same_tx", "SAME TX", app::Category::Transmit,
                                  ui::Color::red(), nullptr,
                                  [] { return std::make_unique<app::same_tx::SameTxView>(); }}};
}  // namespace
