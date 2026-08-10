/*
 * mayhem-b200 — audio capture via ALSA.
 *
 * The Linux counterpart of audio_in.cpp. Same audio::AudioIn contract. waveIn
 * hands captured blocks to a callback; ALSA is read from, so the equivalent
 * work happens on a capture thread instead — the conversion, peak tracking and
 * overrun accounting are otherwise identical.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#if !defined(_WIN32)

#include "audio_in.hpp"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

namespace audio {

struct AudioIn::Impl {
    snd_pcm_t* pcm{nullptr};
    std::unique_ptr<dsp::RingBuffer<float>> ring;

    std::thread capture;
    std::atomic<bool> stopping{false};
    std::atomic<uint32_t> overruns{0};
    std::atomic<float> peak{0.0f};

    size_t block_frames{512};
};

AudioIn::AudioIn() = default;

AudioIn::~AudioIn() {
    stop();
}

bool AudioIn::start(uint32_t rate, size_t block_frames, size_t block_count) {
    stop();

    if (block_frames == 0) block_frames = 512;
    if (block_count < 2) block_count = 2;

    impl_ = new Impl();
    impl_->ring = std::make_unique<dsp::RingBuffer<float>>(block_frames * block_count * 8);

    int err = snd_pcm_open(&impl_->pcm, "default", SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        last_error_ = std::string{"snd_pcm_open (capture) failed: "} + snd_strerror(err);
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    unsigned int actual_rate = rate;
    snd_pcm_uframes_t period = block_frames;
    snd_pcm_uframes_t buffer = block_frames * block_count;

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(impl_->pcm, hw);

    err = snd_pcm_hw_params_set_access(impl_->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err >= 0) err = snd_pcm_hw_params_set_format(impl_->pcm, hw, SND_PCM_FORMAT_S16_LE);
    if (err >= 0) err = snd_pcm_hw_params_set_channels(impl_->pcm, hw, 1);
    if (err >= 0) err = snd_pcm_hw_params_set_rate_near(impl_->pcm, hw, &actual_rate, nullptr);
    if (err >= 0) err = snd_pcm_hw_params_set_period_size_near(impl_->pcm, hw, &period, nullptr);
    if (err >= 0) err = snd_pcm_hw_params_set_buffer_size_near(impl_->pcm, hw, &buffer);
    if (err >= 0) err = snd_pcm_hw_params(impl_->pcm, hw);
    if (err < 0) {
        last_error_ = std::string{"ALSA capture setup failed: "} + snd_strerror(err);
        snd_pcm_close(impl_->pcm);
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    impl_->block_frames = period;

    err = snd_pcm_start(impl_->pcm);
    if (err < 0) {
        last_error_ = std::string{"snd_pcm_start failed: "} + snd_strerror(err);
        snd_pcm_close(impl_->pcm);
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    impl_->capture = std::thread([impl = impl_] {
        std::vector<int16_t> pcm(impl->block_frames);
        std::vector<float> scratch(impl->block_frames);

        while (!impl->stopping.load()) {
            const snd_pcm_sframes_t n = snd_pcm_readi(impl->pcm, pcm.data(), pcm.size());
            if (n < 0) {
                /* -EPIPE here is an overrun: the ring was not drained fast
                 * enough and the device dropped samples. Recoverable. */
                const int r = snd_pcm_recover(impl->pcm, static_cast<int>(n), 1);
                if (r < 0) break;
                impl->overruns.fetch_add(1);
                continue;
            }

            const size_t frames = static_cast<size_t>(n);
            if (frames == 0) continue;

            float peak = 0.0f;
            for (size_t i = 0; i < frames; i++) {
                const float v = static_cast<float>(pcm[i]) / 32768.0f;
                scratch[i] = v;
                peak = std::max(peak, std::fabs(v));
            }

            float previous = impl->peak.load();
            while (peak > previous && !impl->peak.compare_exchange_weak(previous, peak)) {
            }

            const size_t written = impl->ring->write(scratch.data(), frames);
            if (written < frames) impl->overruns.fetch_add(1);
        }
    });

    running_.store(true);
    return true;
}

void AudioIn::stop() {
    if (impl_ == nullptr) return;

    impl_->stopping.store(true);
    /* The capture thread is the only user of impl_->pcm, so joining it before
     * closing the device is what makes the close below safe. */
    if (impl_->capture.joinable()) impl_->capture.join();

    if (impl_->pcm != nullptr) {
        snd_pcm_drop(impl_->pcm);
        snd_pcm_close(impl_->pcm);
    }

    overruns_.store(impl_->overruns.load());

    delete impl_;
    impl_ = nullptr;
    running_.store(false);
}

size_t AudioIn::read(float* samples, size_t count) {
    if (impl_ == nullptr || !impl_->ring) return 0;
    return impl_->ring->read(samples, count);
}

size_t AudioIn::available() const {
    if (impl_ == nullptr || !impl_->ring) return 0;
    return impl_->ring->size();
}

float AudioIn::take_peak() {
    if (impl_ == nullptr) return 0.0f;
    return impl_->peak.exchange(0.0f);
}

uint32_t AudioIn::overruns() const {
    /* While running the live counter lives in Impl; stop() copies it out so the
     * value survives the device being closed. */
    if (impl_ != nullptr) return impl_->overruns.load();
    return overruns_.load();
}

}  // namespace audio

#endif /* !_WIN32 */
