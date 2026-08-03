/*
 * mayhem-b200 — BHT TX implementation.
 *
 * Copyright (C) 2015 Jared Boone; Copyright (C) 2016 Furrtek (original)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_bht_tx.hpp"

#include "../dsp/modulate.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "string_format.hpp"

#include <cstddef>
#include <initializer_list>

namespace app {

namespace {

/* UM3750 encoder def (encoders.hpp ENCODER_UM3750). */
constexpr char kUm3750WordFormat[] = "SAAAAAAAAAAAA";
constexpr char kUm3750Sync[] = "001";
constexpr const char* kUm3750BitFormat[2] = {"011", "001"};

/* EPAR: OOK_SAMPLERATE / 580 samples per symbol -> ~580 symbols/s on air. */
constexpr float kEparSymbolRateHz = 580.0f;
constexpr uint32_t kEparRepeat = 26;
constexpr uint32_t kEparPauseSymbols = (3 * 12) - 6;  /* UM3750 pause_symbols */

/* Xylos: 100 ms per CCIR tone, FM-modulated. */
constexpr float kXyToneSeconds = 0.1f;
constexpr float kXyDeviationHz = 2500.0f;

constexpr float kSampleRateHz = 500000.0f;

std::vector<uint8_t> pack_ook_bits(const std::string& s) {
    std::vector<uint8_t> bytes((s.size() + 7) / 8, 0u);
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] != '0')
            bytes[i >> 3] = static_cast<uint8_t>(bytes[i >> 3] | (0x80u >> (i & 7u)));
    return bytes;
}

void render_ook(const std::string& fragments, uint32_t repeat, uint32_t pause,
                std::vector<std::complex<float>>& dst) {
    const auto bits = pack_ook_bits(fragments);
    dsp::OokKeyer keyer;
    keyer.configure(kSampleRateHz, kEparSymbolRateHz);
    keyer.set_data(bits.data(), fragments.size());
    keyer.set_repeat(repeat, pause);

    const size_t start = dst.size();
    const size_t n = keyer.total_samples();
    dst.resize(start + n, std::complex<float>{0.0f, 0.0f});
    if (n) keyer.process(dst.data() + start, n);
}

}  // namespace

std::string bht_gen_message_ep(uint8_t city_code, size_t family_code_ep,
                               uint32_t relay_number, uint32_t relay_state) {
    uint8_t bits[12];
    for (size_t c = 0; c < 8; ++c) bits[c] = static_cast<uint8_t>((city_code >> c) & 1u);
    bits[8] = static_cast<uint8_t>((family_code_ep >> 1) & 1u);
    bits[9] = static_cast<uint8_t>(family_code_ep & 1u);
    bits[10] = static_cast<uint8_t>(relay_number & 1u);
    bits[11] = relay_state ? 1u : 0u;

    std::string fragments;
    fragments.reserve(39);
    size_t c = 0;
    for (const char* p = kUm3750WordFormat; *p; ++p) {
        if (*p == 'S')
            fragments += kUm3750Sync;
        else
            fragments += kUm3750BitFormat[bits[c++]];
    }
    return fragments;
}

std::array<uint8_t, 20> bht_gen_message_xy(size_t header_a, size_t header_b,
                                           size_t city, size_t family,
                                           bool subfamily_wc, size_t subfamily_code,
                                           bool id_wc, size_t receiver,
                                           size_t relay_a, size_t relay_b,
                                           size_t relay_c, size_t relay_d) {
    std::array<uint8_t, 20> m{};
    m[0] = static_cast<uint8_t>(header_a / 10);
    m[1] = static_cast<uint8_t>(header_a % 10);
    m[2] = static_cast<uint8_t>(header_b / 10);
    m[3] = static_cast<uint8_t>(header_b % 10);
    m[4] = static_cast<uint8_t>(city / 10);
    m[5] = static_cast<uint8_t>(city % 10);
    m[6] = static_cast<uint8_t>(family);
    m[7] = subfamily_wc ? 0xAu : static_cast<uint8_t>(subfamily_code);
    if (id_wc) {
        m[8] = 0xAu;
        m[9] = 0xAu;
    } else {
        m[8] = static_cast<uint8_t>(receiver / 10);
        m[9] = static_cast<uint8_t>(receiver % 10);
    }
    m[10] = 0xBu;
    m[11] = static_cast<uint8_t>(relay_a);
    m[12] = static_cast<uint8_t>(relay_b);
    m[13] = static_cast<uint8_t>(relay_c);
    m[14] = static_cast<uint8_t>(relay_d);
    m[15] = 0xBu;
    for (size_t c = 16; c < 20; ++c) m[c] = 0u;

    /* Replace a repeat of the previous (possibly already substituted) symbol. */
    for (size_t c = 1; c < 20; ++c)
        if (m[c] == m[c - 1]) m[c] = 0xEu;

    return m;
}

std::string bht_ccir_to_ascii(const std::array<uint8_t, 20>& msg) {
    std::string ascii;
    ascii.reserve(20);
    for (uint8_t v : msg) {
        if (v > 9)
            ascii += static_cast<char>(v - 10 + 'A');
        else
            ascii += static_cast<char>(v + '0');
    }
    return ascii;
}

/* --- View ------------------------------------------------------------------ */

BHTView::BHTView() {
    add_children({&labels_, &field_system_,
                  &xy_labels_, &xy_header_a_, &xy_header_b_, &xy_city_, &xy_family_,
                  &xy_subfamily_, &xy_sub_all_, &xy_receiver_, &xy_id_all_,
                  &xy_relay_a_, &xy_relay_b_, &xy_relay_c_, &xy_relay_d_,
                  &ep_labels_, &ep_city_, &ep_group_, &ep_relay_a_, &ep_relay_b_,
                  &field_freq_, &text_msg_, &console_, &button_tx_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_freq_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                              static_cast<uint64_t>(caps.tx_freq.max));
    }
    field_freq_.set_value(433'920'000, false);

    /* Upstream defaults. */
    xy_city_.set_value(10, false);
    xy_family_.set_value(1, false);
    xy_subfamily_.set_value(1, false);
    xy_receiver_.set_value(1, false);
    ep_group_.set_selected_index(2, false);  /* group C */

    const auto refresh = [this](int32_t) { refresh_preview(); };
    const auto refresh_opt = [this](size_t, int32_t) { refresh_preview(); };

    field_system_.on_change = [this](size_t, int32_t v) {
        system_ = static_cast<System>(v);
        update_system_visibility();
        refresh_preview();
    };

    xy_header_a_.on_change = refresh;
    xy_header_b_.on_change = refresh;
    xy_city_.on_change = refresh;
    xy_family_.on_change = refresh;
    xy_subfamily_.on_change = refresh;
    xy_receiver_.on_change = refresh;
    xy_sub_all_.on_select = [this](ui::Checkbox&, bool) { refresh_preview(); };
    xy_id_all_.on_select = [this](ui::Checkbox&, bool) { refresh_preview(); };
    xy_relay_a_.on_change = refresh_opt;
    xy_relay_b_.on_change = refresh_opt;
    xy_relay_c_.on_change = refresh_opt;
    xy_relay_d_.on_change = refresh_opt;

    ep_city_.on_change = refresh;
    ep_group_.on_change = refresh_opt;
    ep_relay_a_.on_change = refresh_opt;
    ep_relay_b_.on_change = refresh_opt;

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
    console_.writeln(STR_COLOR_YELLOW "Drives gate/relay receivers.");
    console_.writeln(STR_COLOR_YELLOW "TX may be illegal - your call.");
    console_.writeln(STR_COLOR_LIGHT_GREY "RF needs a USRP B200.");

    update_system_visibility();
    refresh_preview();
}

BHTView::~BHTView() {
    if (transmitting_) stop_tx();
}

void BHTView::focus() {
    button_tx_.focus();
}

void BHTView::update_system_visibility() {
    const bool xy = (system_ == System::Xylos);
    const std::initializer_list<ui::Widget*> xy_widgets = {
        &xy_labels_, &xy_header_a_, &xy_header_b_, &xy_city_, &xy_family_,
        &xy_subfamily_, &xy_sub_all_, &xy_receiver_, &xy_id_all_,
        &xy_relay_a_, &xy_relay_b_, &xy_relay_c_, &xy_relay_d_};
    for (ui::Widget* w : xy_widgets) w->hidden(!xy);
    const std::initializer_list<ui::Widget*> ep_widgets = {
        &ep_labels_, &ep_city_, &ep_group_, &ep_relay_a_, &ep_relay_b_};
    for (ui::Widget* w : ep_widgets) w->hidden(xy);
    set_dirty();
}

void BHTView::refresh_preview() {
    if (system_ == System::Xylos) {
        const auto msg = bht_gen_message_xy(
            static_cast<size_t>(xy_header_a_.value()), static_cast<size_t>(xy_header_b_.value()),
            static_cast<size_t>(xy_city_.value()), static_cast<size_t>(xy_family_.value()),
            xy_sub_all_.value(), static_cast<size_t>(xy_subfamily_.value()),
            xy_id_all_.value(), static_cast<size_t>(xy_receiver_.value()),
            static_cast<size_t>(xy_relay_a_.selected_index_value()),
            static_cast<size_t>(xy_relay_b_.selected_index_value()),
            static_cast<size_t>(xy_relay_c_.selected_index_value()),
            static_cast<size_t>(xy_relay_d_.selected_index_value()));
        text_msg_.set(bht_ccir_to_ascii(msg));
    } else {
        text_msg_.set("C" + to_string_dec_uint(static_cast<uint32_t>(ep_city_.value())) +
                      " G" + to_string_dec_uint(static_cast<uint32_t>(ep_group_.selected_index_value())));
    }
}

void BHTView::start_tx() {
    auto* tx = globals().transmitter;
    if (!tx) {
        console_.writeln(STR_COLOR_RED "No transmitter wired.");
        return;
    }

    tx_iq_.clear();

    if (system_ == System::Epar) {
        const uint8_t city = static_cast<uint8_t>(ep_city_.value());
        const size_t group = static_cast<size_t>(ep_group_.selected_index_value());
        /* R2 (half 0, relay_number 1) then R1 (half 1, relay_number 0). */
        render_ook(bht_gen_message_ep(city, group, 1,
                                      static_cast<uint32_t>(ep_relay_a_.selected_index_value())),
                   kEparRepeat, kEparPauseSymbols, tx_iq_);
        render_ook(bht_gen_message_ep(city, group, 0,
                                      static_cast<uint32_t>(ep_relay_b_.selected_index_value())),
                   kEparRepeat, kEparPauseSymbols, tx_iq_);
    } else {
        const auto msg = bht_gen_message_xy(
            static_cast<size_t>(xy_header_a_.value()), static_cast<size_t>(xy_header_b_.value()),
            static_cast<size_t>(xy_city_.value()), static_cast<size_t>(xy_family_.value()),
            xy_sub_all_.value(), static_cast<size_t>(xy_subfamily_.value()),
            xy_id_all_.value(), static_cast<size_t>(xy_receiver_.value()),
            static_cast<size_t>(xy_relay_a_.selected_index_value()),
            static_cast<size_t>(xy_relay_b_.selected_index_value()),
            static_cast<size_t>(xy_relay_c_.selected_index_value()),
            static_cast<size_t>(xy_relay_d_.selected_index_value()));

        const size_t samples_per_tone = static_cast<size_t>(kXyToneSeconds * kSampleRateHz);
        std::vector<float> audio;
        audio.reserve(samples_per_tone * msg.size());
        dsp::ToneGen tone;
        tone.configure(dsp::tones::ccir[msg[0] & 0xF], kSampleRateHz);
        std::vector<float> block(samples_per_tone, 0.0f);
        for (uint8_t idx : msg) {
            tone.set_frequency(dsp::tones::ccir[idx & 0xF]);
            tone.process(block.data(), block.size());
            audio.insert(audio.end(), block.begin(), block.end());
        }

        dsp::FmModulator fm;
        fm.configure(kSampleRateHz, kXyDeviationHz);
        fm.process(audio.data(), audio.size(), tx_iq_);
    }

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
    console_.writeln(system_ == System::Epar ? "TX EPAR" : "TX Xylos");
}

void BHTView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    transmitting_ = false;
    button_tx_.set_text("Start");
}

void BHTView::on_frame_sync() {
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
const app::Registrar reg_bht_tx{{
    "bht_tx", "BHT TX", app::Category::Transmit,
    ui::Color::green(), nullptr,
    [] { return std::make_unique<app::BHTView>(); }}};
}  // namespace
