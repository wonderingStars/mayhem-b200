/*
 * mayhem-b200 — audio output via ALSA.
 *
 * The Linux counterpart of audio_out.cpp. Same audio::AudioOut contract: the
 * caller writes mono floats into a ring buffer and a feeder thread drains it
 * into the device. waveOut wants a queue of blocks it owns; ALSA wants blocking
 * writes of one period, so the feeder here writes rather than waiting on
 * completion callbacks — but the volume, mute, peak and underrun accounting is
 * the same arithmetic in the same order.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#if !defined(_WIN32)

#include "audio_out.hpp"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

namespace audio {

struct AudioOut::Impl {
    snd_pcm_t* pcm{nullptr};

    std::unique_ptr<dsp::RingBuffer<float>> ring;

    std::thread feeder;
    std::atomic<bool> stop{false};

    uint32_t rate{sample_rate};
    size_t block_frames{512};
};

namespace {

int16_t to_pcm16(float x) {
    /* Clamp before scaling so a hot signal saturates rather than wrapping. */
    x = std::clamp(x, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lrint(x * 32767.0f));
}

}  // namespace

AudioOut::AudioOut() = default;

AudioOut::~AudioOut() {
    stop();
}

bool AudioOut::start(uint32_t rate, size_t block_frames, size_t block_count) {
    stop();

    if (block_frames == 0) block_frames = 512;
    if (block_count < 2) block_count = 2;

    impl_ = new Impl();
    impl_->rate = rate;
    impl_->block_frames = block_frames;

    /* Ring holds several blocks' worth so a slow DSP pass does not starve the
     * device before the next one lands. */
    impl_->ring = std::make_unique<dsp::RingBuffer<float>>(block_frames * block_count * 4);

    int err = snd_pcm_open(&impl_->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        last_error_ = std::string{"snd_pcm_open failed: "} + snd_strerror(err);
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    /* block_frames/block_count map onto ALSA's period/buffer, so the latency
     * this costs matches the waveOut path's for the same arguments. */
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
        last_error_ = std::string{"ALSA playback setup failed: "} + snd_strerror(err);
        snd_pcm_close(impl_->pcm);
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    impl_->rate = actual_rate;
    impl_->block_frames = period;

    impl_->stop.store(false);
    running_.store(true);

    impl_->feeder = std::thread([this] {
        Impl* impl = impl_;
        std::vector<float> scratch(impl->block_frames);
        std::vector<int16_t> pcm(impl->block_frames);

        while (!impl->stop.load()) {
            const size_t got = impl->ring->read(scratch.data(), scratch.size());
            if (got < scratch.size()) {
                std::fill(scratch.begin() + static_cast<ptrdiff_t>(got), scratch.end(), 0.0f);
                if (got == 0) underruns_.fetch_add(1);
            }

            const float gain = muted_.load() ? 0.0f : static_cast<float>(volume_.load()) / 99.0f;

            float peak = 0.0f;
            for (size_t s = 0; s < scratch.size(); s++) {
                const float v = scratch[s] * gain;
                peak = std::max(peak, std::fabs(v));
                pcm[s] = to_pcm16(v);
            }

            /* Publish the running peak for the level meter. */
            float previous = peak_.load();
            while (peak > previous && !peak_.compare_exchange_weak(previous, peak)) {
            }

            const snd_pcm_sframes_t written =
                snd_pcm_writei(impl->pcm, pcm.data(), pcm.size());
            if (written < 0) {
                /* -EPIPE is an underrun: the device drained while the DSP was
                 * behind. Recover and carry on rather than killing playback —
                 * waveOut simply plays the next queued block in that case. */
                const int r = snd_pcm_recover(impl->pcm, static_cast<int>(written), 1);
                if (r < 0) break;
                underruns_.fetch_add(1);
            }
        }
    });

    return true;
}

void AudioOut::stop() {
    if (impl_ == nullptr) return;

    impl_->stop.store(true);
    if (impl_->feeder.joinable()) impl_->feeder.join();

    if (impl_->pcm != nullptr) {
        snd_pcm_drop(impl_->pcm);
        snd_pcm_close(impl_->pcm);
    }

    delete impl_;
    impl_ = nullptr;
    running_.store(false);
}

size_t AudioOut::write(const float* samples, size_t count) {
    if (impl_ == nullptr || !impl_->ring) return 0;
    return impl_->ring->write(samples, count);
}

size_t AudioOut::space() const {
    if (impl_ == nullptr || !impl_->ring) return 0;
    return impl_->ring->space();
}

void AudioOut::set_volume(uint8_t volume_0_99) {
    volume_.store(std::min<uint8_t>(volume_0_99, 99));
}

float AudioOut::take_peak() {
    return peak_.exchange(0.0f);
}

}  // namespace audio

#endif /* !_WIN32 */
