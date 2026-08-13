/*
 * mayhem-b200 — the apps that were NOT migrated still get their samples.
 *
 * 29 apps take their input from ReceiverModel::take_spectrum_samples(), and the
 * gapless tap was added to the same DSP thread that feeds it. The regression
 * that would matter and would be quiet is not a crash: it is a decoder that
 * still paints, still says "running", and never sees a sample again.
 *
 * take_spectrum_samples() itself is byte-identical to the pre-change version,
 * and tests/test_tap_coexistence.cpp shows the two taps do not interfere at the
 * ReceiverModel level. What neither of those covers is the app end: that a real
 * app view, built by the real registry factory, pushed on a real navigation
 * stack, still pulls a snapshot out of the receiver on its frame handler. That
 * is what this file checks, for three of the 29 — AIS Boats, POCSAG and Looking
 * Glass, one wideband decoder, one narrowband decoder and one display app.
 *
 * How "the app took a sample" is established, without touching the app:
 *
 *   take_spectrum_samples() clears the ready flag, so a snapshot can be taken
 *   exactly once. The DSP thread only re-arms it when it has read new samples
 *   from the radio. So with the radio stopped and its ring drained, the flag is
 *   stable, and:
 *
 *     probe -> true            a snapshot is waiting, and the probe works
 *     (stream a little more, let the DSP thread re-arm the flag)
 *     app->on_frame_sync()     the app runs its normal frame
 *     probe -> false           the app consumed it
 *
 *   The first probe is the control: without it, a false at the end could just
 *   mean no snapshot was ever ready. Both halves have to hold for the app to
 *   have taken it, and neither is timing-dependent.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_context.hpp"
#include "app_registry.hpp"
#include "audio_out.hpp"
#include "counter_radio.hpp"
#include "receiver_model.hpp"
#include "remote/app_bridge.hpp"
#include "ui.hpp"
#include "ui_navigation.hpp"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using mb200test::CounterRadio;

constexpr double kRate = 2.0e6;

/* Builds the globals an app view expects, with a radio that actually streams.
 * Modelled on ProviderHarness in tests/test_provider_ais_ble.cpp, including its
 * teardown: AppBridge is a process-global singleton and a test that leaves it
 * believing an app is open hands that state to every later test in the binary. */
struct StreamingAppHarness {
    CounterRadio radio{};
    audio::AudioOut audio{};
    radio::ReceiverModel receiver{radio, audio};
    ui::NavigationView nav{{0, 0, 240, 304}};

    radio::RadioDevice* saved_radio{app::globals().radio};
    radio::ReceiverModel* saved_receiver{app::globals().receiver};
    ui::NavigationView* saved_nav{app::globals().nav};

    StreamingAppHarness() {
        receiver.set_sampling_rate(kRate);

        app::globals().radio = &radio;
        app::globals().receiver = &receiver;
        app::globals().nav = &nav;

        nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304})); /* root */
        nav.service();
    }

    ~StreamingAppHarness() {
        remote::AppBridge::instance().request_home();
        remote::AppBridge::instance().drain_launch_queue();
        nav.service();

        receiver.stop();
        radio.stop_producer();

        app::globals().radio = saved_radio;
        app::globals().receiver = saved_receiver;
        app::globals().nav = saved_nav;
    }

    /* Puts an app on the stack the way the portal does, which is also what
     * calls the view's on_show() and therefore starts the receiver. */
    void launch(const std::string& app_id) {
        remote::AppBridge::instance().request_launch(app_id);
        remote::AppBridge::instance().drain_launch_queue();
        nav.service();
    }

    /* Streams `count` samples and waits for the DSP thread to have consumed
     * them all, so the spectrum tap's ready flag settles and stops moving. */
    void stream_and_settle(uint64_t count) {
        radio.produce(count);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while ((!radio.producer_done() || !radio.rx_ring().empty()) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        radio.stop_producer();
        CHECK(radio.producer_done());
        CHECK(radio.rx_ring().empty());
        /* The DSP thread may be mid-block when the ring empties; give it time
         * to finish that block and arm the flag before anything is asserted. */
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    bool snapshot_waiting() {
        std::vector<dsp::cfloat> probe;
        return receiver.take_spectrum_samples(probe, 4096);
    }
};

/* The whole check for one app, so the three tests differ only in the app id and
 * cannot accidentally diverge. */
void check_app_still_takes_snapshots(const char* app_id) {
    StreamingAppHarness h;

    /* The id has to be the one the app registers, or nothing is pushed and
     * every assertion below would be about the root view. */
    const auto* entry = app::AppRegistry::instance().by_id(app_id);
    CHECK(entry != nullptr);

    h.launch(app_id);
    CHECK_EQ(h.nav.depth(), size_t{2});

    ui::View* view = h.nav.top();
    CHECK(view != nullptr);
    if (view == nullptr) return;

    /* on_show() starts the receiver; without that the DSP thread never runs and
     * no app in this group can see anything. */
    CHECK(h.receiver.running());

    /* The gapless tap belongs to ADS-B. None of these 29 asked for it, so it
     * must still be closed — an app paying 8 bytes a sample for a ring it never
     * reads would be a regression in itself. */
    CHECK(!h.receiver.raw_tap_enabled());

    h.stream_and_settle(400'000);

    /* Control: a snapshot really is waiting, and the probe can see it. */
    CHECK(h.snapshot_waiting());

    /* Re-arm, then let the app have its frame. */
    h.stream_and_settle(200'000);
    view->on_frame_sync();

    /* The app took it. */
    CHECK(!h.snapshot_waiting());

    /* And it keeps taking them, frame after frame, rather than once — a decoder
     * that consumed one snapshot and then went quiet would pass everything
     * above. Each round re-arms the flag, checks the control, re-arms again,
     * then gives the app its frame. */
    int armed = 0;
    int taken = 0;
    for (int i = 0; i < 5; i++) {
        h.stream_and_settle(100'000);
        if (h.snapshot_waiting()) armed++;
        h.stream_and_settle(100'000);
        view->on_frame_sync();
        if (!h.snapshot_waiting()) taken++;
    }
    CHECK_EQ(armed, 5);
    CHECK_EQ(taken, 5);

    /* Still streaming at the end: the app did not stop the receiver, and the
     * radio saw no overrun that would mean the DSP thread had stalled. */
    CHECK(h.receiver.running());
    CHECK_EQ(h.radio.stats().rx_dropped.load(), uint32_t{0});
}

}  // namespace

TEST(ais_still_takes_spectrum_snapshots_after_the_gapless_tap_landed) {
    check_app_still_takes_snapshots("ais");
}

TEST(pocsag_still_takes_spectrum_snapshots_after_the_gapless_tap_landed) {
    check_app_still_takes_snapshots("pocsag");
}

TEST(looking_glass_still_takes_spectrum_snapshots_after_the_gapless_tap_landed) {
    check_app_still_takes_snapshots("lookingglass");
}
