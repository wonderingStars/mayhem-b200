/*
 * mayhem-b200 — Time Sink logic tests.
 *
 * Upstream's Time Sink is an oscilloscope, not a time-signal (DCF77/WWVB)
 * decoder — see the header of src/apps/ui_time_sink.hpp. What is testable
 * without hardware is therefore the whole of its signal path: the baseband
 * trace builder (proc_time_sink.cpp execute_time_domain), the stabilising
 * trigger search (TimeSinkView::find_stable_trigger_index), the trace->screen
 * mapping (on_channel_spectrum) and the sample->row mapping
 * (TimeSinkWaveformWidget::sample_to_y). All four are exercised against values
 * derived by hand from the upstream expressions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_time_sink.hpp"

#include <vector>

using namespace app;

namespace {

/* A trace that is flat at `low` and steps up to `high` at index `edge`. */
TimeSinkTrace step_trace(size_t edge, uint8_t low, uint8_t high) {
    TimeSinkTrace t{};
    for (size_t i = 0; i < t.db.size(); i++) t.db[i] = (i < edge) ? low : high;
    return t;
}

}  // namespace

/* --- trace builder (proc_time_sink execute_time_domain) ------------------ */

TEST(time_sink_trace_strides_and_biases) {
    /* 1024 input samples -> stride 4 (1024/256). Real part walks a sawtooth
     * -128..127 in units of 1/127 full scale, so db[] should come back as the
     * +128-biased integer at every 4th input. */
    std::vector<dsp::cfloat> in(1024);
    for (size_t i = 0; i < in.size(); i++) {
        const int v = static_cast<int>(i % 256) - 128;
        in[i] = dsp::cfloat{static_cast<float>(v) / 127.0f, 0.5f};
    }

    TimeSinkTrace trace{};
    time_sink_fill_trace(in.data(), in.size(), 2'000'000, trace);

    CHECK_EQ(trace.sampling_rate, 500'000u);  /* 2 MHz / stride 4 */
    for (size_t i = 0; i < trace.db.size(); i++) {
        CHECK_EQ(static_cast<int>(trace.db[i]), static_cast<int>((i * 4) % 256));
    }
}

TEST(time_sink_trace_stride_one_when_short) {
    /* Fewer samples than trace points: stride stays 1 and the tail repeats the
     * last sample (upstream's min(i*stride, count-1)). */
    std::vector<dsp::cfloat> in(4);
    in[0] = {0.0f, 0.0f};
    in[1] = {1.0f, 0.0f};
    in[2] = {-1.0f, 0.0f};
    in[3] = {0.5f, 0.0f};

    TimeSinkTrace trace{};
    time_sink_fill_trace(in.data(), in.size(), 1'000'000, trace);

    CHECK_EQ(trace.sampling_rate, 1'000'000u);
    CHECK_EQ(static_cast<int>(trace.db[0]), 128);
    CHECK_EQ(static_cast<int>(trace.db[1]), 255);
    CHECK_EQ(static_cast<int>(trace.db[2]), 1);    /* -127 + 128 */
    CHECK_EQ(static_cast<int>(trace.db[3]), 192);  /* round(0.5*127)=64, +128 */
    /* Everything past the input repeats the last sample. */
    CHECK_EQ(static_cast<int>(trace.db[4]), 192);
    CHECK_EQ(static_cast<int>(trace.db[255]), 192);
}

TEST(time_sink_trace_clamps_and_handles_empty) {
    std::vector<dsp::cfloat> in(8, dsp::cfloat{4.0f, 0.0f});  /* way over full scale */
    TimeSinkTrace trace{};
    time_sink_fill_trace(in.data(), in.size(), 1'000'000, trace);
    CHECK_EQ(static_cast<int>(trace.db[0]), 255);

    std::vector<dsp::cfloat> under(8, dsp::cfloat{-4.0f, 0.0f});
    time_sink_fill_trace(under.data(), under.size(), 1'000'000, trace);
    CHECK_EQ(static_cast<int>(trace.db[0]), 0);

    /* No samples: mid-scale everywhere, and no read of a null pointer. */
    time_sink_fill_trace(nullptr, 0, 1'000'000, trace);
    CHECK_EQ(static_cast<int>(trace.db[0]), 128);
    CHECK_EQ(static_cast<int>(trace.db[255]), 128);
}

/* --- trigger search ------------------------------------------------------ */

TEST(time_sink_trigger_off_returns_zero_and_drops_lock) {
    TimeSinkTriggerState state{};
    state.lock_index = 42;
    state.lock_valid = true;

    const auto trace = step_trace(100, 0, 255);
    CHECK_EQ(time_sink_find_trigger(trace, TimeSinkTriggerMode::Off, 0, state), 0u);
    CHECK(!state.lock_valid);
}

TEST(time_sink_trigger_finds_rising_edge) {
    /* Level 0 -> threshold 128. Rising needs prev <= 126 and curr >= 130, so
     * the crossing is reported at the first high sample. */
    TimeSinkTriggerState state{};
    const auto trace = step_trace(100, 0, 255);
    CHECK_EQ(time_sink_find_trigger(trace, TimeSinkTriggerMode::Rising, 0, state), 100u);
    CHECK(state.lock_valid);
    CHECK_EQ(state.lock_index, 100u);

    /* A falling search on the same trace finds nothing and, with no prior
     * lock, falls back to 0. */
    TimeSinkTriggerState fresh{};
    CHECK_EQ(time_sink_find_trigger(trace, TimeSinkTriggerMode::Falling, 0, fresh), 0u);
    CHECK(!fresh.lock_valid);
}

TEST(time_sink_trigger_finds_falling_edge) {
    TimeSinkTriggerState state{};
    const auto trace = step_trace(60, 255, 0);
    CHECK_EQ(time_sink_find_trigger(trace, TimeSinkTriggerMode::Falling, 0, state), 60u);
    CHECK_EQ(state.lock_index, 60u);
}

TEST(time_sink_trigger_hysteresis_rejects_small_steps) {
    /* threshold 128, hysteresis 2: a step from 127 to 129 satisfies neither
     * prev <= 126 nor curr >= 130, so it is not a crossing. */
    TimeSinkTriggerState state{};
    const auto trace = step_trace(100, 127, 129);
    CHECK_EQ(time_sink_find_trigger(trace, TimeSinkTriggerMode::Rising, 0, state), 0u);
    CHECK(!state.lock_valid);

    /* 126 -> 130 is exactly at the limits and does count. */
    TimeSinkTriggerState state2{};
    const auto trace2 = step_trace(100, 126, 130);
    CHECK_EQ(time_sink_find_trigger(trace2, TimeSinkTriggerMode::Rising, 0, state2), 100u);
}

TEST(time_sink_trigger_level_shifts_threshold) {
    /* Level +100 -> threshold 228, so a 0->200 step is no longer a crossing. */
    TimeSinkTriggerState state{};
    const auto trace = step_trace(100, 0, 200);
    CHECK_EQ(time_sink_find_trigger(trace, TimeSinkTriggerMode::Rising, 100, state), 0u);
    CHECK(!state.lock_valid);

    /* The same step does cross at the default threshold. */
    TimeSinkTriggerState state2{};
    CHECK_EQ(time_sink_find_trigger(trace, TimeSinkTriggerMode::Rising, 0, state2), 100u);

    /* Threshold is clamped into [2, 253]: level +127 gives 255 -> clamped to
     * 253, and a 0->255 step still crosses. */
    TimeSinkTriggerState state3{};
    const auto full = step_trace(30, 0, 255);
    CHECK_EQ(time_sink_find_trigger(full, TimeSinkTriggerMode::Rising, 127, state3), 30u);
}

TEST(time_sink_trigger_prefers_crossing_nearest_previous_lock) {
    /* Two rising edges, at 20 and 200. With no lock the reference is the centre
     * (128), so 200 (distance 72) beats 20 (distance 108 linear, 256-108=148
     * circular -> 108). */
    TimeSinkTrace trace{};
    for (size_t i = 0; i < trace.db.size(); i++) trace.db[i] = 0;
    for (size_t i = 20; i < 40; i++) trace.db[i] = 255;
    for (size_t i = 200; i < 220; i++) trace.db[i] = 255;

    TimeSinkTriggerState state{};
    CHECK_EQ(time_sink_find_trigger(trace, TimeSinkTriggerMode::Rising, 0, state), 200u);

    /* Now that the lock sits at 200 it stays there on the next frame. */
    CHECK_EQ(time_sink_find_trigger(trace, TimeSinkTriggerMode::Rising, 0, state), 200u);

    /* Seed the lock near the first edge and the search follows it instead. */
    TimeSinkTriggerState near_first{};
    near_first.lock_index = 25;
    near_first.lock_valid = true;
    CHECK_EQ(time_sink_find_trigger(trace, TimeSinkTriggerMode::Rising, 0, near_first), 20u);
}

TEST(time_sink_trigger_distance_wraps_around_the_trace) {
    /* Edges at 5 and 130, previous lock at 250. Circular distance from 250 to
     * 5 is 11 (wrapping), to 130 is 120 — so the wrapped edge wins. */
    TimeSinkTrace trace{};
    for (size_t i = 0; i < trace.db.size(); i++) trace.db[i] = 0;
    for (size_t i = 5; i < 15; i++) trace.db[i] = 255;
    for (size_t i = 130; i < 140; i++) trace.db[i] = 255;

    TimeSinkTriggerState state{};
    state.lock_index = 250;
    state.lock_valid = true;
    CHECK_EQ(time_sink_find_trigger(trace, TimeSinkTriggerMode::Rising, 0, state), 5u);
}

TEST(time_sink_trigger_holds_last_lock_when_nothing_crosses) {
    TimeSinkTriggerState state{};
    state.lock_index = 77;
    state.lock_valid = true;

    TimeSinkTrace flat{};
    flat.db.fill(10);
    CHECK_EQ(time_sink_find_trigger(flat, TimeSinkTriggerMode::Rising, 0, state), 77u);
    CHECK(state.lock_valid);
}

/* --- trace -> screen mapping --------------------------------------------- */

TEST(time_sink_waveform_mapping_centres_and_wraps) {
    TimeSinkTrace trace{};
    for (size_t i = 0; i < trace.db.size(); i++) trace.db[i] = static_cast<uint8_t>(i);

    int8_t out[kTimeSinkWaveformPoints]{};
    time_sink_map_waveform(trace, 0, out, kTimeSinkWaveformPoints);

    /* x -> offset = x*256/240; db[offset] = offset; centred = offset - 128. */
    for (size_t x = 0; x < kTimeSinkWaveformPoints; x++) {
        const size_t offset = (x * 256) / kTimeSinkWaveformPoints;
        CHECK_EQ(static_cast<int>(out[x]), static_cast<int>(offset) - 128);
    }

    /* Starting at a trigger index rotates the window modulo 256. */
    time_sink_map_waveform(trace, 200, out, kTimeSinkWaveformPoints);
    for (size_t x = 0; x < kTimeSinkWaveformPoints; x++) {
        const size_t offset = (x * 256) / kTimeSinkWaveformPoints;
        const size_t src = (200 + offset) % 256;
        CHECK_EQ(static_cast<int>(out[x]), static_cast<int>(src) - 128);
    }
}

TEST(time_sink_waveform_mapping_endpoints) {
    TimeSinkTrace trace{};
    trace.db.fill(128);
    trace.db[0] = 0;
    trace.db[1] = 255;

    int8_t out[kTimeSinkWaveformPoints]{};
    time_sink_map_waveform(trace, 0, out, kTimeSinkWaveformPoints);
    CHECK_EQ(static_cast<int>(out[0]), -128);
    CHECK_EQ(static_cast<int>(out[1]), 127);
    CHECK_EQ(static_cast<int>(out[100]), 0);
}

/* --- sample -> screen row ------------------------------------------------- */

TEST(time_sink_sample_to_y_centres_zero) {
    /* A 256-high pane starting at row 48: centre is 48 + 128 = 176. */
    CHECK_EQ(time_sink_sample_to_y(48, 256, 0), 176);
    /* +127 pushes up by 127*255/256 = 126 rows. */
    CHECK_EQ(time_sink_sample_to_y(48, 256, 127), 176 - 126);
    /* -128 pushes down by 128*255/256 = 127 rows. */
    CHECK_EQ(time_sink_sample_to_y(48, 256, -128), 176 + 127);
}

TEST(time_sink_sample_to_y_clamps_to_the_pane) {
    /* A two-row pane cannot represent much: everything lands inside it. */
    for (int s = -128; s < 128; s += 17) {
        const int32_t y = time_sink_sample_to_y(10, 2, static_cast<int8_t>(s));
        CHECK(y >= 10);
        CHECK(y <= 11);
    }
    /* Degenerate height still yields the top row rather than reading off-pane. */
    CHECK_EQ(time_sink_sample_to_y(10, 1, 127), 10);
}
