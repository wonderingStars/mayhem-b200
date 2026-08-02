/*
 * mayhem-b200 — audio output.
 *
 * Stands in for the PortaPack's WM8731/AK4951 codec. waveOut rather than WASAPI:
 * the buffering model is a straightforward queue of blocks, it has no COM
 * apartment requirements to tangle with the Win32 UI thread, and the ~40 ms of
 * latency it costs is irrelevant for listening to a radio.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_AUDIO_OUT_H__
#define __MB200_AUDIO_OUT_H__

#include "../dsp/ring_buffer.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace audio {

/* The rate every demodulator resamples to. 48 kHz is what the B200's audio
 * consumers expect and what every Windows endpoint supports natively. */
constexpr uint32_t sample_rate = 48000;

class AudioOut {
   public:
    AudioOut();
    ~AudioOut();

    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    /* Opens the default output device. `block_frames` sets the latency:
     * block_frames * block_count / sample_rate seconds. */
    bool start(uint32_t rate = sample_rate,
               size_t block_frames = 512,
               size_t block_count = 4);
    void stop();
    bool running() const { return running_.load(); }

    /* Queues mono float samples in [-1, 1]. Returns how many were accepted;
     * a short write means the ring is full, which is the caller's cue that it
     * is producing faster than real time. */
    size_t write(const float* samples, size_t count);

    /* 0..99, matching Mayhem's volume control. Applied in the mixing step. */
    void set_volume(uint8_t volume_0_99);
    uint8_t volume() const { return volume_.load(); }

    void set_muted(bool muted) { muted_.store(muted); }
    bool muted() const { return muted_.load(); }

    /* Peak absolute sample seen since the last call, for the level meter. */
    float take_peak();

    uint32_t underruns() const { return underruns_.load(); }
    const std::string& last_error() const { return last_error_; }

    size_t space() const;

    /* Public only so the waveOut callback, which lives at file scope in
     * audio_out.cpp, can reach it. Not part of the intended API. */
    struct Impl;

   private:
    Impl* impl_{nullptr};

    std::atomic<bool> running_{false};
    std::atomic<uint8_t> volume_{40};
    std::atomic<bool> muted_{false};
    std::atomic<uint32_t> underruns_{0};
    std::atomic<float> peak_{0.0f};
    std::string last_error_{};
};

}  // namespace audio

#endif /*__MB200_AUDIO_OUT_H__*/
