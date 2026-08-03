/*
 * mayhem-b200 — KeeLoq TX implementation.
 *
 * Copyright (C) 2026 lifegame1lu111 (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_keeloqtx.hpp"

#include "../dsp/modulate.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "string_format.hpp"

#include <cstddef>

namespace app {

/* --- KeeLoq cipher --------------------------------------------------------- */

namespace {

constexpr uint32_t KEELOQ_NLF = 0x3A5C742Eu;

inline uint32_t kl_bit(uint64_t x, uint32_t n) {
    return static_cast<uint32_t>((x >> n) & 1ULL);
}

inline uint32_t kl_g5(uint32_t x, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    return kl_bit(x, a) + kl_bit(x, b) * 2 + kl_bit(x, c) * 4 + kl_bit(x, d) * 8 + kl_bit(x, e) * 16;
}

/* KeeLoq bit read over the 64-bit payload, LSB first (matches upstream's
 * bit(payload, i) in encode_data). */
inline uint32_t payload_bit(uint64_t payload, uint32_t i) {
    return static_cast<uint32_t>((payload >> i) & 1ULL);
}

/* One symbol is 400 us on air; the whole burst is rendered at this host sample
 * rate before being handed to the transmitter's Raw path. */
constexpr float kSymbolRateHz = 2500.0f;    /* 1 / 400 us */
constexpr float kSampleRateHz = 500000.0f;  /* 0.5 Msps */
constexpr uint32_t kPauseSymbols = 39;      /* upstream keeloq inter-repeat gap */

std::vector<uint8_t> pack_ook_bits(const std::string& s) {
    std::vector<uint8_t> bytes((s.size() + 7) / 8, 0u);
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] != '0')
            bytes[i >> 3] = static_cast<uint8_t>(bytes[i >> 3] | (0x80u >> (i & 7u)));
    return bytes;
}

}  // namespace

uint32_t keeloq_encrypt(uint32_t data, uint64_t key) {
    uint32_t x = data;
    for (uint32_t r = 0; r < 528; ++r) {
        const uint32_t msb = kl_bit(x, 0) ^ kl_bit(x, 16) ^ kl_bit(key, r & 63u) ^
                             kl_bit(KEELOQ_NLF, kl_g5(x, 1, 9, 20, 26, 31));
        x = (x >> 1) ^ (msb << 31);
    }
    return x;
}

uint32_t keeloq_decrypt(uint32_t data, uint64_t key) {
    uint32_t x = data;
    for (uint32_t r = 0; r < 528; ++r) {
        const uint32_t lsb = kl_bit(x, 31) ^ kl_bit(x, 15) ^ kl_bit(key, (15u - r) & 63u) ^
                             kl_bit(KEELOQ_NLF, kl_g5(x, 0, 8, 19, 25, 30));
        x = (x << 1) ^ lsb;
    }
    return x;
}

uint64_t keeloq_normal_learning(uint32_t data, uint64_t key) {
    data &= 0x0FFFFFFFu;
    data |= 0x20000000u;
    const uint32_t k1 = keeloq_decrypt(data, key);

    data &= 0x0FFFFFFFu;
    data |= 0x60000000u;
    const uint32_t k2 = keeloq_decrypt(data, key);

    return (static_cast<uint64_t>(k2) << 32) | k1;
}

uint64_t keeloq_reverse_key(uint64_t key, uint8_t bit_count) {
    uint64_t reversed = 0;
    for (uint8_t i = 0; i < bit_count; ++i)
        reversed = (reversed << 1) | ((key >> i) & 1ULL);
    return reversed;
}

uint32_t keeloq_build_hop(KeeloqHop type, uint8_t btn, uint32_t serial, uint16_t counter) {
    const uint32_t b = static_cast<uint32_t>(btn) << 28;
    const uint32_t cnt = static_cast<uint32_t>(counter);

    switch (type) {
        case KeeloqHop::Generic:
            return b | ((serial & 0x3FFu) << 16) | cnt;
        case KeeloqHop::Serial12:
            return b | ((serial & 0xFFFu) << 16) | cnt;
        case KeeloqHop::Serial8:
            return b | ((serial & 0xFFu) << 16) | cnt;
        case KeeloqHop::Merlin:
            return b | (0x000u << 16) | cnt;
        case KeeloqHop::Centurion:
            return b | (0x1CEu << 16) | cnt;
        case KeeloqHop::Monarch:
            return b | (0x100u << 16) | cnt;
        case KeeloqHop::Aprimatic: {
            uint32_t apri = serial;
            uint8_t apr1 = 0;
            for (uint16_t i = 1; i != 0b10000000000; i = static_cast<uint16_t>(i << 1))
                if (apri & i) ++apr1;
            apri &= 0b00001111111111u;
            if (apr1 % 2 == 0) apri |= 0b110000000000u;
            return b | ((apri & 0xFFFu) << 16) | cnt;
        }
        case KeeloqHop::DeaMio: {
            const uint8_t first_disc_num = static_cast<uint8_t>((serial >> 8) & 0xFu);
            const uint8_t result_disc = static_cast<uint8_t>(0xC + (first_disc_num % 4));
            const uint32_t dea = (serial & 0xFFu) | (static_cast<uint32_t>(result_disc) << 8);
            return b | ((dea & 0xFFFu) << 16) | cnt;
        }
    }
    return b | ((serial & 0x3FFu) << 16) | cnt;
}

std::string keeloq_encode_fragments(uint64_t payload) {
    static const char* const header = "101010101010101010101010000000000";
    static const char* const frag[2] = {"110", "100"};

    std::string fragments{header};
    fragments.reserve(33 + 64 * 3 + 4);
    for (uint32_t i = 0; i < 64; ++i)
        fragments += frag[payload_bit(payload, i)];
    fragments += "1001";
    return fragments;
}

/* --- View ------------------------------------------------------------------ */

KeeloqTXView::KeeloqTXView() {
    add_children({&labels_, &field_maker_, &field_learn_, &field_key_, &field_serial_,
                  &field_button_, &field_counter_, &field_repeat_, &field_freq_,
                  &text_payload_, &console_, &button_tx_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_freq_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                              static_cast<uint64_t>(caps.tx_freq.max));
    }
    field_freq_.set_value(433'920'000, false);
    field_button_.set_value(1, false);
    field_repeat_.set_value(4, false);

    field_maker_.on_change = [this](size_t, int32_t v) {
        hop_type_ = static_cast<KeeloqHop>(v);
        rebuild();
    };
    field_learn_.on_change = [this](size_t, int32_t v) {
        learning_ = static_cast<KeeloqLearning>(v);
        rebuild();
    };
    field_key_.on_change = [this](ui::SymField& f) {
        key_ = f.to_integer();
        rebuild();
    };
    field_serial_.on_change = [this](ui::SymField& f) {
        serial_ = static_cast<uint32_t>(f.to_integer()) & 0x0FFFFFFFu;
        rebuild();
    };
    field_button_.on_change = [this](int32_t v) {
        button_ = static_cast<uint8_t>(v);
        rebuild();
    };
    field_counter_.on_change = [this](int32_t v) {
        counter_ = static_cast<uint16_t>(v);
        rebuild();
    };
    field_repeat_.on_change = [this](int32_t v) { repeat_ = static_cast<uint32_t>(v); };
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
    console_.writeln(STR_COLOR_YELLOW "Rolling-code remote.");
    console_.writeln(STR_COLOR_YELLOW "TX may be illegal - your call.");
    console_.writeln(STR_COLOR_LIGHT_GREY "RF needs a USRP B200.");

    rebuild();
}

KeeloqTXView::~KeeloqTXView() {
    if (transmitting_) stop_tx();
}

void KeeloqTXView::focus() {
    button_tx_.focus();
}

void KeeloqTXView::rebuild() {
    fix_ = (static_cast<uint32_t>(button_) << 28) | (serial_ & 0x0FFFFFFFu);
    hop_ = keeloq_build_hop(hop_type_, button_, serial_, counter_);

    uint32_t encrypted;
    if (learning_ == KeeloqLearning::Normal) {
        const uint64_t device_key = keeloq_normal_learning(fix_, key_);
        encrypted = keeloq_encrypt(hop_, device_key);
    } else {
        encrypted = keeloq_encrypt(hop_, key_);
    }

    payload_ = (static_cast<uint64_t>(fix_) << 32) | encrypted;
    const uint64_t preview = keeloq_reverse_key(payload_, 64);
    text_payload_.set(to_string_hex(preview, 16));
}

void KeeloqTXView::start_tx() {
    auto* tx = globals().transmitter;
    if (!tx) {
        console_.writeln(STR_COLOR_RED "No transmitter wired.");
        return;
    }

    rebuild();
    const std::string fragments = keeloq_encode_fragments(payload_);
    const auto bits = pack_ook_bits(fragments);

    dsp::OokKeyer keyer;
    keyer.configure(kSampleRateHz, kSymbolRateHz);
    keyer.set_data(bits.data(), fragments.size());
    keyer.set_repeat(repeat_ == 0 ? 1u : repeat_, kPauseSymbols);

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
    console_.writeln("TX " + to_string_dec_uint(repeat_) + "x @ " +
                     to_string_dec_uint(field_freq_.value() / 1000) + " kHz");
}

void KeeloqTXView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    transmitting_ = false;
    button_tx_.set_text("Start");

    /* Advance the rolling counter after a send, as upstream does. */
    counter_ = static_cast<uint16_t>(counter_ + 1);
    field_counter_.set_value(counter_, false);
    rebuild();
}

void KeeloqTXView::on_frame_sync() {
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
const app::Registrar reg_keeloqtx{{
    "keeloqtx", "KeeLoq TX", app::Category::Transmit,
    ui::Color::yellow(), nullptr,
    [] { return std::make_unique<app::KeeloqTXView>(); }}};
}  // namespace
