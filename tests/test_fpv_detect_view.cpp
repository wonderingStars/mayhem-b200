/*
 * mayhem-b200 — FPV Detect's FRAME HANDLER, against a radio that answers.
 *
 * tests/test_fpv_detect.cpp drives app::fpv::Scanner directly: one on_timer()
 * and one on_statistics() per step, which is the detector's logic and nothing
 * else. That leaves the piece between the scanner and the radio untested, and
 * that piece was broken in a way the Scanner tests could not see:
 *
 *   FpvDetectView::on_frame_sync() let the scanner's dwell tick while the
 *   retune settle window was still open. The dwell steps the scan every
 *   kScanDwellFrames + 1 = 2 frames and a step retunes, which re-arms a
 *   3-frame window -- so the window ran 2, 1, 2, 1, ... and never reached
 *   zero. on_statistics() was never called once. The detector took no
 *   measurement at all, never left Scanning, and issued a hardware retune
 *   every 2 frames (~31 a second at the 60 Hz frame loop) for as long as the
 *   app was open. Every one of those retunes interrupts the RX stream, which
 *   is precisely why the settle window exists.
 *
 *   The level it would have read was frozen too: channel_level_db() is
 *   computed after the channel filter and the DSP thread returns before
 *   computing it in SpectrumAnalysis mode, which is the mode on_show()
 *   selects. rf_level_db() -- the RMS the radio takes over each received
 *   block -- is the live equivalent, and since this app captures exactly the
 *   750 kHz channel it is measuring, it is also the right quantity.
 *
 * The radio here answers with a level that depends on the frequency it is
 * tuned to and that takes two frames to catch up after a retune, so "the app
 * measured the channel it tuned to, after the window closed" is checkable
 * rather than assumed.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_context.hpp"
#include "audio_out.hpp"
#include "counter_radio.hpp"
#include "receiver_model.hpp"
#include "ui_fpv_detect.hpp"
#include "ui_navigation.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace {

using app::fpv::kChannelsPerBand;
using app::fpv::kFrequencies;
using app::fpv::kNumBands;

/* The level the fake radio reports for a given tuned frequency. Monotone in
 * frequency and distinct for every pair of channels the sweep visits
 * consecutively, so a stale reading cannot pass as a fresh one. Every value
 * lands in [-96, -66]: below the detector's -38 dB threshold, so the sweep
 * stays in Scanning and its cadence is deterministic, and well above the
 * view's -120 clamp, so "measured" is distinguishable from ChannelMemory's
 * -120 default. */
int16_t level_for_hz(double hz) {
    return static_cast<int16_t>(-100 + static_cast<int>((hz - 5.6e9) / 10.0e6));
}

/* CounterRadio with the three things this test needs: a record of every
 * hardware retune and which frame it happened on, a level that follows the
 * tuning, and counters for the operations that would restart the stream. */
class ProbeRadio : public mb200test::CounterRadio {
   public:
    double set_rx_frequency(double hz) override {
        tune_frames_.push_back(frame_);
        /* A retune does not produce a settled reading immediately: the radio
         * goes on reporting the old channel's level for two more frames. Two,
         * not three, so correct code clears the window with a frame to spare
         * and code that reads too early gets the previous channel back. */
        previous_level_ = current_level_;
        current_level_ = static_cast<float>(level_for_hz(hz));
        stale_frames_ = 2;
        return mb200test::CounterRadio::set_rx_frequency(hz);
    }

    double set_rx_rate(double hz) override {
        rate_changes_++;
        return mb200test::CounterRadio::set_rx_rate(hz);
    }

    bool start_rx() override {
        starts_++;
        return mb200test::CounterRadio::start_rx();
    }

    void stop_rx() override {
        stops_++;
        mb200test::CounterRadio::stop_rx();
    }

    float rx_level_db() const override {
        return (stale_frames_ > 0) ? previous_level_ : current_level_;
    }

    /* Called by the harness after each frame the view has run. */
    void end_frame() {
        frame_++;
        if (stale_frames_ > 0) stale_frames_--;
    }

    void forget_startup() {
        tune_frames_.clear();
        rate_changes_ = 0;
        starts_ = 0;
        stops_ = 0;
        frame_ = 0;
    }

    const std::vector<int>& tune_frames() const { return tune_frames_; }
    int rate_changes() const { return rate_changes_; }
    int starts() const { return starts_; }
    int stops() const { return stops_; }

   private:
    std::vector<int> tune_frames_{};
    int frame_{0};
    int stale_frames_{0};
    float current_level_{-100.0f};
    float previous_level_{-100.0f};
    int rate_changes_{0};
    int starts_{0};
    int stops_{0};
};

/* The app on a navigation stack over that radio, exactly as the portal's
 * launch path builds it: construct, push, service (which calls on_show and so
 * starts the receiver). Modelled on ViewHarness in tests/test_adsb_tap.cpp,
 * including its teardown of the globals. */
struct FpvHarness {
    ProbeRadio radio{};
    audio::AudioOut audio{};
    radio::ReceiverModel receiver{radio, audio};
    ui::NavigationView nav{{0, 0, 240, 304}};

    radio::RadioDevice* saved_radio{app::globals().radio};
    radio::ReceiverModel* saved_receiver{app::globals().receiver};
    audio::AudioOut* saved_audio_out{app::globals().audio_out};
    ui::NavigationView* saved_nav{app::globals().nav};

    app::FpvDetectView* view{nullptr};

    FpvHarness() {
        app::globals().radio = &radio;
        app::globals().receiver = &receiver;
        /* Not wired: the view's beep() returns immediately on a null audio
         * device, which keeps the frame loop free of audio timing. */
        app::globals().audio_out = nullptr;
        app::globals().nav = &nav;

        auto owned = std::make_unique<app::FpvDetectView>();
        view = owned.get();
        nav.push(std::move(owned));
        nav.service();
    }

    ~FpvHarness() {
        receiver.stop();
        radio.stop_producer();

        app::globals().radio = saved_radio;
        app::globals().receiver = saved_receiver;
        app::globals().audio_out = saved_audio_out;
        app::globals().nav = saved_nav;
    }

    FpvHarness(const FpvHarness&) = delete;
    FpvHarness& operator=(const FpvHarness&) = delete;

    void run_frames(int n) {
        for (int i = 0; i < n; i++) {
            view->on_frame_sync();
            radio.end_frame();
        }
    }

    /* Channels carrying a reading, i.e. anything other than ChannelMemory's
     * untouched -120 default. */
    int measured_channels() const {
        int n = 0;
        for (uint8_t b = 0; b < kNumBands; b++)
            for (uint8_t c = 0; c < kChannelsPerBand; c++)
                if (view->scanner().memory(b, c).last_db != -120) n++;
        return n;
    }
};

/* 120 frames is two seconds of the 60 Hz frame loop. The fixed handler runs a
 * five-frame cycle per channel -- one step frame, the three settle frames that
 * step asked for, and the frame that reads the settled level -- with the first
 * measurement at frame 3, because the view's constructor has already tuned to
 * A1 and armed the window. So measurements land on frames 3, 8, ... 118 and
 * retunes on 4, 9, ... 119: 24 of each, which is bands A, B and E end to end. */
constexpr int kFrames = 120;
constexpr int kCycleFrames = 5;
constexpr int kExpectedChannels = kFrames / kCycleFrames;

}  // namespace

TEST(fpv_detect_view_measures_a_channel_for_every_channel_it_tunes) {
    /* The defect this pins: a sweep that retunes and never reads. Before the
     * fix this app made 60 hardware retunes in these 120 frames and took zero
     * measurements. */
    FpvHarness h;
    CHECK(h.view != nullptr);
    if (h.view == nullptr) return;

    h.radio.forget_startup();
    h.run_frames(kFrames);

    const int tunes = static_cast<int>(h.radio.tune_frames().size());
    const int measured = h.measured_channels();

    CHECK(measured > 0);
    CHECK_EQ(measured, kExpectedChannels);

    /* One reading per retune, give or take the cycle in flight at the end. */
    CHECK(tunes > 0);
    CHECK(tunes - measured <= 1);
    CHECK(measured - tunes <= 1);

    /* And the sweep is still a sweep: it did not sit on one channel. */
    CHECK(measured >= 8);
}

TEST(fpv_detect_view_does_not_retune_faster_than_its_settle_window) {
    /* The cost side of the same defect. Every retune is a synchronous UHD
     * set_rx_freq on the UI thread and interrupts the RX stream; issuing them
     * faster than the reading can settle buys nothing at all. Before the fix
     * the gap between retunes was 2 frames; the settle window is 3. */
    FpvHarness h;
    if (h.view == nullptr) return;

    h.radio.forget_startup();
    h.run_frames(kFrames);

    const auto& frames = h.radio.tune_frames();
    CHECK(frames.size() >= 2);
    if (frames.size() < 2) return;

    int smallest_gap = kFrames;
    for (size_t i = 1; i < frames.size(); i++) {
        const int gap = frames[i] - frames[i - 1];
        if (gap < smallest_gap) smallest_gap = gap;
    }

    /* A retune, then the three frames its window asks for, then the frame that
     * reads it: five, and never fewer than four. */
    CHECK(smallest_gap >= 4);
    CHECK_EQ(smallest_gap, kCycleFrames);

    /* Two seconds of frames, and the radio was asked to tune at most a quarter
     * as often as the frame loop runs. */
    CHECK(static_cast<int>(frames.size()) <= kFrames / 4);
}

TEST(fpv_detect_view_reads_a_level_the_radio_actually_updates) {
    /* Every channel's recorded reading must be that channel's own level, taken
     * after the radio settled -- not the previous channel's, and not the
     * frozen channel_level_db() the app used to read, which in
     * SpectrumAnalysis mode never leaves its -140 initialiser and arrives here
     * clamped to -120. */
    FpvHarness h;
    if (h.view == nullptr) return;

    h.radio.forget_startup();
    h.run_frames(kFrames);

    int checked = 0;
    int wrong = 0;
    for (uint8_t b = 0; b < kNumBands; b++) {
        for (uint8_t c = 0; c < kChannelsPerBand; c++) {
            const int16_t got = h.view->scanner().memory(b, c).last_db;
            if (got == -120) continue; /* not visited in this window */
            checked++;
            if (got != level_for_hz(static_cast<double>(kFrequencies[b][c]))) wrong++;
        }
    }

    CHECK_EQ(checked, kExpectedChannels);
    CHECK_EQ(wrong, 0);

    /* The reading is live, so the meter follows it: the last channel measured
     * is the one the detector is holding. */
    CHECK(h.view->scanner().memory(0, 0).last_db ==
          level_for_hz(static_cast<double>(kFrequencies[0][0])));
}

TEST(fpv_detect_view_sweeps_without_restarting_the_stream) {
    /* A retune is cheap; a rate change is not -- on a B200 it can move the
     * master clock, which re-initialises the radio and forces the RX streamer
     * to be rebuilt (see UsrpRadio::set_rx_rate). The sweep must cost retunes
     * only: one stream start at on_show, and nothing per channel. */
    FpvHarness h;
    if (h.view == nullptr) return;

    CHECK(h.receiver.running());
    CHECK_NEAR(h.receiver.sampling_rate(), 750'000.0, 1.0);

    h.radio.forget_startup();
    h.run_frames(kFrames);

    CHECK_EQ(h.radio.rate_changes(), 0);
    CHECK_EQ(h.radio.starts(), 0);
    CHECK_EQ(h.radio.stops(), 0);
    CHECK(h.receiver.running());
    CHECK(!h.radio.tune_frames().empty());
}
