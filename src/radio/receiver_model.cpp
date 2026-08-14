/*
 * mayhem-b200 — receive chain.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "receiver_model.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace radio {

namespace {

/* Channel-filter bandwidths, chosen to match the labels Mayhem uses. */
struct ChannelSpec {
    double bandwidth_hz;   /* two-sided, for the complex channel filter */
    double audio_hz;       /* audio lowpass corner */
    double deviation_hz;   /* FM only */
    double deemph_us;      /* FM only, 0 = none */
};

ChannelSpec spec_for_am(ReceiverModel::AmConfig cfg) {
    switch (cfg) {
        case ReceiverModel::AmConfig::DSB6k: return {6000.0, 3000.0, 0.0, 0.0};
        case ReceiverModel::AmConfig::USB:   return {2800.0, 2800.0, 0.0, 0.0};
        case ReceiverModel::AmConfig::LSB:   return {2800.0, 2800.0, 0.0, 0.0};
        case ReceiverModel::AmConfig::CW:    return {1400.0, 1400.0, 0.0, 0.0};
        case ReceiverModel::AmConfig::DSB9k:
        default:                             return {9000.0, 4500.0, 0.0, 0.0};
    }
}

ChannelSpec spec_for_nfm(ReceiverModel::NfmConfig cfg) {
    /* Deviation follows the usual pairing for these channel spacings. */
    switch (cfg) {
        case ReceiverModel::NfmConfig::Narrow8k5: return {8500.0, 3000.0, 2500.0, 750.0};
        case ReceiverModel::NfmConfig::Wide16k:   return {16000.0, 3400.0, 5000.0, 750.0};
        case ReceiverModel::NfmConfig::Medium11k:
        default:                                  return {11000.0, 3200.0, 3500.0, 750.0};
    }
}

ChannelSpec spec_for_wfm(ReceiverModel::WfmConfig cfg) {
    switch (cfg) {
        case ReceiverModel::WfmConfig::Narrow180k: return {180000.0, 15000.0, 75000.0, 50.0};
        case ReceiverModel::WfmConfig::Wide200k:
        default:                                   return {200000.0, 15000.0, 75000.0, 50.0};
    }
}

/* Samples pulled from the RX ring per DSP iteration. Big enough to amortise the
 * per-block overhead, small enough that a mode change feels immediate. */
constexpr size_t kBlockSamples = 8192;

/* How many raw samples the spectrum tap keeps. Matches the largest FFT the
 * waterfall uses. */
constexpr size_t kSpectrumSamples = 4096;

}  // namespace

/* --- Gapless raw sample tap -------------------------------------------------
 *
 * Memory ordering, since this is the only lock-free code in the receive path
 * that two threads write to.
 *
 * The data path is a textbook single-producer / single-consumer ring over
 * absolute (never wrapped) 64-bit positions. The producer owns write_, the
 * consumer owns read_. The producer stores write_ with release after filling
 * the slots, and the consumer loads it with acquire before touching them, so
 * every sample it copies out is a sample the producer has finished writing.
 * Symmetrically the consumer stores read_ with release after copying out, and
 * the producer loads it with acquire before computing free space, so the
 * producer never overwrites a slot the consumer is still reading. Nothing else
 * is needed: absolute positions make "full" and "empty" distinguishable without
 * sacrificing a slot, and a 64-bit position cannot wrap in any deployment.
 *
 * The tear protocol adds one more pairing. tear_pos_ is stored relaxed and then
 * published by the release store to torn_; the consumer's acquire load of torn_
 * is what makes that position visible. The consumer clears torn_ with release
 * only AFTER its release store to read_, so a producer that observes torn_
 * false has necessarily also observed the drained read_.
 *
 * gap_pending_ is published by the producer BEFORE the samples it precedes are
 * committed, and the consumer exchanges it AFTER its acquire load of write_.
 * That order guarantees the direction that matters: the consumer can never see
 * post-gap samples without also seeing the gap, so a hole is never reported
 * late and never lands inside a block the consumer believes is contiguous.
 *
 * The converse does NOT hold, deliberately, and any consumer migrating to this
 * tap has to be built for it. A read that lands between the gap's publication
 * and the store to write_ gets {samples = 0, lost_before = G}: the hole
 * arrives one read BEFORE the samples it precedes. That is not a corner case —
 * it is the normal outcome whenever the consumer polls faster than the DSP
 * thread produces, and it was measured at 5 178 420 of 5 182 326 holes
 * (99.92%) in a hostile-polling run. It is also the safe direction: a
 * demodulator that resets on lost_before resets slightly early, before the
 * discontinuity reaches it, rather than after it has already been fed a
 * splice. Handle `samples == 0 && lost_before != 0` — do not treat an empty
 * block as "nothing happened". ui_adsb_rx.cpp's pump() is the worked example.
 *
 * The control path (open / restart / close, which reallocate the buffer) uses a
 * Dekker handshake rather than a mutex: the producer publishes producer_in_ and
 * then re-reads enabled_, both seq_cst, so a concurrent close() either sees the
 * producer inside and waits for it, or the producer sees the tap closed and
 * never touches the buffer. The waiting is done by the control thread; the DSP
 * thread is never made to wait for anything.
 */

size_t RawSampleTap::capacity_for(double rate_hz, double seconds) {
    if (!(rate_hz > 0.0) || !(seconds > 0.0)) return kMinCapacitySamples;

    const double want = rate_hz * seconds;
    if (want <= static_cast<double>(kMinCapacitySamples)) return kMinCapacitySamples;
    if (want >= static_cast<double>(kMaxCapacitySamples)) return kMaxCapacitySamples;
    return static_cast<size_t>(want);
}

RawSampleTap::~RawSampleTap() {
    close();
}

void RawSampleTap::drain_producer() {
    enabled_.store(false, std::memory_order_seq_cst);
    /* The producer's critical section is one memcpy of a DSP block, so this
     * spins for microseconds at most — and it is the caller's thread that
     * spins, never the DSP thread. */
    while (producer_in_.load(std::memory_order_seq_cst)) std::this_thread::yield();
}

void RawSampleTap::restart_stream() {
    /* Whatever was buffered belongs to the stream that just ended; it is not
     * adjacent in time to what comes next, so it is loss, and loss is reported
     * rather than swept up. */
    const uint64_t unread = write_.load(std::memory_order_relaxed) -
                            read_.load(std::memory_order_relaxed);
    if (unread != 0) {
        dropped_.fetch_add(unread, std::memory_order_relaxed);
        gap_pending_.fetch_add(unread, std::memory_order_relaxed);
    }

    /* A tear in progress is already counted in dropped_, but it has not been
     * announced yet — the producer only publishes a hole when it writes the
     * samples that follow it, and here there will be no such write. Publish it
     * now, or a restart during an overflow would hide the loss from the only
     * report that says WHERE the stream broke. It precedes the next samples the
     * consumer sees, exactly like the unread ones above. */
    if (episode_gap_ != 0) gap_pending_.fetch_add(episode_gap_, std::memory_order_relaxed);

    write_.store(0, std::memory_order_relaxed);
    read_.store(0, std::memory_order_relaxed);
    tear_pos_.store(0, std::memory_order_relaxed);
    torn_.store(false, std::memory_order_relaxed);
    episode_gap_ = 0;
}

bool RawSampleTap::open(size_t capacity_samples) {
    if (capacity_samples == 0) return false;

    drain_producer();

    buffer_.resize(capacity_samples);
    buffer_.shrink_to_fit();
    capacity_.store(buffer_.size(), std::memory_order_relaxed);
    restart_stream();

    enabled_.store(true, std::memory_order_seq_cst);
    return true;
}

void RawSampleTap::restart(size_t capacity_samples) {
    if (!is_open() || capacity_samples == 0) return;
    open(capacity_samples);
}

void RawSampleTap::close() {
    drain_producer();

    /* Anything still buffered is now unreachable, so account for it. */
    restart_stream();

    /* ...but do NOT leave the hole queued for announcement. restart_stream()
     * pushes it into gap_pending_ so that a consumer who keeps reading learns
     * where the stream broke, and after a close there is no such consumer: the
     * subscription is over. The next read of this slot belongs to whoever calls
     * open() next, which is a NEW subscription on a NEW stream, and charging
     * them a hole from the previous one is a lie about their data. The samples
     * stay counted in dropped_, which is a lifetime total and is true.
     *
     * This was live. ~AdsbRxView calls disable_raw_tap() and a later on_show()
     * calls enable_raw_tap() on the one process-lifetime ReceiverModel, so
     * every relaunch of the app opened with a fabricated gap the size of
     * whatever was unread — up to the whole ring — which inflated its
     * "lost N in M" readout, dragged air_fraction() below the truth, and forced
     * one pointless demod_.reset() on the first pump. */
    gap_pending_.store(0, std::memory_order_relaxed);

    std::vector<dsp::cfloat>{}.swap(buffer_);
    capacity_.store(0, std::memory_order_relaxed);
}

void RawSampleTap::reset_stats() {
    offered_.store(0, std::memory_order_relaxed);
    delivered_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    overflows_.store(0, std::memory_order_relaxed);
}

size_t RawSampleTap::available() const {
    /* read_ FIRST. Both positions only ever advance, and write_ is never behind
     * read_ at any real instant — but these are two separate loads, so an
     * observer that is neither the producer nor the consumer can pair a stale
     * write_ with a fresher read_ and underflow the subtraction. That reported
     * about 1.8e19 samples buffered in a 4096-slot ring (measured: 512 such
     * readings in 236 million polls, worst 2^64 - 64, exactly one producer
     * block of underflow).
     *
     * Loading the consumer's position first makes that impossible: whatever
     * write_ we then see is at least as new as the read_ we already have, so
     * the difference cannot go negative. It can still be stale-HIGH by however
     * much the producer committed in between, which the clamp bounds — the ring
     * cannot hold more than its capacity, so any larger answer is not a
     * conservative estimate, it is a wrong one.
     *
     * On the consumer's own thread, which is where the accounting invariant is
     * checked, read_ cannot move underneath us and the result is exact. */
    const uint64_t r = read_.load(std::memory_order_acquire);
    const uint64_t w = write_.load(std::memory_order_acquire);
    const uint64_t n = (w > r) ? (w - r) : 0;

    const size_t cap = capacity_.load(std::memory_order_acquire);
    return (n > static_cast<uint64_t>(cap)) ? cap : static_cast<size_t>(n);
}

void RawSampleTap::write(const dsp::cfloat* data, size_t count) {
    /* Announce first, then re-check: with both operations sequentially
     * consistent, a concurrent close() cannot slip between them. Either it sees
     * producer_in_ and waits, or we see the tap closed and leave the buffer
     * alone. */
    producer_in_.store(true, std::memory_order_seq_cst);
    if (enabled_.load(std::memory_order_seq_cst) && data != nullptr && count != 0)
        produce(data, count);
    producer_in_.store(false, std::memory_order_release);
}

void RawSampleTap::produce(const dsp::cfloat* data, size_t count) {
    const size_t cap = capacity_.load(std::memory_order_relaxed);
    if (cap == 0) return;

    offered_.fetch_add(count, std::memory_order_relaxed);

    if (torn_.load(std::memory_order_acquire)) {
        /* Still waiting for the consumer to reach the seam. Everything now is
         * part of the same hole; writing it would put that hole in the middle
         * of a block the consumer believes is contiguous. */
        episode_gap_ += count;
        dropped_.fetch_add(count, std::memory_order_relaxed);
        return;
    }

    /* First write after a hole: publish its size before the samples that follow
     * it become visible. */
    if (episode_gap_ != 0) {
        gap_pending_.fetch_add(episode_gap_, std::memory_order_release);
        episode_gap_ = 0;
    }

    const uint64_t w = write_.load(std::memory_order_relaxed);
    const uint64_t r = read_.load(std::memory_order_acquire);
    const size_t buffered = static_cast<size_t>(w - r);
    const size_t free_slots = cap - buffered;
    const size_t n = (count < free_slots) ? count : free_slots;

    if (n != 0) {
        const size_t head = static_cast<size_t>(w % cap);
        const size_t first = std::min(n, cap - head);
        std::copy(data, data + first, buffer_.data() + head);
        if (n > first) std::copy(data + first, data + n, buffer_.data());
        write_.store(w + n, std::memory_order_release);
    }

    if (n < count) {
        const size_t lost = count - n;
        dropped_.fetch_add(lost, std::memory_order_relaxed);
        overflows_.fetch_add(1, std::memory_order_relaxed);
        episode_gap_ += lost;
        tear_pos_.store(w + n, std::memory_order_relaxed);
        torn_.store(true, std::memory_order_release);
    }
}

RawSampleTap::Block RawSampleTap::read(std::vector<dsp::cfloat>& out) {
    Block block{};

    const size_t cap = capacity_.load(std::memory_order_relaxed);
    const uint64_t r = read_.load(std::memory_order_relaxed);
    const uint64_t w = write_.load(std::memory_order_acquire);

    /* After the acquire on write_, so a gap can never arrive later than the
     * samples it precedes. */
    block.lost_before = gap_pending_.exchange(0, std::memory_order_acq_rel);

    const size_t n = (cap == 0) ? 0 : static_cast<size_t>(w - r);
    out.resize(n);

    if (n != 0) {
        const size_t head = static_cast<size_t>(r % cap);
        const size_t first = std::min(n, cap - head);
        std::copy(buffer_.data() + head, buffer_.data() + head + first, out.data());
        if (n > first)
            std::copy(buffer_.data(), buffer_.data() + (n - first), out.data() + first);

        read_.store(r + n, std::memory_order_release);
        delivered_.fetch_add(n, std::memory_order_relaxed);
        block.samples = n;
    }

    /* Reaching the seam exactly is what lets the producer start again. Checking
     * the position rather than just the flag matters: the producer may have
     * torn after this read's snapshot of write_, in which case we have not in
     * fact drained to the seam and must leave it standing. */
    if (torn_.load(std::memory_order_acquire) &&
        (r + n) == tear_pos_.load(std::memory_order_relaxed)) {
        torn_.store(false, std::memory_order_release);
    }

    return block;
}

ReceiverModel::ReceiverModel(RadioDevice& radio, audio::AudioOut& audio_out)
    : radio_{radio},
      audio_{audio_out} {
    spectrum_buffer_.resize(kSpectrumSamples);
}

ReceiverModel::~ReceiverModel() {
    stop();
}

const char* ReceiverModel::mode_label(Mode m) {
    switch (m) {
        case Mode::AMAudio: return "AM";
        case Mode::NarrowbandFMAudio: return "NFM";
        case Mode::WidebandFMAudio: return "WFM";
        case Mode::SpectrumAnalysis: return "SPEC";
    }
    return "?";
}

std::string ReceiverModel::mode_name() const {
    switch (mode_) {
        case Mode::AMAudio:
            switch (am_config_) {
                case AmConfig::DSB9k: return "AM DSB 9k";
                case AmConfig::DSB6k: return "AM DSB 6k";
                case AmConfig::USB: return "AM USB";
                case AmConfig::LSB: return "AM LSB";
                case AmConfig::CW: return "AM CW";
            }
            return "AM";
        case Mode::NarrowbandFMAudio:
            switch (nfm_config_) {
                case NfmConfig::Narrow8k5: return "NFM 8k5";
                case NfmConfig::Medium11k: return "NFM 11k";
                case NfmConfig::Wide16k: return "NFM 16k";
            }
            return "NFM";
        case Mode::WidebandFMAudio:
            return (wfm_config_ == WfmConfig::Wide200k) ? "WFM 200k" : "WFM 180k";
        case Mode::SpectrumAnalysis:
            return "SPEC";
    }
    return "?";
}

double ReceiverModel::channel_bandwidth() const {
    switch (mode_) {
        case Mode::AMAudio: return spec_for_am(am_config_).bandwidth_hz;
        case Mode::NarrowbandFMAudio: return spec_for_nfm(nfm_config_).bandwidth_hz;
        case Mode::WidebandFMAudio: return spec_for_wfm(wfm_config_).bandwidth_hz;
        case Mode::SpectrumAnalysis: return sample_rate_;
    }
    return 0.0;
}

/* --- Control --------------------------------------------------------------- */

bool ReceiverModel::start() {
    if (running_.load()) return true;

    radio_.set_rx_rate(sample_rate_);
    sample_rate_ = radio_.rx_rate();

    /* Re-size the gapless tap for the rate the hardware actually gave us. It is
     * sized in time, so the sample count has to move with the rate; leaving it
     * alone would make a faster rate hold proportionally less air, which is the
     * exact trap that makes raising the sample rate look like a regression. */
    if (raw_tap_.is_open())
        raw_tap_.restart(RawSampleTap::capacity_for(sample_rate_, raw_tap_seconds_));

    apply_rx_bandwidth();

    retune_if_needed();
    chain_dirty_.store(true);

    if (!radio_.start_rx()) return false;

    stop_.store(false);
    running_.store(true);
    dsp_thread_ = std::thread(&ReceiverModel::dsp_thread_main, this);
    return true;
}

void ReceiverModel::stop() {
    /* Close the capture file before the DSP thread goes away, so a recording is
     * always flushed rather than truncated. */
    stop_capture();

    if (running_.load()) {
        stop_.store(true);
        if (dsp_thread_.joinable()) dsp_thread_.join();
        running_.store(false);
    } else if (dsp_thread_.joinable()) {
        dsp_thread_.join();
    }

    radio_.stop_rx();
}

void ReceiverModel::set_target_frequency(uint64_t hz) {
    target_frequency_ = hz;
    retune_if_needed();
}

void ReceiverModel::retune_if_needed() {
    const double target = static_cast<double>(target_frequency_);
    const double lo = radio_.rx_frequency();
    const double offset = target - lo;

    /* Keep the wanted signal inside the middle 80% of the captured band; retune
     * the LO only when it would otherwise drift towards the filter skirts. */
    const double window = sample_rate_ * 0.4;

    const bool move_lo = (lo == 0.0) || (std::fabs(offset) > window);
    if (move_lo) radio_.set_rx_frequency(target);

    /* The NCO is the DSP thread's, so take the chain lock before touching it.
     * Tuning happens on the UI thread and mixing happens on the DSP thread;
     * without this they race on the oscillator's phase state. */
    std::lock_guard<std::mutex> g{chain_mutex_};
    if (move_lo) {
        nco_.set_frequency(0.0, sample_rate_);
    } else {
        /* Mixing by -offset brings the signal from +offset down to baseband. */
        nco_.set_frequency(-offset, sample_rate_);
    }
}

/* The mode selectors are written from the UI thread and read by the DSP thread
 * inside its chain_mutex_ section, so they take the same lock. They are driven
 * by user input, so the cost is irrelevant. */

void ReceiverModel::set_mode(Mode mode) {
    /* Every app configures the receiver through set_mode, so this is where the
     * speaker monitor's default lives: ON. A pure-data app (ACARS, FLEX, the
     * pagers...) that wants no sound calls set_audio_monitor(false) AFTER
     * set_mode; a listening app just leaves it. Set unconditionally, before
     * the same-mode early return, so a decoder->listening-app switch that
     * happens to keep the same demod mode still comes back audible. */
    audio_monitor_.store(true);
    {
        std::lock_guard<std::mutex> g{chain_mutex_};
        if (mode == mode_) return;
        mode_ = mode;
    }
    chain_dirty_.store(true);
}

void ReceiverModel::set_am_configuration(AmConfig cfg) {
    {
        std::lock_guard<std::mutex> g{chain_mutex_};
        if (cfg == am_config_) return;
        am_config_ = cfg;
    }
    chain_dirty_.store(true);
}

void ReceiverModel::set_nfm_configuration(NfmConfig cfg) {
    {
        std::lock_guard<std::mutex> g{chain_mutex_};
        if (cfg == nfm_config_) return;
        nfm_config_ = cfg;
    }
    chain_dirty_.store(true);
}

void ReceiverModel::set_wfm_configuration(WfmConfig cfg) {
    {
        std::lock_guard<std::mutex> g{chain_mutex_};
        if (cfg == wfm_config_) return;
        wfm_config_ = cfg;
    }
    chain_dirty_.store(true);
}

/* Let the analog filter pass the captured band and no more. Anything narrower
 * is the channel filter's job and doing it in the AD936x would fight the NCO;
 * anything wider folds the neighbours in. choose_rx_bandwidth() decides where
 * that lands on the attached device and whether the device said enough for the
 * question to be answerable at all — see capability_policy.hpp. */
void ReceiverModel::apply_rx_bandwidth() {
    /* An app hint wins over the rate-based default: it means "this signal is
     * narrower than the rate, filter for it". Clamp to what the device's
     * analog filter can do, then apply it directly. */
    if (rx_bandwidth_override_hz_ > 0.0) {
        const double bw = radio_.caps().rx_bandwidth.clamp(rx_bandwidth_override_hz_);
        if (bw > 0.0) {
            radio_.set_rx_bandwidth(bw);
            return;
        }
    }
    rx_bandwidth_choice_ = choose_rx_bandwidth(radio_.caps(), sample_rate_);
    if (rx_bandwidth_choice_.should_apply())
        radio_.set_rx_bandwidth(rx_bandwidth_choice_.bandwidth_hz);
}

void ReceiverModel::set_rx_bandwidth_hint(double hz) {
    rx_bandwidth_override_hz_ = (std::isfinite(hz) && hz > 0.0) ? hz : 0.0;
    apply_rx_bandwidth();
}

/* Gain is the one front-end control an app hands straight from a UI field to
 * the radio, so it is the one most likely to arrive out of range — or, from a
 * remote caller, not a number at all. Validate once here rather than trusting
 * each backend to do it: NetworkRadio only clamps while its link is DOWN. */
void ReceiverModel::set_gain(double db) {
    gain_choice_ = choose_rx_gain(radio_.caps(), db);
    if (!gain_choice_.should_apply()) return;
    radio_.set_rx_gain(gain_choice_.gain_db);
}

double ReceiverModel::gain() const { return radio_.rx_gain(); }

void ReceiverModel::set_agc(bool enabled) {
    hw_agc_ = enabled;
    radio_.set_rx_agc(enabled);
}

void ReceiverModel::set_audio_agc(bool enabled) {
    std::lock_guard<std::mutex> g{chain_mutex_};
    audio_agc_enabled_ = enabled;
    agc_.set_enabled(enabled);
}

void ReceiverModel::set_squelch_level(uint8_t level_0_99) {
    std::lock_guard<std::mutex> g{chain_mutex_};
    squelch_level_ = std::min<uint8_t>(level_0_99, 99);
    squelch_.set_level(squelch_level_);
}

void ReceiverModel::set_volume(uint8_t volume_0_99) { audio_.set_volume(volume_0_99); }
uint8_t ReceiverModel::volume() const { return audio_.volume(); }

void ReceiverModel::set_sampling_rate(double hz) {
    /* The bandwidth hint is per-app configuration; clear it whenever the rate
     * is (re)set so the next app starts from the capability default. An app
     * that wants a narrow filter re-asserts it after this call. */
    rx_bandwidth_override_hz_ = 0.0;
    if (hz == sample_rate_) {
        apply_rx_bandwidth(); /* reflect a just-cleared override */
        return;
    }

    const bool was_running = running_.load();
    if (was_running) stop();

    sample_rate_ = radio_.set_rx_rate(hz);
    apply_rx_bandwidth();
    chain_dirty_.store(true);

    if (was_running) start();
}

bool ReceiverModel::take_spectrum_samples(std::vector<dsp::cfloat>& out, size_t count) {
    std::lock_guard<std::mutex> g{spectrum_mutex_};
    if (!spectrum_ready_ || count == 0) return false;
    if (count > spectrum_buffer_.size()) count = spectrum_buffer_.size();

    out.assign(spectrum_buffer_.end() - static_cast<ptrdiff_t>(count),
               spectrum_buffer_.end());
    spectrum_ready_ = false;
    return true;
}

bool ReceiverModel::peek_spectrum_samples(std::vector<dsp::cfloat>& out, size_t count) {
    std::lock_guard<std::mutex> g{spectrum_mutex_};
    if (!spectrum_filled_once_ || count == 0) return false;
    if (count > spectrum_buffer_.size()) count = spectrum_buffer_.size();

    out.assign(spectrum_buffer_.end() - static_cast<ptrdiff_t>(count),
               spectrum_buffer_.end());
    return true;
}

bool ReceiverModel::enable_raw_tap(double history_seconds) {
    if (!(history_seconds > 0.0)) history_seconds = RawSampleTap::kDefaultHistorySeconds;
    raw_tap_seconds_ = history_seconds;
    return raw_tap_.open(RawSampleTap::capacity_for(sample_rate_, history_seconds));
}

void ReceiverModel::disable_raw_tap() {
    raw_tap_.close();
}

/* --- IQ capture ------------------------------------------------------------ */

bool ReceiverModel::start_capture(const std::string& path_stem) {
    stop_capture();

    std::lock_guard<std::mutex> g{capture_mutex_};
    capture_error_.clear();

    const std::string data_path = path_stem + ".C16";
    const std::string meta_path = path_stem + ".TXT";

    auto file = std::make_unique<std::ofstream>(data_path, std::ios::binary | std::ios::trunc);
    if (!file->is_open()) {
        capture_error_ = "cannot create " + data_path;
        return false;
    }

    /* A large stream buffer keeps the DSP thread from hitting the disk on every
     * block; at 2.4 Msps this path moves ~9.6 MB/s. */
    static thread_local std::vector<char> unused;
    file->rdbuf()->pubsetbuf(nullptr, 1 << 20);

    /* Metadata in Mayhem's format, so captures open in its viewer and in the
     * usual host-side tooling. */
    std::ofstream meta(meta_path, std::ios::trunc);
    if (!meta.is_open()) {
        capture_error_ = "cannot create " + meta_path;
        return false;
    }
    meta << "center_frequency=" << target_frequency_ << "\n";
    meta << "sample_rate=" << static_cast<uint64_t>(std::llround(sample_rate_)) << "\n";
    meta.close();

    capture_file_ = std::move(file);
    capture_path_ = data_path;
    captured_bytes_.store(0);
    capturing_.store(true);
    return true;
}

void ReceiverModel::stop_capture() {
    std::lock_guard<std::mutex> g{capture_mutex_};
    capturing_.store(false);
    if (capture_file_) {
        capture_file_->flush();
        capture_file_->close();
        capture_file_.reset();
    }
}

/* --- Chain construction ---------------------------------------------------- */

void ReceiverModel::rebuild_chain() {
    std::lock_guard<std::mutex> g{chain_mutex_};

    ChannelSpec spec{};
    switch (mode_) {
        case Mode::AMAudio: spec = spec_for_am(am_config_); break;
        case Mode::NarrowbandFMAudio: spec = spec_for_nfm(nfm_config_); break;
        case Mode::WidebandFMAudio: spec = spec_for_wfm(wfm_config_); break;
        case Mode::SpectrumAnalysis: spec = {sample_rate_, 0.0, 0.0, 0.0}; break;
    }

    /* Pick a channel rate at least 2.5x the channel bandwidth so the filter has
     * room for its transition band, then decimate by the largest integer that
     * still clears it. */
    const double min_channel_rate = std::max(spec.bandwidth_hz * 2.5, 24000.0);
    size_t decim = 1;
    if (sample_rate_ > min_channel_rate)
        decim = static_cast<size_t>(std::floor(sample_rate_ / min_channel_rate));
    if (decim < 1) decim = 1;

    const double channel_rate = sample_rate_ / static_cast<double>(decim);
    channel_rate_.store(channel_rate);

    if (mode_ == Mode::SpectrumAnalysis) {
        /* Nothing to demodulate; the DSP thread only feeds the spectrum tap. */
        return;
    }

    /* Channel filter: half the channel bandwidth on each side of DC, with a
     * transition band a quarter as wide again. */
    const double cutoff = spec.bandwidth_hz / 2.0;
    const double transition = std::max(cutoff * 0.25, 500.0);
    channel_filter_.configure(
        dsp::design_lowpass(cutoff, transition, sample_rate_, 60.0, 1023), decim);

    /* Audio side: decimate the demodulated audio down towards 48 kHz. */
    size_t audio_decim = 1;
    if (channel_rate > 48000.0)
        audio_decim = static_cast<size_t>(std::floor(channel_rate / 48000.0));
    if (audio_decim < 1) audio_decim = 1;

    const double audio_rate = channel_rate / static_cast<double>(audio_decim);
    const double audio_cutoff = std::min(spec.audio_hz, audio_rate * 0.45);
    audio_filter_.configure(
        dsp::design_lowpass(audio_cutoff, audio_cutoff * 0.3, channel_rate, 60.0, 511),
        audio_decim);

    switch (mode_) {
        case Mode::AMAudio:
            if (am_config_ == AmConfig::USB || am_config_ == AmConfig::CW) {
                ssb_.configure(static_cast<float>(channel_rate),
                               dsp::SsbDemod::Sideband::Upper, 127);
            } else if (am_config_ == AmConfig::LSB) {
                ssb_.configure(static_cast<float>(channel_rate),
                               dsp::SsbDemod::Sideband::Lower, 127);
            } else {
                am_.configure(static_cast<float>(channel_rate));
            }
            break;

        case Mode::NarrowbandFMAudio:
        case Mode::WidebandFMAudio:
            fm_.configure(static_cast<float>(channel_rate),
                          static_cast<float>(spec.deviation_hz));
            break;

        default:
            break;
    }

    if (spec.deemph_us > 0.0)
        deemph_.configure(static_cast<float>(spec.deemph_us), static_cast<float>(audio_rate));
    else
        deemph_.configure(0.0f, static_cast<float>(audio_rate));

    agc_.configure(static_cast<float>(audio_rate), 5.0f, 300.0f, 0.35f, 64.0f);
    agc_.set_enabled(audio_agc_enabled_);

    squelch_.configure(static_cast<float>(channel_rate));
    squelch_.set_level(squelch_level_);

    resampler_.configure(audio_rate, static_cast<double>(audio::sample_rate));

    nco_.set_frequency(nco_.frequency(), sample_rate_);
}

/* --- DSP thread ------------------------------------------------------------ */

void ReceiverModel::dsp_thread_main() {
    std::vector<dsp::cfloat> raw(kBlockSamples);
    std::vector<dsp::cfloat> mixed(kBlockSamples);
    std::vector<dsp::cfloat> channel;
    std::vector<float> demodulated;
    std::vector<float> audio_block;
    std::vector<float> resampled;

    channel.reserve(kBlockSamples);
    demodulated.reserve(kBlockSamples);
    audio_block.reserve(kBlockSamples);
    resampled.reserve(kBlockSamples);

    auto& ring = radio_.rx_ring();

    while (!stop_.load()) {
        if (chain_dirty_.exchange(false)) rebuild_chain();

        const size_t got = ring.read(raw.data(), raw.size());
        if (got == 0) {
            /* Nothing buffered yet — yield rather than spin. At 2.4 Msps a full
             * block arrives every ~3.4 ms, so 1 ms keeps latency low. */
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        /* Spectrum tap: keep the most recent raw samples, pre-channel-filter,
         * so the waterfall shows the whole captured band. */
        {
            std::lock_guard<std::mutex> g{spectrum_mutex_};
            const size_t n = std::min(got, spectrum_buffer_.size());
            /* Slide the buffer and append the newest n samples. */
            if (n < spectrum_buffer_.size()) {
                std::move(spectrum_buffer_.begin() + static_cast<ptrdiff_t>(n),
                          spectrum_buffer_.end(), spectrum_buffer_.begin());
            }
            std::copy(raw.data() + (got - n), raw.data() + got,
                      spectrum_buffer_.end() - static_cast<ptrdiff_t>(n));
            spectrum_ready_ = true;
            spectrum_filled_once_ = true;
        }

        /* Gapless tap: the same pre-channel-filter samples, but all of them and
         * in order, for decoders that cannot afford the holes the snapshot
         * above leaves between polls. Closed unless an app asked for it, in
         * which case this costs two atomics. */
        raw_tap_.write(raw.data(), got);

        /* Capture tap, before any channel filtering, so the file holds the full
         * captured band exactly as the radio delivered it. */
        if (capturing_.load()) {
            std::lock_guard<std::mutex> g{capture_mutex_};
            if (capture_file_ && capture_file_->good()) {
                capture_scratch_.resize(got * 2);
                for (size_t i = 0; i < got; i++) {
                    /* fc32 samples are nominally in [-1, 1]; scale to int16 and
                     * clamp so an overdriven front end saturates rather than
                     * wrapping to the opposite rail. */
                    const float re = std::clamp(raw[i].real(), -1.0f, 1.0f);
                    const float im = std::clamp(raw[i].imag(), -1.0f, 1.0f);
                    capture_scratch_[i * 2 + 0] = static_cast<int16_t>(std::lrint(re * 32767.0f));
                    capture_scratch_[i * 2 + 1] = static_cast<int16_t>(std::lrint(im * 32767.0f));
                }
                const auto bytes = capture_scratch_.size() * sizeof(int16_t);
                capture_file_->write(reinterpret_cast<const char*>(capture_scratch_.data()),
                                     static_cast<std::streamsize>(bytes));
                if (capture_file_->good())
                    captured_bytes_ += bytes;
                else
                    capturing_.store(false);  /* disk full or gone: stop cleanly */
            }
        }

        std::lock_guard<std::mutex> g{chain_mutex_};

        if (mode_ == Mode::SpectrumAnalysis) continue;

        nco_.mix(raw.data(), mixed.data(), got);

        channel.clear();
        channel_filter_.process(mixed.data(), got, channel);
        if (channel.empty()) continue;

        const float level = dsp::rms(channel.data(), channel.size());
        channel_level_db_.store(dsp::to_db(level));
        const bool open = squelch_.update(level);
        squelch_open_.store(open);

        /* Monitor off: this app decodes from its own tap and wants no sound,
         * so skip the entire demod-to-speaker chain. The channel level above
         * is still published (some decoder UIs show it), and with nothing
         * written to the audio ring the output goes idle and the DAC stops
         * clocking silence. Decoding is unaffected — it reads the raw tap,
         * not this. */
        if (!audio_monitor_.load()) continue;

        demodulated.clear();
        switch (mode_) {
            case Mode::AMAudio:
                if (am_config_ == AmConfig::USB || am_config_ == AmConfig::LSB ||
                    am_config_ == AmConfig::CW) {
                    ssb_.process(channel.data(), channel.size(), demodulated);
                } else {
                    am_.process(channel.data(), channel.size(), demodulated);
                }
                break;

            case Mode::NarrowbandFMAudio:
            case Mode::WidebandFMAudio:
                fm_.process(channel.data(), channel.size(), demodulated);
                break;

            default:
                break;
        }

        if (demodulated.empty()) continue;

        audio_block.clear();
        audio_filter_.process(demodulated.data(), demodulated.size(), audio_block);
        if (audio_block.empty()) continue;

        deemph_.process(audio_block.data(), audio_block.size());
        agc_.process(audio_block.data(), audio_block.size());

        /* A closed squelch mutes rather than stopping the chain, so filter and
         * AGC state stay warm and the first syllable after it opens is clean. */
        if (!open) std::fill(audio_block.begin(), audio_block.end(), 0.0f);

        resampled.clear();
        resampler_.process(audio_block.data(), audio_block.size(), resampled);
        if (resampled.empty()) continue;

        const size_t written = audio_.write(resampled.data(), resampled.size());
        if (written < resampled.size())
            audio_dropped_ += static_cast<uint32_t>(resampled.size() - written);
    }
}

}  // namespace radio
