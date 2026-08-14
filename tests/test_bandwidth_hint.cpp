/*
 * mayhem-b200 — the app-specific RX analog-bandwidth hint.
 *
 * A decoder whose signal is much narrower than its sample rate (ADS-B: ~2 MHz
 * of signal at up to 8 Msps) wants the analog filter sized for the SIGNAL, not
 * the rate — the capability default (a filter about as wide as the rate) lets
 * in several times the noise the decoder needs, and ~3 dB of it is exactly
 * what a weak distant frame cannot spare. set_rx_bandwidth_hint narrows it;
 * set_sampling_rate clears the hint so it stays per-app.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "audio_out.hpp"
#include "counter_radio.hpp"
#include "receiver_model.hpp"

using mb200test::CounterRadio;

namespace {

/* A CounterRadio that reports a real analog-filter range, so
 * set_rx_bandwidth() has something to clamp against (the bare fake leaves it
 * empty on purpose). */
struct BwRadio {
    CounterRadio radio;
    BwRadio() { radio.mutable_caps().rx_bandwidth = {200'000.0, 56'000'000.0, 1.0}; }
};

}  // namespace

TEST(bandwidth_hint_narrows_the_filter_below_the_rate_default) {
    BwRadio r;
    audio::AudioOut audio;
    radio::ReceiverModel rx{r.radio, audio};

    rx.set_sampling_rate(8'000'000.0);
    const double def = r.radio.rx_bandwidth();
    CHECK(def > 5'000'000.0); /* the rate-based default is wide (~8 MHz) */

    rx.set_rx_bandwidth_hint(4'000'000.0);
    CHECK_NEAR(r.radio.rx_bandwidth(), 4'000'000.0, 1.0);
}

TEST(setting_the_rate_clears_the_bandwidth_hint) {
    /* The hint is per-app: the next app that sets its rate must get the
     * capability default, not the previous app's narrow filter. */
    BwRadio r;
    audio::AudioOut audio;
    radio::ReceiverModel rx{r.radio, audio};

    rx.set_sampling_rate(8'000'000.0);
    rx.set_rx_bandwidth_hint(4'000'000.0);
    CHECK_NEAR(r.radio.rx_bandwidth(), 4'000'000.0, 1.0);

    rx.set_sampling_rate(2'400'000.0); /* a different app, a different rate */
    CHECK(r.radio.rx_bandwidth() < 4'000'000.0 + 1.0 &&
          r.radio.rx_bandwidth() != 4'000'000.0); /* back to the rate default, not the hint */
}

TEST(bandwidth_hint_is_clamped_to_the_devices_filter_range) {
    BwRadio r;
    audio::AudioOut audio;
    radio::ReceiverModel rx{r.radio, audio};
    rx.set_sampling_rate(8'000'000.0);

    /* Below the device's 200 kHz floor: clamped up, never sent as-is. */
    rx.set_rx_bandwidth_hint(1'000.0);
    CHECK(r.radio.rx_bandwidth() >= 200'000.0);
}
