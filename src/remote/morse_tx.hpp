/*
 * mayhem-b200 — browser-driven Morse (CW) transmit.
 *
 * The panels are otherwise read-only; this is the one path where the browser
 * keys the radio. POST /api/morse/transmit calls morse_tx_request() on the
 * HTTP thread, which ONLY validates and queues — every TransmitterModel call
 * happens later on the UI thread in morse_tx_tick(), driven from
 * AppBridge::refresh(), so the radio is never touched from two threads. The
 * queued waveform auto-stops when it finishes, exactly as Key Fob's TX does.
 *
 * Safety: a request is refused unless a transmit-capable radio is open and
 * idle, the text encodes to something, and the frequency (the one the app is
 * tuned to) is inside the device's TX range. CW transmit is licensed almost
 * everywhere; the browser panel carries that warning, and this layer will not
 * transmit without a real radio behind it.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef MB200_REMOTE_MORSE_TX_HPP
#define MB200_REMOTE_MORSE_TX_HPP

#include <cstdint>
#include <string>

namespace remote {

struct MorseTxResult {
    bool ok{false};
    std::string error{};   /* set when !ok */
    uint64_t duration_ms{0};
    uint64_t frequency_hz{0};
};

/* HTTP thread: validate the request and, on success, queue it. Never touches
 * the transmitter — that is morse_tx_tick's job. wpm is clamped to [5, 60]. */
MorseTxResult morse_tx_request(const std::string& text, uint16_t wpm);

/* UI thread only (AppBridge::refresh): start a queued transmit, and stop one
 * that has finished. A no-op when nothing is queued or in flight. */
void morse_tx_tick();

/* True while a browser-queued transmit is keying the radio. */
bool morse_tx_active();

}  // namespace remote

#endif  // MB200_REMOTE_MORSE_TX_HPP
