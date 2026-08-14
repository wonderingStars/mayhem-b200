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

#include "../src/dsp/fft.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

/* --- Transmit frequency verification ----------------------------------------
 *
 * Self-loopback: the B200 is full duplex, so it can transmit on the TX/RX
 * port while receiving on RX2, and the internal port-to-port leakage is
 * ample signal at bench gains with NOTHING connected. A tone transmitted at
 * a known baseband offset must appear in the received spectrum at exactly
 * that offset: if it does, the DAC path, the upconverter and the LO
 * programming are all putting energy where they were told to. Two widely
 * separated centres exercise the LO across divider ranges.
 *
 * The offset is +100 kHz, not 0: a direct-conversion radio has an LO leakage
 * spike at DC on both sides, so energy AT the centre proves nothing. The
 * search excludes a guard band around DC for the same reason.
 *
 * What this cannot see: an absolute error in the shared reference crystal —
 * TX and RX derive from the same TCXO, so a ppm offset cancels. That is
 * bounded separately by off-air evidence (ADS-B at 1090 MHz decodes with
 * correct CPR positions, which a meaningfully wrong reference would not).
 *
 * Emission: tx gain starts at the caps minimum and only escalates while no
 * peak is found, capped well below half scale, with no antenna connected —
 * leakage inside the enclosure is the whole point. */

namespace {

struct TonePeak {
    double freq_hz{0.0};
    float level_db{-200.0f};
    float noise_db{0.0f};
};

/* Strongest bin outside ±dc_guard_hz, plus the median level as a noise
 * reference. */
TonePeak find_tone(const std::vector<radio::cfloat>& samples, double rate, double dc_guard_hz) {
    const size_t n = 16384;
    dsp::Fft fft{n};
    const auto window = dsp::make_window(dsp::WindowType::Hann, n);
    std::vector<float> db;
    fft.spectrum_db(samples.data(), window, db);

    const double hz_per_bin = rate / static_cast<double>(n);
    TonePeak peak;
    std::vector<float> sorted;
    sorted.reserve(db.size());
    for (size_t i = 0; i < db.size(); i++) {
        const double f = (static_cast<double>(i) - static_cast<double>(n) / 2.0) * hz_per_bin;
        sorted.push_back(db[i]);
        if (std::fabs(f) < dc_guard_hz) continue;
        if (db[i] > peak.level_db) {
            peak.level_db = db[i];
            peak.freq_hz = f;
        }
    }
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<ptrdiff_t>(sorted.size() / 2), sorted.end());
    peak.noise_db = sorted[sorted.size() / 2];
    return peak;
}

/* One band: tone at center+offset out of TX/RX, listen on RX2, return the
 * measured offset of the strongest non-DC bin. */
bool loopback_tone_check(radio::UsrpRadio& r, double center_hz, double offset_hz, TonePeak& out) {
    constexpr double kRate = 1e6;

    r.set_rx_antenna("RX2");
    r.set_rx_rate(kRate);
    r.set_rx_frequency(center_hz);
    r.set_rx_gain(60.0);
    r.set_tx_rate(kRate);
    r.set_tx_frequency(center_hz);
    r.set_tx_gain(r.caps().tx_gain.min);

    if (!r.start_tx()) return false;
    if (!r.start_rx()) {
        r.stop_tx();
        return false;
    }

    /* Continuous-phase tone fed well ahead of the TX thread so underrun
     * zero-padding cannot splatter the spectrum. */
    std::atomic<bool> feed_stop{false};
    std::thread feeder([&] {
        std::vector<radio::cfloat> block(8192);
        double phase = 0.0;
        const double step = 2.0 * 3.14159265358979323846 * offset_hz / kRate;
        while (!feed_stop.load()) {
            for (auto& s : block) {
                s = radio::cfloat{0.35f * static_cast<float>(std::cos(phase)),
                                  0.35f * static_cast<float>(std::sin(phase))};
                phase += step;
                if (phase > 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
            }
            r.tx_ring().write(block.data(), block.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    /* Escalate TX gain only while nothing shows: leakage at minimum gain is
     * usually enough, but isolation varies by band. 40 dB is still far below
     * anything that matters without an antenna. */
    const double gains[] = {r.caps().tx_gain.min, 20.0, 40.0};
    bool found = false;
    for (double g : gains) {
        r.set_tx_gain(g);
        /* Let the front end settle, then collect fresh samples. */
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        r.rx_ring().clear();
        std::vector<radio::cfloat> buf(16384), all;
        all.reserve(65536);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (all.size() < 65536 && std::chrono::steady_clock::now() < deadline) {
            const size_t got = r.rx_ring().read(buf.data(), buf.size());
            if (got == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            all.insert(all.end(), buf.begin(), buf.begin() + static_cast<ptrdiff_t>(got));
        }
        if (all.size() < 16384) continue;

        out = find_tone(all, kRate, 20e3);
        std::printf("  %.3f MHz, tx gain %.0f dB: peak %+.0f Hz at %.1f dB (noise %.1f dB)\n",
                    center_hz / 1e6, g, out.freq_hz, out.level_db, out.noise_db);
        if (out.level_db - out.noise_db > 20.0f) {
            found = true;
            break;
        }
    }

    feed_stop.store(true);
    feeder.join();
    r.stop_rx();
    r.stop_tx();
    return found;
}

}  // namespace

TEST(usrp_hw_tx_tone_lands_on_the_programmed_frequency) {
    if (!hw_tests_enabled()) return;

    radio::UsrpRadio r;
    CHECK(open_with_retry(r));
    if (!r.is_open()) return;

    constexpr double kOffset = 100e3;
    /* Two centres in licence-free bands, far apart so the LO's divider
     * ranges both get exercised. */
    const double centers[] = {433.92e6, 2.45e9};

    for (double c : centers) {
        TonePeak peak;
        const bool found = loopback_tone_check(r, c, kOffset, peak);
        CHECK(found); /* no tone above the floor at any gain: TX chain dead at this band */
        if (!found) continue;

        /* The bin grid is 61 Hz at this FFT size; 500 Hz of slack covers
         * windowing spill without ever confusing +100 kHz for anything else. */
        CHECK(std::fabs(peak.freq_hz - kOffset) < 500.0);
    }

    r.close();
}
