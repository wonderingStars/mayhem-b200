/*
 * mayhem-b200 — browser-driven Morse (CW) transmit. See morse_tx.hpp.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "morse_tx.hpp"

#include "../apps/app_context.hpp"
#include "../apps/ui_morse_tx.hpp" /* morse_encode, morse_symbols, timing, MorseSymbol */
#include "../radio/receiver_model.hpp"
#include "../radio/transmitter_model.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

namespace remote {
namespace {

/* Baseband rate for the CW envelope. Well above the B200's 200 kSps floor and
 * a generous oversample of even fast keying. The carrier is the tuned RF
 * frequency, so at complex baseband a keyed-on element is just constant
 * amplitude and a gap is zero — this builds that envelope. */
constexpr double kTxRateHz = 500'000.0;

/* Raised-cosine edge, in milliseconds, to keep hard keying from splattering
 * as clicks — standard CW shaping. Short relative to a dot (a dot at 40 wpm is
 * 30 ms). */
constexpr double kEdgeMs = 4.0;

struct State {
    std::mutex mu;

    bool pending{false};
    std::string text;
    uint16_t wpm{18};
    uint64_t freq_hz{0};

    bool active{false};
    std::vector<radio::cfloat> iq;   /* stable while active; built before start, cleared after stop */
    std::atomic<size_t> pos{0};      /* read/written by the transmitter's DSP thread */
};

State& state() {
    static State s;
    return s;
}

bool tx_capable(radio::RadioDevice* radio) {
    return radio != nullptr && radio->is_open() && radio->caps().has_tx;
}

/* Build the CW envelope for `symbols` at kTxRateHz, real amplitude in the I
 * channel (Q zero). ON elements ramp up/down with a raised cosine. */
void build_envelope(const std::vector<uint8_t>& symbols, uint16_t wpm,
                    std::vector<radio::cfloat>& out) {
    const uint32_t unit_ms = app::morse_tx::morse_time_unit_ms(wpm);
    const double unit_samples = (static_cast<double>(unit_ms) * kTxRateHz) / 1000.0;
    const size_t edge = static_cast<size_t>((kEdgeMs * kTxRateHz) / 1000.0);

    out.clear();
    for (uint8_t sym : symbols) {
        if (sym >= 5) continue;
        const size_t len = static_cast<size_t>(unit_samples * app::morse_tx::morse_symbols[sym] + 0.5);
        const bool on = (sym == app::morse_tx::DOT || sym == app::morse_tx::DASH);
        const size_t base = out.size();
        out.resize(base + len, radio::cfloat{0.0f, 0.0f});
        if (!on) continue;

        for (size_t i = 0; i < len; i++) {
            float a = 1.0f;
            if (edge > 0 && i < edge)
                a = 0.5f * (1.0f - std::cos(3.14159265358979323846f * static_cast<float>(i) / static_cast<float>(edge)));
            else if (edge > 0 && i >= len - edge)
                a = 0.5f * (1.0f - std::cos(3.14159265358979323846f * static_cast<float>(len - 1 - i) / static_cast<float>(edge)));
            out[base + i] = radio::cfloat{a, 0.0f};
        }
    }
}

}  // namespace

MorseTxResult morse_tx_request(const std::string& text, uint16_t wpm) {
    auto& s = state();
    std::lock_guard<std::mutex> lk{s.mu};

    auto& ctx = app::globals();
    if (ctx.transmitter == nullptr || !tx_capable(ctx.radio))
        return {false, "no transmit radio", 0, 0};

    /* Refuse if the radio is already transmitting — a TX app (Key Fob, Morse
     * TX) or a previous request may hold it. Never key over an active TX. */
    if (s.active || s.pending || ctx.transmitter->running())
        return {false, "radio busy transmitting", 0, 0};

    const uint16_t clamped = std::clamp<uint16_t>(wpm == 0 ? 18 : wpm, 5, 60);

    std::vector<uint8_t> symbols;
    if (app::morse_tx::morse_encode(text, symbols) == 0)
        return {false, "nothing to send (empty or too long)", 0, 0};

    /* Transmit where the app is tuned: the receiver's frequency if it has one,
     * else the transmitter's own. It must be inside the device's TX range. */
    uint64_t freq = (ctx.receiver != nullptr) ? ctx.receiver->target_frequency()
                                              : ctx.transmitter->target_frequency();
    if (freq == 0) freq = ctx.transmitter->target_frequency();
    if (!ctx.radio->caps().tx_freq.contains(static_cast<double>(freq)))
        return {false, "tune to a frequency the radio can transmit on first", 0, freq};

    const uint32_t units = app::morse_tx::morse_time_units(symbols);
    const uint64_t duration_ms = static_cast<uint64_t>(units) * app::morse_tx::morse_time_unit_ms(clamped);

    s.pending = true;
    s.text = text;
    s.wpm = clamped;
    s.freq_hz = freq;

    return {true, "", duration_ms, freq};
}

void morse_tx_tick() {
    auto& s = state();
    std::lock_guard<std::mutex> lk{s.mu};
    auto& ctx = app::globals();

    if (s.active) {
        const bool finished = s.pos.load() >= s.iq.size();
        const bool externally_stopped = (ctx.transmitter == nullptr) || !ctx.transmitter->running();
        if (finished || externally_stopped) {
            if (ctx.transmitter != nullptr) {
                ctx.transmitter->stop();
                ctx.transmitter->set_iq_source(nullptr);
            }
            s.active = false;
            s.iq.clear();
            s.pos.store(0);
        }
        return;
    }

    if (!s.pending) return;
    s.pending = false;

    if (ctx.transmitter == nullptr || !tx_capable(ctx.radio)) return; /* went away since the request */
    if (ctx.transmitter->running()) return;                           /* a TX app grabbed it meanwhile */

    std::vector<uint8_t> symbols;
    if (app::morse_tx::morse_encode(s.text, symbols) == 0) return;

    build_envelope(symbols, s.wpm, s.iq);
    if (s.iq.empty()) return;
    s.pos.store(0);

    ctx.transmitter->set_mode(radio::TransmitterModel::Mode::Raw);
    ctx.transmitter->set_sampling_rate(kTxRateHz);
    ctx.transmitter->set_target_frequency(s.freq_hz);
    ctx.transmitter->set_iq_source([](radio::cfloat* out, size_t n) -> size_t {
        /* Runs on the transmitter's DSP thread. iq is stable while active and
         * pos is atomic, so no lock — and taking one here would block the DSP
         * thread on the UI thread. */
        auto& st = state();
        const size_t pos = st.pos.load();
        const size_t remaining = (pos < st.iq.size()) ? (st.iq.size() - pos) : 0;
        const size_t take = std::min(n, remaining);
        for (size_t i = 0; i < take; i++) out[i] = st.iq[pos + i];
        st.pos.store(pos + take);
        return take;
    });

    if (ctx.transmitter->start()) {
        s.active = true;
    } else {
        ctx.transmitter->set_iq_source(nullptr);
        s.iq.clear();
    }
}

bool morse_tx_active() {
    auto& s = state();
    std::lock_guard<std::mutex> lk{s.mu};
    return s.active;
}

}  // namespace remote
