/*
 * mayhem-b200 — radio backend interface.
 *
 * Everything above this layer (ReceiverModel, TransmitterModel and all ~103
 * apps) talks to a RadioDevice, never to a concrete driver. That keeps the app
 * independent of how the samples are actually obtained:
 *
 *   UsrpRadio     — UHD, talking to a USRP over USB (src/radio/usrp_radio.hpp)
 *   NetworkRadio  — a remote SDR reached over the sdrlink protocol
 *   (future)      — any other backend implementing this interface
 *
 * The contract every implementation owes the caller:
 *
 *  - Every setter returns the value the hardware actually accepted, which is
 *    rarely exactly what was asked for. Callers must use the returned value.
 *  - Every range in DeviceCaps is read from the device, never hard-coded, so
 *    the UI adapts to whatever radio is attached.
 *  - Setters are safe to call while streaming.
 *  - Samples arrive in rx_ring() and are consumed from tx_ring(); both are
 *    single-producer/single-consumer.
 *  - Nothing transmits until start_tx() is called explicitly.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_RADIO_DEVICE_H__
#define __MB200_RADIO_DEVICE_H__

#include "../dsp/ring_buffer.hpp"

#include <atomic>
#include <complex>
#include <cstdint>
#include <string>
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
    std::string type;      /* "b200", "b210", "rtlsdr", "hackrf", ... */
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

    /* Not every radio can do everything a B200 can. These let the UI disable
     * what the attached hardware genuinely cannot do rather than failing at the
     * moment the user presses a button. An RTL-SDR is receive-only; a HackRF
     * transmits but cannot receive at the same time. */
    bool has_rx{true};
    bool has_tx{true};
    bool full_duplex{true};
};

/* Counters the UI surfaces so streaming problems are visible rather than
 * silently degrading the audio. */
struct StreamStats {
    std::atomic<uint64_t> rx_samples{0};
    std::atomic<uint64_t> tx_samples{0};
    std::atomic<uint32_t> overflows{0};   /* device -> host */
    std::atomic<uint32_t> underflows{0};  /* host -> device */
    std::atomic<uint32_t> rx_dropped{0};  /* ring full: consumer too slow */
    std::atomic<uint32_t> timeouts{0};
    std::atomic<uint32_t> errors{0};

    void reset();
};

class RadioDevice {
   public:
    virtual ~RadioDevice() = default;

    /* --- Lifecycle --- */
    virtual bool open(const std::string& args = "") = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    virtual const DeviceCaps& caps() const = 0;
    virtual const std::string& last_error() const = 0;

    /* Short name of the backend, for the UI and logs: "uhd", "sdrlink", ... */
    virtual const char* driver_name() const = 0;

    /* --- Configuration --- */
    virtual double set_master_clock_rate(double rate_hz) = 0;

    virtual double set_rx_rate(double rate_hz) = 0;
    virtual double rx_rate() const = 0;
    virtual double set_tx_rate(double rate_hz) = 0;
    virtual double tx_rate() const = 0;

    virtual double set_rx_frequency(double freq_hz) = 0;
    virtual double rx_frequency() const = 0;
    virtual double set_tx_frequency(double freq_hz) = 0;
    virtual double tx_frequency() const = 0;

    virtual void set_lo_offset(double offset_hz) = 0;
    virtual double lo_offset() const = 0;

    virtual double set_rx_gain(double gain_db) = 0;
    virtual double rx_gain() const = 0;
    virtual double set_tx_gain(double gain_db) = 0;
    virtual double tx_gain() const = 0;

    virtual double set_rx_bandwidth(double bw_hz) = 0;
    virtual double rx_bandwidth() const = 0;
    virtual double set_tx_bandwidth(double bw_hz) = 0;
    virtual double tx_bandwidth() const = 0;

    virtual bool set_rx_antenna(const std::string& antenna) = 0;
    virtual const std::string& rx_antenna() const = 0;
    virtual bool set_tx_antenna(const std::string& antenna) = 0;
    virtual const std::string& tx_antenna() const = 0;

    /* Automatic front-end corrections. A backend that cannot do these should
     * accept the call and ignore it rather than failing. */
    virtual void set_rx_dc_offset_auto(bool enable) = 0;
    virtual void set_rx_iq_balance_auto(bool enable) = 0;

    /* Hardware AGC, distinct from the audio AGC. Returns false if unsupported. */
    virtual bool set_rx_agc(bool enable) = 0;

    /* --- Streaming --- */
    virtual bool start_rx() = 0;
    virtual void stop_rx() = 0;
    virtual bool rx_running() const = 0;

    virtual bool start_tx() = 0;
    virtual void stop_tx() = 0;
    virtual bool tx_running() const = 0;

    virtual dsp::RingBuffer<cfloat>& rx_ring() = 0;
    virtual dsp::RingBuffer<cfloat>& tx_ring() = 0;

    virtual StreamStats& stats() = 0;
    virtual const StreamStats& stats() const = 0;

    /* RMS of the most recent RX block, in dBFS. */
    virtual float rx_level_db() const = 0;

    /* Sizes the RX/TX rings in samples. Must be called before start_*(). */
    virtual void set_ring_capacity(size_t samples) = 0;
};

}  // namespace radio

#endif /*__MB200_RADIO_DEVICE_H__*/
