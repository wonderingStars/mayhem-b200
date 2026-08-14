/*
 * mayhem-b200 — the audio output goes idle when nothing feeds it.
 *
 * Asked directly by the operator (2026-08-14): "do we need the output in the
 * speakers for the ERT meter?" No — ERT and every other pure decoder produce
 * no audio, yet the output used to keep the DAC clocking blocks of digital
 * silence forever: an always-active audio session and an audible idle hiss
 * on some outputs. The feeder now parks once the ring has been dry for a
 * device-buffer's worth of blocks, and one write() wakes it.
 *
 * These run against the REAL waveOut device (AudioOut is already opened by
 * dozens of harness tests in this suite, so this adds no new hardware
 * dependency). Timings are generous bounds, not tight ones: the idle
 * threshold is a block count, and at 48 kHz with the shipped block size the
 * whole buffer is well under a second.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "audio_out.hpp"

#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

namespace {

bool wait_for(const std::function<bool()>& p, int ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return p();
}

}  // namespace

TEST(audio_out_goes_idle_when_nothing_feeds_it) {
    audio::AudioOut out;
    if (!out.start(48'000)) {
        std::printf("  [ SKIP ] no audio device on this machine\n");
        return;
    }

    /* Never fed: the feeder must park rather than clock silence forever.
     * 10 s is a hang guard, not the expectation — idling takes one buffer. */
    CHECK(wait_for([&] { return out.idle(); }, 10'000));

    out.stop();
}

TEST(audio_out_wakes_from_idle_on_the_first_write) {
    audio::AudioOut out;
    if (!out.start(48'000)) {
        std::printf("  [ SKIP ] no audio device on this machine\n");
        return;
    }
    CHECK(wait_for([&] { return out.idle(); }, 10'000));

    /* Feed a burst of quiet tone; the gate must lift. Keep feeding while we
     * poll so a fast drain cannot re-idle before the check sees it. */
    std::vector<float> tone(4800);
    for (size_t i = 0; i < tone.size(); i++)
        tone[i] = 0.05f * std::sin(static_cast<float>(i) * 0.06f);

    bool woke = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        out.write(tone.data(), tone.size());
        if (!out.idle()) {
            woke = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(woke);

    out.stop();
}
