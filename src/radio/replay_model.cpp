/*
 * mayhem-b200 — IQ file replay.
 *
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "replay_model.hpp"

#include <algorithm>
#include <cmath>

namespace radio {

namespace {

/* Input samples read per DSP iteration. At 2.4 Msps that is ~27 ms of signal,
 * so a pause or a seek takes effect well inside one video frame; the pacing
 * clock never asks for more than real time has earned anyway. */
constexpr size_t kBlockSamples = 65536;

/* How long the DSP thread sleeps when it is ahead of the clock or the sink is
 * full. Short enough that the prefill absorbs the jitter, long enough not to
 * spin a core. */
constexpr auto kIdleSleep = std::chrono::milliseconds(1);

}  // namespace

ReplayModel::ReplayModel() {
    input_.resize(kBlockSamples);
}

ReplayModel::~ReplayModel() {
    stop();
    close();
}

/* --- Output wiring --------------------------------------------------------- */

bool ReplayModel::set_sink(SampleSink sink) {
    if (thread_running_.load()) {
        set_error("cannot change the output sink while playing");
        return false;
    }
    std::lock_guard<std::mutex> g{sink_mutex_};
    sink_ = std::move(sink);
    return true;
}

bool ReplayModel::set_ring_sink(dsp::RingBuffer<dsp::cfloat>* ring) {
    if (ring == nullptr) return clear_sink();

    return set_sink([ring](const dsp::cfloat* samples, size_t count) {
        return ring->write(samples, count);
    });
}

bool ReplayModel::clear_sink() {
    if (thread_running_.load()) {
        set_error("cannot change the output sink while playing");
        return false;
    }
    std::lock_guard<std::mutex> g{sink_mutex_};
    sink_ = nullptr;
    return true;
}

bool ReplayModel::has_sink() const {
    std::lock_guard<std::mutex> g{sink_mutex_};
    return static_cast<bool>(sink_);
}

void ReplayModel::set_output_sample_rate(double hz) {
    output_rate_ = (hz > 0.0) ? hz : 0.0;
    std::lock_guard<std::mutex> g{reader_mutex_};
    configure_resamplers();
}

double ReplayModel::effective_output_rate() const {
    return (output_rate_ > 0.0) ? output_rate_ : file_rate_;
}

void ReplayModel::set_prefill_ms(double ms) {
    prefill_ms_ = (ms > 0.0) ? ms : 0.0;
}

void ReplayModel::set_default_sample_rate(double hz) {
    if (hz > 0.0) default_rate_ = hz;
}

/* --- File ------------------------------------------------------------------ */

bool ReplayModel::open(const std::string& path) {
    stop();
    close();

    std::lock_guard<std::mutex> g{reader_mutex_};

    if (!reader_.open(path)) {
        set_error(reader_.error());
        return false;
    }

    path_ = path;
    format_ = reader_.format();
    total_samples_ = reader_.total_samples();
    has_metadata_ = reader_.has_metadata();
    metadata_ = reader_.metadata();

    /* No sidecar means no recorded sample rate. Upstream's PlaylistView falls
     * back to a fixed 500 kHz in the same situation; here the fallback is
     * settable and has_metadata() says it was used. */
    file_rate_ = has_metadata_ ? static_cast<double>(metadata_.sample_rate) : default_rate_;

    position_.store(0);
    delivered_.store(0);
    stalls_.store(0);
    loops_.store(0);
    finished_.store(false);

    configure_resamplers();
    set_error({});
    return true;
}

void ReplayModel::close() {
    stop();

    std::lock_guard<std::mutex> g{reader_mutex_};
    reader_.close();
    path_.clear();
    format_ = core::IqFormat::Unknown;
    metadata_ = core::CaptureMetadata{};
    has_metadata_ = false;
    file_rate_ = 0.0;
    total_samples_ = 0;
    position_.store(0);
    finished_.store(false);
}

bool ReplayModel::is_open() const {
    std::lock_guard<std::mutex> g{reader_mutex_};
    return reader_.is_open();
}

/* --- Transport ------------------------------------------------------------- */

bool ReplayModel::play() {
    const State s = state_.load();

    if (s == State::Playing) return true;

    if (s == State::Paused) {
        /* Clear the acknowledgement before unparking the thread, so a pause()
         * that follows immediately cannot be satisfied by the stale one. */
        idle_ack_.store(false);
        std::lock_guard<std::mutex> g{pace_mutex_};
        clock_origin_ += (std::chrono::steady_clock::now() - pause_started_);
        state_.store(State::Playing);
        return true;
    }

    /* Stopped: reap a thread that ran to the end of the file by itself. */
    join_thread();

    if (!is_open()) {
        set_error("no capture file is open");
        return false;
    }
    if (total_samples_ == 0) {
        set_error("capture file has no samples");
        return false;
    }
    if (!has_sink()) {
        set_error("no output sink is wired");
        return false;
    }
    if (file_rate_ <= 0.0) {
        set_error("capture file has no usable sample rate");
        return false;
    }

    {
        std::lock_guard<std::mutex> g{reader_mutex_};
        /* Playing again after reaching the end starts over, rather than
         * returning immediately at EOF. */
        if (finished_.load() || reader_.at_end()) {
            if (!reader_.seek(0)) {
                set_error(reader_.error());
                return false;
            }
            position_.store(0);
        }
        configure_resamplers();
    }

    finished_.store(false);
    delivered_.store(0);
    stalls_.store(0);
    loops_.store(0);
    pending_.clear();
    pending_offset_ = 0;

    reset_pacing();
    set_error({});

    stop_.store(false);
    idle_ack_.store(false);
    state_.store(State::Playing);
    start_thread();
    return true;
}

void ReplayModel::pause() {
    if (state_.load() != State::Playing) return;

    {
        std::lock_guard<std::mutex> g{pace_mutex_};
        pause_started_ = std::chrono::steady_clock::now();
        state_.store(State::Paused);
    }

    /* Let the block already in flight finish before returning, so the caller
     * sees a position that has stopped moving. */
    wait_for_idle();
}

void ReplayModel::stop() {
    stop_.store(true);
    join_thread();
    state_.store(State::Stopped);

    pending_.clear();
    pending_offset_ = 0;

    std::lock_guard<std::mutex> g{reader_mutex_};
    if (reader_.is_open()) {
        reader_.seek(0);
        position_.store(0);
    }
    finished_.store(false);
}

void ReplayModel::start_thread() {
    join_thread();
    thread_running_.store(true);
    dsp_thread_ = std::thread(&ReplayModel::dsp_thread_main, this);
}

void ReplayModel::join_thread() {
    if (dsp_thread_.joinable()) dsp_thread_.join();
    thread_running_.store(false);
}

void ReplayModel::wait_for_idle() {
    if (!thread_running_.load()) return;

    /* Bounded: a block is sub-millisecond and even a stalled sink returns to
     * the top of the loop every kIdleSleep, so this is a guard against a wedged
     * thread rather than an expected wait. */
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!idle_ack_.load() && thread_running_.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

/* --- Position -------------------------------------------------------------- */

double ReplayModel::progress() const {
    if (total_samples_ == 0) return 0.0;
    const double p = static_cast<double>(position_.load()) / static_cast<double>(total_samples_);
    return std::clamp(p, 0.0, 1.0);
}

double ReplayModel::duration_seconds() const {
    if (file_rate_ <= 0.0) return 0.0;
    return static_cast<double>(total_samples_) / file_rate_;
}

double ReplayModel::elapsed_seconds() const {
    if (file_rate_ <= 0.0) return 0.0;
    return static_cast<double>(position_.load()) / file_rate_;
}

bool ReplayModel::seek_samples(uint64_t sample_index) {
    {
        std::lock_guard<std::mutex> g{reader_mutex_};
        if (!reader_.is_open()) {
            set_error("no capture file is open");
            return false;
        }
        if (!reader_.seek(sample_index)) {
            set_error(reader_.error());
            return false;
        }
        /* The resamplers hold one sample of history; after a jump it belongs to
         * a different part of the file. */
        resample_i_.reset();
        resample_q_.reset();
    }

    position_.store(sample_index);
    finished_.store(false);

    /* Anything already handed to the sink stays there — but nothing further
     * from the old position should go out, and real time restarts here. */
    if (thread_running_.load()) {
        flush_pending_.store(true);
    } else {
        pending_.clear();
        pending_offset_ = 0;
    }

    reset_pacing();
    return true;
}

bool ReplayModel::seek_seconds(double seconds) {
    if (file_rate_ <= 0.0) {
        set_error("capture file has no usable sample rate");
        return false;
    }
    if (seconds < 0.0) seconds = 0.0;

    const double index = seconds * file_rate_;
    const double limit = static_cast<double>(total_samples_);
    return seek_samples(static_cast<uint64_t>(std::min(index, limit)));
}

/* --- Internals ------------------------------------------------------------- */

void ReplayModel::configure_resamplers() {
    const double out_rate = effective_output_rate();

    resampling_ = (out_rate > 0.0) && (file_rate_ > 0.0) &&
                  (std::fabs(out_rate - file_rate_) > 1e-6);

    if (resampling_) {
        resample_i_.configure(file_rate_, out_rate);
        resample_q_.configure(file_rate_, out_rate);
    }
}

void ReplayModel::reset_pacing() {
    std::lock_guard<std::mutex> g{pace_mutex_};
    clock_origin_ = std::chrono::steady_clock::now();
    pause_started_ = clock_origin_;
    paced_samples_ = 0;
}

size_t ReplayModel::push(const dsp::cfloat* samples, size_t count) {
    std::lock_guard<std::mutex> g{sink_mutex_};
    if (!sink_ || count == 0) return 0;
    return sink_(samples, count);
}

void ReplayModel::set_error(std::string message) {
    std::lock_guard<std::mutex> g{error_mutex_};
    error_ = std::move(message);
}

std::string ReplayModel::error() const {
    std::lock_guard<std::mutex> g{error_mutex_};
    return error_;
}

/* --- DSP thread ------------------------------------------------------------
 *
 * One pass per iteration:
 *
 *   drain anything the sink refused last time
 *     -> how many input samples has real time earned since play()?
 *        -> read that many, resample, hand to the sink
 *
 * Pacing is on the input side, at the file's own sample rate, because that is
 * the rate the capture was taken at and therefore the rate it has to go out at
 * for the signal to be correct in time as well as in frequency. */

void ReplayModel::dsp_thread_main() {
    /* Read once: prefill only matters at the start of a run, and the UI cannot
     * meaningfully change it mid-playback. */
    const uint64_t prefill_samples =
        static_cast<uint64_t>(std::max(0.0, prefill_ms_ * 0.001 * file_rate_));

    while (!stop_.load()) {
        if (state_.load() != State::Playing) {
            idle_ack_.store(true);
            std::this_thread::sleep_for(kIdleSleep);
            continue;
        }

        idle_ack_.store(false);

        /* 0. A seek landed: the held block is from the old position. */
        if (flush_pending_.exchange(false)) {
            pending_.clear();
            pending_offset_ = 0;
        }

        /* 1. Whatever the sink refused last time goes first, so samples never
         *    leave the file in the wrong order. */
        if (pending_offset_ < pending_.size()) {
            const size_t left = pending_.size() - pending_offset_;
            const size_t took = push(pending_.data() + pending_offset_, left);
            delivered_.fetch_add(took);
            pending_offset_ += took;

            if (pending_offset_ < pending_.size()) {
                stalls_.fetch_add(1);
                std::this_thread::sleep_for(kIdleSleep);
                continue;
            }

            pending_.clear();
            pending_offset_ = 0;
        }

        /* 2. How much does the clock say we owe? */
        uint64_t want64 = 0;
        {
            std::lock_guard<std::mutex> g{pace_mutex_};
            const double elapsed = std::chrono::duration<double>(
                                       std::chrono::steady_clock::now() - clock_origin_)
                                       .count();
            const uint64_t due =
                static_cast<uint64_t>(elapsed * file_rate_) + prefill_samples;
            if (due > paced_samples_) want64 = due - paced_samples_;
        }

        if (want64 == 0) {
            std::this_thread::sleep_for(kIdleSleep);
            continue;
        }

        const size_t want = static_cast<size_t>(std::min<uint64_t>(want64, kBlockSamples));

        /* 3. Read and resample under the lock the UI's seek also takes. */
        size_t got = 0;
        size_t produced = 0;
        bool read_failed = false;
        uint64_t new_position = 0;
        std::string read_error{};

        {
            std::lock_guard<std::mutex> g{reader_mutex_};

            got = reader_.read(input_.data(), want);
            read_failed = reader_.failed();
            new_position = reader_.position();
            if (read_failed) read_error = reader_.error();

            if (got > 0) {
                if (!resampling_) {
                    output_.assign(input_.begin(),
                                   input_.begin() + static_cast<ptrdiff_t>(got));
                } else {
                    in_i_.resize(got);
                    in_q_.resize(got);
                    for (size_t i = 0; i < got; i++) {
                        in_i_[i] = input_[i].real();
                        in_q_[i] = input_[i].imag();
                    }

                    /* Two real resamplers driven with identical rates and
                     * identical block sizes stay sample-for-sample in step, so
                     * I and Q cannot drift apart. */
                    out_i_.clear();
                    out_q_.clear();
                    resample_i_.process(in_i_.data(), got, out_i_);
                    resample_q_.process(in_q_.data(), got, out_q_);

                    const size_t n = std::min(out_i_.size(), out_q_.size());
                    output_.resize(n);
                    for (size_t i = 0; i < n; i++)
                        output_[i] = dsp::cfloat{out_i_[i], out_q_[i]};
                }
                produced = output_.size();
            }
        }

        if (got == 0) {
            if (read_failed) {
                set_error(read_error);
                state_.store(State::Stopped);
                break;
            }

            /* End of file. */
            if (loop_.load()) {
                bool rewound = false;
                {
                    std::lock_guard<std::mutex> g{reader_mutex_};
                    rewound = reader_.seek(0);
                    if (!rewound) {
                        read_error = reader_.error();
                    } else {
                        resample_i_.reset();
                        resample_q_.reset();
                    }
                }

                if (!rewound) {
                    set_error(read_error);
                    state_.store(State::Stopped);
                    break;
                }

                position_.store(0);
                loops_.fetch_add(1);
                continue;
            }

            finished_.store(true);
            state_.store(State::Stopped);
            break;
        }

        {
            std::lock_guard<std::mutex> g{pace_mutex_};
            paced_samples_ += got;
        }
        position_.store(new_position);

        /* 4. Hand it over; the remainder waits for the next pass. */
        if (produced > 0) {
            const size_t took = push(output_.data(), produced);
            delivered_.fetch_add(took);

            if (took < produced) {
                stalls_.fetch_add(1);
                pending_.assign(output_.begin() + static_cast<ptrdiff_t>(took), output_.end());
                pending_offset_ = 0;
            }
        }
    }

    /* At rest for good: release anything waiting on a pause acknowledgement. */
    idle_ack_.store(true);
    thread_running_.store(false);
}

}  // namespace radio
