/*
 * mayhem-b200 — audio capture via waveIn.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Windows implementation of audio::AudioIn. audio_in_linux.cpp is the ALSA
 * one; CMake compiles only the file matching the target platform, and this
 * guard keeps that true even if the source globs ever pick up both. */
#if defined(_WIN32)

#include "audio_in.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace audio {

/* Everything the waveIn callback touches lives here, so the callback never
 * reaches back into AudioIn and there is no lifetime question about the owner
 * object while a buffer is still in flight. */
struct AudioIn::Impl {
    HWAVEIN handle{nullptr};
    std::vector<WAVEHDR> headers;
    std::vector<std::vector<int16_t>> blocks;
    std::unique_ptr<dsp::RingBuffer<float>> ring;
    std::vector<float> scratch;

    std::atomic<bool> stopping{false};
    std::atomic<uint32_t> overruns{0};
    std::atomic<float> peak{0.0f};
};

namespace {

void CALLBACK wave_in_proc(HWAVEIN handle, UINT msg, DWORD_PTR instance,
                           DWORD_PTR param1, DWORD_PTR) {
    if (msg != WIM_DATA) return;

    auto* impl = reinterpret_cast<AudioIn::Impl*>(instance);
    if (impl == nullptr || impl->stopping.load()) return;

    auto* hdr = reinterpret_cast<WAVEHDR*>(param1);
    if (hdr == nullptr) return;

    const size_t frames = hdr->dwBytesRecorded / sizeof(int16_t);
    if (frames > 0 && impl->ring) {
        const auto* pcm = reinterpret_cast<const int16_t*>(hdr->lpData);

        if (impl->scratch.size() < frames) impl->scratch.resize(frames);

        float peak = 0.0f;
        for (size_t i = 0; i < frames; i++) {
            const float v = static_cast<float>(pcm[i]) / 32768.0f;
            impl->scratch[i] = v;
            peak = std::max(peak, std::fabs(v));
        }

        float previous = impl->peak.load();
        while (peak > previous && !impl->peak.compare_exchange_weak(previous, peak)) {
        }

        const size_t written = impl->ring->write(impl->scratch.data(), frames);
        if (written < frames) impl->overruns.fetch_add(1);
    }

    /* Requeue. waveInAddBuffer is explicitly permitted from this callback. */
    waveInAddBuffer(handle, hdr, sizeof(WAVEHDR));
}

}  // namespace

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
    impl_->scratch.resize(block_frames);

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = static_cast<WORD>(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    const MMRESULT r = waveInOpen(&impl_->handle, WAVE_MAPPER, &fmt,
                                  reinterpret_cast<DWORD_PTR>(wave_in_proc),
                                  reinterpret_cast<DWORD_PTR>(impl_),
                                  CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) {
        last_error_ = "waveInOpen failed (" + std::to_string(r) + ")";
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    impl_->blocks.resize(block_count);
    impl_->headers.resize(block_count);
    for (size_t i = 0; i < block_count; i++) {
        impl_->blocks[i].assign(block_frames, 0);
        WAVEHDR& h = impl_->headers[i];
        h = WAVEHDR{};
        h.lpData = reinterpret_cast<LPSTR>(impl_->blocks[i].data());
        h.dwBufferLength = static_cast<DWORD>(block_frames * sizeof(int16_t));
        waveInPrepareHeader(impl_->handle, &h, sizeof(WAVEHDR));
        waveInAddBuffer(impl_->handle, &h, sizeof(WAVEHDR));
    }

    if (waveInStart(impl_->handle) != MMSYSERR_NOERROR) {
        last_error_ = "waveInStart failed";
        stop();
        return false;
    }

    running_.store(true);
    return true;
}

void AudioIn::stop() {
    if (impl_ == nullptr) return;

    impl_->stopping.store(true);

    if (impl_->handle) {
        waveInStop(impl_->handle);
        /* waveInReset returns every queued buffer to us; after it completes no
         * further callbacks can fire, so deleting Impl below is safe. */
        waveInReset(impl_->handle);
        for (auto& h : impl_->headers) {
            if (h.dwFlags & WHDR_PREPARED)
                waveInUnprepareHeader(impl_->handle, &h, sizeof(WAVEHDR));
        }
        waveInClose(impl_->handle);
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

#endif /* _WIN32 */
