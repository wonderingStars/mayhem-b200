/*
 * mayhem-b200 — IQ file replay.
 *
 * The host counterpart of the firmware's ReplayThread plus proc_replay. On a
 * PortaPack the app thread refills a set of shared-memory buffers that the M4
 * drains at the baseband clock; here a DSP thread reads the capture, resamples
 * it to whatever rate the radio is running, and pushes it into the transmit
 * ring — paced by a steady_clock so the file plays at its own sample rate
 * rather than as fast as the disk can supply it.
 *
 * Structured like ReceiverModel: one thread, atomics for the state the UI
 * polls in on_frame_sync(), a mutex where the UI has to touch the thread's
 * objects (seeking).
 *
 * The output is a generic sink rather than a TransmitterModel reference, so
 * this compiles and is testable without any radio present. main() wires
 * set_ring_sink(&radio.tx_ring()) — or a callback — once the transmit chain
 * exists.
 *
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_REPLAY_MODEL_H__
#define __MB200_REPLAY_MODEL_H__

#include "../core/iq_file.hpp"
#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "../dsp/ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace radio {

class ReplayModel {
   public:
    enum class State : uint8_t {
        Stopped = 0,
        Playing = 1,
        Paused = 2,
    };

    /* Accepts up to `count` samples, returns how many it took. A short return
     * means the sink is full; ReplayModel holds the remainder and retries, so
     * a sink must never block. */
    using SampleSink = std::function<size_t(const dsp::cfloat* samples, size_t count)>;

    ReplayModel();
    ~ReplayModel();

    ReplayModel(const ReplayModel&) = delete;
    ReplayModel& operator=(const ReplayModel&) = delete;

    /* --- Output wiring ---
     * Both reject the change while a thread is running, so the DSP thread never
     * sees the sink swapped underneath it. Stop first. */

    bool set_sink(SampleSink sink);
    bool set_ring_sink(dsp::RingBuffer<dsp::cfloat>* ring);
    bool clear_sink();
    bool has_sink() const;

    /* Rate the sink expects, in Hz. 0 (the default) means "whatever the file
     * is", i.e. no resampling — main() sets this to the transmitter's actual
     * sample rate. */
    void set_output_sample_rate(double hz);
    double output_sample_rate() const { return output_rate_; }

    /* The rate samples will actually leave at: the output rate when one is set,
     * otherwise the file's. */
    double effective_output_rate() const;

    /* How far ahead of real time to run, in milliseconds. This is the buffer
     * that absorbs disk and scheduler jitter; it is handed to the sink up front
     * when playback starts. */
    void set_prefill_ms(double ms);
    double prefill_ms() const { return prefill_ms_; }

    /* --- File ---
     * Opens a .C16 or .C8 capture and its .TXT sidecar. When there is no
     * sidecar the file's rate falls back to default_sample_rate() — upstream's
     * PlaylistView does the same thing with 500 kHz — and has_metadata() is
     * false so the caller can say so on screen instead of pretending. */

    bool open(const std::string& path);
    void close();
    bool is_open() const;

    const std::string& path() const { return path_; }
    bool has_metadata() const { return has_metadata_; }
    const core::CaptureMetadata& metadata() const { return metadata_; }

    /* Centre frequency from the sidecar, 0 when unknown. The caller tunes the
     * transmitter with it; ReplayModel does not touch the radio. */
    uint64_t center_frequency() const { return metadata_.center_frequency; }

    double file_sample_rate() const { return file_rate_; }
    core::IqFormat format() const { return format_; }

    void set_default_sample_rate(double hz);
    double default_sample_rate() const { return default_rate_; }

    /* --- Transport --- */

    /* Starts playing, or resumes from a pause. Fails if no file is open, the
     * file is empty, or no sink is wired. */
    bool play();
    void pause();
    bool resume() { return play(); }

    /* Stops and rewinds to the start. Joins the DSP thread, so it is safe to
     * re-wire the sink afterwards. */
    void stop();

    /* pause() and stop() both return only once the DSP thread has actually
     * come to rest, so position_samples() read straight afterwards is a
     * settled value rather than one more block's worth ahead. */

    void set_loop(bool enabled) { loop_.store(enabled); }
    bool loop() const { return loop_.load(); }

    State state() const { return state_.load(); }
    bool playing() const { return state_.load() == State::Playing; }
    bool paused() const { return state_.load() == State::Paused; }

    /* True once the file has played to the end without looping. Cleared by the
     * next play() or seek(). */
    bool finished() const { return finished_.load(); }

    /* --- Position and progress --- */

    uint64_t total_samples() const { return total_samples_; }
    uint64_t position_samples() const { return position_.load(); }

    /* 0.0 .. 1.0, or 0.0 for an empty/closed file. */
    double progress() const;

    double duration_seconds() const;
    double elapsed_seconds() const;

    /* Moves the read point. Legal while stopped, paused or playing; the pacing
     * clock restarts from the new position. */
    bool seek_samples(uint64_t sample_index);
    bool seek_seconds(double seconds);

    /* --- Diagnostics --- */

    /* Samples the sink has accepted since play(). Note that position() and
     * finished() track what has been *read from the file*, which runs
     * prefill_ms() ahead of what has actually gone on the air — the same thing
     * upstream's transmit progress bar reports. */
    uint64_t samples_delivered() const { return delivered_.load(); }

    /* Times the sink was full and playback had to wait. Non-zero means the
     * transmit ring is not being drained fast enough. */
    uint32_t stalls() const { return stalls_.load(); }

    /* Completed passes through the file, when looping. */
    uint32_t loops_completed() const { return loops_.load(); }

    /* By value, not by reference: the DSP thread writes this when a read fails
     * mid-playback, so handing out a reference would race. */
    std::string error() const;

   private:
    void dsp_thread_main();
    void start_thread();
    void join_thread();
    void wait_for_idle();
    void configure_resamplers();
    void reset_pacing();
    size_t push(const dsp::cfloat* samples, size_t count);
    void set_error(std::string message);

    core::IqFileReader reader_{};
    std::string path_{};
    core::IqFormat format_{core::IqFormat::Unknown};
    core::CaptureMetadata metadata_{};
    bool has_metadata_{false};
    double file_rate_{0.0};
    double default_rate_{500'000.0};
    double output_rate_{0.0};
    double prefill_ms_{50.0};
    uint64_t total_samples_{0};

    mutable std::mutex error_mutex_;
    std::string error_{};

    /* Guards reader_ and the two resamplers — the DSP thread reads and
     * resamples through them, the UI thread seeks and reconfigures. */
    mutable std::mutex reader_mutex_;

    /* Guards the sink against being swapped mid-block. Only contended when the
     * UI rewires the output, which happens while stopped. */
    mutable std::mutex sink_mutex_;
    SampleSink sink_{};

    /* Pacing clock. Written by the UI thread on play/pause/seek, read by the
     * DSP thread once per block. */
    mutable std::mutex pace_mutex_;
    std::chrono::steady_clock::time_point clock_origin_{};
    std::chrono::steady_clock::time_point pause_started_{};
    uint64_t paced_samples_{0};

    std::thread dsp_thread_{};
    std::atomic<bool> thread_running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Stopped};
    std::atomic<bool> loop_{false};
    std::atomic<bool> finished_{false};
    std::atomic<uint64_t> position_{0};
    std::atomic<uint64_t> delivered_{0};
    std::atomic<uint32_t> stalls_{0};
    std::atomic<uint32_t> loops_{0};

    /* Set by a seek while the thread is running: pending_ belongs to the DSP
     * thread, so the UI asks it to drop the block rather than freeing it from
     * underneath. */
    std::atomic<bool> flush_pending_{false};

    /* Raised by the DSP thread whenever it is parked between blocks. pause()
     * waits on it so a block already in flight cannot advance the position
     * after pause() has returned. */
    std::atomic<bool> idle_ack_{false};

    /* DSP-thread scratch. */
    std::vector<dsp::cfloat> input_{};
    std::vector<float> in_i_{};
    std::vector<float> in_q_{};
    std::vector<float> out_i_{};
    std::vector<float> out_q_{};
    std::vector<dsp::cfloat> output_{};
    std::vector<dsp::cfloat> pending_{};
    size_t pending_offset_{0};

    dsp::Resampler resample_i_{};
    dsp::Resampler resample_q_{};
    bool resampling_{false};
};

}  // namespace radio

#endif /*__MB200_REPLAY_MODEL_H__*/
