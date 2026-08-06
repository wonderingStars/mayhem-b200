/*
 * mayhem-b200 — Two-Tone (Motorola Quik-Call II) sequential paging receiver.
 *
 * Ported from firmware/application/external/two_tone_rx/ (the view and its
 * detection state machine) plus firmware/baseband/proc_tonedetect.cpp (the
 * Goertzel tone-measurement front end). Upstream splits the work across two
 * cores: the M4 FM-demodulates the channel, runs a bank of 45 Goertzel filters
 * (one per Motorola/EIA QCII tone) over 40 ms windows, and pushes one
 * ToneDetectDataMessage per window to the M0; the M0's TwoToneRxView runs a
 * two-phase (A tone -> B tone) state machine over those messages and logs a
 * pair when both phases are long enough and both snap to table entries.
 *
 * On the host there is no second core and no message queue, so the same two
 * stages are plain classes driven from on_frame_sync():
 *
 *   ToneDetector      the Goertzel bank + CTCSS gate, emitting ToneWindow
 *                     records that are byte-for-byte the fields upstream sends
 *                     in ToneDetectDataMessage (freq_hz, duration_ms, tone_end)
 *   TwoToneSequencer  the A/B state machine from TwoToneRxView::on_tone_data
 *
 * Both are free of UI and radio dependencies so tests/test_two_tone_rx.cpp can
 * drive them with synthesised audio.
 *
 * SAMPLE TAP — see the note in ui_two_tone_rx.cpp. The ideal input is a
 * continuous 24 kHz FM-demodulated channel stream. ReceiverModel does not
 * expose one, so the view mixes/decimates/demodulates the wideband spectrum tap
 * itself. That tap is block-based and lossy; the decode logic is correct when
 * fed a continuous stream, but a real over-the-air pair may be missed.
 *
 * Copyright (C) 2024 PortaPack Mayhem (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_TWO_TONE_RX_H__
#define __MB200_UI_TWO_TONE_RX_H__

#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_recent_entries.hpp"
#include "ui_widget.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace radio {
class ReceiverModel;
}

namespace app {
namespace two_tone {

/* --- Tone tables ---------------------------------------------------------- */

inline constexpr size_t kMotoToneCount = 45;
inline constexpr size_t kCtcssOptionCount = 51;

/* Motorola / EIA "Quik-Call II" sequential paging tones, in tenths of a hertz.
 * Verbatim MOTO_FREQS_X10 from proc_tonedetect.cpp. */
extern const uint32_t kMotoFreqsX10[kMotoToneCount];

/* Index 0 = None (0), 1..50 = the standard CTCSS tones, tenths of a hertz.
 * Verbatim CTCSS_FREQS from ui_two_tone_rx.cpp. */
extern const uint32_t kCtcssFreqsX10[kCtcssOptionCount];

/* Sentinel returned by moto_index() when nothing is close enough. */
inline constexpr uint32_t kMotoNone = 255;

/* Upstream's three matching tolerances, all in whole hertz. */
inline constexpr uint32_t kMotoTransitionSnapHz = 30;   /* live A->B snap      */
inline constexpr uint32_t kMotoFinalMatchHz = 60;       /* phase-end / logging */
inline constexpr uint32_t kMotoTransitionDeltaHz = 25;  /* close-in A->B shift */

/* The baseband measurement window, 960 samples at 24 kHz. */
inline constexpr uint32_t kDetectWindowMs = 40;

std::string ctcss_name(uint32_t freq_x10);

/* Nearest MOTO table index within match_hz, or kMotoNone. */
uint32_t moto_index(uint32_t freq_hz, uint32_t match_hz = kMotoTransitionSnapHz);

/* "1153.4Hz 1000ms", snapping to the nearest table entry when one is within
 * kMotoFinalMatchHz. Upstream format_tone(). */
std::string format_tone(uint32_t freq_hz, uint32_t duration_ms);

/* --- Goertzel tone measurement (proc_tonedetect.cpp) ---------------------- */

/* One 40 ms measurement window. The fields are exactly ToneDetectDataMessage's:
 * `tone_end` false means "here is a measurement", true means "the detection
 * gate just closed". freq_hz of 0 means "no candidate in this window". */
struct ToneWindow {
    uint32_t freq_hz{0};
    uint32_t duration_ms{0};
    bool tone_end{false};
};

/* Second-order-section high-pass used as a noise gate, a port of the firmware's
 * FMSquelch: an elliptic HPF (scipy.signal.iirdesign wp=8k/24k ws=4k/24k
 * gpass=1 gstop=18, common/dsp_iir_config.hpp non_audio_hpf_config) whose peak
 * squared output over a block is compared against a threshold. Everything above
 * the audio band in an FM discriminator's output is noise, so a *low* reading
 * means a carrier is present. */
class FmNoiseSquelch {
   public:
    void set_threshold(float value) { threshold_squared_ = value * value; }
    bool enabled() const { return threshold_squared_ > 0.0f; }
    void reset();

    /* True when the block's noise is below threshold (carrier present). A
     * threshold of zero disables the gate and always returns true, as upstream
     * does. */
    bool execute(const float* audio, size_t count);

   private:
    float threshold_squared_{0.0f};
    std::array<float, 3> x_{{0.0f, 0.0f, 0.0f}};
    std::array<float, 3> y_{{0.0f, 0.0f, 0.0f}};
};

class ToneDetector {
   public:
    /* `audio_rate_hz` is the rate of the samples handed to process().
     * `ctcss_freq_x10` of 0 disables the CTCSS half of the gate, exactly as
     * upstream's "None" option does. */
    void configure(float audio_rate_hz, uint32_t ctcss_freq_x10);
    void reset();

    /* The carrier half of the detection gate. Upstream derives it inside the
     * baseband from FMSquelch at a fixed 0.20 threshold; the host view does the
     * same with FmNoiseSquelch and pushes the answer in here, which also lets
     * tests drive the gate directly. */
    void set_carrier_open(bool open) { carrier_open_ = open; }
    bool carrier_open() const { return carrier_open_; }

    /* Consumes `count` audio samples and appends one ToneWindow per completed
     * window. */
    void process(const float* audio, size_t count, std::vector<ToneWindow>& out);

    size_t window_samples() const { return window_samples_; }
    uint32_t window_ms() const { return window_ms_; }
    float moto_energy_threshold() const { return moto_threshold_; }
    /* Per-bin coherent energy of the most recently completed window; index
     * order matches kMotoFreqsX10. Empty until the first window completes. */
    const std::array<float, kMotoToneCount>& last_energies() const { return last_energies_; }

   private:
    void close_window(std::vector<ToneWindow>& out);

    float sample_rate_{24000.0f};
    size_t window_samples_{960};
    uint32_t window_ms_{kDetectWindowMs};
    uint32_t ctcss_freq_x10_{0};

    float goertzel_coeff_{0.0f};
    float goertzel_s1_{0.0f};
    float goertzel_s2_{0.0f};
    float ctcss_threshold_{30.0f};

    std::array<float, kMotoToneCount> moto_coeff_{};
    std::array<float, kMotoToneCount> moto_s1_{};
    std::array<float, kMotoToneCount> moto_s2_{};
    std::array<float, kMotoToneCount> last_energies_{};
    float moto_threshold_{1000.0f};

    size_t window_sample_count_{0};
    bool was_detected_{false};
    uint32_t tone_duration_windows_{0};
    bool carrier_open_{false};
};

/* --- A/B sequencing (TwoToneRxView::on_tone_data) ------------------------- */

struct DetectedPair {
    uint32_t t1_freq_hz{0};
    uint32_t t1_duration_ms{0};
    uint32_t t2_freq_hz{0};
    uint32_t t2_duration_ms{0};
};

/* Window-based two-tone detection, a direct port of the upstream state machine.
 *
 * Collection rule: the first and last window of each phase are discarded. The
 * first may contain carrier ramp-up; the last is held pending and thrown away
 * when the phase ends. The A->B transition fires on two consecutive windows
 * that snap to different table entries, or on two consecutive windows that both
 * differ from the running phase average by at least kMotoTransitionDeltaHz and
 * snap to a different entry at the looser tolerance.
 *
 * feed() returns true when the window just consumed completed a loggable pair,
 * filling `out`. */
class TwoToneSequencer {
   public:
    void set_window_ms(uint32_t ms) { window_ms_ = ms ? ms : 1; }
    uint32_t window_ms() const { return window_ms_; }

    bool feed(const ToneWindow& window, DetectedPair& out);
    void reset();

    /* The live one-line status upstream paints into text_status. */
    const std::string& status() const { return status_; }

    enum class State : uint8_t { Idle, T1Collecting, T2Collecting };
    State state() const { return state_; }

   private:
    bool finalize(uint32_t t2_avg, uint32_t t2_dur, DetectedPair& out);

    uint32_t window_ms_{kDetectWindowMs};

    State state_{State::Idle};
    uint32_t phase_window_count_{0};
    uint32_t phase_freq_accum_{0};
    uint32_t phase_valid_windows_{0};
    uint32_t phase_last_freq_{0};
    bool phase_last_is_first_{true};

    uint32_t t1_avg_freq_{0};
    uint32_t t1_window_count_{0};
    uint8_t t1_transition_candidate_windows_{0};
    uint8_t t2_zero_window_count_{0};

    std::string status_{};
};

}  // namespace two_tone

/* --- View ----------------------------------------------------------------- */

struct TwoToneLogEntry {
    using Key = uint32_t;
    static constexpr Key invalid_key = 0;

    TwoToneLogEntry() = default;
    explicit TwoToneLogEntry(Key k) : serial{k} {}

    Key serial{0};
    std::string line{};

    Key key() const { return serial; }
};

using TwoToneLogEntries = ui::RecentEntries<TwoToneLogEntry>;

class TwoToneRxView : public ui::View {
   public:
    TwoToneRxView();
    ~TwoToneRxView() override;

    TwoToneRxView(const TwoToneRxView&) = delete;
    TwoToneRxView& operator=(const TwoToneRxView&) = delete;

    std::string title() const override { return "2-Tone RX"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

    /* Read-only access to the logged tone pairs, so the web portal's panel
     * provider (src/remote/provider_two_tone_rx.cpp) can publish the same table
     * this view draws. Same idiom as AprsTableView::entries(). */
    const TwoToneLogEntries& entries() const { return log_entries_; }

   private:
    void start_rx();
    void stop_rx();
    void rebuild_front_end();
    void log_pair(const two_tone::DetectedPair& pair);

    radio::ReceiverModel& receiver_;

    bool running_{false};
    uint32_t squelch_val_{50};
    uint32_t ctcss_idx_{0};
    uint32_t next_log_serial_{0};

    /* Front end: wideband tap -> decimating lowpass -> FM demod -> detector. */
    double configured_input_rate_{0.0};
    size_t decimation_{1};
    float audio_rate_{24000.0f};
    dsp::FirDecimateC channel_filter_{};
    dsp::FmDemod fm_{};
    two_tone::FmNoiseSquelch carrier_squelch_{};

    std::vector<dsp::cfloat> samples_{};
    std::vector<dsp::cfloat> channel_{};
    std::vector<float> audio_{};
    std::vector<two_tone::ToneWindow> windows_{};

    two_tone::ToneDetector detector_{};
    two_tone::TwoToneSequencer sequencer_{};

    TwoToneLogEntries log_entries_{};

    ui::Labels labels_{
        {{0, 2}, "Freq", ui::Color::light_grey()},
        {{0, 22}, "CTCSS", ui::Color::light_grey()},
        {{144, 22}, "Sq", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 2}};
    ui::FrequencyStepView step_view_{{132, 2}, field_frequency_};

    ui::OptionsField options_ctcss_{{48, 22}, 8, {}};
    ui::NumberField field_squelch_{{168, 22}, 2, {0, 99}, 1, ' '};

    ui::Button button_startstop_{{192, 20, 44, 20}, "Start"};
    ui::Button button_clear_{{192, 44, 44, 20}, "Clr"};

    ui::Text text_status_{{0, 46, 184, 16}, ""};
    ui::Text text_tap_{{0, 64, 240, 16}, ""};

    ui::RecentEntriesColumns log_columns_{{{"A tone / B tone", 0}}};
    ui::RecentEntriesTable<TwoToneLogEntries> log_view_{log_entries_, log_columns_};

    ui::Labels notes_{
        {{0, 250}, "Tap: wideband spectrum block", ui::Color::grey()},
        {{0, 266}, "(no channel tap; see .cpp).", ui::Color::grey()},
        {{0, 286}, "Untested on air - no radio.", ui::Color::yellow()},
    };
};

}  // namespace app

#endif /*__MB200_UI_TWO_TONE_RX_H__*/
