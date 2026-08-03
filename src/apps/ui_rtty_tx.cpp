/*
 * mayhem-b200 — RTTY TX (Baudot/ITA2 2FSK teletype transmitter).
 *
 * Copyright (C) 2026 HTotoo (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_rtty_tx.hpp"

#include "../core/string_format.hpp"
#include "../dsp/modulate.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_alphanum.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <cmath>

namespace app {

using namespace rtty_tx;

RttyTxView::RttyTxView() {
    add_children({&labels_, &text_warning_, &options_baud_, &options_shift_,
                  &options_stop_, &check_inverted_, &text_tones_,
                  &field_frequency_, &button_message_, &text_message_,
                  &button_tx_, &text_status_});

    text_warning_.set(STR_COLOR_YELLOW "Licensed/amateur use only");

    /* Frequency range from the device, falling back to the published B200
     * window when no radio is attached. */
    uint64_t f_min = 70'000'000, f_max = 6'000'000'000ull;
    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        if (caps.tx_freq.max > caps.tx_freq.min) {
            f_min = static_cast<uint64_t>(caps.tx_freq.min);
            f_max = static_cast<uint64_t>(caps.tx_freq.max);
        }
    }
    field_frequency_.set_range(f_min, f_max);
    uint64_t f0 = 14'090'000;
    if (auto* tx = globals().transmitter) {
        const uint64_t t = tx->target_frequency();
        if (t >= f_min && t <= f_max) f0 = t;
    }
    field_frequency_.set_value(f0, false);
    field_frequency_.on_change = [this](uint64_t f) {
        if (auto* tx = globals().transmitter) tx->set_target_frequency(f);
    };

    options_baud_.set_by_value(4545, false);
    options_shift_.set_by_value(170, false);
    options_stop_.set_selected_index(1, false);  /* 1.5 -> two whole stop bits */

    options_shift_.on_change = [this](size_t, int32_t) { refresh_tones(); };
    check_inverted_.on_select = [this](ui::Checkbox&, bool) { refresh_tones(); };

    button_message_.on_select = [this](ui::Button&) {
        auto* nav = globals().nav;
        if (!nav) return;
        ui::text_prompt(*nav, message_, 64, ENTER_KEYBOARD_MODE_ALPHA,
                        [this](std::string& v) {
                            message_ = v;
                            text_message_.set(message_);
                        });
    };

    button_tx_.on_select = [this](ui::Button&) {
        if (transmitting_)
            stop_tx();
        else
            start_tx();
    };

    text_message_.set(message_);
    refresh_tones();
}

RttyTxView::~RttyTxView() {
    stop_tx();
}

void RttyTxView::focus() {
    button_message_.focus();
}

void RttyTxView::on_hide() {
    stop_tx();
    View::on_hide();
}

void RttyTxView::refresh_tones() {
    const int shift = options_shift_.selected_index_value();
    const int half = shift / 2;
    int mark = +half, space = -half;
    if (check_inverted_.value()) std::swap(mark, space);
    text_tones_.set("Mark " + to_string_dec_int(mark) + " / Space " +
                    to_string_dec_int(space) + " Hz");
}

void RttyTxView::build_waveform() {
    waveform_.clear();

    const auto codes = baudot_encode(message_, /*usos*/ false);
    const int stop_bits = options_stop_.selected_index_value();  /* 1 or 2 */
    const auto frame_bits = rtty_build_frame_bits(codes, stop_bits);
    if (frame_bits.empty()) return;

    const auto packed = pack_bits_msb(frame_bits);

    const double baud = options_baud_.selected_index_value() / 100.0;  /* centibaud */
    const double shift = options_shift_.selected_index_value();
    double deviation = shift / 2.0;
    if (check_inverted_.value()) deviation = -deviation;

    dsp::FskKeyer keyer;
    keyer.configure(static_cast<float>(kSampleRate), static_cast<float>(baud),
                    static_cast<float>(deviation));
    keyer.set_data(packed.data(), frame_bits.size());

    std::vector<dsp::cfloat> chunk(4096);
    size_t guard = 0;
    while (!keyer.done()) {
        const size_t n = keyer.process(chunk.data(), chunk.size());
        if (n == 0) break;
        waveform_.insert(waveform_.end(), chunk.begin(),
                         chunk.begin() + static_cast<std::ptrdiff_t>(n));
        if (++guard > 200000) break;  /* safety cap */
    }
}

void RttyTxView::start_tx() {
    if (message_.empty()) {
        ui::display_modal("Empty", "Set a message first.");
        return;
    }

    build_waveform();
    if (waveform_.empty()) {
        ui::display_modal("Error", "Nothing to send.");
        return;
    }

    auto* tx = globals().transmitter;
    if (!tx) {
        text_status_.set(STR_COLOR_YELLOW "No transmitter (needs B200).");
        return;
    }

    play_pos_.store(0);
    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(kSampleRate);
    tx->set_target_frequency(field_frequency_.value());
    tx->set_iq_source([this](dsp::cfloat* out, size_t n) -> size_t {
        const size_t total = waveform_.size();
        size_t pos = play_pos_.load();
        if (pos >= total) return 0;
        size_t avail = std::min(n, total - pos);
        std::copy(waveform_.begin() + static_cast<std::ptrdiff_t>(pos),
                  waveform_.begin() + static_cast<std::ptrdiff_t>(pos + avail), out);
        play_pos_.store(pos + avail);
        return avail;
    });

    if (!tx->start()) {
        tx->set_iq_source(nullptr);
        text_status_.set(STR_COLOR_YELLOW "TX start failed (needs B200).");
        return;
    }

    transmitting_.store(true);
    button_tx_.set_text("Stop TX");
    const double secs = static_cast<double>(waveform_.size()) / kSampleRate;
    text_status_.set(STR_COLOR_GREEN "Sending " + to_string_decimal((float)secs, 1) + "s");
}

void RttyTxView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    if (transmitting_.exchange(false)) {
        button_tx_.set_text("Start TX");
        text_status_.set("Idle");
    }
}

void RttyTxView::on_frame_sync() {
    View::on_frame_sync();
    if (!transmitting_.load()) return;
    if (play_pos_.load() >= waveform_.size()) {
        stop_tx();
        text_status_.set(STR_COLOR_LIGHT_GREY "Sent.");
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream menu_location TX, iconColor yellow. Genuinely functional on the host:
 * it builds a correct 2FSK Baudot waveform and streams it to the transmitter. RF
 * only radiates with a USRP B200 attached, so hardware_limited stays false. */
const app::Registrar reg_rtty_tx{{
    "rtty_tx", "RTTY TX", app::Category::Transmit,
    ui::Color::yellow(), &ui::bitmap_icon_transmit,
    [] { return std::make_unique<app::RttyTxView>(); }}};
}  // namespace
