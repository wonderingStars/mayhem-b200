/*
 * mayhem-b200 — Signal Hunter.
 *
 * Ported from firmware/application/external/signal_hunter/ui_signal_hunter.*
 * and its baseband processor firmware/baseband/proc_signal_hunter.*.
 *
 * What upstream does: the M4 decimates the 2 MHz baseband by 8 to 250 kHz, keeps
 * a 64-sample sliding mean of per-sample energy ((I*I + Q*Q) >> 16), and runs a
 * five-state machine — IDLE, AWAITING_STREAM, RECORDING, HANGTIME,
 * AWAITING_CLOSE. Crossing `energy_threshold` upward sends a HunterTrigger to
 * the M0, which opens a .C16 capture; falling below it starts a hangtime
 * countdown of `hangtime_ms * 250` post-decimation samples, and when that
 * expires a HunterStop closes the capture. The M0 half adds frequency hopping
 * (dwell `hop_dwell_ms`, advanced from DisplayFrameSync) and a three-tab UI.
 *
 * The port keeps all of that. What changes, and why:
 *
 *   - There is no M4 and no message queue. HuntDetector runs the identical
 *     integer energy/window/state maths on the host, fed from
 *     radio::ReceiverModel's sample tap in on_frame_sync(). The energy
 *     expression, the 64-entry window, the hangtime-to-samples conversion
 *     (including its "0 becomes 1" underflow guard) and the state transitions
 *     are upstream's, byte for byte.
 *   - Upstream's pre-trigger IQ ring (2048 samples flushed ahead of the live
 *     stream) is kept as PreRollRing so a capture still starts ~8 ms before the
 *     trigger. On the host it is drained into the capture file rather than into
 *     a StreamInput.
 *   - Recording uses radio::ReceiverModel::start_capture()/stop_capture(), which
 *     writes the same <name>.C16 + <name>.TXT (center_frequency, sample_rate)
 *     pair upstream writes. Filenames keep upstream's "HNT_yyyymmddThhmmss" form.
 *   - HOST ADDITION, clearly marked: an FFT peak search over the captured span.
 *     A PortaPack hunts one 250 kHz channel at a time; a B200 hands the host the
 *     whole captured band, so the app also reports *which* frequency in that
 *     band is hottest (peak_bin / bin_to_frequency, dsp::Fft). It is display and
 *     tuning assistance only — the trigger is still upstream's time-domain
 *     energy detector, unchanged.
 *
 * The sample tap: ReceiverModel exposes take_spectrum_samples(), the wideband
 * pre-channel-filter tap. That is what both the detector and the FFT run on
 * here. The ideal tap for a faithful port would be the post-channel-filter
 * decimated stream (upstream's decim_0 output at 250 kHz) — see the report; the
 * receiver does not expose one yet, so the sample rate the detector sees is the
 * capture rate rather than 250 kHz, and hangtime in milliseconds is converted
 * against the actual rate instead of the fixed 250 samples/ms.
 *
 * Copyright (C) 2026 Matej Sochan (original app and baseband processor)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SIGNAL_HUNTER_H__
#define __MB200_UI_SIGNAL_HUNTER_H__

#include "../dsp/fft.hpp"

#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace radio {
class ReceiverModel;
}

namespace app {

/* ======================================================================== *
 *  Pure Signal Hunter logic — no UI, no radio.                              *
 * ======================================================================== */
namespace signal_hunter {

/* proc_signal_hunter.hpp constants, verbatim. */
inline constexpr size_t kBasebandFs = 2'000'000;
inline constexpr size_t kDecimation = 8;                     /* FIRC8xR16x24FS4Decim8 */
inline constexpr size_t kChannelFs = kBasebandFs / kDecimation;  /* 250 kHz */
inline constexpr size_t kWindowSize = 64;                    /* WINDOW_SIZE */
inline constexpr size_t kIqRingSamples = 2048;               /* IQ_RING_SAMPLES */
inline constexpr uint32_t kDefaultThreshold = 5000;          /* energy_threshold */
inline constexpr uint32_t kDefaultHangtimeMs = 500;
inline constexpr uint32_t kDefaultDwellMs = 200;

/* proc_signal_hunter.hpp HuntState. */
enum class HuntState : uint8_t {
    Idle,
    AwaitingStream,
    Recording,
    Hangtime,
    AwaitingClose,
};

/* Per-sample energy, exactly proc_signal_hunter.cpp:
 *     ((int32_t)s.real() * s.real() + (int32_t)s.imag() * s.imag()) >> 16
 * Full-scale int16 both axes gives 2147352578 >> 16 = 32764, which is why the
 * int32 accumulator does not overflow. */
inline uint32_t sample_energy(int16_t re, int16_t im) {
    const int32_t e = static_cast<int32_t>(re) * re + static_cast<int32_t>(im) * im;
    return static_cast<uint32_t>(e >> 16);
}

/* Upstream's hangtime conversion. 1 ms = 250 samples at the 250 kHz
 * post-decimation rate; a zero result is forced to 1 to stop the
 * `--hangtime_counter == 0` pre-decrement underflowing. `samples_per_ms` is a
 * host parameter so the same maths works when the tap is not exactly 250 kHz —
 * pass 250 for the upstream value. */
inline uint32_t hangtime_to_samples(uint32_t hangtime_ms, uint32_t samples_per_ms) {
    uint32_t n = hangtime_ms * samples_per_ms;
    if (n == 0) n = 1;
    return n;
}

/* --- HuntDetector ----------------------------------------------------------
 * The sliding-window energy detector and its state machine, lifted out of
 * SignalHunterProcessor::execute(). One push() is one post-decimation sample. */
class HuntDetector {
   public:
    /* What the sample produced. Upstream pushed these onto the M0 queue as
     * HunterTriggerMessage / HunterStopMessage. */
    struct Event {
        bool trigger{false};  /* energy crossed up: open a capture   */
        bool stop{false};     /* hangtime expired: close the capture */
        uint32_t energy{0};   /* the window average that caused it   */
    };

    /* SignalHunterProcessor::configure() + on_message(HunterConfig). */
    void configure(uint32_t energy_threshold, uint32_t hangtime_samples) {
        energy_threshold_ = energy_threshold;
        hangtime_samples_limit_ = (hangtime_samples == 0) ? 1 : hangtime_samples;
        window_idx_ = 0;
        window_sum_ = 0;
        window_.fill(0);
        reset();
    }

    /* SignalHunterProcessor::reset_hunt_state(). */
    void reset() {
        hangtime_counter_ = 0;
        state_ = HuntState::Idle;
    }

    void set_hunting(bool v) { hunting_ = v; }
    bool hunting() const { return hunting_; }

    /* M0 answered the trigger and the capture is open — upstream's
     * CaptureConfigMessage with a config. */
    void begin_recording() { state_ = HuntState::Recording; }

    /* The capture has been closed — upstream's CaptureConfigMessage(nullptr),
     * the only place the stream is torn down and the state returns to IDLE. */
    void end_recording() { reset(); }

    HuntState state() const { return state_; }
    uint32_t average() const { return average_; }
    uint32_t threshold() const { return energy_threshold_; }
    uint32_t hangtime_samples() const { return hangtime_samples_limit_; }
    uint32_t hangtime_counter() const { return hangtime_counter_; }

    Event push(int16_t re, int16_t im) {
        Event ev{};

        const uint32_t energy = sample_energy(re, im);
        window_sum_ -= window_[window_idx_];
        window_[window_idx_] = energy;
        window_sum_ += energy;
        window_idx_ = (window_idx_ + 1) % kWindowSize;

        const uint32_t avg = window_sum_ / static_cast<uint32_t>(kWindowSize);
        average_ = avg;

        switch (state_) {
            case HuntState::Idle:
                /* Only trigger while actually hunting — stops a stray sample
                 * from starting a capture right after a manual STOP. */
                if (hunting_ && (avg > energy_threshold_)) {
                    ev.trigger = true;
                    ev.energy = avg;
                    state_ = HuntState::AwaitingStream;
                }
                break;

            case HuntState::AwaitingStream:
                break;

            case HuntState::Recording:
                if (avg < energy_threshold_) {
                    hangtime_counter_ = hangtime_samples_limit_;
                    state_ = HuntState::Hangtime;
                }
                break;

            case HuntState::Hangtime:
                if (avg > energy_threshold_) {
                    state_ = HuntState::Recording;
                } else if (--hangtime_counter_ == 0) {
                    ev.stop = true;
                    ev.energy = avg;
                    state_ = HuntState::AwaitingClose;
                }
                break;

            case HuntState::AwaitingClose:
                /* Wait for end_recording(); never self-transition. */
                break;
        }

        return ev;
    }

    /* Convenience for a float tap: scales [-1, 1] to the int16 domain the
     * upstream maths is defined in. */
    Event push(float re, float im) {
        const auto q = [](float v) {
            const float s = std::clamp(v, -1.0f, 1.0f) * 32767.0f;
            return static_cast<int16_t>(s);
        };
        return push(q(re), q(im));
    }

   private:
    std::array<uint32_t, kWindowSize> window_{};
    size_t window_idx_{0};
    uint32_t window_sum_{0};
    uint32_t average_{0};

    uint32_t energy_threshold_{kDefaultThreshold};
    uint32_t hangtime_samples_limit_{1};
    uint32_t hangtime_counter_{0};

    bool hunting_{false};
    HuntState state_{HuntState::Idle};
};

/* --- PreRollRing -----------------------------------------------------------
 * Upstream's iq_ring: the last IQ_RING_SAMPLES samples, flushed ahead of the
 * live stream when a capture opens so an OOK preamble is not lost.
 *
 * Ported and unit-tested, but NOT yet wired into SignalHunterView: the host
 * capture path (radio::ReceiverModel::start_capture) opens and owns the .C16
 * file itself and has no hook for prepending samples, so there is nowhere to
 * flush the ring to. Rather than pretend, the view records from the trigger
 * onward and this class waits for a receiver-side pre-roll hook. */
class PreRollRing {
   public:
    explicit PreRollRing(size_t capacity = kIqRingSamples)
        : buf_(capacity * 2, 0.0f), capacity_{capacity} {}

    void clear() {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        idx_ = 0;
    }

    void push(float re, float im) {
        buf_[idx_ * 2] = re;
        buf_[idx_ * 2 + 1] = im;
        idx_ = (idx_ + 1) % capacity_;
    }

    size_t capacity() const { return capacity_; }
    size_t write_index() const { return idx_; }

    /* Oldest-to-newest copy, the two-chunk wrap-around drain upstream does. */
    void drain(std::vector<float>& out) const {
        out.clear();
        out.reserve(capacity_ * 2);
        const size_t start = idx_;
        out.insert(out.end(), buf_.begin() + static_cast<std::ptrdiff_t>(start * 2), buf_.end());
        if (start > 0)
            out.insert(out.end(), buf_.begin(),
                       buf_.begin() + static_cast<std::ptrdiff_t>(start * 2));
    }

   private:
    std::vector<float> buf_;
    size_t capacity_;
    size_t idx_{0};
};

/* --- FFT peak search (HOST ADDITION) ---------------------------------------
 * Not in upstream: a PortaPack hunts a single channel, a B200 hands the host a
 * whole captured band. Finding the hottest bin in that band tells the operator
 * where in the span the signal actually is. The bin layout is dsp::Fft's, which
 * is -Fs/2 .. +Fs/2 with DC at nbins/2 — the same layout the Search port uses. */

struct PeakBin {
    int bin{-1};
    float db{-1000.0f};
};

/* Strongest bin, skipping `dc_guard` bins either side of DC (the AD936x LO
 * leakage spike sits there) and `edge_guard` bins at each end of the span. */
inline PeakBin find_peak_bin(const std::vector<float>& db, int dc_guard = 2,
                             int edge_guard = 2) {
    PeakBin p;
    const int n = static_cast<int>(db.size());
    if (n <= 0) return p;
    const int dc = n / 2;
    for (int i = edge_guard; i < n - edge_guard; i++) {
        if (i >= dc - dc_guard && i <= dc + dc_guard) continue;
        if (db[static_cast<size_t>(i)] > p.db) {
            p.db = db[static_cast<size_t>(i)];
            p.bin = i;
        }
    }
    return p;
}

/* Absolute frequency of a bin. Bin nbins/2 is the centre frequency. */
inline int64_t bin_to_frequency(int64_t center_hz, int bin, double span_hz, int nbins) {
    if (nbins <= 0) return center_hz;
    const double bw = span_hz / static_cast<double>(nbins);
    return center_hz + static_cast<int64_t>(bw * (bin - nbins / 2));
}

}  // namespace signal_hunter

/* ======================================================================== *
 *  Views                                                                    *
 * ======================================================================== */

class SignalHunterView;

/* Tab 1 — target frequency, hunt state, hit count. */
class HunterMainView : public ui::View {
   public:
    HunterMainView(ui::Rect parent_rect, SignalHunterView& parent);

    void focus() override;

    void update_frequency(int64_t freq);
    void update_status(const std::string& status, const ui::Style* style);
    void update_hits(uint32_t hits);
    void update_energy(uint32_t avg, uint32_t threshold);
    void update_peak(const std::string& text);
    void set_recording_state(bool recording);
    void set_start_button_text(const std::string& text) { button_start_stop_.set_text(text); }

   private:
    SignalHunterView& parent_;
    int64_t current_freq_{433'920'000};

    ui::Text text_current_freq_{{0, 8, 240, 16}, ""};
    ui::Text text_status_{{0, 32, 240, 16}, "IDLE"};
    ui::Text text_energy_{{0, 52, 240, 16}, ""};
    ui::Text text_peak_{{0, 72, 240, 16}, ""};
    ui::Button button_start_stop_{{72, 96, 96, 32}, "START"};
    ui::Text text_hits_{{0, 136, 240, 16}, "Hits: 0"};
};

/* Tab 2 — hop list and dwell time. */
class HunterFreqsView : public ui::View {
   public:
    HunterFreqsView(ui::Rect parent_rect, SignalHunterView& parent);

    void focus() override;
    void update_list_count();

   private:
    SignalHunterView& parent_;

    ui::Button button_load_file_{{8, 8, 100, 28}, "LOAD FILE"};
    ui::Button button_clear_{{124, 8, 100, 28}, "CLEAR"};

    ui::Labels labels_{
        {{8, 52}, "Dwell (ms)", ui::Color::light_grey()},
    };
    ui::NumberField field_dwell_{{104, 52}, 4, {10, 9999}, 10, ' '};

    ui::Text text_loaded_info_{{8, 80, 224, 16}, "Loaded: 0 freqs"};
};

/* Tab 3 — single frequency, threshold, hang time. */
class HunterConfigView : public ui::View {
   public:
    HunterConfigView(ui::Rect parent_rect, SignalHunterView& parent);

    void focus() override;
    void update_mode_display();

   private:
    SignalHunterView& parent_;

    ui::Button button_mode_{{8, 8, 224, 28}, "MODE: SINGLE"};

    ui::Labels labels_{
        {{8, 46}, "Freq", ui::Color::light_grey()},
        {{8, 74}, "Energy threshold", ui::Color::light_grey()},
        {{8, 96}, "Hang time (ms)", ui::Color::light_grey()},
    };

    ui::FrequencyField field_single_freq_{{48, 46}};
    ui::NumberField field_threshold_{{144, 74}, 5, {100, 99999}, 100, ' '};
    ui::NumberField field_hang_time_{{144, 96}, 4, {10, 5000}, 10, ' '};

    ui::Text text_info_{{8, 120, 224, 16}, "Restart HUNT after change"};
};

class SignalHunterView : public ui::View {
   public:
    SignalHunterView();
    ~SignalHunterView() override;

    SignalHunterView(const SignalHunterView&) = delete;
    SignalHunterView& operator=(const SignalHunterView&) = delete;

    std::string title() const override { return "SignalHunter"; }

    void on_show() override;
    void on_frame_sync() override;

    /* --- shared application state, as upstream's SignalHunterAppView --- */
    std::vector<int64_t> frequency_list{};
    size_t current_freq_index{0};
    uint32_t energy_threshold{signal_hunter::kDefaultThreshold};
    uint32_t hangtime_ms{signal_hunter::kDefaultHangtimeMs};
    uint32_t hop_dwell_ms{signal_hunter::kDefaultDwellMs};
    std::string freqman_file{"TARGETS"};
    bool is_hunting{false};
    uint32_t trigger_hits{0};
    int64_t single_frequency{433'920'000};
    bool freq_hop_mode{false};

    void send_hunter_config(bool start);
    void load_frequency_list(const std::string& stem);
    void set_target_frequency(int64_t hz);
    int64_t target_frequency() const;

    HunterMainView* main_view() { return &view_main_; }
    HunterConfigView* config_view() { return &view_config_; }

   private:
    void process_samples();
    void on_trigger(uint32_t energy);
    void on_stop();
    void start_recording();
    void stop_recording();
    void update_peak_readout();

    radio::ReceiverModel& receiver_;

    signal_hunter::HuntDetector detector_{};

    /* FFT machinery for the host peak search. */
    static constexpr int kFftBins = 256;
    dsp::Fft fft_{kFftBins};
    std::vector<float> window_{};
    std::vector<dsp::cfloat> samples_{};
    std::vector<float> spectrum_db_{};

    uint32_t hop_timer_ms_{0};
    uint32_t frame_counter_{0};
    std::string capture_name_{};

    static constexpr ui::Rect kTabRect{0, 40, 240, 24};
    static constexpr ui::Rect kContentRect{0, 66, 240, 238};

    ui::Labels labels_{
        {{0, 0}, "Gn", ui::Color::light_grey()},
        {{80, 0}, "Lvl", ui::Color::light_grey()},
    };
    ui::NumberField field_gain_{{24, 0}, 3, {0, 76}, 1, ' '};
    ui::Text text_level_{{112, 0, 128, 16}, ""};
    ui::VuMeter level_meter_{{0, 22, 240, 12}, 24, false};

    HunterMainView view_main_{kContentRect, *this};
    HunterFreqsView view_freqs_{kContentRect, *this};
    HunterConfigView view_config_{kContentRect, *this};

    ui::TabView tab_view_{{{"Target", ui::Color::cyan(), &view_main_},
                           {"Freqs", ui::Color::green(), &view_freqs_},
                           {"Config", ui::Color::yellow(), &view_config_}}};
};

}  // namespace app

#endif /*__MB200_UI_SIGNAL_HUNTER_H__*/
