/*
 * mayhem-b200 — audio capture, for the microphone transmit app.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_AUDIO_IN_H__
#define __MB200_AUDIO_IN_H__

#include "../dsp/ring_buffer.hpp"
#include "audio_out.hpp"  /* for audio::sample_rate */

#include <atomic>
#include <cstdint>
#include <string>

namespace audio {

class AudioIn {
   public:
    AudioIn();
    ~AudioIn();

    AudioIn(const AudioIn&) = delete;
    AudioIn& operator=(const AudioIn&) = delete;

    bool start(uint32_t rate = sample_rate,
               size_t block_frames = 512,
               size_t block_count = 4);
    void stop();
    bool running() const { return running_.load(); }

    /* Reads captured mono float samples in [-1, 1]. Returns how many were
     * available; a short read means the caller is ahead of the device. */
    size_t read(float* samples, size_t count);

    size_t available() const;

    /* Peak absolute sample since the last call, for the input level meter. */
    float take_peak();

    uint32_t overruns() const;
    const std::string& last_error() const { return last_error_; }

    struct Impl;

   private:
    Impl* impl_{nullptr};
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> overruns_{0};
    std::atomic<float> peak_{0.0f};
    std::string last_error_{};
};

}  // namespace audio

#endif /*__MB200_AUDIO_IN_H__*/
