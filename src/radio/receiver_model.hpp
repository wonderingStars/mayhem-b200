/*
 * mayhem-b200 — receive chain.
 *
 * The host counterpart of the firmware's ReceiverModel plus its M4 baseband
 * processors. Modes and their sub-configurations deliberately mirror Mayhem's
 * (AM with DSB/USB/LSB/CW variants, NFM at three deviations, WFM, and a
 * spectrum-only mode) so the on-screen controls mean the same thing.
 *
 * The signal path, per block:
 *
 *   B200 -> NCO fine tune -> channel decimation FIR -> demodulator
 *        -> audio lowpass -> de-emphasis -> AGC -> resample to 48 kHz -> waveOut
 *
 * The NCO matters more here than on a PortaPack: retuning an AD936x costs
 * milliseconds and a settling transient, so small tuning steps inside the
 * captured bandwidth are done in the DSP and the LO is only moved when the
 * target leaves the window.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_RECEIVER_MODEL_H__
#define __MB200_RECEIVER_MODEL_H__

#include "../audio/audio_out.hpp"
#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "radio_device.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace radio {

/* --- Gapless raw sample tap -------------------------------------------------
 *
 * ReceiverModel::take_spectrum_samples() hands out the newest N samples of a
 * sliding window, so a consumer polling it once per UI frame sees only
 * (N / sample_rate) * poll_hz of the air — 12.3% at 2 Msps, 4096 samples and
 * 60 Hz, and *less* as the sample rate rises, because the window is a fixed
 * sample count and a faster rate makes it a shorter time. That is right for a
 * waterfall, which only wants a recent snapshot, and wrong for a decoder, which
 * wants every sample.
 *
 * RawSampleTap is the decoder's side of that trade: a lock-free ring the DSP
 * thread fills and one consumer drains, handing back every sample that arrived
 * since the consumer's previous read, in order, with any loss counted and
 * placed rather than silent.
 *
 * Threading contract:
 *
 *  - write() is called only by the DSP thread. It never blocks, never
 *    allocates, and never waits on the consumer: if the ring is full it
 *    discards, counts, and returns. Blocking here would back up the radio's own
 *    RX ring and cost samples for every other consumer in the process, so
 *    dropping is the correct failure and back-pressure is not.
 *  - read() is called only by the consumer thread (in this app, the UI thread).
 *  - open() / restart() / close() are control operations. They may run while
 *    the DSP thread is producing — a drain handshake keeps the producer out of
 *    the buffer while it is reallocated, and it is the CONTROL thread that
 *    waits, never the DSP thread — but they must not run concurrently with
 *    read(), because they free the buffer read() is copying out of. In practice
 *    both live on the UI thread (an app opens the tap in on_show, drains it in
 *    its frame handler, and closes it in on_hide), so that costs nothing.
 *
 * Loss is reported where it happened, not merely as a running total. When a
 * write does not fit, the tap remembers the stream position of the seam and
 * discards everything after it until the consumer has drained up to exactly
 * that point. A returned block is therefore always internally contiguous — no
 * read ever splices two moments together, which for a preamble detector would
 * mean a fabricated preamble rather than a missed one — and the size of the
 * hole immediately preceding it comes back with it, so a demodulator can reset
 * its state exactly when the stream actually broke instead of resetting on
 * every poll to be safe.
 *
 * The accounting is exact at every instant, for as long as the counters have
 * been running:
 *
 *      offered() == delivered() + dropped() + available()
 *
 * reset_stats() zeroes the three counters without emptying the ring, so it
 * breaks that identity by exactly whatever was buffered at the time and it
 * stays broken. Call it on a drained tap if the invariant matters to you.
 *
 * Two things the counters deliberately do NOT track together. dropped() is the
 * live truth and updates the moment a sample is discarded; the per-block
 * lost_before is a PLACED figure and is only published by the write that
 * follows the hole, so while a tear is still open dropped() runs ahead of the
 * sum of everything announced. They converge on the next successful write. A
 * consumer that wants the instantaneous total should read dropped() rather than
 * accumulate lost_before, which lags by the tear in progress and, if the
 * stream stops mid-tear, permanently omits that last episode.
 */
class RawSampleTap {
   public:
    /* How much air the ring holds, in seconds. Sizing in TIME rather than in
     * samples is the whole point: a decoder needs to survive a slow UI frame,
     * and a frame is a duration. 250 ms absorbs a ~15-frame stall at 60 Hz,
     * which covers the hitches a desktop UI actually takes (a file dialog, a
     * map tile decode, a window resize) without pretending to survive a hang. */
    static constexpr double kDefaultHistorySeconds = 0.25;

    /* Floor, so a low-rate device still has a usable window. */
    static constexpr size_t kMinCapacitySamples = 1u << 16;  /* 512 KiB */

    /* Ceiling, so an implausible rate cannot ask for a gigabyte. 8 Msamples of
     * cfloat is 64 MiB; at the B200's 16 Msps USB-2 ceiling that is 500 ms, and
     * at UHD's published 61.44 Msps it still holds 136 ms. */
    static constexpr size_t kMaxCapacitySamples = 8u << 20;  /* 64 MiB */

    /* What one read() returned. `lost_before` is the number of samples the tap
     * discarded between the last sample of the previous block and the first
     * sample of this one; the samples in this block are contiguous. */
    struct Block {
        size_t samples{0};
        uint64_t lost_before{0};
        bool contiguous() const { return lost_before == 0; }
    };

    /* Samples needed to hold `seconds` of `rate_hz`, clamped to the bounds
     * above. */
    static size_t capacity_for(double rate_hz, double seconds);

    RawSampleTap() = default;
    ~RawSampleTap();

    RawSampleTap(const RawSampleTap&) = delete;
    RawSampleTap& operator=(const RawSampleTap&) = delete;

    /* --- Control (consumer's thread) --- */

    /* Allocates the ring and starts accepting samples. Anything already
     * buffered is discarded and reported to the consumer as a gap, because an
     * open() is a new subscription and the old samples are not adjacent in time
     * to the new ones. Returns false only for a zero capacity. */
    bool open(size_t capacity_samples);

    /* Re-sizes an open tap; a no-op if the tap is closed. Used when the sample
     * rate changes, where keeping the sample count would silently shorten the
     * window in time. Like open(), it restarts the stream. */
    void restart(size_t capacity_samples);

    /* Stops accepting samples and releases the ring. Whatever was buffered is
     * counted as dropped — it went nowhere — but is NOT left queued as a gap
     * for the next open() to announce: that would be a different subscription
     * on a different stream, and it lost nothing. */
    void close();

    bool is_open() const { return enabled_.load(std::memory_order_acquire); }
    size_t capacity() const { return capacity_.load(std::memory_order_acquire); }

    /* Zeroes the lifetime counters. Does not touch buffered samples. */
    void reset_stats();

    /* --- Producer (DSP thread) --- */

    /* Offers `count` samples. Accepts what fits and counts the rest as lost.
     * Does nothing at all while the tap is closed, so the 29 apps that never
     * open it pay two atomics per DSP block and no copy. */
    void write(const dsp::cfloat* data, size_t count);

    /* --- Consumer (its own thread) --- */

    /* Replaces `out` with every sample buffered since the previous read, and
     * reports the hole, if any, that precedes them. */
    Block read(std::vector<dsp::cfloat>& out);

    /* Samples buffered and not yet read.
     *
     * Exact on the consumer's thread. Safe from any other thread, including the
     * producer's, but there it is only a snapshot: it can read low by whatever
     * the producer has committed since, and it is clamped to capacity() so it
     * can never read impossibly high. */
    size_t available() const;

    /* --- Counters --- */

    uint64_t offered() const { return offered_.load(std::memory_order_relaxed); }
    uint64_t delivered() const { return delivered_.load(std::memory_order_relaxed); }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

    /* Number of times the ring filled up, i.e. distinct holes in the stream —
     * as opposed to dropped(), which counts the samples lost in them. */
    uint32_t overflows() const { return overflows_.load(std::memory_order_relaxed); }

   private:
    void produce(const dsp::cfloat* data, size_t count);

    /* Disables the tap and waits for the DSP thread to leave write(), so the
     * buffer can be reallocated under it. Only the caller waits. */
    void drain_producer();

    /* Throws away whatever is buffered and marks the discontinuity. Only valid
     * with the producer drained. */
    void restart_stream();

    std::vector<dsp::cfloat> buffer_;
    std::atomic<size_t> capacity_{0};

    /* Absolute stream positions, never wrapped: the ring index is position %
     * capacity. At 16 Msps a 64-bit counter wraps after 36 000 years. */
    std::atomic<uint64_t> write_{0};
    std::atomic<uint64_t> read_{0};

    /* Torn: the producer overflowed at tear_pos_ and is discarding until the
     * consumer has drained to exactly that position. This is what keeps a hole
     * from ever landing in the middle of a returned block. */
    std::atomic<bool> torn_{false};
    std::atomic<uint64_t> tear_pos_{0};

    /* Size of the hole that precedes the next samples the producer commits.
     * Published by the producer before it writes them, consumed by the reader. */
    std::atomic<uint64_t> gap_pending_{0};

    /* Producer-private: samples lost in the tear now in progress. */
    uint64_t episode_gap_{0};

    std::atomic<uint64_t> offered_{0};
    std::atomic<uint64_t> delivered_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint32_t> overflows_{0};

    std::atomic<bool> enabled_{false};
    std::atomic<bool> producer_in_{false};
};

class ReceiverModel {
   public:
    /* Matches ReceiverModel::Mode in the firmware. */
    enum class Mode : uint8_t {
        AMAudio = 0,
        NarrowbandFMAudio = 1,
        WidebandFMAudio = 2,
        SpectrumAnalysis = 3,
    };

    /* AM sub-configurations, as Mayhem presents them. */
    enum class AmConfig : uint8_t {
        DSB9k = 0,
        DSB6k = 1,
        USB = 2,
        LSB = 3,
        CW = 4,
    };

    enum class NfmConfig : uint8_t {
        Narrow8k5 = 0,
        Medium11k = 1,
        Wide16k = 2,
    };

    enum class WfmConfig : uint8_t {
        Wide200k = 0,
        Narrow180k = 1,
    };

    ReceiverModel(RadioDevice& radio, audio::AudioOut& audio_out);
    ~ReceiverModel();

    ReceiverModel(const ReceiverModel&) = delete;
    ReceiverModel& operator=(const ReceiverModel&) = delete;

    /* --- Control --- */

    bool start();
    void stop();
    bool running() const { return running_.load(); }

    /* Frequency the user asked for. The LO may sit elsewhere; the NCO makes up
     * the difference. */
    void set_target_frequency(uint64_t hz);
    uint64_t target_frequency() const { return target_frequency_; }

    void set_frequency_step(uint64_t hz) { frequency_step_ = hz; }
    uint64_t frequency_step() const { return frequency_step_; }

    void set_mode(Mode mode);
    Mode mode() const { return mode_; }

    void set_am_configuration(AmConfig cfg);
    AmConfig am_configuration() const { return am_config_; }

    void set_nfm_configuration(NfmConfig cfg);
    NfmConfig nfm_configuration() const { return nfm_config_; }

    void set_wfm_configuration(WfmConfig cfg);
    WfmConfig wfm_configuration() const { return wfm_config_; }

    /* Front-end gain in dB. The B200 has one continuous gain control rather
     * than the HackRF's separate LNA/VGA/AMP, so Mayhem's three controls
     * collapse to this one plus the AD936x's own AGC. */
    void set_gain(double db);
    double gain() const;

    void set_agc(bool enabled);
    bool agc() const { return hw_agc_; }

    void set_audio_agc(bool enabled);
    bool audio_agc() const { return audio_agc_enabled_; }

    void set_squelch_level(uint8_t level_0_99);
    uint8_t squelch_level() const { return squelch_level_; }

    void set_volume(uint8_t volume_0_99);
    uint8_t volume() const;

    /* Capture bandwidth. Larger gives a wider waterfall at more CPU cost. */
    void set_sampling_rate(double hz);
    double sampling_rate() const { return sample_rate_; }

    /* --- Readouts --- */

    /* Smoothed channel level in dBFS, after the channel filter — this is the
     * signal-strength figure that tracks the tuned signal rather than the whole
     * captured band. */
    float channel_level_db() const { return channel_level_db_.load(); }

    /* Wideband level straight off the radio, in dBFS. */
    float rf_level_db() const { return radio_.rx_level_db(); }

    bool squelch_open() const { return squelch_open_.load(); }

    double channel_rate() const { return channel_rate_.load(); }

    /* Number of audio samples the DSP thread failed to hand to waveOut because
     * the output ring was full. Non-zero means the machine is not keeping up. */
    uint32_t audio_dropped() const { return audio_dropped_.load(); }

    /* Copies the most recent block of raw (pre-channel-filter) samples for the
     * spectrum display. Returns false if none are ready yet. */
    bool take_spectrum_samples(std::vector<dsp::cfloat>& out, size_t count);

    /* --- Gapless raw tap ---
     * A second, additive tap on the same pre-channel-filter samples that feed
     * take_spectrum_samples(). That one is a sliding snapshot and loses
     * everything between polls; this one loses nothing while the consumer keeps
     * up, and counts and locates what it loses when the consumer does not. See
     * RawSampleTap above for the contract.
     *
     * It is off by default and costs nothing when off, because only decoders
     * want it and copying every sample at 16 Msps is 128 MB/s.
     *
     * The ring is sized in time, from the rate the hardware actually gave us,
     * so it is re-sized on start() — a rate change must move the sample count
     * or a faster rate would silently shorten the window. */
    bool enable_raw_tap(double history_seconds = RawSampleTap::kDefaultHistorySeconds);
    void disable_raw_tap();
    bool raw_tap_enabled() const { return raw_tap_.is_open(); }
    RawSampleTap& raw_tap() { return raw_tap_; }
    const RawSampleTap& raw_tap() const { return raw_tap_; }

    /* --- IQ capture ---
     * Writes the raw captured band to `<path>.C16` as interleaved int16 I/Q,
     * plus a Mayhem-compatible `<path>.TXT` holding center_frequency and
     * sample_rate. Unlike the PortaPack, capture runs alongside demodulation:
     * the samples are already in the DSP thread's hands, so tapping them costs
     * only the file write.
     *
     * `path_stem` is the file name without extension. */
    bool start_capture(const std::string& path_stem);
    void stop_capture();
    bool capturing() const { return capturing_.load(); }
    uint64_t captured_bytes() const { return captured_bytes_.load(); }
    const std::string& capture_path() const { return capture_path_; }
    const std::string& capture_error() const { return capture_error_; }

    /* Human-readable name of the current mode + sub-config, e.g. "NFM 11k". */
    std::string mode_name() const;

    /* Bandwidth of the current demodulator's channel filter, in Hz. */
    double channel_bandwidth() const;

    static const char* mode_label(Mode m);

   private:
    void dsp_thread_main();
    void rebuild_chain();
    void retune_if_needed();

    RadioDevice& radio_;
    audio::AudioOut& audio_;

    std::thread dsp_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};

    /* Guards the filter/demod objects against reconfiguration mid-block. */
    std::mutex chain_mutex_;
    std::atomic<bool> chain_dirty_{true};

    /* Configuration */
    uint64_t target_frequency_{100'000'000};
    uint64_t frequency_step_{25'000};
    Mode mode_{Mode::NarrowbandFMAudio};
    AmConfig am_config_{AmConfig::DSB9k};
    NfmConfig nfm_config_{NfmConfig::Medium11k};
    WfmConfig wfm_config_{WfmConfig::Wide200k};
    double sample_rate_{2'400'000.0};
    uint8_t squelch_level_{0};
    bool hw_agc_{false};
    bool audio_agc_enabled_{true};

    /* DSP chain — only touched by the DSP thread, or under chain_mutex_. */
    dsp::Nco nco_{};
    dsp::FirDecimateC channel_filter_{};
    dsp::FirDecimateR audio_filter_{};
    dsp::AmDemod am_{};
    dsp::FmDemod fm_{};
    dsp::SsbDemod ssb_{};
    dsp::Deemphasis deemph_{};
    dsp::AudioAgc agc_{};
    dsp::Squelch squelch_{};
    dsp::Resampler resampler_{};

    std::atomic<double> channel_rate_{0.0};
    std::atomic<float> channel_level_db_{-140.0f};
    std::atomic<bool> squelch_open_{true};
    std::atomic<uint32_t> audio_dropped_{0};

    /* Spectrum tap. */
    std::mutex spectrum_mutex_;
    std::vector<dsp::cfloat> spectrum_buffer_;
    bool spectrum_ready_{false};

    /* Gapless tap. Deliberately not sharing spectrum_buffer_: the two want
     * opposite things from the same samples. The spectrum tap must keep the
     * newest 4096 whatever the consumer does and must survive being read twice
     * or not at all; the gapless tap must keep the OLDEST unread sample and is
     * emptied by a read. Serving the old call from the new ring would make
     * take_spectrum_samples() return nothing whenever a decoder had just
     * drained it, which is a behaviour change in 29 apps that are out of scope
     * here. Two buffers, one shared copy loop, no shared state. */
    RawSampleTap raw_tap_;
    double raw_tap_seconds_{RawSampleTap::kDefaultHistorySeconds};

    /* Capture sink. Opened and closed on the caller's thread, written only by
     * the DSP thread; capture_mutex_ covers the handover. */
    std::mutex capture_mutex_;
    std::unique_ptr<std::ofstream> capture_file_;
    std::vector<int16_t> capture_scratch_;
    std::string capture_path_{};
    std::string capture_error_{};
    std::atomic<bool> capturing_{false};
    std::atomic<uint64_t> captured_bytes_{0};
};

}  // namespace radio

#endif /*__MB200_RECEIVER_MODEL_H__*/
