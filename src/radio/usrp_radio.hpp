/*
 * mayhem-b200 — USRP B200/B210 radio backend.
 *
 * This is the layer that replaces the PortaPack's MAX2837/MAX5864/RFFC5072
 * front end and its SGPIO baseband bus. Where the firmware pokes registers over
 * SPI, here we drive UHD and let the AD936x do the same job.
 *
 * Deliberate choices worth knowing about:
 *
 *  - Every range (frequency, gain, sample rate, analog bandwidth) is queried
 *    from the device rather than hard-coded, so a B200, B210, B200mini or
 *    B205mini each report their own limits and the UI adapts. The published
 *    B200 figures (70 MHz - 6 GHz, 5-61.44 MHz master clock, 200 kHz - 56 MHz
 *    filter, ~76 dB RX / ~89.8 dB TX gain) are used only as fallbacks for the
 *    no-device case.
 *
 *  - Tuning uses an LO offset by default. The AD936x is a direct-conversion
 *    receiver, so it has an LO leakage spike dead centre in the passband;
 *    offsetting the LO and correcting in the DSP moves that spike out of the
 *    band of interest. The PortaPack has exactly the same artefact and no way
 *    to avoid it.
 *
 *  - Streaming runs on its own threads feeding lock-free rings. UHD's recv()
 *    must be serviced promptly or the FPGA's buffer overflows; keeping it off
 *    the UI thread is not optional.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_USRP_RADIO_H__
#define __MB200_USRP_RADIO_H__

#include "../dsp/ring_buffer.hpp"
#include "radio_device.hpp"

#include <atomic>
#include <complex>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace radio {

/* Range, DeviceInfo, DeviceCaps, StreamStats and the RadioDevice interface all
 * live in radio_device.hpp — they are shared by every backend, not specific to
 * UHD. This file is only the USRP implementation of that interface. */

class UsrpRadio : public RadioDevice {
   public:
    UsrpRadio();
    ~UsrpRadio() override;

    UsrpRadio(const UsrpRadio&) = delete;
    UsrpRadio& operator=(const UsrpRadio&) = delete;

    /* Discovery. `type_filter` of "b200" covers B200/B210/B200mini/B205mini and
     * avoids the multi-second network scan for X300-class devices. Pass "" to
     * search everything. */
    static std::vector<DeviceInfo> find(const std::string& type_filter = "b200");

    /* Opens a device. `args` is a UHD device-address string, e.g.
     * "type=b200,serial=31C9297". Empty picks the first B200-family device.
     * Returns false and sets last_error() on failure. */
    bool open(const std::string& args = "") override;
    void close() override;
    bool is_open() const override { return open_.load(); }

    const DeviceCaps& caps() const override { return caps_; }
    const std::string& last_error() const override { return last_error_; }

    const char* driver_name() const override { return "uhd"; }

    /* --- Configuration ---
     * All of these are safe to call while streaming; UHD applies them between
     * buffers. Each returns the value the hardware actually accepted, which is
     * rarely exactly what was asked for. */

    /* The master clock rate constrains every achievable sample rate. Changing
     * it re-tunes the AD936x PLL, so do it before starting a stream. */
    double set_master_clock_rate(double rate_hz) override;

    double set_rx_rate(double rate_hz) override;
    double rx_rate() const override { return rx_rate_; }

    double set_tx_rate(double rate_hz) override;
    double tx_rate() const override { return tx_rate_; }

    /* Tunes to `freq_hz`. With a non-zero LO offset the RF LO is placed
     * `lo_offset` away and the CORDIC brings the signal back to baseband. */
    double set_rx_frequency(double freq_hz) override;
    double rx_frequency() const override { return rx_freq_; }

    double set_tx_frequency(double freq_hz) override;
    double tx_frequency() const override { return tx_freq_; }

    void set_lo_offset(double offset_hz) override;
    double lo_offset() const override { return lo_offset_; }

    double set_rx_gain(double gain_db) override;
    double rx_gain() const override { return rx_gain_; }

    double set_tx_gain(double gain_db) override;
    double tx_gain() const override { return tx_gain_; }

    double set_rx_bandwidth(double bw_hz) override;
    double rx_bandwidth() const override { return rx_bw_; }

    double set_tx_bandwidth(double bw_hz) override;
    double tx_bandwidth() const override { return tx_bw_; }

    bool set_rx_antenna(const std::string& antenna) override;
    const std::string& rx_antenna() const override { return rx_antenna_; }

    bool set_tx_antenna(const std::string& antenna) override;
    const std::string& tx_antenna() const override { return tx_antenna_; }

    /* AD936x automatic corrections. Both default to on: the DC offset
     * correction in particular removes most of the centre spike. */
    void set_rx_dc_offset_auto(bool enable) override;
    void set_rx_iq_balance_auto(bool enable) override;

    /* AGC in the AD936x itself, distinct from the audio AGC. */
    bool set_rx_agc(bool enable) override;

    /* --- Streaming --- */

    /* Starts the RX thread. Samples land in rx_ring(). */
    bool start_rx() override;
    void stop_rx() override;
    bool rx_running() const override { return rx_running_.load(); }

    /* Starts the TX thread. It drains tx_ring(), sending zeros on underrun so
     * the transmitter does not glitch. */
    bool start_tx() override;
    void stop_tx() override;
    bool tx_running() const override { return tx_running_.load(); }

    dsp::RingBuffer<cfloat>& rx_ring() override { return *rx_ring_; }
    dsp::RingBuffer<cfloat>& tx_ring() override { return *tx_ring_; }

    StreamStats& stats() override { return stats_; }
    const StreamStats& stats() const override { return stats_; }

    /* Instantaneous RSSI-ish measure: RMS of the most recent RX block, in
     * dBFS. Updated by the RX thread. */
    float rx_level_db() const override { return rx_level_db_.load(); }

    /* Sizes the RX/TX rings in samples. Must be called before start_*(). */
    void set_ring_capacity(size_t samples) override;

   private:
    void rx_thread_main();
    void tx_thread_main();
    void read_caps();

    struct Impl;                 /* holds the UHD handles, keeps UHD out of this header */
    std::unique_ptr<Impl> impl_;

    DeviceCaps caps_{};
    std::string last_error_{};

    std::atomic<bool> open_{false};
    std::atomic<bool> rx_running_{false};
    std::atomic<bool> tx_running_{false};
    std::atomic<bool> rx_stop_{false};
    std::atomic<bool> tx_stop_{false};
    std::atomic<float> rx_level_db_{-140.0f};

    std::thread rx_thread_;
    std::thread tx_thread_;

    std::unique_ptr<dsp::RingBuffer<cfloat>> rx_ring_;
    std::unique_ptr<dsp::RingBuffer<cfloat>> tx_ring_;
    size_t ring_capacity_{1 << 20};

    /* Guards configuration calls against the streaming threads. */
    mutable std::mutex config_mutex_;

    double rx_rate_{0.0};
    double tx_rate_{0.0};
    double rx_freq_{0.0};
    double tx_freq_{0.0};
    double rx_gain_{0.0};
    double tx_gain_{0.0};
    double rx_bw_{0.0};
    double tx_bw_{0.0};
    double lo_offset_{0.0};
    std::string rx_antenna_{};
    std::string tx_antenna_{};

    StreamStats stats_{};
};

/* Fallback limits for the UI when no device is attached. These match the
 * published B200 specification; a live device always overrides them. */
DeviceCaps default_b200_caps();

}  // namespace radio

#endif /*__MB200_USRP_RADIO_H__*/
