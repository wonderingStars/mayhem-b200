/*
 * mayhem-b200 — hardware self-test.
 *
 * Drives the real radio backend (radio::UsrpRadio) against an attached USRP and
 * reports quantitative results: the capabilities read back from the device, and
 * for a set of frequencies the measured band power, sample throughput and
 * overflow count. This is the hardware counterpart of the unit tests — it needs
 * a device and proves the RX path end to end with numbers, not screenshots.
 *
 * Build:  cmake --build build --target mb200_hwtest
 * Run:    build\mb200_hwtest.exe
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "dsp/demod.hpp"
#include "radio/usrp_radio.hpp"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? " OK " : "FAIL", what);
    if (!cond) failures++;
}

/* Reads whatever is in the RX ring right now and returns its power in dBFS,
 * plus the sample count seen. */
double measure_band_power(radio::UsrpRadio& radio, size_t& out_samples) {
    std::vector<std::complex<float>> buf(65536);
    const size_t got = radio.rx_ring().read(buf.data(), buf.size());
    out_samples = got;
    if (got == 0) return -200.0;
    return dsp::to_db(dsp::rms(buf.data(), got));
}

}  // namespace

int main(int argc, char** argv) {
    const std::string args = (argc > 1) ? argv[1] : "";

    std::printf("=== mayhem-b200 hardware self-test ===\n\n");

    radio::UsrpRadio radio;

    std::printf("[1] Opening device...\n");
    if (!radio.open(args)) {
        std::printf("  FAIL could not open: %s\n", radio.last_error().c_str());
        return 2;
    }
    const auto& caps = radio.caps();
    std::printf("  opened: %s  serial=%s\n", caps.mboard.c_str(), caps.serial.c_str());

    std::printf("\n[2] Capabilities read from the hardware:\n");
    std::printf("  RX freq : %.3f - %.3f MHz\n", caps.rx_freq.min / 1e6, caps.rx_freq.max / 1e6);
    std::printf("  RX gain : %.1f - %.1f dB\n", caps.rx_gain.min, caps.rx_gain.max);
    std::printf("  RX rate : %.3f - %.3f Msps\n", caps.rx_rate.min / 1e6, caps.rx_rate.max / 1e6);
    std::printf("  RX bw   : %.3f - %.3f MHz\n", caps.rx_bandwidth.min / 1e6, caps.rx_bandwidth.max / 1e6);
    std::printf("  master clock : %.3f MHz\n", caps.master_clock_rate / 1e6);
    std::printf("  antennas:");
    for (const auto& a : caps.rx_antennas) std::printf(" %s", a.c_str());
    std::printf("\n");
    check(caps.rx_freq.max >= 5.9e9, "tuning range reaches ~6 GHz");
    check(caps.rx_gain.max >= 70.0, "gain range is B200-class (>=70 dB)");
    check(!caps.serial.empty(), "device reported a serial number");

    /* USB 2 tops out well below 61.44 Msps; pick a rate the bus can sustain. */
    std::printf("\n[3] Setting sample rate...\n");
    const double want_rate = 2.4e6;
    const double got_rate = radio.set_rx_rate(want_rate);
    radio.set_rx_bandwidth(got_rate * 0.75);
    std::printf("  asked %.3f Msps, got %.3f Msps\n", want_rate / 1e6, got_rate / 1e6);
    check(got_rate > 1.0e6, "sample rate accepted");

    std::printf("\n[4] Gain-sweep sanity at 100 MHz (band power should rise with gain):\n");
    radio.set_rx_frequency(100e6);
    if (!radio.start_rx()) {
        std::printf("  FAIL start_rx: %s\n", radio.last_error().c_str());
        radio.close();
        return 3;
    }
    std::this_thread::sleep_for(300ms);  /* let the LO settle + ring fill */

    double last_db = -200.0;
    bool rose = true;
    for (double g : {0.0, 20.0, 40.0, 60.0}) {
        radio.set_rx_gain(g);
        std::this_thread::sleep_for(250ms);
        size_t n = 0;
        radio.rx_ring().clear();
        std::this_thread::sleep_for(150ms);
        const double db = measure_band_power(radio, n);
        std::printf("  gain %4.0f dB -> %7.2f dBFS  (%zu samples)\n", g, db, n);
        if (g > 0.0 && db < last_db - 3.0) rose = false;
        last_db = db;
    }
    check(rose, "band power increases (or holds) as gain increases");

    std::printf("\n[5] Tuning across the band (FM / air / ISM / ADS-B / cell):\n");
    struct Spot { double hz; const char* label; };
    const Spot spots[] = {
        {100.9e6, "FM broadcast"},
        {124.0e6, "airband"},
        {315.0e6, "ISM 315"},
        {433.92e6, "ISM 433"},
        {868.0e6, "ISM 868"},
        {1090.0e6, "ADS-B"},
        {1575.42e6, "GPS L1"},
        {2437.0e6, "WiFi ch6"},
    };
    radio.set_rx_gain(40.0);
    int tuned_ok = 0;
    for (const auto& s : spots) {
        const double got = radio.set_rx_frequency(s.hz);
        std::this_thread::sleep_for(200ms);
        radio.rx_ring().clear();
        std::this_thread::sleep_for(120ms);
        size_t n = 0;
        const double db = measure_band_power(radio, n);
        const bool on_freq = std::abs(got - s.hz) < 1e3;
        if (on_freq && n > 0) tuned_ok++;
        std::printf("  %-14s %8.3f MHz -> tuned %8.3f MHz, %7.2f dBFS (%zu samp)%s\n",
                    s.label, s.hz / 1e6, got / 1e6, db, n, on_freq ? "" : "  <-- OFF FREQ");
    }
    check(tuned_ok == (int)(sizeof(spots) / sizeof(spots[0])),
          "tuned to and streamed at every test frequency across 100 MHz - 2.4 GHz");

    std::printf("\n[6] Throughput / overflow check (2 s at %.2f Msps):\n", got_rate / 1e6);
    radio.set_rx_frequency(100e6);
    radio.stats().reset();
    radio.rx_ring().clear();
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t drained = 0;
    std::vector<std::complex<float>> buf(65536);
    while (std::chrono::steady_clock::now() - t0 < 2s) {
        drained += radio.rx_ring().read(buf.data(), buf.size());
        std::this_thread::sleep_for(2ms);
    }
    const auto& st = radio.stats();
    std::printf("  drained %.2f Msamp in 2 s (%.2f Msps effective)\n",
                drained / 1e6, drained / 2e6);
    std::printf("  overflows=%u  rx_dropped=%u  errors=%u\n",
                st.overflows.load(), st.rx_dropped.load(), st.errors.load());
    check(drained > 1'000'000, "sustained a real sample stream for 2 s");
    check(st.errors.load() == 0, "no stream errors");

    std::printf("\n[7] RSSI readout (receiver's own dBFS estimate):\n");
    std::printf("  rx_level_db = %.2f dBFS\n", radio.rx_level_db());
    check(radio.rx_level_db() > -200.0, "receiver produced a live level estimate");

    radio.stop_rx();
    radio.close();

    std::printf("\n=== %d check(s) failed ===\n", failures);
    return failures == 0 ? 0 : 1;
}
