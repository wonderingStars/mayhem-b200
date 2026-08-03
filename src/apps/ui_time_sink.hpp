/*
 * mayhem-b200 — Time Sink (time-domain scope).
 *
 * Ported from firmware/application/external/time_sink/ plus its baseband
 * processor firmware/baseband/proc_time_sink.cpp.
 *
 * WHAT UPSTREAM'S "Time Sink" ACTUALLY IS
 * ---------------------------------------
 * It is an oscilloscope, in the GNU Radio sense of a "time sink": the baseband
 * processor takes the raw captured buffer, keeps the I (real) component of
 * every `stride`-th sample, biases it by +128 into 0..255 and ships 256 of
 * those to the UI as a ChannelSpectrum message; the UI finds a rising/falling
 * threshold crossing to stabilise the trace, resamples the 256 points to the
 * 240-pixel screen and draws it with a persistence buffer.
 *
 * It is NOT a DCF77/WWVB time-signal decoder — there is no second/minute bit
 * framing and no date/time field decode anywhere in the upstream app, its
 * processor, or anywhere else in the upstream tree (`grep -ril dcf77|wwvb|msf60`
 * over firmware/ returns nothing). Per doc/PORTING.md this port implements what
 * upstream has and does not invent a protocol upstream never carried.
 *
 * HOST DIFFERENCES, all marked at the point of use:
 *   - There is no M4 and no ChannelSpectrumFIFO. The view pulls raw complex
 *     baseband from ReceiverModel::take_spectrum_samples() in on_frame_sync()
 *     and builds the same 256-point trace itself. That tap is exactly the right
 *     one here: upstream also samples the *pre-channel-filter* buffer.
 *   - Host samples are normalised floats, so the int8 real() of upstream
 *     becomes lrintf(real * 127) before the +128 bias. Same 0..255 codomain.
 *   - Upstream's `trigger` field is a baseband frame-skip counter
 *     (`if (phase < trigger) { phase++; return; }`). Here it skips that many
 *     UI frames between trace updates, which is the same "slow the trace down"
 *     control.
 *
 * Copyright (C) 2026 zxkmm (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_TIME_SINK_H__
#define __MB200_UI_TIME_SINK_H__

#include "../dsp/protocol.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* Upstream ChannelSpectrum::db is 256 bytes; the trace inherits that length. */
constexpr size_t kTimeSinkTracePoints = 256;
/* Upstream time_sink_waveform_points. */
constexpr size_t kTimeSinkWaveformPoints = 240;

/* The message upstream's baseband pushes to the UI, minus the fields the time
 * sink zeroes out (channel filter edges). */
struct TimeSinkTrace {
    std::array<uint8_t, kTimeSinkTracePoints> db{};
    uint32_t sampling_rate{0};
};

/* --- Pure logic, all tested in tests/test_time_sink.cpp ------------------- */

/* Port of TimeSinkProcessor::execute_time_domain().
 *
 * Upstream:
 *     stride = max(1, buffer.count / db.size())
 *     sampling_rate = buffer.sampling_rate / stride
 *     db[i] = clamp(buffer.p[min(i*stride, count-1)].real() + 128, 0, 255)
 *
 * with buffer.p[] being complex8_t, i.e. real() already in -128..127. The host
 * carries normalised floats, so the same code point scales by 127 first. */
inline void time_sink_fill_trace(const dsp::cfloat* in,
                                 size_t count,
                                 uint32_t sample_rate_hz,
                                 TimeSinkTrace& out) {
    if (in == nullptr || count == 0) {
        out.db.fill(128);
        out.sampling_rate = sample_rate_hz;
        return;
    }

    const size_t stride = std::max<size_t>(1, count / out.db.size());
    out.sampling_rate = sample_rate_hz / static_cast<uint32_t>(stride);

    for (size_t i = 0; i < out.db.size(); i++) {
        const size_t sample_index = std::min(i * stride, count - 1);
        const long scaled = std::lrintf(in[sample_index].real() * 127.0f);
        const int32_t normalized =
            std::clamp<int32_t>(static_cast<int32_t>(scaled) + 128, 0, 255);
        out.db[i] = static_cast<uint8_t>(normalized);
    }
}

enum class TimeSinkTriggerMode : uint8_t {
    Off = 0,
    Rising = 1,
    Falling = 2,
};

/* The trigger's "sticky" state across frames — upstream's trigger_lock_index /
 * trigger_lock_valid members, lifted out so the search is a pure function. */
struct TimeSinkTriggerState {
    size_t lock_index{0};
    bool lock_valid{false};
};

/* Port of TimeSinkView::find_stable_trigger_index(), including its choice of
 * the crossing *closest to the previous lock* (circularly) rather than the
 * first one, which is what stops the trace sliding sideways frame to frame. */
inline size_t time_sink_find_trigger(const TimeSinkTrace& trace,
                                     TimeSinkTriggerMode mode,
                                     int32_t trigger_level,
                                     TimeSinkTriggerState& state) {
    const size_t count = trace.db.size();
    if (count < 2) return 0;

    if (mode == TimeSinkTriggerMode::Off) {
        state.lock_valid = false;
        return 0;
    }

    constexpr int32_t hysteresis = 2;
    const int32_t threshold =
        std::clamp<int32_t>(128 + trigger_level, hysteresis, 255 - hysteresis);
    const size_t center = count / 2;
    const size_t reference_index = state.lock_valid ? state.lock_index : center;

    bool found = false;
    size_t best_index = 0;
    size_t best_distance = count;

    const auto circular_distance = [count](size_t a, size_t b) -> size_t {
        const size_t linear = (a > b) ? (a - b) : (b - a);
        return std::min(linear, count - linear);
    };

    for (size_t i = 1; i < count; ++i) {
        const int32_t prev = trace.db[i - 1];
        const int32_t curr = trace.db[i];

        bool crossing = false;
        if (mode == TimeSinkTriggerMode::Rising) {
            crossing = (prev <= (threshold - hysteresis)) && (curr >= (threshold + hysteresis));
        } else {
            crossing = (prev >= (threshold + hysteresis)) && (curr <= (threshold - hysteresis));
        }

        if (!crossing) continue;

        const size_t distance = circular_distance(i, reference_index);
        if (!found || (distance < best_distance)) {
            found = true;
            best_index = i;
            best_distance = distance;
        }
    }

    if (found) {
        state.lock_index = best_index;
        state.lock_valid = true;
        return best_index;
    }

    return state.lock_valid ? state.lock_index : 0;
}

/* Port of TimeSinkView::on_channel_spectrum(): resample the 256-point trace
 * onto `out_points` screen columns starting at the trigger, wrapping, and
 * re-centre 0..255 to -128..127. */
inline void time_sink_map_waveform(const TimeSinkTrace& trace,
                                   size_t trigger_index,
                                   int8_t* out,
                                   size_t out_points) {
    const size_t source_count = trace.db.size();
    if (out == nullptr || out_points == 0 || source_count == 0) return;

    const size_t window_size = source_count;
    for (size_t x = 0; x < out_points; x++) {
        const size_t offset = (x * window_size) / out_points;
        const size_t src_index = (trigger_index + offset) % source_count;
        const int32_t centered = static_cast<int32_t>(trace.db[src_index]) - 128;
        out[x] = static_cast<int8_t>(std::clamp<int32_t>(centered, -128, 127));
    }
}

/* Port of TimeSinkWaveformWidget::sample_to_y(), taking the rectangle's top and
 * height instead of a Rect so it is callable without any UI object. */
inline int32_t time_sink_sample_to_y(int32_t rect_top, int32_t rect_height, int8_t sample) {
    const int32_t y_center = rect_top + (rect_height / 2);
    const int32_t y_span = std::max<int32_t>(1, rect_height - 1);
    const int32_t y = y_center - (static_cast<int32_t>(sample) * y_span) / 256;
    return std::clamp<int32_t>(y, rect_top, rect_top + rect_height - 1);
}

/* --- Widgets and view ---------------------------------------------------- */

/* Direct port of upstream's TimeSinkWaveformWidget: one lit pixel per column
 * with an N-frame persistence buffer, erasing an expired pixel only when no
 * other retained frame still lights it. */
class TimeSinkWaveformWidget : public ui::Widget {
   public:
    TimeSinkWaveformWidget(ui::Rect parent_rect, const int8_t* data, size_t length, ui::Color color);

    TimeSinkWaveformWidget(const TimeSinkWaveformWidget&) = delete;
    TimeSinkWaveformWidget& operator=(const TimeSinkWaveformWidget&) = delete;

    void set_parent_rect(const ui::Rect new_parent_rect) override;
    void on_show() override;
    void paint(ui::Painter& painter) override;
    void set_persistence_frames(uint8_t frames);

   private:
    /* Upstream caps this at 16 because the PortaPack external app has no RAM
     * for more. The host does, but the number is a display characteristic, so
     * it stays as upstream chose it. */
    static constexpr size_t max_persistence_frames = 16;
    static constexpr size_t max_columns = kTimeSinkWaveformPoints;

    void reset_cache();

    const int8_t* data_;
    size_t length_;
    ui::Color color_;
    std::array<ui::Coord, max_columns> current_y_{};
    std::array<std::array<int8_t, max_columns>, max_persistence_frames> history_samples_{};
    size_t history_count_{0};
    size_t history_head_{0};
    uint8_t persistence_frames_{1};
    bool needs_clear_{true};
};

class TimeSinkView : public ui::View {
   public:
    TimeSinkView();
    ~TimeSinkView() override;

    TimeSinkView(const TimeSinkView&) = delete;
    TimeSinkView& operator=(const TimeSinkView&) = delete;

    std::string title() const override { return "Time Sink"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;
    void set_parent_rect(const ui::Rect new_parent_rect) override;

   private:
    static constexpr ui::Dim header_height = 3 * 16;

    void apply_sample_rate();
    void update_trace();

    radio::ReceiverModel& receiver_;

    uint32_t sampling_rate_{2'000'000};
    uint8_t frame_skip_{0};
    uint8_t frame_phase_{0};
    uint8_t persistence_frames_{1};
    TimeSinkTriggerMode trigger_mode_{TimeSinkTriggerMode::Rising};
    int32_t trigger_level_{0};
    TimeSinkTriggerState trigger_state_{};

    TimeSinkTrace trace_{};
    std::vector<dsp::cfloat> samples_{};
    int8_t waveform_buffer_[kTimeSinkWaveformPoints]{0};

    ui::Labels labels_{
        {{0, 16}, "SR:", ui::Color::light_grey()},
        {{88, 16}, "SKIP:", ui::Color::light_grey()},
        {{0, 32}, "PST:", ui::Color::light_grey()},
        {{72, 32}, "TRM:", ui::Color::light_grey()},
        {{152, 32}, "LVL:", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{0, 0}};
    ui::FrequencyStepView field_frequency_step_{{80, 0}, field_frequency_};
    ui::NumberField field_gain_{{184, 0}, 3, {0, 76}, 1, ' '};

    ui::OptionsField options_sample_rate_{
        {24, 16},
        6,
        {{"1.0M  ", 1'000'000},
         {"2.0M  ", 2'000'000},
         {"5.0M  ", 5'000'000},
         {"10.0M ", 10'000'000},
         {"20.0M ", 20'000'000}}};

    ui::NumberField field_frame_skip_{{136, 16}, 3, {0, 128}, 1, ' '};

    ui::OptionsField options_persistence_{
        {32, 32},
        3,
        {{"1  ", 1}, {"2  ", 2}, {"4  ", 4}, {"8  ", 8}, {"16 ", 16}}};

    ui::OptionsField options_trigger_mode_{
        {104, 32},
        4,
        {{"Off ", static_cast<int32_t>(TimeSinkTriggerMode::Off)},
         {"Rise", static_cast<int32_t>(TimeSinkTriggerMode::Rising)},
         {"Fall", static_cast<int32_t>(TimeSinkTriggerMode::Falling)}}};

    ui::NumberField field_trigger_level_{{184, 32}, 4, {-127, 127}, 1, ' '};

    TimeSinkWaveformWidget waveform_{
        {0, header_height, 240, 304 - header_height},
        waveform_buffer_,
        kTimeSinkWaveformPoints,
        ui::Color::light_grey()};
};

}  // namespace app

#endif /*__MB200_UI_TIME_SINK_H__*/
