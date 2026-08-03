/*
 * mayhem-b200 — OOK Brute: brute-force an OOK remote-control code space.
 *
 * Copyright (C) 2024 HTotoo
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_ookbrute.hpp"

#include "../core/string_format.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"

namespace app {
namespace ookbrute {

uint8_t protocol_bits(uint32_t protocol) {
    switch (protocol) {
        case Came24:
        case Nice24:
        case Princeton24:
            return 24;
        default:
            return 12;
    }
}

uint32_t protocol_max_code(uint32_t protocol) {
    return (1u << protocol_bits(protocol)) - 1u;
}

BrutePacket generate_packet(uint32_t protocol, uint32_t code) {
    BrutePacket pkt{};

    std::string dataFormat;
    std::string zero;
    std::string one;
    uint16_t databits = 0;
    uint16_t repeat = 1;
    uint16_t pause_sym = 0;
    uint32_t samples_per_bit = 0;

    /* All expressions are copied verbatim from OOKBruteView::generate_packet();
     * the 36-zero preamble, the start bit and trailer, the fragment patterns and
     * the timing are what each real remote transmits. */
    if (protocol == Came12) {
        samples_per_bit = kOokSampleRate / ((3 * 1000) / 1);
        dataFormat = "0000000000000000000000000000000000001CCCCCCCCCCCC0000";
        databits = 12;
        zero = "011";
        one = "001";
        repeat = 2;
        pause_sym = 0;
    } else if (protocol == Came24) {
        samples_per_bit = kOokSampleRate / ((3 * 1000) / 1);
        dataFormat = "0000000000000000000000000000000000001CCCCCCCCCCCCCCCCCCCCCCCC0000";
        databits = 24;
        zero = "011";
        one = "001";
        repeat = 2;
        pause_sym = 0;
    } else if (protocol == Nice12) {
        samples_per_bit = static_cast<uint32_t>(kOokSampleRate * (680.0 / 1000000.0));
        dataFormat = "000000000000000000000000000000000000000001CCCCCCCCCCCC0000";
        databits = 12;
        zero = "011";
        one = "001";
        repeat = 2;
        pause_sym = 0;
    } else if (protocol == Nice24) {
        samples_per_bit = static_cast<uint32_t>(kOokSampleRate * (680.0 / 1000000.0));
        dataFormat = "000000000000000000000000000000000000000001CCCCCCCCCCCCCCCCCCCCCCCC0000";
        databits = 24;
        zero = "011";
        one = "001";
        repeat = 2;
        pause_sym = 0;
    } else if (protocol == Holtek12) {
        samples_per_bit = static_cast<uint32_t>(kOokSampleRate * (390.0 / 1000000.0));
        dataFormat = "0000000000000000000000000000000000001CCCCCCCCCCCC00000000000";
        databits = 12;
        zero = "011";
        one = "001";
        repeat = 2;
        pause_sym = 0;
    } else {  // Princeton24
        samples_per_bit = static_cast<uint32_t>(kOokSampleRate * (450.0 / 1000000.0));
        dataFormat = "000000000000000000000000000000000000CCCCCCCCCCCCCCCCCCCCCCCC10000000";
        databits = 24;
        zero = "1000";
        one = "1110";
        repeat = 6;
        pause_sym = 0;
    }

    std::string fragments;
    uint16_t cdb = 0;  // current data bit
    for (char c : dataFormat) {
        if (c == '0') {
            fragments += '0';
        } else if (c == '1') {
            fragments += '1';
        } else if (c == 'C') {
            if (code & (1u << (databits - cdb - 1)))
                fragments += one;
            else
                fragments += zero;
            cdb++;
        }
    }

    pkt.fragments = std::move(fragments);
    pkt.samples_per_bit = samples_per_bit;
    pkt.repeat = repeat;
    pkt.pause_symbols = pause_sym;
    pkt.databits = databits;
    return pkt;
}

size_t pack_fragments(const std::string& fragments, std::vector<uint8_t>& out) {
    const size_t n = fragments.size();
    out.assign((n + 7) / 8, 0);

    uint8_t byte = 0;
    size_t len = 0;
    for (char c : fragments) {
        byte <<= 1;
        if (c != '0') byte |= 1;
        if ((len & 7) == 7) out[len >> 3] = byte;
        len++;
    }

    const size_t padding = 8 - (len & 7);
    if (padding != 8) {
        byte <<= padding;
        out[(len + padding - 1) >> 3] = byte;
    }

    return len;
}

}  // namespace ookbrute

/* ======================================================================== *
 *  View                                                                     *
 * ======================================================================== */

OOKBruteView::OOKBruteView() {
    add_children({&field_frequency_, &labels_, &field_start_, &field_stop_,
                  &options_atkmode_, &text_status_, &progressbar_, &text_warning_,
                  &button_startstop_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_frequency_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                                   static_cast<uint64_t>(caps.tx_freq.max));
    }
    field_frequency_.set_step_index(9);  // 100 kHz
    field_frequency_.set_value(433'920'000, false);
    field_frequency_.on_change = [this](uint64_t hz) {
        if (auto* tx = globals().transmitter) tx->set_target_frequency(hz);
    };

    options_atkmode_.on_change = [this](size_t, int32_t proto) {
        update_start_stop(static_cast<uint32_t>(proto));
        validate_start_stop();
    };
    field_start_.on_change = [this](int32_t) { validate_start_stop(); };
    field_stop_.on_change = [this](int32_t) { validate_start_stop(); };

    button_startstop_.on_select = [this](ui::Button&) {
        if (is_running)
            stop();
        else
            start();
    };

    update_start_stop(ookbrute::Came12);
}

OOKBruteView::~OOKBruteView() {
    stop();
}

void OOKBruteView::focus() {
    button_startstop_.focus();
}

void OOKBruteView::on_hide() {
    stop();
    View::on_hide();
}

void OOKBruteView::update_start_stop(uint32_t protocol) {
    const uint32_t max = ookbrute::protocol_max_code(protocol);
    field_start_.set_range(0, static_cast<int32_t>(max));
    field_stop_.set_range(0, static_cast<int32_t>(max));
    field_start_.set_value(0);
    field_stop_.set_value(static_cast<int32_t>(max));
}

void OOKBruteView::validate_start_stop() {
    if (field_start_.value() > field_stop_.value())
        field_start_.set_value(field_stop_.value());
}

void OOKBruteView::arm_current() {
    auto* tx = globals().transmitter;
    if (!tx) return;

    const uint32_t protocol = static_cast<uint32_t>(options_atkmode_.selected_index_value());
    const auto pkt = ookbrute::generate_packet(protocol, counter_);

    const size_t bit_count = ookbrute::pack_fragments(pkt.fragments, bits_);
    const double symbol_rate =
        (pkt.samples_per_bit > 0)
            ? static_cast<double>(ookbrute::kOokSampleRate) / pkt.samples_per_bit
            : 1000.0;

    auto keyer = std::make_shared<dsp::OokKeyer>();
    keyer->configure(static_cast<float>(ookbrute::kOokSampleRate),
                     static_cast<float>(symbol_rate));
    keyer->set_data(bits_.data(), bit_count);
    keyer->set_repeat(pkt.repeat, pkt.pause_symbols);
    keyer_ = keyer;

    tx->set_iq_source([keyer](dsp::cfloat* out, size_t count) {
        return keyer->process(out, count);
    });
}

void OOKBruteView::start() {
    auto* tx = globals().transmitter;
    if (!tx) {
        text_status_.set("No TX (needs B200)");
        return;
    }

    start_code_ = static_cast<uint32_t>(field_start_.value());
    stop_code_ = static_cast<uint32_t>(field_stop_.value());
    counter_ = start_code_;

    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(static_cast<double>(ookbrute::kOokSampleRate));
    tx->set_target_frequency(field_frequency_.value());

    arm_current();

    if (!tx->start()) {
        text_status_.set("TX start failed (B200?)");
        stop();
        return;
    }

    is_running = true;
    button_startstop_.set_text("Stop");
    progressbar_.set_max(stop_code_ - start_code_ + 1);
    progressbar_.set_value(0);
    text_status_.set("Brute " + to_string_dec_uint(counter_));
}

void OOKBruteView::stop() {
    is_running = false;
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    keyer_.reset();
    button_startstop_.set_text("Start");
}

void OOKBruteView::on_frame_sync() {
    View::on_frame_sync();
    if (!is_running) return;

    auto k = keyer_;
    if (!k) return;

    if (!k->done()) return;

    /* Current code finished — advance. */
    counter_++;

    if (counter_ > stop_code_) {
        stop();
        progressbar_.set_value(stop_code_ - start_code_ + 1);
        text_status_.set("Done");
        return;
    }

    progressbar_.set_value(counter_ - start_code_);
    text_status_.set("Brute " + to_string_dec_uint(counter_));
    arm_current();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_ookbrute{{
    "ookbrute", "OOK Brute", app::Category::Transmit,
    ui::Color::red(), &ui::bitmap_icon_remote,
    [] { return std::make_unique<app::OOKBruteView>(); }}};
}  // namespace
