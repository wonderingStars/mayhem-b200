/*
 * mayhem-b200 — USRP B200/B210 radio backend.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "usrp_radio.hpp"

#include "../dsp/demod.hpp"

#include <uhd/device.hpp>
#include <uhd/exception.hpp>
#include <uhd/stream.hpp>
#include <uhd/types/device_addr.hpp>
#include <uhd/types/metadata.hpp>
#include <uhd/types/ranges.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/usrp/multi_usrp.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace radio {

namespace {

constexpr size_t kChannel = 0;

/* recv() timeout. Long enough that a momentarily busy USB bus is not reported
 * as an error, short enough that stop_rx() is responsive. */
constexpr double kRecvTimeout = 0.2;

Range from_meta(const uhd::meta_range_t& r) {
    Range out;
    out.min = r.start();
    out.max = r.stop();
    out.step = r.step();
    return out;
}

}  // namespace

/* Range::clamp, DeviceInfo::label and StreamStats::reset now live in
 * radio_device.cpp — they are shared by every backend, not UHD-specific. */

DeviceCaps default_b200_caps() {
    /* Published B200 figures, used only when nothing is connected so the UI has
     * sensible limits to draw. Verified against the Ettus UHD manual's
     * "USRP B2x0 Series" page. */
    DeviceCaps c;
    c.mboard = "B200 (not connected)";
    c.rx_freq = {70e6, 6e9, 1.0};
    c.tx_freq = {70e6, 6e9, 1.0};
    c.rx_gain = {0.0, 76.0, 1.0};
    c.tx_gain = {0.0, 89.8, 0.2};
    c.rx_rate = {200e3, 61.44e6, 1.0};
    c.tx_rate = {200e3, 61.44e6, 1.0};
    c.rx_bandwidth = {200e3, 56e6, 1.0};
    c.tx_bandwidth = {200e3, 56e6, 1.0};
    c.master_clock_rate = 32e6;
    c.rx_antennas = {"TX/RX", "RX2"};
    c.tx_antennas = {"TX/RX"};
    c.rx_channels = 1;
    c.tx_channels = 1;
    return c;
}

/* --- Impl ------------------------------------------------------------------ */

struct UsrpRadio::Impl {
    uhd::usrp::multi_usrp::sptr usrp;
    uhd::rx_streamer::sptr rx_stream;
    uhd::tx_streamer::sptr tx_stream;
};

UsrpRadio::UsrpRadio()
    : impl_{std::make_unique<Impl>()} {
    caps_ = default_b200_caps();
}

UsrpRadio::~UsrpRadio() {
    close();
}

/* --- Discovery ------------------------------------------------------------- */

std::vector<DeviceInfo> UsrpRadio::find(const std::string& type_filter) {
    std::vector<DeviceInfo> found;

    try {
        /* Restricting by type keeps UHD from doing a UDP broadcast sweep for
         * networked USRPs, which is slow and logs errors on machines with an
         * unusual network setup. */
        uhd::device_addr_t hint;
        if (!type_filter.empty()) hint["type"] = type_filter;

        for (const auto& addr : uhd::device::find(hint)) {
            DeviceInfo info;
            info.type = addr.get("type", "");
            info.serial = addr.get("serial", "");
            info.name = addr.get("name", "");
            info.product = addr.get("product", "");
            info.args = addr.to_string();
            found.push_back(std::move(info));
        }
    } catch (const std::exception&) {
        /* Discovery failing is not fatal — the caller shows "no device". */
    }

    return found;
}

/* --- Open / close ---------------------------------------------------------- */

bool UsrpRadio::open(const std::string& args) {
    close();

    try {
        std::string device_args = args;
        if (device_args.empty()) device_args = "type=b200";

        impl_->usrp = uhd::usrp::multi_usrp::make(device_args);
        if (!impl_->usrp) {
            last_error_ = "multi_usrp::make returned null";
            return false;
        }

        /* One channel: the B200 is 1x1, and even on a B210 Mayhem's app model
         * is single-channel. */
        impl_->usrp->set_rx_subdev_spec(uhd::usrp::subdev_spec_t("A:A"), 0);
        impl_->usrp->set_tx_subdev_spec(uhd::usrp::subdev_spec_t("A:A"), 0);

        read_caps();

        rx_ring_ = std::make_unique<dsp::RingBuffer<cfloat>>(ring_capacity_);
        tx_ring_ = std::make_unique<dsp::RingBuffer<cfloat>>(ring_capacity_);

        /* Direct-conversion housekeeping: both corrections are cheap and both
         * visibly improve the waterfall. */
        set_rx_dc_offset_auto(true);
        set_rx_iq_balance_auto(true);

        rx_antenna_ = impl_->usrp->get_rx_antenna(kChannel);
        tx_antenna_ = impl_->usrp->get_tx_antenna(kChannel);
        rx_rate_ = impl_->usrp->get_rx_rate(kChannel);
        tx_rate_ = impl_->usrp->get_tx_rate(kChannel);
        rx_freq_ = impl_->usrp->get_rx_freq(kChannel);
        tx_freq_ = impl_->usrp->get_tx_freq(kChannel);
        rx_gain_ = impl_->usrp->get_rx_gain(kChannel);
        tx_gain_ = impl_->usrp->get_tx_gain(kChannel);
        rx_bw_ = impl_->usrp->get_rx_bandwidth(kChannel);
        tx_bw_ = impl_->usrp->get_tx_bandwidth(kChannel);

        stats_.reset();
        last_error_.clear();
        open_.store(true);
        return true;

    } catch (const uhd::exception& e) {
        last_error_ = std::string{"UHD: "} + e.what();
    } catch (const std::exception& e) {
        last_error_ = e.what();
    }

    impl_->usrp.reset();
    return false;
}

void UsrpRadio::close() {
    stop_rx();
    stop_tx();

    std::lock_guard<std::mutex> g{config_mutex_};
    impl_->rx_stream.reset();
    impl_->tx_stream.reset();
    impl_->usrp.reset();
    open_.store(false);
}

void UsrpRadio::read_caps() {
    DeviceCaps c;

    try {
        c.mboard = impl_->usrp->get_mboard_name();
    } catch (const std::exception&) {
        c.mboard = "USRP";
    }

    /* These property-tree reads are individually guarded: a B200mini exposes a
     * slightly different tree and one missing key should not lose the rest. */
    try {
        const auto usrp_info = impl_->usrp->get_usrp_rx_info(kChannel);
        c.serial = usrp_info.get("mboard_serial", "");
    } catch (const std::exception&) {
    }

    try {
        c.rx_freq = from_meta(impl_->usrp->get_rx_freq_range(kChannel));
        c.tx_freq = from_meta(impl_->usrp->get_tx_freq_range(kChannel));
        c.rx_gain = from_meta(impl_->usrp->get_rx_gain_range(kChannel));
        c.tx_gain = from_meta(impl_->usrp->get_tx_gain_range(kChannel));
        c.rx_rate = from_meta(impl_->usrp->get_rx_rates(kChannel));
        c.tx_rate = from_meta(impl_->usrp->get_tx_rates(kChannel));
        c.rx_bandwidth = from_meta(impl_->usrp->get_rx_bandwidth_range(kChannel));
        c.tx_bandwidth = from_meta(impl_->usrp->get_tx_bandwidth_range(kChannel));
    } catch (const std::exception&) {
        const auto fallback = default_b200_caps();
        if (c.rx_freq.max == 0.0) c.rx_freq = fallback.rx_freq;
        if (c.tx_freq.max == 0.0) c.tx_freq = fallback.tx_freq;
        if (c.rx_gain.max == 0.0) c.rx_gain = fallback.rx_gain;
        if (c.tx_gain.max == 0.0) c.tx_gain = fallback.tx_gain;
        if (c.rx_rate.max == 0.0) c.rx_rate = fallback.rx_rate;
        if (c.tx_rate.max == 0.0) c.tx_rate = fallback.tx_rate;
        if (c.rx_bandwidth.max == 0.0) c.rx_bandwidth = fallback.rx_bandwidth;
        if (c.tx_bandwidth.max == 0.0) c.tx_bandwidth = fallback.tx_bandwidth;
    }

    try {
        c.master_clock_rate = impl_->usrp->get_master_clock_rate(0);
    } catch (const std::exception&) {
    }

    try {
        c.rx_antennas = impl_->usrp->get_rx_antennas(kChannel);
        c.tx_antennas = impl_->usrp->get_tx_antennas(kChannel);
    } catch (const std::exception&) {
    }

    try {
        c.rx_channels = impl_->usrp->get_rx_num_channels();
        c.tx_channels = impl_->usrp->get_tx_num_channels();
    } catch (const std::exception&) {
        c.rx_channels = 1;
        c.tx_channels = 1;
    }

    caps_ = std::move(c);
}

/* --- Configuration --------------------------------------------------------- */

void UsrpRadio::set_ring_capacity(size_t samples) {
    if (rx_running_.load() || tx_running_.load()) return;
    ring_capacity_ = std::max<size_t>(samples, 4096);
    if (open_.load()) {
        rx_ring_ = std::make_unique<dsp::RingBuffer<cfloat>>(ring_capacity_);
        tx_ring_ = std::make_unique<dsp::RingBuffer<cfloat>>(ring_capacity_);
    }
}

double UsrpRadio::set_master_clock_rate(double rate_hz) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!impl_->usrp) return caps_.master_clock_rate;

    try {
        impl_->usrp->set_master_clock_rate(rate_hz, 0);
        caps_.master_clock_rate = impl_->usrp->get_master_clock_rate(0);
        /* The achievable sample-rate set moves with the master clock. */
        caps_.rx_rate = from_meta(impl_->usrp->get_rx_rates(kChannel));
        caps_.tx_rate = from_meta(impl_->usrp->get_tx_rates(kChannel));
    } catch (const std::exception& e) {
        last_error_ = e.what();
    }

    return caps_.master_clock_rate;
}

double UsrpRadio::set_rx_rate(double rate_hz) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!impl_->usrp) {
        rx_rate_ = caps_.rx_rate.clamp(rate_hz);
        return rx_rate_;
    }

    /* A B200 has no fixed master clock: UHD picks one to suit the requested
     * sample rate and re-tunes the FPGA's clocking to match. That
     * re-initialises the radio underneath any rx_streamer already handed out,
     * and the stale streamer then returns nothing at all -- no error, no
     * samples, just a receiver that has quietly gone deaf.
     *
     * Observed as Search failing to receive whenever it was opened directly
     * from another app rather than from the menu (2026-08-13): the departing
     * app's destructor restored its own rate and the arriving app then set
     * 2.5 MHz, two master-clock moves in quick succession with the stream
     * live, and the second one killed it. Launching the same app from the
     * menu is a single move made before streaming starts, which is exactly
     * why it worked and made this look app-specific.
     *
     * So the streamer is rebuilt whenever the clock actually moved. The
     * comparison is on the master clock rather than on the sample rate: most
     * rate changes are just a decimation change and keep the same clock, and
     * rebuilding for those would drop samples for no reason. */
    const double clock_before = impl_->usrp->get_master_clock_rate(0);

    try {
        impl_->usrp->set_rx_rate(caps_.rx_rate.clamp(rate_hz), kChannel);
        rx_rate_ = impl_->usrp->get_rx_rate(kChannel);
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return rx_rate_;
    }

    const double clock_after = impl_->usrp->get_master_clock_rate(0);
    /* Kept honest on every call, not only when the rebuild branch runs: a
     * caller (or test) probing which clock a rate selects reads caps, and a
     * stale value here is exactly how this fix's own regression test managed
     * to pass with the fix disabled -- its rate pair never actually moved the
     * clock and nothing could tell. */
    caps_.master_clock_rate = clock_after;
    if (clock_after != clock_before && rx_running_.load() && impl_->rx_stream) {
        try {
            uhd::stream_args_t args("fc32", "sc16");
            args.channels = {kChannel};
            impl_->rx_stream = impl_->usrp->get_rx_stream(args);
            /* The rx thread caches the streamer for the life of one recv loop,
             * so replacing the pointer alone would leave it reading the dead
             * one forever. Bumping the generation is what tells it to pick the
             * new streamer up; it is an atomic rather than a lock so the recv
             * loop pays one relaxed load per block instead of contending with
             * every setter. */
            rx_stream_generation_.fetch_add(1, std::memory_order_release);
        } catch (const std::exception& e) {
            last_error_ = std::string{"rx streamer rebuild after a master-clock change failed: "} + e.what();
        }
    }

    return rx_rate_;
}

double UsrpRadio::set_tx_rate(double rate_hz) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!impl_->usrp) {
        tx_rate_ = caps_.tx_rate.clamp(rate_hz);
        return tx_rate_;
    }

    try {
        impl_->usrp->set_tx_rate(caps_.tx_rate.clamp(rate_hz), kChannel);
        tx_rate_ = impl_->usrp->get_tx_rate(kChannel);
    } catch (const std::exception& e) {
        last_error_ = e.what();
    }
    return tx_rate_;
}

double UsrpRadio::set_rx_frequency(double freq_hz) {
    std::lock_guard<std::mutex> g{config_mutex_};
    freq_hz = caps_.rx_freq.clamp(freq_hz);

    if (!impl_->usrp) {
        rx_freq_ = freq_hz;
        return rx_freq_;
    }

    try {
        /* With a non-zero offset UHD places the RF LO at target+offset and sets
         * the DDC to bring the wanted signal back to 0 Hz, moving LO leakage
         * out of the displayed band. */
        uhd::tune_request_t req = (lo_offset_ != 0.0)
                                      ? uhd::tune_request_t(freq_hz, lo_offset_)
                                      : uhd::tune_request_t(freq_hz);
        impl_->usrp->set_rx_freq(req, kChannel);
        rx_freq_ = impl_->usrp->get_rx_freq(kChannel);
    } catch (const std::exception& e) {
        last_error_ = e.what();
    }
    return rx_freq_;
}

double UsrpRadio::set_tx_frequency(double freq_hz) {
    std::lock_guard<std::mutex> g{config_mutex_};
    freq_hz = caps_.tx_freq.clamp(freq_hz);

    if (!impl_->usrp) {
        tx_freq_ = freq_hz;
        return tx_freq_;
    }

    try {
        uhd::tune_request_t req = (lo_offset_ != 0.0)
                                      ? uhd::tune_request_t(freq_hz, lo_offset_)
                                      : uhd::tune_request_t(freq_hz);
        impl_->usrp->set_tx_freq(req, kChannel);
        tx_freq_ = impl_->usrp->get_tx_freq(kChannel);
    } catch (const std::exception& e) {
        last_error_ = e.what();
    }
    return tx_freq_;
}

void UsrpRadio::set_lo_offset(double offset_hz) {
    lo_offset_ = offset_hz;
    /* Re-tune so the change takes effect immediately. */
    if (rx_freq_ != 0.0) set_rx_frequency(rx_freq_);
}

double UsrpRadio::set_rx_gain(double gain_db) {
    std::lock_guard<std::mutex> g{config_mutex_};
    gain_db = caps_.rx_gain.clamp(gain_db);

    if (!impl_->usrp) {
        rx_gain_ = gain_db;
        return rx_gain_;
    }

    try {
        impl_->usrp->set_rx_gain(gain_db, kChannel);
        rx_gain_ = impl_->usrp->get_rx_gain(kChannel);
    } catch (const std::exception& e) {
        last_error_ = e.what();
    }
    return rx_gain_;
}

double UsrpRadio::set_tx_gain(double gain_db) {
    std::lock_guard<std::mutex> g{config_mutex_};
    gain_db = caps_.tx_gain.clamp(gain_db);

    if (!impl_->usrp) {
        tx_gain_ = gain_db;
        return tx_gain_;
    }

    try {
        impl_->usrp->set_tx_gain(gain_db, kChannel);
        tx_gain_ = impl_->usrp->get_tx_gain(kChannel);
    } catch (const std::exception& e) {
        last_error_ = e.what();
    }
    return tx_gain_;
}

double UsrpRadio::set_rx_bandwidth(double bw_hz) {
    std::lock_guard<std::mutex> g{config_mutex_};
    bw_hz = caps_.rx_bandwidth.clamp(bw_hz);

    if (!impl_->usrp) {
        rx_bw_ = bw_hz;
        return rx_bw_;
    }

    try {
        impl_->usrp->set_rx_bandwidth(bw_hz, kChannel);
        rx_bw_ = impl_->usrp->get_rx_bandwidth(kChannel);
    } catch (const std::exception& e) {
        last_error_ = e.what();
    }
    return rx_bw_;
}

double UsrpRadio::set_tx_bandwidth(double bw_hz) {
    std::lock_guard<std::mutex> g{config_mutex_};
    bw_hz = caps_.tx_bandwidth.clamp(bw_hz);

    if (!impl_->usrp) {
        tx_bw_ = bw_hz;
        return tx_bw_;
    }

    try {
        impl_->usrp->set_tx_bandwidth(bw_hz, kChannel);
        tx_bw_ = impl_->usrp->get_tx_bandwidth(kChannel);
    } catch (const std::exception& e) {
        last_error_ = e.what();
    }
    return tx_bw_;
}

bool UsrpRadio::set_rx_antenna(const std::string& antenna) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!impl_->usrp) {
        rx_antenna_ = antenna;
        return true;
    }

    try {
        impl_->usrp->set_rx_antenna(antenna, kChannel);
        rx_antenna_ = impl_->usrp->get_rx_antenna(kChannel);
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    }
}

bool UsrpRadio::set_tx_antenna(const std::string& antenna) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!impl_->usrp) {
        tx_antenna_ = antenna;
        return true;
    }

    try {
        impl_->usrp->set_tx_antenna(antenna, kChannel);
        tx_antenna_ = impl_->usrp->get_tx_antenna(kChannel);
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    }
}

void UsrpRadio::set_rx_dc_offset_auto(bool enable) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!impl_->usrp) return;
    try {
        impl_->usrp->set_rx_dc_offset(enable, kChannel);
    } catch (const std::exception& e) {
        last_error_ = e.what();
    }
}

void UsrpRadio::set_rx_iq_balance_auto(bool enable) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!impl_->usrp) return;
    try {
        impl_->usrp->set_rx_iq_balance(enable, kChannel);
    } catch (const std::exception& e) {
        last_error_ = e.what();
    }
}

bool UsrpRadio::set_rx_agc(bool enable) {
    std::lock_guard<std::mutex> g{config_mutex_};
    if (!impl_->usrp) return false;
    try {
        impl_->usrp->set_rx_agc(enable, kChannel);
        return true;
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    }
}

/* --- Streaming ------------------------------------------------------------- */

bool UsrpRadio::start_rx() {
    if (rx_running_.load()) return true;
    if (!open_.load()) {
        last_error_ = "no device open";
        return false;
    }

    try {
        std::lock_guard<std::mutex> g{config_mutex_};
        /* fc32 on the host, sc16 over the wire: the AD936x is a 12-bit part, so
         * sc16 carries everything it has at half the USB bandwidth of fc32. */
        uhd::stream_args_t args("fc32", "sc16");
        args.channels = {kChannel};
        impl_->rx_stream = impl_->usrp->get_rx_stream(args);
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    }

    if (!rx_ring_) rx_ring_ = std::make_unique<dsp::RingBuffer<cfloat>>(ring_capacity_);
    rx_ring_->clear();

    rx_stop_.store(false);
    rx_running_.store(true);
    rx_thread_ = std::thread(&UsrpRadio::rx_thread_main, this);
    return true;
}

void UsrpRadio::stop_rx() {
    if (!rx_running_.load()) {
        if (rx_thread_.joinable()) rx_thread_.join();
        return;
    }

    rx_stop_.store(true);
    if (rx_thread_.joinable()) rx_thread_.join();
    rx_running_.store(false);

    std::lock_guard<std::mutex> g{config_mutex_};
    impl_->rx_stream.reset();
}

void UsrpRadio::rx_thread_main() {
    uhd::rx_streamer::sptr stream;
    {
        std::lock_guard<std::mutex> g{config_mutex_};
        stream = impl_->rx_stream;
    }
    if (!stream) {
        rx_running_.store(false);
        return;
    }

    uint32_t stream_generation = rx_stream_generation_.load(std::memory_order_acquire);
    std::vector<cfloat> buffer(stream->get_max_num_samps());
    uhd::rx_metadata_t md;

    try {
        uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        cmd.stream_now = true;
        stream->issue_stream_cmd(cmd);
    } catch (const std::exception&) {
        stats_.errors++;
        rx_running_.store(false);
        return;
    }

    while (!rx_stop_.load()) {
        /* set_rx_rate rebuilds the streamer when UHD moves the B200's master
         * clock; without picking the new one up here, this loop would keep
         * recv()ing on a streamer whose hardware was re-initialised out from
         * under it and simply never return another sample. */
        const uint32_t generation_now = rx_stream_generation_.load(std::memory_order_acquire);
        if (generation_now != stream_generation) {
            stream_generation = generation_now;
            {
                std::lock_guard<std::mutex> g{config_mutex_};
                stream = impl_->rx_stream;
            }
            if (!stream) break;
            buffer.assign(stream->get_max_num_samps(), cfloat{});
            try {
                uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
                cmd.stream_now = true;
                stream->issue_stream_cmd(cmd);
            } catch (const std::exception&) {
                stats_.errors++;
                break;
            }
        }

        size_t received = 0;
        try {
            received = stream->recv(buffer.data(), buffer.size(), md, kRecvTimeout);
        } catch (const std::exception&) {
            stats_.errors++;
            break;
        }

        switch (md.error_code) {
            case uhd::rx_metadata_t::ERROR_CODE_NONE:
                break;
            case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
                stats_.timeouts++;
                continue;
            case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
                /* The FPGA's buffer filled because the host did not keep up.
                 * Samples are already lost; keep streaming rather than tearing
                 * down, which is what UHD's own examples do. */
                stats_.overflows++;
                break;
            default:
                stats_.errors++;
                continue;
        }

        if (received == 0) continue;

        rx_level_db_.store(dsp::to_db(dsp::rms(buffer.data(), received)));

        const size_t written = rx_ring_->write(buffer.data(), received);
        stats_.rx_samples += written;
        if (written < received) stats_.rx_dropped += static_cast<uint32_t>(received - written);
    }

    try {
        stream->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);

        /* Drain whatever is still in flight so the next start_rx() does not
         * receive this session's stale samples. */
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < deadline) {
            const size_t n = stream->recv(buffer.data(), buffer.size(), md, 0.05);
            if (n == 0 && md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) break;
        }
    } catch (const std::exception&) {
        /* Shutdown path: nothing useful to do with a failure here. */
    }

    rx_running_.store(false);
}

bool UsrpRadio::start_tx() {
    if (tx_running_.load()) return true;
    if (!open_.load()) {
        last_error_ = "no device open";
        return false;
    }

    try {
        std::lock_guard<std::mutex> g{config_mutex_};
        uhd::stream_args_t args("fc32", "sc16");
        args.channels = {kChannel};
        impl_->tx_stream = impl_->usrp->get_tx_stream(args);
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return false;
    }

    if (!tx_ring_) tx_ring_ = std::make_unique<dsp::RingBuffer<cfloat>>(ring_capacity_);
    tx_ring_->clear();

    tx_stop_.store(false);
    tx_running_.store(true);
    tx_thread_ = std::thread(&UsrpRadio::tx_thread_main, this);
    return true;
}

void UsrpRadio::stop_tx() {
    if (!tx_running_.load()) {
        if (tx_thread_.joinable()) tx_thread_.join();
        return;
    }

    tx_stop_.store(true);
    if (tx_thread_.joinable()) tx_thread_.join();
    tx_running_.store(false);

    std::lock_guard<std::mutex> g{config_mutex_};
    impl_->tx_stream.reset();
}

void UsrpRadio::tx_thread_main() {
    uhd::tx_streamer::sptr stream;
    {
        std::lock_guard<std::mutex> g{config_mutex_};
        stream = impl_->tx_stream;
    }
    if (!stream) {
        tx_running_.store(false);
        return;
    }

    const size_t max_samps = stream->get_max_num_samps();
    std::vector<cfloat> buffer(max_samps);

    uhd::tx_metadata_t md;
    md.start_of_burst = true;
    md.end_of_burst = false;
    md.has_time_spec = false;

    while (!tx_stop_.load()) {
        const size_t got = tx_ring_->read(buffer.data(), buffer.size());

        if (got < buffer.size()) {
            /* Underrun: pad with zeros rather than sending a short buffer.
             * A short send is legal but leaves the transmitter idle between
             * bursts, which the AD936x turns into a click. */
            std::fill(buffer.begin() + got, buffer.end(), cfloat{0.0f, 0.0f});
            if (got == 0) stats_.underflows++;
        }

        try {
            const size_t sent = stream->send(buffer.data(), buffer.size(), md, 0.5);
            stats_.tx_samples += sent;
            md.start_of_burst = false;
        } catch (const std::exception&) {
            stats_.errors++;
            break;
        }
    }

    try {
        md.end_of_burst = true;
        stream->send(buffer.data(), 0, md, 0.5);
    } catch (const std::exception&) {
    }

    tx_running_.store(false);
}

}  // namespace radio
