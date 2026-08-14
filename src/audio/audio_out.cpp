/*
 * mayhem-b200 — audio output via waveOut.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Windows implementation of audio::AudioOut. audio_out_linux.cpp is the ALSA
 * one; CMake compiles only the file matching the target platform, and this
 * guard keeps that true even if the source globs ever pick up both. */
#if defined(_WIN32)

#include "audio_out.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace audio {

struct AudioOut::Impl {
    HWAVEOUT handle{nullptr};

    std::vector<WAVEHDR> headers;
    std::vector<std::vector<int16_t>> blocks;

    std::unique_ptr<dsp::RingBuffer<float>> ring;

    std::thread feeder;
    std::atomic<bool> stop{false};
    /* write() -> feeder handshake for leaving the idle state. */
    std::atomic<bool> data_pending{false};

    /* Signalled by the waveOut callback when a block completes. */
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<int> free_blocks{0};

    uint32_t rate{sample_rate};
    size_t block_frames{512};
};

namespace {

void CALLBACK wave_out_proc(HWAVEOUT, UINT msg, DWORD_PTR instance, DWORD_PTR, DWORD_PTR) {
    if (msg != WOM_DONE) return;

    auto* impl = reinterpret_cast<AudioOut::Impl*>(instance);
    if (impl == nullptr) return;

    /* Keep this callback trivial: MSDN forbids calling most waveOut functions
     * from inside it, so all the real work happens on the feeder thread. */
    impl->free_blocks.fetch_add(1);
    impl->cv.notify_one();
}

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

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = static_cast<WORD>(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    fmt.cbSize = 0;

    const MMRESULT r = waveOutOpen(&impl_->handle, WAVE_MAPPER, &fmt,
                                   reinterpret_cast<DWORD_PTR>(wave_out_proc),
                                   reinterpret_cast<DWORD_PTR>(impl_),
                                   CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) {
        last_error_ = "waveOutOpen failed (" + std::to_string(r) + ")";
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
        waveOutPrepareHeader(impl_->handle, &h, sizeof(WAVEHDR));
        h.dwFlags |= WHDR_DONE;  /* start out owned by us */
    }

    impl_->free_blocks.store(static_cast<int>(block_count));
    impl_->stop.store(false);
    running_.store(true);

    impl_->feeder = std::thread([this] {
        Impl* impl = impl_;
        std::vector<float> scratch(impl->block_frames);
        size_t consecutive_dry = 0;

        while (!impl->stop.load()) {
            /* Wait for a block the device has finished with. */
            {
                std::unique_lock<std::mutex> lk{impl->mutex};
                impl->cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                    return impl->stop.load() || impl->free_blocks.load() > 0;
                });
            }
            if (impl->stop.load()) break;

            /* --- Idle gate -------------------------------------------------
             *
             * A decoder app (ERT, ADS-B, POCSAG...) and the menu produce NO
             * audio, and this loop used to keep the DAC clocking blocks of
             * digital silence through the speakers anyway — an always-active
             * audio session, and an audible idle hiss on some outputs. Once
             * the ring has been empty for a full device-buffer's worth of
             * consecutive blocks, stop submitting: every header completes,
             * the device goes quiet, and this thread parks on the condition
             * variable until write() signals fresh samples. Resume costs one
             * cv wake. The zero-fill below still handles the ordinary brief
             * underrun mid-stream — idling is for silence that has clearly
             * become the steady state, not for a late demod block. */
            if (idle_.load()) {
                std::unique_lock<std::mutex> lk{impl->mutex};
                impl->cv.wait_for(lk, std::chrono::milliseconds(200), [&] {
                    return impl->stop.load() || impl->data_pending.load();
                });
                if (impl->stop.load()) break;
                if (!impl->data_pending.load()) continue;
                impl->data_pending.store(false);
                idle_.store(false);
            }

            for (size_t i = 0; i < impl->headers.size() && !impl->stop.load(); i++) {
                WAVEHDR& h = impl->headers[i];
                if ((h.dwFlags & WHDR_DONE) == 0) continue;

                const size_t got = impl->ring->read(scratch.data(), scratch.size());
                if (got < scratch.size()) {
                    std::fill(scratch.begin() + got, scratch.end(), 0.0f);
                    if (got == 0) {
                        underruns_.fetch_add(1);
                        if (++consecutive_dry >= impl->headers.size()) {
                            idle_.store(true);
                            break; /* let the queued blocks drain and go quiet */
                        }
                    } else {
                        consecutive_dry = 0;
                    }
                } else {
                    consecutive_dry = 0;
                }

                const float gain = muted_.load()
                                       ? 0.0f
                                       : static_cast<float>(volume_.load()) / 99.0f;

                float peak = 0.0f;
                auto& pcm = impl->blocks[i];
                for (size_t s = 0; s < scratch.size(); s++) {
                    const float v = scratch[s] * gain;
                    peak = std::max(peak, std::fabs(v));
                    pcm[s] = to_pcm16(v);
                }

                /* Publish the running peak for the level meter. */
                float previous = peak_.load();
                while (peak > previous && !peak_.compare_exchange_weak(previous, peak)) {
                }

                h.dwFlags &= ~WHDR_DONE;
                impl->free_blocks.fetch_sub(1);
                if (waveOutWrite(impl->handle, &h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                    h.dwFlags |= WHDR_DONE;
                    impl->free_blocks.fetch_add(1);
                }
            }
        }
    });

    return true;
}

void AudioOut::stop() {
    if (impl_ == nullptr) return;

    impl_->stop.store(true);
    impl_->cv.notify_all();
    if (impl_->feeder.joinable()) impl_->feeder.join();

    if (impl_->handle) {
        waveOutReset(impl_->handle);
        for (auto& h : impl_->headers) {
            if (h.dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(impl_->handle, &h, sizeof(WAVEHDR));
        }
        waveOutClose(impl_->handle);
    }

    delete impl_;
    impl_ = nullptr;
    running_.store(false);
}

size_t AudioOut::write(const float* samples, size_t count) {
    if (impl_ == nullptr || !impl_->ring) return 0;
    const size_t written = impl_->ring->write(samples, count);
    if (written > 0 && idle_.load()) {
        /* Wake the parked feeder; see the idle gate in its loop. */
        impl_->data_pending.store(true);
        impl_->cv.notify_all();
    }
    return written;
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

#endif /* _WIN32 */
