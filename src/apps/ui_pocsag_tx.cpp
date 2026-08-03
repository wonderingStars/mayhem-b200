/*
 * mayhem-b200 — POCSAG pager transmitter (implementation).
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_pocsag_tx.hpp"

#include "../core/string_format.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_alphanum.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <complex>

namespace app::pocsag_tx {

POCSAGTXView::POCSAGTXView() {
    add_children({&labels_,
                  &field_freq_,
                  &options_bitrate_,
                  &field_address_,
                  &options_type_,
                  &options_function_,
                  &options_polarity_,
                  &text_message_,
                  &text_message_l2_,
                  &button_message_,
                  &button_tx_,
                  &progressbar_,
                  &text_warning_,
                  &text_status_});

    text_warning_.set(STR_COLOR_YELLOW "Illegal to radiate in most areas");

    options_bitrate_.set_selected_index(1);   // 1200 bps
    options_type_.set_selected_index(2);       // Alphanumeric
    options_function_.set_selected_index(0);   // Function A
    options_polarity_.set_selected_index(0);   // Standard (CCIR Rec. 584)

    field_address_.set_value(static_cast<uint64_t>(1337007));

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_freq_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                              static_cast<uint64_t>(caps.tx_freq.max));
    }
    if (auto* tx = globals().transmitter)
        field_freq_.set_value(tx->target_frequency(), false);
    else
        field_freq_.set_value(466'175'000, false);

    /* Alphanumeric implies function D, matching upstream's convenience rule. */
    options_type_.on_change = [this](size_t, int32_t v) {
        if (v == pocsag::ALPHANUMERIC)
            options_function_.set_selected_index(3);
    };

    button_message_.on_select = [this](ui::Button&) { set_message(); };
    button_tx_.on_select = [this](ui::Button&) {
        if (transmitting_)
            stop_tx();
        else
            start_tx();
    };

    update_message_text();
    progressbar_.set_max(1000);
}

POCSAGTXView::~POCSAGTXView() {
    stop_tx();
}

void POCSAGTXView::focus() {
    field_address_.focus();
}

void POCSAGTXView::on_show() {
    ui::View::on_show();
    update_message_text();
}

void POCSAGTXView::on_hide() {
    stop_tx();
    ui::View::on_hide();
}

uint32_t POCSAGTXView::selected_bitrate() const {
    return static_cast<uint32_t>(options_bitrate_.selected_index_value());
}

void POCSAGTXView::set_message() {
    auto* nav = globals().nav;
    if (!nav) return;
    ui::text_prompt(*nav, message_, 80, ENTER_KEYBOARD_MODE_ALPHA,
                    [this](std::string&) { update_message_text(); });
}

void POCSAGTXView::update_message_text() {
    /* Same two-line split upstream uses. */
    if (message_.length() <= 30) {
        text_message_.set(message_);
        text_message_l2_.set("");
    } else if (message_.length() <= 60) {
        text_message_.set(message_.substr(0, 29));
        text_message_l2_.set(message_.substr(29));
    } else {
        text_message_.set(message_.substr(0, 29));
        text_message_l2_.set(message_.substr(29, 27) + "...");
    }
}

bool POCSAGTXView::start_tx() {
    const uint32_t address = static_cast<uint32_t>(field_address_.to_integer());
    if (address > max_address) {
        ui::display_modal("Bad address",
                          "Address must be less\nthan 2097152.");
        return false;
    }

    const MessageType type =
        static_cast<MessageType>(options_type_.selected_index_value());

    if (type == pocsag::NUMERIC_ONLY) {
        for (char c : message_) {
            const bool ok = (c >= '0' && c <= '9') || c == 'S' || c == 'U' ||
                            c == ' ' || c == '-' || c == '[' || c == ']';
            if (!ok) {
                ui::display_modal(
                    "Bad message",
                    "A numeric message may only\ncontain 0-9 S U ] [ -\nor space.");
                return false;
            }
        }
    }

    /* "Standard" (index value 0) = CCIR Rec. 584: bit 1 at negative deviation,
     * which means inverting the codewords for a modulator that puts bit 1 at
     * positive deviation. "Inverted" sends them as-is. */
    const bool invert = (options_polarity_.selected_index_value() == 0);

    const auto codewords = encode_codewords(
        type, ecc_,
        static_cast<uint32_t>(options_function_.selected_index_value()),
        message_, address);
    tx_bytes_ = codewords_to_bytes(codewords, invert);

    const uint32_t bitrate = selected_bitrate();
    fsk_.configure(static_cast<float>(sample_rate_hz),
                   static_cast<float>(bitrate), deviation_hz);
    fsk_.set_gaussian(0.0f);      // POCSAG is hard-keyed 2FSK, not GFSK
    fsk_.set_repeat(1, 0);
    fsk_.set_data(tx_bytes_.data(), tx_bytes_.size() * 8);

    total_samples_ = static_cast<uint64_t>(
        static_cast<double>(tx_bytes_.size() * 8) * (sample_rate_hz / bitrate));
    produced_samples_.store(0);
    tx_done_.store(false);

    auto* tx = globals().transmitter;
    if (!tx) {
        text_status_.set(STR_COLOR_RED "No transmitter (needs USRP B200)");
        return false;
    }

    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(sample_rate_hz);
    tx->set_target_frequency(field_freq_.value());
    tx->set_iq_source([this](std::complex<float>* out, size_t n) -> size_t {
        const size_t w = fsk_.process(out, n);
        produced_samples_.fetch_add(w);
        if (w < n) tx_done_.store(true);
        return w;
    });

    if (!tx->start()) {
        tx->set_iq_source(nullptr);
        std::string err = "TX start failed (needs USRP B200)";
        if (auto* r = globals().radio) {
            const auto& e = r->last_error();
            if (!e.empty()) err = e;
        }
        text_status_.set(STR_COLOR_RED + err);
        return false;
    }

    transmitting_ = true;
    button_tx_.set_text("Stop");
    text_status_.set(STR_COLOR_GREEN "Transmitting " +
                     to_string_dec_uint(bitrate) + " bps");
    return true;
}

void POCSAGTXView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    if (transmitting_) {
        transmitting_ = false;
        button_tx_.set_text("Start TX");
        progressbar_.set_value(0);
        text_status_.set("");
    }
}

void POCSAGTXView::on_frame_sync() {
    ui::View::on_frame_sync();
    if (!transmitting_) return;

    if (total_samples_ > 0) {
        const uint64_t p = std::min(produced_samples_.load(), total_samples_);
        progressbar_.set_value(static_cast<uint32_t>(p * 1000 / total_samples_));
    }

    if (tx_done_.load()) {
        stop_tx();
        text_status_.set(STR_COLOR_GREEN "Sent");
    }
}

}  // namespace app::pocsag_tx

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_pocsag_tx{{
    "pocsag_tx",
    "POCSAG TX",
    app::Category::Transmit,
    ui::Color::green(),
    &ui::bitmap_icon_pocsag,
    [] { return std::make_unique<app::pocsag_tx::POCSAGTXView>(); },
    false}};
}  // namespace
