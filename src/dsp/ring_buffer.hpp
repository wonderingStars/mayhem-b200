/*
 * mayhem-b200 — single-producer / single-consumer ring buffer.
 *
 * The UHD streamer thread produces, the DSP thread consumes; the DSP thread
 * produces audio, the waveOut callback consumes. Both are strictly one writer
 * and one reader, so a lock-free ring is both correct and the right tool: it
 * keeps the audio path off any mutex the UI thread might hold.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_RING_BUFFER_H__
#define __MB200_RING_BUFFER_H__

#include <atomic>
#include <cstddef>
#include <vector>

namespace dsp {

/* Capacity is rounded up to a power of two so index wrapping is a mask.
 * One slot is always left empty to distinguish full from empty. */
template <typename T>
class RingBuffer {
   public:
    explicit RingBuffer(size_t capacity) {
        size_t n = 1;
        while (n < capacity + 1) n <<= 1;
        buffer_.resize(n);
        mask_ = n - 1;
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    size_t capacity() const { return mask_; }

    size_t size() const {
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t r = read_.load(std::memory_order_acquire);
        return (w - r) & mask_;
    }

    size_t space() const { return mask_ - size(); }

    bool empty() const { return size() == 0; }

    /* Writes up to `count` items. Returns how many were actually written;
     * a short write means the consumer is behind (an overrun). */
    size_t write(const T* data, size_t count) {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        const size_t free_slots = mask_ - ((w - r) & mask_);
        const size_t n = (count < free_slots) ? count : free_slots;

        for (size_t i = 0; i < n; i++)
            buffer_[(w + i) & mask_] = data[i];

        write_.store((w + n) & mask_, std::memory_order_release);
        return n;
    }

    /* Reads up to `count` items. Returns how many were read; a short read
     * means the producer is behind (an underrun). */
    size_t read(T* data, size_t count) {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t available = (w - r) & mask_;
        const size_t n = (count < available) ? count : available;

        for (size_t i = 0; i < n; i++)
            data[i] = buffer_[(r + i) & mask_];

        read_.store((r + n) & mask_, std::memory_order_release);
        return n;
    }

    /* Discards all buffered data. Only safe when the producer is stopped. */
    void clear() {
        read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    }

   private:
    std::vector<T> buffer_;
    size_t mask_{0};
    std::atomic<size_t> write_{0};
    std::atomic<size_t> read_{0};
};

}  // namespace dsp

#endif /*__MB200_RING_BUFFER_H__*/
