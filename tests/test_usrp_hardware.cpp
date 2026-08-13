/*
 * mayhem-b200 — hardware-gated USRP tests.
 *
 * Everything in this file talks to a REAL, attached USRP. It exists because
 * two of this project's worst bugs were invisible to every fake: the
 * master-clock streamer kill (an app switch left the receiver silently deaf)
 * and the idle-link keepalive gap. Logic that only a real radio can exercise
 * gets its regression tests here, against the real radio.
 *
 * Gating: these tests run only when MB200_HW_TESTS=1 is set in the
 * environment. Unset, each prints a LOUD skip line and asserts nothing —
 * a silent skip is how a suite quietly loses coverage (see the split-loop
 * lesson in test_adsb_tap.cpp). Set, an absent or unopenable device is a
 * FAILURE, not a skip: the operator explicitly said hardware is attached,
 * so "cannot open it" is a real finding (radio held by another process,
 * cable fault, driver state), never something to shrug off.
 *
 * The TX test transmits ZEROS at minimum gain. A zero-valued sample vector
 * drives the DAC at zero amplitude, so the whole software path — streamer
 * creation, worker thread, ring hand-off, send loop, counters — is exercised
 * with nothing radiated, whatever is or is not connected to the TX port.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "usrp_radio.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

bool hw_tests_enabled() {
    const char* v = std::getenv("MB200_HW_TESTS");
    if (v != nullptr && v[0] == '1') return true;
    std::printf("  [ SKIP ] hardware test skipped: set MB200_HW_TESTS=1 with a USRP attached to run it\n");
    return false;
}

/* Opens the radio, retrying briefly. Two hardware tests in one process open
 * the device back-to-back, and a B200 needs a moment after close() before the
 * USB interface can be claimed again — a single-shot open failed roughly one
 * suite run in twenty on exactly that edge. Retrying is what a real
 * application does too; three attempts spanning ~3 s is far past the settling
 * time while still failing fast when the radio is genuinely absent. */
bool open_with_retry(radio::UsrpRadio& r) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (r.open("")) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    return r.open("");
}

/* Reads from the radio's rx ring until `want` samples arrived or `timeout`
 * passed. Returns the number actually read. */
size_t drain_rx(radio::UsrpRadio& r, size_t want, std::chrono::milliseconds timeout) {
    std::vector<radio::cfloat> buf(8192);
    size_t total = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (total < want && std::chrono::steady_clock::now() < deadline) {
        const size_t got = r.rx_ring().read(buf.data(), buf.size());
        if (got == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        total += got;
    }
    return total;
}

}  // namespace

/* The regression test for the 0.11.3 fix. set_rx_rate on a B200 can move the
 * master clock; UHD then re-initialises the radio underneath the rx_streamer
 * already handed out, and without the rebuild the stale streamer returns
 * nothing forever — no error, just silence. Observed as "Search receives
 * nothing when opened from another app" (2026-08-13). This drives the exact
 * sequence on the real radio: stream, force a clock move, and require that
 * samples keep arriving. */
TEST(usrp_hw_clock_move_mid_stream_keeps_receiving) {
    if (!hw_tests_enabled()) return;

    radio::UsrpRadio r;
    CHECK(open_with_retry(r));
    if (!r.is_open()) return; /* CHECK already recorded the failure */

    /* Probe which master clock UHD selects per rate, and REQUIRE a pair that
     * differs — a pair sharing a clock never triggers the rebuild and turns
     * this whole test into a vacuous pass. That is not hypothetical: the
     * first version of this test streamed at 8 Msps and switched to 2.5 Msps,
     * both of which run at a 40 MHz clock, and it passed identically with the
     * fix disabled. (set_rx_rate now refreshes caps().master_clock_rate on
     * every call, which is what makes this probe readable at all.)
     *
     * On UHD 4.10 with a B200: 2.5/8/10 Msps -> 40 MHz, 3.072 Msps ->
     * 49.152 MHz. The probe walks candidates so a future UHD policy change
     * surfaces as a loud failure here rather than silent vacuity. */
    const double candidates[] = {3.072e6, 2.5e6, 8e6, 6.144e6, 10e6, 4.096e6};
    double rate_first = 0.0, rate_second = 0.0;
    r.set_rx_rate(candidates[0]);
    const double clock_first = r.caps().master_clock_rate;
    for (size_t i = 1; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        r.set_rx_rate(candidates[i]);
        if (r.caps().master_clock_rate != clock_first) {
            rate_first = candidates[0];
            rate_second = candidates[i];
            break;
        }
    }
    std::printf("  clock probe: %.3f Msps -> %.3f MHz, %.3f Msps -> %.3f MHz\n",
                rate_first / 1e6, clock_first / 1e6,
                rate_second / 1e6, r.caps().master_clock_rate / 1e6);
    CHECK(rate_second != 0.0); /* no clock-moving pair found: the trigger is gone, investigate */
    if (rate_second == 0.0) {
        r.close();
        return;
    }

    r.set_rx_frequency(915e6);
    r.set_rx_gain(40.0);
    r.set_rx_rate(rate_first);
    const double clock_streaming = r.caps().master_clock_rate;

    CHECK(r.start_rx());

    /* Phase 1: streaming at the current rate delivers samples. */
    const size_t before = drain_rx(r, 100'000, std::chrono::seconds(5));
    CHECK(before >= 100'000);

    /* Phase 2: move the clock mid-stream — the bug's trigger — and prove it
     * actually moved, so this test can never quietly regress to testing a
     * plain rate change. */
    r.set_rx_rate(rate_second);
    CHECK(r.caps().master_clock_rate != clock_streaming);

    /* Phase 3: samples must keep arriving on the rebuilt streamer. Without
     * the 0.11.3 rebuild this read 0 forever. Generous timeout: the rebuild
     * re-issues the stream command and the B200 re-locks its clocking. */
    const size_t after = drain_rx(r, 100'000, std::chrono::seconds(8));
    if (after < 100'000) {
        std::printf("  after clock move: only %zu samples in 8s (errors=%u timeouts=%u)\n",
                    after, r.stats().errors.load(), r.stats().timeouts.load());
    }
    CHECK(after >= 100'000);

    r.stop_rx();
    r.close();
}

/* First-ever exercise of the transmit plumbing against real hardware. Zeros
 * at minimum gain: nothing is radiated, but every stage of the software path
 * has to work for tx_samples to climb — get_tx_stream, the worker thread,
 * the ring hand-off, stream->send, the counter. */
TEST(usrp_hw_tx_plumbing_moves_zero_samples) {
    if (!hw_tests_enabled()) return;

    radio::UsrpRadio r;
    CHECK(open_with_retry(r));
    if (!r.is_open()) return;

    r.set_tx_gain(r.caps().tx_gain.min); /* 0 dB on a B200 */
    r.set_tx_frequency(2.45e9);          /* ISM band, and it does not matter: the samples are zeros */
    r.set_tx_rate(1e6);

    CHECK(r.start_tx());
    CHECK(r.tx_running());

    /* Feed zeros. The TX thread also zero-pads on underrun, so the DAC input
     * is all-zero from start to stop whatever the ring timing does. */
    std::vector<radio::cfloat> zeros(8192, radio::cfloat{0.0f, 0.0f});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        r.tx_ring().write(zeros.data(), zeros.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    const uint64_t sent = r.stats().tx_samples.load();
    if (sent == 0) {
        std::printf("  tx_samples stayed 0 (errors=%u underflows=%u)\n",
                    r.stats().errors.load(), r.stats().underflows.load());
    }
    /* ~2s at 1 Msps should send on the order of 2M samples; require far less
     * so a slow first lock cannot flake this, but far more than noise. */
    CHECK(sent > 100'000);

    r.stop_tx();
    CHECK(!r.tx_running());
    r.close();
}
