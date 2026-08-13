/*
 * mayhem-b200 — a RadioDevice that streams a counter sequence, for tests.
 *
 * Shared by tests/test_tap_coexistence.cpp and
 * tests/test_snapshot_apps_still_receive.cpp. Not a test file itself, so the
 * tests/CMakeLists.txt test_*.cpp glob leaves it alone.
 *
 * The point of a counter sequence rather than noise is that every sample can be
 * identified by value: a consumer's claim to have received "the stream" can be
 * checked sample by sample, and a gap, a splice or a repeat shows up as a
 * position that does not follow.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_TEST_COUNTER_RADIO_H__
#define __MB200_TEST_COUNTER_RADIO_H__

#include "radio_device.hpp"
#include "receiver_model.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace mb200test {

/* Sample i carries its own stream position in both components, so a torn or
 * mis-ordered sample cannot pass as a good one. Positions start at 1 and stay
 * below 2^24, where a float still holds every integer exactly — starting at 1
 * means a zero sample can only be the untouched initial contents of
 * spectrum_buffer_, which is how the startup windows are recognised. */
inline dsp::cfloat seq_sample(uint64_t i) {
    return dsp::cfloat{static_cast<float>(i), -static_cast<float>(i)};
}

/* The stream position a sample claims, or 0 for the initial buffer contents. */
inline uint64_t position_of(const dsp::cfloat& s) {
    return static_cast<uint64_t>(s.real());
}

inline bool is_seq_sample(const dsp::cfloat& s, uint64_t i) {
    return s == seq_sample(i);
}

/* --- A radio that streams a counter ----------------------------------------
 *
 * Everything RadioDevice requires, with a producer thread standing in for the
 * UHD streamer. It never drops: a short write is retried until it fits, so the
 * sequence the DSP thread sees is guaranteed contiguous and any hole found
 * downstream was made downstream. `retries` counts how often the ring was full,
 * which is the only visible sign of the DSP thread falling behind. */
class CounterRadio : public radio::RadioDevice {
   public:
    explicit CounterRadio(size_t ring_samples = 1u << 18)
        : rx_ring_{ring_samples}, tx_ring_{1024} {
        caps_.mboard = "counter";
        caps_.rx_rate = {200e3, 61.44e6, 1.0};
        caps_.rx_freq = {70e6, 6e9, 1.0};
        caps_.rx_gain = {0.0, 76.0, 1.0};
    }

    ~CounterRadio() override { stop_producer(); }

    /* Streams `count` samples as fast as the DSP thread will take them, then
     * stops. Returns immediately; join with stop_producer(). */
    void produce(uint64_t count) {
        stop_producer();
        stop_.store(false);
        done_.store(false);
        produced_.store(0);
        retries_.store(0);
        producer_ = std::thread([this, count] {
            std::vector<dsp::cfloat> block(4096);
            uint64_t next = 1;
            while (!stop_.load() && next <= count) {
                const uint64_t remaining = count + 1 - next;
                const size_t n = static_cast<size_t>(
                    remaining < block.size() ? remaining : block.size());
                for (size_t k = 0; k < n; k++) block[k] = seq_sample(next + k);

                size_t offset = 0;
                while (offset < n && !stop_.load()) {
                    const size_t wrote = rx_ring_.write(block.data() + offset, n - offset);
                    offset += wrote;
                    if (wrote == 0) {
                        retries_.fetch_add(1);
                        std::this_thread::yield();
                    }
                }
                next += offset;
                produced_.store(next - 1);
            }
            done_.store(true);
        });
    }

    void stop_producer() {
        stop_.store(true);
        if (producer_.joinable()) producer_.join();
    }

    bool producer_done() const { return done_.load(); }
    uint64_t produced() const { return produced_.load(); }
    uint64_t ring_full_retries() const { return retries_.load(); }

    /* --- RadioDevice --- */
    bool open(const std::string& = "") override { return true; }
    void close() override {}
    bool is_open() const override { return true; }

    const radio::DeviceCaps& caps() const override { return caps_; }
    const std::string& last_error() const override { return error_; }
    const char* driver_name() const override { return "counter"; }

    double set_master_clock_rate(double hz) override { return hz; }

    double set_rx_rate(double hz) override { rx_rate_ = hz; return rx_rate_; }
    double rx_rate() const override { return rx_rate_; }
    double set_tx_rate(double hz) override { tx_rate_ = hz; return tx_rate_; }
    double tx_rate() const override { return tx_rate_; }

    double set_rx_frequency(double hz) override { rx_freq_ = hz; return rx_freq_; }
    double rx_frequency() const override { return rx_freq_; }
    double set_tx_frequency(double hz) override { tx_freq_ = hz; return tx_freq_; }
    double tx_frequency() const override { return tx_freq_; }

    void set_lo_offset(double hz) override { lo_offset_ = hz; }
    double lo_offset() const override { return lo_offset_; }

    double set_rx_gain(double db) override { rx_gain_ = db; return rx_gain_; }
    double rx_gain() const override { return rx_gain_; }
    double set_tx_gain(double db) override { tx_gain_ = db; return tx_gain_; }
    double tx_gain() const override { return tx_gain_; }

    double set_rx_bandwidth(double hz) override { rx_bw_ = hz; return rx_bw_; }
    double rx_bandwidth() const override { return rx_bw_; }
    double set_tx_bandwidth(double hz) override { tx_bw_ = hz; return tx_bw_; }
    double tx_bandwidth() const override { return tx_bw_; }

    bool set_rx_antenna(const std::string& a) override { rx_ant_ = a; return true; }
    const std::string& rx_antenna() const override { return rx_ant_; }
    bool set_tx_antenna(const std::string& a) override { tx_ant_ = a; return true; }
    const std::string& tx_antenna() const override { return tx_ant_; }

    void set_rx_dc_offset_auto(bool) override {}
    void set_rx_iq_balance_auto(bool) override {}
    bool set_rx_agc(bool) override { return false; }

    bool start_rx() override { rx_running_ = true; return true; }
    void stop_rx() override { rx_running_ = false; }
    bool rx_running() const override { return rx_running_; }

    bool start_tx() override { return false; }
    void stop_tx() override {}
    bool tx_running() const override { return false; }

    dsp::RingBuffer<radio::cfloat>& rx_ring() override { return rx_ring_; }
    dsp::RingBuffer<radio::cfloat>& tx_ring() override { return tx_ring_; }

    radio::StreamStats& stats() override { return stats_; }
    const radio::StreamStats& stats() const override { return stats_; }

    float rx_level_db() const override { return -30.0f; }
    void set_ring_capacity(size_t) override {}

   private:
    radio::DeviceCaps caps_{};
    std::string error_{};
    std::string rx_ant_{"RX2"};
    std::string tx_ant_{"TX/RX"};
    radio::StreamStats stats_{};

    dsp::RingBuffer<radio::cfloat> rx_ring_;
    dsp::RingBuffer<radio::cfloat> tx_ring_;

    double rx_rate_{2.0e6};
    double tx_rate_{2.0e6};
    double rx_freq_{0.0};
    double tx_freq_{0.0};
    double lo_offset_{0.0};
    double rx_gain_{30.0};
    double tx_gain_{0.0};
    double rx_bw_{1.6e6};
    double tx_bw_{1.6e6};
    bool rx_running_{false};

    std::thread producer_;
    std::atomic<bool> stop_{true};
    std::atomic<bool> done_{false};
    std::atomic<uint64_t> produced_{0};
    std::atomic<uint64_t> retries_{0};
};

}  // namespace mb200test

#endif /*__MB200_TEST_COUNTER_RADIO_H__*/
