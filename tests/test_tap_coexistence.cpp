/*
 * mayhem-b200 — the two RX taps, running at the same time.
 *
 * ReceiverModel now offers two taps on the same pre-channel-filter samples:
 * take_spectrum_samples(), which 29 apps use and which must not have changed at
 * all, and the gapless RawSampleTap, which ADS-B uses. tests/test_gapless_tap.cpp
 * proves the new tap correct on its own. What is NOT covered there is the
 * question a reviewer of a shared producer has to ask: with both taps live on
 * one DSP thread, can one consumer starve or corrupt the other?
 *
 * By construction they share no buffer — spectrum_buffer_ is guarded by
 * spectrum_mutex_ and keeps the NEWEST 4096 samples whatever the consumer does;
 * RawSampleTap is a lock-free ring that keeps the OLDEST unread sample and is
 * emptied by a read (receiver_model.hpp, the comment above raw_tap_). But they
 * do share the DSP thread, and the DSP thread takes spectrum_mutex_ once per
 * block before it writes the tap. That is a real coupling and it is what these
 * tests exercise, on a real ReceiverModel with its real DSP thread running,
 * against a fake radio that streams a counter sequence so every sample can be
 * identified by value.
 *
 * The three couplings, and where each is pinned:
 *
 *  - a decoder draining the gapless tap must not change what the spectrum tap
 *    hands out                                        -> both_taps_* below
 *  - a gapless consumer that has stopped reading must lose ITS samples and
 *    nothing else: no back-pressure onto the DSP thread, no effect on the
 *    spectrum snapshot                                -> an_overflowing_*
 *  - a spectrum consumer hammering the mutex must not cost the gapless tap a
 *    sample or reorder its stream                     -> a_hammering_*
 *
 * Deliberately not asserted: any timing or throughput figure. Where a test
 * needs zero loss it gets it by sizing the ring larger than the whole run, so
 * the assertion holds on a loaded machine; loss under a genuinely slow consumer
 * is test_gapless_tap.cpp's subject, not this file's.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "audio_out.hpp"
#include "counter_radio.hpp"
#include "receiver_model.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

using mb200test::CounterRadio;
using mb200test::position_of;
using mb200test::seq_sample;
using mb200test::is_seq_sample;

/* --- What the spectrum consumer saw ---------------------------------------- */

struct SpectrumWatch {
    size_t snapshots{0};       /* take_spectrum_samples() returned true */
    size_t startup_windows{0}; /* window still holds initial zeros: not yet full */
    size_t bad_size{0};
    size_t bad_content{0};     /* not a contiguous run of the counter sequence */
    size_t went_backwards{0};  /* a window older than one already handed out */
    uint64_t first_base{0};
    uint64_t last_base{0};

    /* Checks one returned window against the counter sequence and folds it in. */
    void observe(const std::vector<dsp::cfloat>& out, size_t want) {
        if (out.size() != want) {
            bad_size++;
            return;
        }
        const uint64_t base = position_of(out.front());
        if (base == 0) {
            /* Fewer than `want` samples have ever been captured, so the window
             * still contains the buffer's initial contents. Not a fault. */
            startup_windows++;
            return;
        }
        for (size_t k = 0; k < out.size(); k++) {
            if (!is_seq_sample(out[k], base + k)) {
                bad_content++;
                return;
            }
        }
        if (snapshots != 0 && base < last_base) went_backwards++;
        if (snapshots == 0) first_base = base;
        last_base = base;
        snapshots++;
    }

    void check_clean() const {
        CHECK_EQ(bad_size, size_t{0});
        CHECK_EQ(bad_content, size_t{0});
        CHECK_EQ(went_backwards, size_t{0});
        CHECK(snapshots > 0);
    }
};

/* --- What the gapless consumer saw ------------------------------------------ */

struct GaplessWatch {
    uint64_t expected{1};   /* stream position the next sample must carry */
    uint64_t received{0};
    uint64_t reported_lost{0};
    size_t holes{0};
    size_t bad_blocks{0};
    size_t reads{0};

    void observe(const radio::RawSampleTap::Block& b,
                 const std::vector<dsp::cfloat>& out) {
        reads++;
        if (b.samples == 0) return;
        if (out.size() != b.samples) {
            bad_blocks++;
            return;
        }
        if (b.lost_before != 0) {
            holes++;
            reported_lost += b.lost_before;
            expected += b.lost_before;
        }
        for (size_t k = 0; k < out.size(); k++) {
            if (!is_seq_sample(out[k], expected + k)) {
                bad_blocks++;
                /* Resynchronise so one bad block does not cascade. */
                expected = position_of(out.back()) + 1;
                received += out.size();
                return;
            }
        }
        expected += out.size();
        received += out.size();
    }
};

/* offered() == delivered() + dropped() + available(), at rest. */
void check_accounting(const radio::RawSampleTap& tap) {
    CHECK_EQ(tap.offered(),
             tap.delivered() + tap.dropped() + static_cast<uint64_t>(tap.available()));
}

/* A run that takes longer than this has stalled rather than been slow; the
 * point of the bound is to turn a back-pressure deadlock into a failure instead
 * of a hung suite. Generous, because it must not trip on a loaded machine. */
constexpr auto kRunLimit = std::chrono::seconds(30);

/* The bound the undrained-tap test measures against, and the one assertion in
 * this file that is about time rather than content — because "write() does not
 * block the DSP thread" IS a claim about time and cannot be pinned any other
 * way. It is calibrated against a measured negative control rather than
 * guessed: with RawSampleTap::write() as shipped that test takes 25 ms, and
 * with a 200 ms bounded back-pressure spin injected into write() it takes
 * 22 415 ms. Five seconds sits three orders of magnitude above the real figure
 * and a quarter of the broken one, so it fires on back-pressure without ever
 * tripping on a slow machine. */
constexpr auto kNoStallLimit = std::chrono::seconds(5);

bool past(const std::chrono::steady_clock::time_point& start,
          std::chrono::seconds limit = kRunLimit) {
    return std::chrono::steady_clock::now() - start > limit;
}

constexpr double kRate = 2.0e6;
constexpr size_t kSpectrumWindow = 4096;

}  // namespace

/* --- 1. Both taps live, both consumers keeping up --------------------------- */

TEST(both_taps_deliver_correct_data_at_the_same_time) {
    CounterRadio radio;
    audio::AudioOut audio;
    radio::ReceiverModel rx{radio, audio};

    rx.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
    rx.set_sampling_rate(kRate);

    /* One second of history at 2 Msps: more than the whole run produces, so
     * "the tap lost nothing" is an assertion about correctness under contention
     * rather than about how fast this machine's scheduler is today. */
    CHECK(rx.enable_raw_tap(1.0));
    CHECK(rx.raw_tap_enabled());

    const uint64_t total = 1'500'000;
    CHECK(rx.raw_tap().capacity() > total);

    CHECK(rx.start());
    radio.produce(total);

    SpectrumWatch spec;
    GaplessWatch gap;
    std::vector<dsp::cfloat> spec_out;
    std::vector<dsp::cfloat> gap_out;

    const auto started = std::chrono::steady_clock::now();
    while (gap.received < total && !past(started)) {
        if (rx.take_spectrum_samples(spec_out, kSpectrumWindow))
            spec.observe(spec_out, kSpectrumWindow);

        const auto b = rx.raw_tap().read(gap_out);
        gap.observe(b, gap_out);
        if (b.samples == 0) std::this_thread::yield();
    }

    radio.stop_producer();
    /* Drain whatever the DSP thread committed after the loop's last read. */
    for (int i = 0; i < 200 && gap.received < total; i++) {
        const auto b = rx.raw_tap().read(gap_out);
        gap.observe(b, gap_out);
        if (b.samples == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    rx.stop();

    /* The gapless tap: every sample, once, in order, none lost. */
    CHECK_EQ(gap.bad_blocks, size_t{0});
    CHECK_EQ(gap.holes, size_t{0});
    CHECK_EQ(gap.reported_lost, uint64_t{0});
    CHECK_EQ(gap.received, total);
    CHECK_EQ(rx.raw_tap().dropped(), uint64_t{0});
    CHECK_EQ(rx.raw_tap().overflows(), uint32_t{0});
    CHECK_EQ(rx.raw_tap().delivered(), total);
    check_accounting(rx.raw_tap());

    /* The spectrum tap, in the same run: every window a genuine contiguous
     * slice of the same stream, and never older than one already handed out. */
    spec.check_clean();
    CHECK(spec.last_base >= spec.first_base);
}

/* --- 2. A gapless consumer that stopped reading ----------------------------- */

TEST(an_overflowing_gapless_tap_costs_only_its_own_samples) {
    CounterRadio radio;
    audio::AudioOut audio;
    radio::ReceiverModel rx{radio, audio};

    rx.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
    rx.set_sampling_rate(kRate);

    /* The smallest ring the tap will build, and nobody reads it: the worst case
     * a stalled decoder can inflict on the DSP thread. */
    CHECK(rx.enable_raw_tap(0.001));
    CHECK_EQ(rx.raw_tap().capacity(), radio::RawSampleTap::kMinCapacitySamples);

    constexpr uint64_t total = 1'500'000;
    static_assert(total > radio::RawSampleTap::kMinCapacitySamples,
                  "the run has to be longer than the ring or it cannot overflow");

    CHECK(rx.start());
    radio.produce(total);

    SpectrumWatch spec;
    std::vector<dsp::cfloat> spec_out;

    const auto started = std::chrono::steady_clock::now();
    while (!radio.producer_done() && !past(started, kNoStallLimit)) {
        if (rx.take_spectrum_samples(spec_out, kSpectrumWindow))
            spec.observe(spec_out, kSpectrumWindow);
        else
            std::this_thread::yield();
    }

    /* If write() back-pressured instead of dropping, the DSP thread would be
     * parked in it, the radio's own ring would back up behind it, and the
     * producer would still be here at the deadline — which is what a stalled
     * decoder must never be able to do to the radio. */
    CHECK(radio.producer_done());
    CHECK_EQ(radio.produced(), total);

    radio.stop_producer();
    rx.stop();

    /* The tap really did overflow — otherwise this proves nothing. */
    CHECK(rx.raw_tap().overflows() > 0);
    CHECK(rx.raw_tap().dropped() > 0);
    check_accounting(rx.raw_tap());

    /* And the spectrum tap carried on as if nothing had happened: real windows,
     * in order, throughout. */
    spec.check_clean();
    CHECK(spec.snapshots > 1);
    CHECK(spec.last_base > spec.first_base);
}

/* --- 3. A spectrum consumer hammering the mutex ----------------------------- */

TEST(a_hammering_spectrum_consumer_does_not_cost_the_gapless_tap_a_sample) {
    CounterRadio radio;
    audio::AudioOut audio;
    radio::ReceiverModel rx{radio, audio};

    rx.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
    rx.set_sampling_rate(kRate);

    CHECK(rx.enable_raw_tap(1.0));

    const uint64_t total = 1'500'000;
    CHECK(rx.raw_tap().capacity() > total);

    CHECK(rx.start());
    radio.produce(total);

    /* A second thread taking spectrum_mutex_ as fast as it can, which is the
     * only lock the DSP thread takes before it writes the gapless tap. */
    std::atomic<bool> stop_spec{false};
    SpectrumWatch spec;
    std::thread spectator{[&] {
        std::vector<dsp::cfloat> out;
        while (!stop_spec.load()) {
            if (rx.take_spectrum_samples(out, kSpectrumWindow))
                spec.observe(out, kSpectrumWindow);
        }
    }};

    GaplessWatch gap;
    std::vector<dsp::cfloat> gap_out;
    const auto started = std::chrono::steady_clock::now();
    while (gap.received < total && !past(started)) {
        const auto b = rx.raw_tap().read(gap_out);
        gap.observe(b, gap_out);
        if (b.samples == 0) std::this_thread::yield();
    }

    radio.stop_producer();
    for (int i = 0; i < 200 && gap.received < total; i++) {
        const auto b = rx.raw_tap().read(gap_out);
        gap.observe(b, gap_out);
        if (b.samples == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    stop_spec.store(true);
    spectator.join();
    rx.stop();

    CHECK_EQ(gap.bad_blocks, size_t{0});
    CHECK_EQ(gap.holes, size_t{0});
    CHECK_EQ(gap.received, total);
    CHECK_EQ(rx.raw_tap().dropped(), uint64_t{0});
    check_accounting(rx.raw_tap());

    spec.check_clean();
}

/* --- 4. The control path, under a live DSP thread --------------------------- */

TEST(opening_and_closing_the_tap_while_streaming_leaves_the_spectrum_tap_alone) {
    CounterRadio radio;
    audio::AudioOut audio;
    radio::ReceiverModel rx{radio, audio};

    rx.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
    rx.set_sampling_rate(kRate);

    const uint64_t total = 1'500'000;
    CHECK(rx.start());
    radio.produce(total);

    SpectrumWatch spec;
    std::vector<dsp::cfloat> spec_out;
    std::vector<dsp::cfloat> gap_out;

    /* enable_raw_tap() and disable_raw_tap() reallocate the ring under a
     * producing DSP thread. An app does this in on_show/on_hide; here it
     * happens hundreds of times mid-stream, with the spectrum consumer polling
     * throughout. */
    int cycles = 0;
    const auto started = std::chrono::steady_clock::now();
    while (!radio.producer_done() && !past(started)) {
        CHECK(rx.enable_raw_tap(0.01));
        if (rx.take_spectrum_samples(spec_out, kSpectrumWindow))
            spec.observe(spec_out, kSpectrumWindow);
        rx.raw_tap().read(gap_out);
        rx.disable_raw_tap();
        if (rx.take_spectrum_samples(spec_out, kSpectrumWindow))
            spec.observe(spec_out, kSpectrumWindow);
        cycles++;
    }

    CHECK(radio.producer_done());
    radio.stop_producer();
    rx.stop();

    CHECK(cycles > 10);
    CHECK(!rx.raw_tap_enabled());
    spec.check_clean();
    CHECK(spec.last_base > spec.first_base);
}

/* --- 5. take_spectrum_samples()'s own contract, tap or no tap --------------- */

/* Runs a fixed stream through a receiver, stops it, and returns the model so
 * the spectrum tap can be interrogated with nothing else moving. */
void run_then_stop(radio::ReceiverModel& rx, CounterRadio& radio, uint64_t total) {
    CHECK(rx.start());
    radio.produce(total);
    const auto started = std::chrono::steady_clock::now();
    while (!radio.producer_done() && !past(started)) std::this_thread::yield();
    radio.stop_producer();
    /* Let the DSP thread drain the ring so the final window is settled. */
    const auto drained = std::chrono::steady_clock::now();
    while (!radio.rx_ring().empty() &&
           std::chrono::steady_clock::now() - drained < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    rx.stop();
}

TEST(take_spectrum_samples_behaves_identically_with_the_tap_open) {
    const uint64_t total = 200'000;

    /* Reference: the tap never opened, exactly as the other 29 apps run. */
    uint64_t closed_base = 0;
    size_t closed_size = 0;
    {
        CounterRadio radio;
        audio::AudioOut audio;
        radio::ReceiverModel rx{radio, audio};
        rx.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
        rx.set_sampling_rate(kRate);
        CHECK(!rx.raw_tap_enabled());

        run_then_stop(rx, radio, total);

        std::vector<dsp::cfloat> out;

        /* A count larger than the buffer is clamped, not refused. */
        CHECK(rx.take_spectrum_samples(out, 65536));
        closed_size = out.size();
        CHECK_EQ(closed_size, kSpectrumWindow);
        closed_base = position_of(out.front());
        CHECK(closed_base > 0);
        for (size_t k = 0; k < out.size(); k++) CHECK(is_seq_sample(out[k], closed_base + k));

        /* The window is the NEWEST samples: it ends on the last one produced. */
        CHECK_EQ(closed_base + closed_size - 1, total);

        /* Consumed once: nothing new has arrived, so the next call refuses. */
        CHECK(!rx.take_spectrum_samples(out, kSpectrumWindow));
        /* And a zero count is refused whatever the state. */
        CHECK(!rx.take_spectrum_samples(out, 0));
    }

    /* Same stream, with the gapless tap open and being drained the whole time. */
    {
        CounterRadio radio;
        audio::AudioOut audio;
        radio::ReceiverModel rx{radio, audio};
        rx.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
        rx.set_sampling_rate(kRate);
        CHECK(rx.enable_raw_tap(1.0));

        run_then_stop(rx, radio, total);

        /* Drain the tap, so the spectrum tap is being read after a decoder has
         * emptied the other one — the case that would break if the two shared
         * a buffer. */
        std::vector<dsp::cfloat> gap_out;
        uint64_t drained = 0;
        for (int i = 0; i < 200; i++) {
            const auto b = rx.raw_tap().read(gap_out);
            if (b.samples == 0) break;
            drained += b.samples;
        }
        CHECK_EQ(drained, total);
        CHECK_EQ(rx.raw_tap().available(), size_t{0});

        std::vector<dsp::cfloat> out;
        CHECK(rx.take_spectrum_samples(out, 65536));
        CHECK_EQ(out.size(), closed_size);
        const uint64_t base = position_of(out.front());
        CHECK_EQ(base, closed_base);
        for (size_t k = 0; k < out.size(); k++) CHECK(is_seq_sample(out[k], base + k));
        CHECK_EQ(base + out.size() - 1, total);

        CHECK(!rx.take_spectrum_samples(out, kSpectrumWindow));
        CHECK(!rx.take_spectrum_samples(out, 0));
    }
}
