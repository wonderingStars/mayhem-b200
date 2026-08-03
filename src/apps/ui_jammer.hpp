/*
 * mayhem-b200 — Jammer (deliberate-interference transmitter).
 *
 * Host port of PortaPack Mayhem's Jammer app
 * (application/external/jammer/ui_jammer.* + baseband/proc_jammer.*, with the
 * shared types from common/jammer.hpp and common/portapack_shared_memory.hpp).
 *
 * WARNING — legality. This app radiates deliberate interference across a chosen
 * band. Transmitting a jamming signal is ILLEGAL in almost every jurisdiction.
 * It is ported only because Mayhem carries it; the view shows a permanent
 * warning, never transmits on its own, and requires an explicit confirmed Start.
 *
 * What is a faithful port vs. a host adaptation
 * ---------------------------------------------
 * Bit-faithful to upstream (the encoder — tested against known vectors):
 *
 *   - The range->channel table (JammerView::update_config): a range at least one
 *     channel wide is split into floor(bw / 1 MHz) equal channels; a narrower
 *     range is one channel. At most 80 channels total. See plan_channels().
 *   - The 32-bit Fibonacci noise LFSR (taps 32,30,16,12 i.e. bits 31,29,15,11,
 *     seed 0xDEAD0012, 0x1337 zero-guard). See JammerLfsr.
 *   - The per-type noise "sample" generator (proc_jammer.cpp execute()): FSK,
 *     TONE, SWEEP, RANDOM, SINE, SQUARE, SAWTOOTH, TRIANGLE, CHIRP, GAUSSIAN,
 *     BRUTEFORCE, including the int8 wrap semantics. See jammer_wave_step().
 *   - The round-robin channel hop with a per-channel dwell (jammer_duration).
 *     See Engine::advance_hop(); the first channel visited is index 1 for a
 *     multi-channel plan, exactly as upstream's do/while lands.
 *
 * Host adaptation (documented, consistent with dsp/modulate.hpp's philosophy):
 *
 *   - The firmware baseband makes DC-centred FM noise at a fixed 3.072 Msps and
 *     retunes the hardware LO per channel (RetuneMessage). A B200 LO cannot be
 *     retuned per-millisecond, so the host places every channel at its offset
 *     from a single streamed centre with a software NCO and streams a band wide
 *     enough to cover the enabled span. The int8 phase accumulator becomes a
 *     double, and the deviation is taken directly in Hz (sample/128 * width/2),
 *     which is the same modulation the fixed-point produced (±width/2 at full
 *     sample amplitude) without the quantisation. Spans wider than the device's
 *     streamable bandwidth cannot all radiate at once — the view says so.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2025 RocketGod (Flipper-derived extra modes)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_JAMMER_H__
#define __MB200_UI_JAMMER_H__

#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace app::jammer {

/* --- Constants (common/jammer.hpp) ---------------------------------------- */

constexpr int64_t kChannelWidthHz = 1'000'000;  /* JAMMER_CH_WIDTH */
constexpr size_t kMaxChannels = 80;             /* JAMMER_MAX_CH   */

/* The firmware baseband runs at exactly this rate; the fixed-point dwell counts
 * (30720 samples = 10 ms) are expressed against it and rescaled to whatever the
 * host streams. */
constexpr double kBasebandRate = 3'072'000.0;

/* Jamming waveform types. Values match upstream JammerType exactly, which is the
 * same numbering the UI's options_type index carries. */
enum class Type : uint32_t {
    Fsk = 0,        /* "Rand FSK" */
    Tone = 1,       /* "FM tone"  */
    Sweep = 2,      /* "CW sweep" */
    Random = 3,     /* "Noise"    */
    Sine = 4,
    Square = 5,
    Sawtooth = 6,
    Triangle = 7,
    Chirp = 8,
    Gaussian = 9,
    Bruteforce = 10,
};

/* proc_jammer applies a /256 (>>8) to the noise period for these eight types, so
 * their sample generator ticks 256x faster than FSK/TONE/RANDOM at the same
 * "speed". Note SWEEP is fast but RANDOM is not — it is not a simple threshold. */
inline bool uses_fast_period(Type t) {
    switch (t) {
        case Type::Sweep:
        case Type::Sine:
        case Type::Square:
        case Type::Sawtooth:
        case Type::Triangle:
        case Type::Chirp:
        case Type::Gaussian:
        case Type::Bruteforce:
            return true;
        default:
            return false;
    }
}

/* --- Range and channel (common/portapack_shared_memory.hpp) ---------------- */

/* jammer_range_t: a user-entered frequency window. */
struct Range {
    bool enabled{false};
    int64_t min{0};
    int64_t max{0};
};

/* JammerChannel: one atomic jamming slot the sequencer hops onto.
 *   center     : Hz
 *   width_hz   : the channel's own width in Hz (the FM deviation is ±width/2)
 *   duration_bb: dwell in 3.072-Msps samples, exactly as set_jammer_channel()
 *                encodes it (hop ? 30720*hop : 3000). */
struct Channel {
    bool enabled{false};
    uint64_t center{0};
    uint32_t width_hz{0};
    uint32_t duration_bb{0};
};

enum class PlanStatus { Ok, NoRangeEnabled, TooManyChannels };

struct Plan {
    std::vector<Channel> channels{};
    PlanStatus status{PlanStatus::NoRangeEnabled};
};

/* set_jammer_channel()'s dwell encoding: hop is in units of 10 ms; 0 means
 * "hop off", a short 3000-sample (~0.98 ms) dwell per channel. */
inline uint32_t hop_duration_bb(uint32_t hop_value) {
    return hop_value ? (30720u * hop_value) : 3000u;
}

/* Port of JammerView::update_config(). Splits the enabled ranges into channels
 * exactly as upstream does, capped at kMaxChannels. Pure and testable. */
inline Plan plan_channels(const std::array<Range, 3>& ranges, uint32_t hop_value) {
    Plan plan;
    const uint32_t dur = hop_duration_bb(hop_value);
    size_t i = 0;
    bool out_of_ranges = false;

    for (size_t r = 0; r < ranges.size() && !out_of_ranges; ++r) {
        if (!ranges[r].enabled) continue;

        const int64_t range_bw = std::llabs(ranges[r].max - ranges[r].min);
        const int64_t start_freq =
            (ranges[r].min < ranges[r].max) ? ranges[r].min : ranges[r].max;

        if (range_bw >= kChannelWidthHz) {
            /* num_channels = floor(range_bw / kChannelWidthHz), via upstream's
             * do/while so the boundary case matches bit-for-bit. */
            size_t num_channels = 0;
            int64_t range_bw_sub = range_bw;
            do {
                range_bw_sub -= kChannelWidthHz;
                num_channels++;
            } while (range_bw_sub >= kChannelWidthHz);

            const int64_t ch_width = range_bw / static_cast<int64_t>(num_channels);

            for (size_t c = 0; c < num_channels; ++c) {
                if (i >= kMaxChannels) {
                    out_of_ranges = true;
                    break;
                }
                Channel ch;
                ch.enabled = true;
                ch.width_hz = static_cast<uint32_t>(ch_width);
                ch.center = static_cast<uint64_t>(start_freq + (ch_width / 2) +
                                                  ch_width * static_cast<int64_t>(c));
                ch.duration_bb = dur;
                plan.channels.push_back(ch);
                i++;
            }
        } else {
            if (i >= kMaxChannels) {
                out_of_ranges = true;
            } else {
                Channel ch;
                ch.enabled = true;
                ch.width_hz = static_cast<uint32_t>(range_bw);
                ch.center = static_cast<uint64_t>(start_freq + range_bw / 2);
                ch.duration_bb = dur;
                plan.channels.push_back(ch);
                i++;
            }
        }
    }

    if (!out_of_ranges && i > 0)
        plan.status = PlanStatus::Ok;
    else if (out_of_ranges)
        plan.status = PlanStatus::TooManyChannels;
    else
        plan.status = PlanStatus::NoRangeEnabled;
    return plan;
}

/* --- Sine table (common/sine_table_int8.hpp), private copy ----------------- */

namespace detail {
inline constexpr int8_t kSineI8[256] = {
    0, 2, 5, 8, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 42, 45,
    48, 51, 54, 57, 59, 62, 65, 67, 70, 73, 75, 78, 80, 83, 85, 87,
    90, 92, 94, 96, 98, 100, 102, 104, 105, 107, 109, 110, 112, 113, 115, 116,
    117, 118, 120, 121, 121, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127,
    127, 127, 127, 127, 126, 126, 126, 125, 125, 124, 123, 122, 121, 121, 120, 118,
    117, 116, 115, 113, 112, 110, 109, 107, 105, 104, 102, 100, 98, 96, 94, 92,
    90, 87, 85, 83, 80, 78, 75, 73, 70, 67, 65, 62, 59, 57, 54, 51,
    48, 45, 42, 39, 36, 33, 30, 27, 24, 21, 18, 15, 12, 8, 5, 2,
    0, -3, -6, -9, -13, -16, -19, -22, -25, -28, -31, -34, -37, -40, -43, -46,
    -49, -52, -55, -58, -60, -63, -66, -68, -71, -74, -76, -79, -81, -84, -86, -88,
    -91, -93, -95, -97, -99, -101, -103, -105, -106, -108, -110, -111, -113, -114, -116, -117,
    -118, -119, -121, -122, -122, -123, -124, -125, -126, -126, -127, -127, -127, -128, -128, -128,
    -128, -128, -128, -128, -127, -127, -127, -126, -126, -125, -124, -123, -122, -122, -121, -119,
    -118, -117, -116, -114, -113, -111, -110, -108, -106, -105, -103, -101, -99, -97, -95, -93,
    -91, -88, -86, -84, -81, -79, -76, -74, -71, -68, -66, -63, -60, -58, -55, -52,
    -49, -46, -43, -40, -37, -34, -31, -28, -25, -22, -19, -16, -13, -9, -6, -3};
}  // namespace detail

/* --- Noise LFSR (proc_jammer.cpp inline generator) ------------------------- */

/* NOT the same as common/lfsr_random.hpp's lfsr_iterate (that 31-bit one is used
 * only for the UI jitter). This is the 32-bit Fibonacci LFSR the baseband draws
 * its noise from. */
class JammerLfsr {
   public:
    static constexpr uint32_t kSeed = 0xDEAD0012u;

    explicit JammerLfsr(uint32_t seed = kSeed) : v_{seed} {}
    void reset(uint32_t seed = kSeed) { v_ = seed; }

    uint32_t value() const { return v_; }

    /* One iteration, matching upstream exactly:
     *   feedback = ((v>>31)^(v>>29)^(v>>15)^(v>>11)) & 1
     *   v = (v<<1) | feedback;  if (!v) v = 0x1337;  */
    uint32_t next() {
        const uint32_t feedback =
            ((v_ >> 31) ^ (v_ >> 29) ^ (v_ >> 15) ^ (v_ >> 11)) & 1u;
        v_ = (v_ << 1) | feedback;
        if (!v_) v_ = 0x1337u;
        return v_;
    }

   private:
    uint32_t v_;
};

/* --- Per-type noise sample generator --------------------------------------- */

/* Mutable state the sample generator carries between period ticks. */
struct WaveState {
    int8_t sample{0};
    uint32_t wave_phase{0};
    uint32_t wave_index{0};
    uint32_t tone_delta{0};
    float chirp_freq{0.0f};
};

/* One noise "period tick": updates st for the given type using the LFSR's
 * CURRENT value (as upstream does), then advances the LFSR. Returns st.sample.
 *
 * For Type::Tone the returned sample is not the emitted one — the caller derives
 * the emitted sample per output sample from tone_delta and its own aphase (see
 * Engine::generate); this call only refreshes tone_delta. */
inline int8_t jammer_wave_step(Type type, JammerLfsr& lfsr, WaveState& st) {
    const uint32_t l = lfsr.value();
    switch (type) {
        case Type::Fsk:
            st.sample = static_cast<int8_t>(
                (static_cast<uint32_t>(static_cast<int>(st.sample)) + l) >> 1);
            break;
        case Type::Tone:
            st.tone_delta = 150000u + (l >> 9);
            break;
        case Type::Sweep:
            st.sample = static_cast<int8_t>(st.sample + 1);
            break;
        case Type::Random:
            st.sample = static_cast<int8_t>(l & 0xFFu);
            break;
        case Type::Sine:
            st.wave_phase += 0x01000000u;
            st.sample = detail::kSineI8[(st.wave_phase >> 24) & 0xFFu];
            break;
        case Type::Square:
            st.wave_index = (st.wave_index + 1u) % 2u;
            st.sample = st.wave_index ? static_cast<int8_t>(127) : static_cast<int8_t>(-128);
            break;
        case Type::Sawtooth:
            st.wave_index = (st.wave_index + 1u) % 256u;
            st.sample = static_cast<int8_t>(
                (static_cast<int>(st.wave_index) * 127) / 255 - 128);
            break;
        case Type::Triangle: {
            st.wave_index = (st.wave_index + 1u) % 256u;
            const int tri = (st.wave_index < 128u)
                                ? static_cast<int>(st.wave_index)
                                : static_cast<int>(255u - st.wave_index);
            st.sample = static_cast<int8_t>(tri * 127 / 127 - 128);
            break;
        }
        case Type::Chirp:
            st.chirp_freq += 0.01f;
            if (st.chirp_freq > 1.0f) st.chirp_freq = 0.0f;
            st.wave_phase +=
                static_cast<uint32_t>(0x01000000 * (1.0f + st.chirp_freq));
            st.sample = detail::kSineI8[(st.wave_phase >> 24) & 0xFFu];
            break;
        case Type::Gaussian: {
            float u1 = static_cast<float>(l & 0xFFFFu) / 65536.0f;
            /* Upstream can feed log(0)->-inf here; guard so the int8 cast is not
             * UB. The one-LSB floor is inaudible in the noise it produces. */
            if (u1 <= 0.0f) u1 = 1.0f / 65536.0f;
            const float u2 = static_cast<float>((l >> 16) & 0xFFFFu) / 65536.0f;
            const float g = std::sqrt(-2.0f * std::log(u1)) *
                            std::cos(2.0f * static_cast<float>(M_PI) * u2);
            st.sample = static_cast<int8_t>(g * 32.0f);
            break;
        }
        case Type::Bruteforce:
            st.sample = 127;
            break;
    }
    lfsr.next();
    return st.sample;
}

/* --- IQ engine ------------------------------------------------------------- */

/* Owns the channel plan, the hop sequencer and the noise/NCO state, and streams
 * complex baseband for the transmitter's raw IQ source. Thread model: the UI
 * thread calls configure()/set_paused()/reset(); the DSP thread calls generate()
 * — a mutex serialises the two (locked once per block, not per sample). */
class Engine {
   public:
    /* channels : the enabled channel list from plan_channels().
     * type     : jamming waveform.
     * speed_hz : options_speed value (10..100000); noise sample-update rate.
     * stream_rate : the rate the transmitter is streaming (Hz).
     * tx_center   : the transmitter's centre frequency (Hz); channels are placed
     *               relative to it by the software NCO. */
    void configure(std::vector<Channel> channels,
                   Type type,
                   uint32_t speed_hz,
                   double stream_rate,
                   uint64_t tx_center) {
        std::lock_guard<std::mutex> lock(mutex_);
        channels_ = std::move(channels);
        type_ = type;
        stream_rate_ = stream_rate > 0.0 ? stream_rate : kBasebandRate;
        tx_center_ = tx_center;

        const uint32_t update_rate =
            uses_fast_period(type) ? (speed_hz * 256u) : speed_hz;
        double period = update_rate > 0
                            ? stream_rate_ / static_cast<double>(update_rate)
                            : 1.0;
        noise_period_ = period < 1.0 ? 1u
                                     : static_cast<uint32_t>(std::lround(period));

        /* Rescale each channel's fixed-point dwell to the streamed rate. */
        const double rate_ratio = stream_rate_ / kBasebandRate;
        duration_samples_.resize(channels_.size());
        for (size_t k = 0; k < channels_.size(); ++k) {
            const double d =
                static_cast<double>(channels_[k].duration_bb) * rate_ratio;
            duration_samples_[k] = d < 1.0 ? 1u : static_cast<uint32_t>(std::lround(d));
        }

        reset_locked();
    }

    void set_paused(bool paused) { paused_.store(paused); }
    bool paused() const { return paused_.load(); }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        reset_locked();
    }

    size_t channel_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return channels_.size();
    }

    /* Index of the currently active channel (into the compact channel list). */
    size_t current_channel() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_channel_;
    }

    /* Centre frequency of the currently active channel, or 0 if none. */
    uint64_t current_center() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (channels_.empty()) return 0;
        return channels_[current_channel_].center;
    }

    /* One per-sample hop update, split out so tests can drive the sequencer
     * without producing IQ. Advances to the next enabled channel when the dwell
     * elapses (round-robin), else counts the dwell down. Must hold no lock:
     * callers are generate() (already locked) and tests (single-threaded). */
    void advance_hop() {
        if (channels_.empty()) return;
        if (jammer_duration_ == 0) {
            current_channel_ = (current_channel_ + 1) % channels_.size();
            jammer_duration_ = duration_samples_[current_channel_];
        } else {
            jammer_duration_--;
        }
    }

    /* Fills out[0..count) with unit-magnitude complex baseband. Always returns
     * count (the jammer is a continuous source). Runs on the DSP thread. */
    size_t generate(std::complex<float>* out, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (channels_.empty()) {
            for (size_t i = 0; i < count; ++i) out[i] = {0.0f, 0.0f};
            return count;
        }

        const bool is_tone = (type_ == Type::Tone);
        const double two_pi = 2.0 * M_PI;

        for (size_t i = 0; i < count; ++i) {
            if (paused_.load()) {
                out[i] = {0.0f, 0.0f};
                continue;
            }

            advance_hop();
            const Channel& ch = channels_[current_channel_];

            /* Noise sample, gated by the period counter. */
            if (period_counter_ == 0) {
                period_counter_ = noise_period_;
                jammer_wave_step(type_, lfsr_, wave_);
            } else {
                period_counter_--;
            }

            int8_t sample = wave_.sample;
            if (is_tone) {
                aphase_ += wave_.tone_delta;
                sample = detail::kSineI8[(aphase_ >> 24) & 0xFFu];
            }

            /* FM deviation in Hz (±width/2 at full sample amplitude) plus the
             * channel's offset from the streamed centre, integrated into phase.*/
            const double deviation =
                (static_cast<double>(sample) / 128.0) *
                (static_cast<double>(ch.width_hz) / 2.0);
            const double offset =
                static_cast<double>(static_cast<int64_t>(ch.center) -
                                    static_cast<int64_t>(tx_center_));
            const double inst = offset + deviation;

            phase_ += inst / stream_rate_;
            phase_ -= std::floor(phase_);
            const double a = two_pi * phase_;
            out[i] = {static_cast<float>(std::cos(a)),
                      static_cast<float>(std::sin(a))};
        }
        return count;
    }

   private:
    void reset_locked() {
        lfsr_.reset();
        wave_ = WaveState{};
        aphase_ = 0;
        phase_ = 0.0;
        period_counter_ = 0;
        current_channel_ = 0;
        jammer_duration_ = 0;
    }

    mutable std::mutex mutex_;

    std::vector<Channel> channels_{};
    std::vector<uint32_t> duration_samples_{};
    Type type_{Type::Random};
    double stream_rate_{kBasebandRate};
    uint64_t tx_center_{0};
    uint32_t noise_period_{1};

    JammerLfsr lfsr_{};
    WaveState wave_{};
    uint32_t aphase_{0};
    double phase_{0.0};
    uint32_t period_counter_{0};

    size_t current_channel_{0};
    uint32_t jammer_duration_{0};

    std::atomic<bool> paused_{false};
};

}  // namespace app::jammer

/* --- View ------------------------------------------------------------------ */

namespace app {

class JammerView : public ui::View {
   public:
    JammerView();
    ~JammerView() override;

    JammerView(const JammerView&) = delete;
    JammerView& operator=(const JammerView&) = delete;

    void focus() override;
    void on_hide() override;
    void on_frame_sync() override;

    std::string title() const override { return "Jammer TX"; }

   private:
    static constexpr size_t kNumRanges = 3;

    void start_tx();
    void stop_tx();
    void update_plan_readout();
    std::array<jammer::Range, kNumRanges> collect_ranges() const;

    /* Duty-cycle scheduler (JammerView::on_timer): TX for field_timetx seconds,
     * pause for field_timepause seconds, with optional per-cycle LFSR jitter. */
    void tick_scheduler();
    static uint32_t lfsr_iterate31(uint32_t v);  /* common/lfsr_random.cpp */

    jammer::Engine engine_{};
    std::atomic<bool> transmitting_{false};
    bool confirmed_legal_{false};

    /* Scheduler state (upstream: seconds/mscounter/cooling). */
    bool cooling_{false};
    uint16_t sched_seconds_{0};
    int16_t sched_mscounter_{0};
    uint32_t jitter_lfsr_{1};

    double stream_rate_{jammer::kBasebandRate};
    uint64_t tx_center_{0};

    /* --- widgets --- */
    ui::Labels labels_{
        {{0, 0}, "Type", ui::Color::light_grey()},
        {{0, 18}, "Speed", ui::Color::light_grey()},
        {{120, 18}, "Hop", ui::Color::light_grey()},
        {{0, 200}, "TXs", ui::Color::light_grey()},
        {{80, 200}, "Pause", ui::Color::light_grey()},
        {{170, 200}, "Jit", ui::Color::light_grey()},
        {{0, 218}, "Gain", ui::Color::light_grey()}};

    ui::Text text_warning_{{0, 288, 240, 16}, ""};

    /* Range rows. Each: enable checkbox, centre frequency, width. */
    ui::Checkbox check_en_0_{{0, 40}, 6, "R1"};
    ui::Checkbox check_en_1_{{0, 88}, 6, "R2"};
    ui::Checkbox check_en_2_{{0, 136}, 6, "R3"};

    ui::FrequencyField field_center_0_{{72, 40}};
    ui::FrequencyField field_center_1_{{72, 88}};
    ui::FrequencyField field_center_2_{{72, 136}};

    ui::OptionsField options_width_0_{{72, 60}, 5, width_options()};
    ui::OptionsField options_width_1_{{72, 108}, 5, width_options()};
    ui::OptionsField options_width_2_{{72, 156}, 5, width_options()};

    ui::OptionsField options_type_{
        {40, 0},
        9,
        {{"Rand FSK", 0},
         {"FM tone", 1},
         {"CW sweep", 2},
         {"Noise", 3},
         {"Sine", 4},
         {"Square", 5},
         {"Sawtooth", 6},
         {"Triangle", 7},
         {"Chirp", 8},
         {"Gauss", 9},
         {"Brute", 10}}};

    ui::OptionsField options_speed_{
        {48, 18},
        6,
        {{"10Hz  ", 10},
         {"100Hz ", 100},
         {"1kHz  ", 1000},
         {"10kHz ", 10000},
         {"100kHz", 100000}}};

    ui::OptionsField options_hop_{
        {144, 18},
        5,
        {{"Off  ", 0},
         {"10ms ", 1},
         {"50ms ", 5},
         {"100ms", 10},
         {"1s   ", 100},
         {"2s   ", 200},
         {"5s   ", 500},
         {"10s  ", 1000}}};

    ui::NumberField field_timetx_{{32, 200}, 3, {1, 180}, 1, ' '};
    ui::NumberField field_timepause_{{128, 200}, 2, {0, 60}, 1, ' '};
    ui::NumberField field_jitter_{{200, 200}, 2, {0, 60}, 1, ' '};
    ui::NumberField field_gain_{{40, 218}, 3, {0, 89}, 1, ' '};

    ui::Text text_plan_{{0, 238, 240, 16}, "Ranges: --"};
    ui::Text text_status_{{0, 256, 240, 16}, "Idle"};

    ui::Button button_tx_{{140, 268, 96, 32}, "Start"};

    static ui::OptionsField::options_t width_options() {
        return {{"100k", 100000},
                {"250k", 250000},
                {"500k", 500000},
                {"1M", 1000000},
                {"2M", 2000000},
                {"5M", 5000000},
                {"10M", 10000000},
                {"20M", 20000000}};
    }

    ui::Checkbox* check_en(size_t i);
    ui::FrequencyField* field_center(size_t i);
    ui::OptionsField* options_width(size_t i);
};

}  // namespace app

#endif /*__MB200_UI_JAMMER_H__*/
