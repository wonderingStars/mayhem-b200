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

#include <atomic>
#include <complex>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace radio {

using cfloat = std::complex<float>;

struct Range {
    double min{0.0};
    double max{0.0};
    double step{0.0};

    double clamp(double v) const;
    bool contains(double v) const { return v >= min && v <= max; }
};

/* What a discovery scan found, before anything is opened. */
struct DeviceInfo {
    std::string type;      /* "b200", "b210", ... */
    std::string serial;
    std::string name;
    std::string product;
    std::string args;      /* pass back to open() to select this device */

    std::string label() const;
};

/* Capabilities read back from a live device. */
struct DeviceCaps {
    std::string mboard;      /* e.g. "B200" */
    std::string serial;
    std::string fpga_version;
    std::string fw_version;

    Range rx_freq{};
    Range tx_freq{};
    Range rx_gain{};
    Range tx_gain{};
    Range rx_rate{};
    Range tx_rate{};
    Range rx_bandwidth{};
    Range tx_bandwidth{};

    double master_clock_rate{0.0};

    std::vector<std::string> rx_antennas;
    std::vector<std::string> tx_antennas;

    size_t rx_channels{0};
    size_t tx_channels{0};
};

/* Counters the UI surfaces so streaming problems are visible rather than
 * silently degrading the audio. */
struct StreamStats {
    std::atomic<uint64_t> rx_samples{0};
    std::atomic<uint64_t> tx_samples{0};
    std::atomic<uint32_t> overflows{0};   /* device -> host, 'O' */
    std::atomic<uint32_t> underflows{0};  /* host -> device, 'U' */
    std::atomic<uint32_t> rx_dropped{0};  /* ring full: DSP thread too slow */
    std::atomic<uint32_t> timeouts{0};
    std::atomic<uint32_t> errors{0};

    void reset();
};

class UsrpRadio {
   public:
    UsrpRadio();
    ~UsrpRadio();

    UsrpRadio(const UsrpRadio&) = delete;
    UsrpRadio& operator=(const UsrpRadio&) = delete;

    /* Discovery. `type_filter` of "b200" covers B200/B210/B200mini/B205mini and
     * avoids the multi-second network scan for X300-class devices. Pass "" to
     * search everything. */
    static std::vector<DeviceInfo> find(const std::string& type_filter = "b200");

    /* Opens a device. `args` is a UHD device-address string, e.g.
     * "type=b200,serial=31C9297". Empty picks the first B200-family device.
     * Returns false and sets last_error() on failure. */
    bool open(const std::string& args = "");
    void close();
    bool is_open() const { return open_.load(); }

    const DeviceCaps& caps() const { return caps_; }
    const std::string& last_error() const { return last_error_; }

    /* --- Configuration ---
     * All of these are safe to call while streaming; UHD applies them between
     * buffers. Each returns the value the hardware actually accepted, which is
     * rarely exactly what was asked for. */

    /* The master clock rate constrains every achievable sample rate. Changing
     * it re-tunes the AD936x PLL, so do it before starting a stream. */
    double set_master_clock_rate(double rate_hz);

    double set_rx_rate(double rate_hz);
    double rx_rate() const { return rx_rate_; }

    double set_tx_rate(double rate_hz);
    double tx_rate() const { return tx_rate_; }

    /* Tunes to `freq_hz`. With a non-zero LO offset the RF LO is placed
     * `lo_offset` away and the CORDIC brings the signal back to baseband. */
    double set_rx_frequency(double freq_hz);
    double rx_frequency() const { return rx_freq_; }

    double set_tx_frequency(double freq_hz);
    double tx_frequency() const { return tx_freq_; }

    void set_lo_offset(double offset_hz);
    double lo_offset() const { return lo_offset_; }

    double set_rx_gain(double gain_db);
    double rx_gain() const { return rx_gain_; }

    double set_tx_gain(double gain_db);
    double tx_gain() const { return tx_gain_; }

    double set_rx_bandwidth(double bw_hz);
    double rx_bandwidth() const { return rx_bw_; }

    double set_tx_bandwidth(double bw_hz);
    double tx_bandwidth() const { return tx_bw_; }

    bool set_rx_antenna(const std::string& antenna);
    const std::string& rx_antenna() const { return rx_antenna_; }

    bool set_tx_antenna(const std::string& antenna);
    const std::string& tx_antenna() const { return tx_antenna_; }

    /* AD936x automatic corrections. Both default to on: the DC offset
     * correction in particular removes most of the centre spike. */
    void set_rx_dc_offset_auto(bool enable);
    void set_rx_iq_balance_auto(bool enable);

    /* AGC in the AD936x itself, distinct from the audio AGC. */
    bool set_rx_agc(bool enable);

    /* --- Streaming --- */

    /* Starts the RX thread. Samples land in rx_ring(). */
    bool start_rx();
    void stop_rx();
    bool rx_running() const { return rx_running_.load(); }

    /* Starts the TX thread. It drains tx_ring(), sending zeros on underrun so
     * the transmitter does not glitch. */
    bool start_tx();
    void stop_tx();
    bool tx_running() const { return tx_running_.load(); }

    dsp::RingBuffer<cfloat>& rx_ring() { return *rx_ring_; }
    dsp::RingBuffer<cfloat>& tx_ring() { return *tx_ring_; }

    StreamStats& stats() { return stats_; }
    const StreamStats& stats() const { return stats_; }

    /* Instantaneous RSSI-ish measure: RMS of the most recent RX block, in
     * dBFS. Updated by the RX thread. */
    float rx_level_db() const { return rx_level_db_.load(); }

    /* Sizes the RX/TX rings in samples. Must be called before start_*(). */
    void set_ring_capacity(size_t samples);

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
