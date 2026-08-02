/*
 * mayhem-b200 — radio layer tests.
 *
 * These run without hardware. They cover the parts that must behave sensibly
 * when no USRP is attached, because that is the state the app boots into on a
 * machine where the device is unplugged, and getting it wrong means a crash
 * rather than a message.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "audio_out.hpp"
#include "receiver_model.hpp"
#include "usrp_radio.hpp"

TEST(range_clamps_within_bounds) {
    radio::Range r{10.0, 20.0, 1.0};
    CHECK_NEAR(r.clamp(5.0), 10.0, 1e-9);
    CHECK_NEAR(r.clamp(25.0), 20.0, 1e-9);
    CHECK_NEAR(r.clamp(15.0), 15.0, 1e-9);
    CHECK(r.contains(15.0));
    CHECK(!r.contains(25.0));
}

TEST(degenerate_range_passes_value_through) {
    /* An unpopulated range must not clamp everything to zero — that would
     * silently pin the tuner to DC when a property-tree read fails. */
    radio::Range empty{};
    CHECK_NEAR(empty.clamp(1234.0), 1234.0, 1e-9);
}

TEST(default_caps_match_published_b200_spec) {
    const auto caps = radio::default_b200_caps();

    /* Ettus UHD manual, "USRP B2x0 Series": 70 MHz - 6 GHz front end. */
    CHECK_NEAR(caps.rx_freq.min, 70e6, 1.0);
    CHECK_NEAR(caps.rx_freq.max, 6e9, 1.0);

    /* Analog filter 200 kHz - 56 MHz. */
    CHECK_NEAR(caps.rx_bandwidth.min, 200e3, 1.0);
    CHECK_NEAR(caps.rx_bandwidth.max, 56e6, 1.0);

    /* Max streaming rate 61.44 Msps. */
    CHECK_NEAR(caps.rx_rate.max, 61.44e6, 1.0);

    /* B200 is 1x1. */
    CHECK_EQ(caps.rx_channels, size_t{1});
    CHECK_EQ(caps.tx_channels, size_t{1});

    /* Antenna ports as the manual names them. */
    CHECK_EQ(caps.rx_antennas.size(), size_t{2});
    CHECK_STR_EQ(caps.rx_antennas[0], "TX/RX");
    CHECK_STR_EQ(caps.rx_antennas[1], "RX2");
}

TEST(device_info_label_prefers_product) {
    radio::DeviceInfo info;
    info.type = "b200";
    info.product = "B210";
    info.serial = "31C9297";
    CHECK_STR_EQ(info.label(), "B210 31C9297");

    radio::DeviceInfo bare;
    bare.type = "b200";
    CHECK_STR_EQ(bare.label(), "b200");

    radio::DeviceInfo unknown;
    CHECK_STR_EQ(unknown.label(), "USRP");
}

TEST(find_is_safe_with_no_hardware) {
    /* Must not throw even when UHD's discovery errors out, which it does on
     * machines with unusual network configuration. */
    const auto devices = radio::UsrpRadio::find("b200");
    CHECK(devices.size() < 64);  /* i.e. it returned, whatever it found */
}

TEST(radio_without_device_reports_closed) {
    radio::UsrpRadio r;
    CHECK(!r.is_open());
    CHECK(!r.rx_running());
    CHECK(!r.tx_running());
}

TEST(setters_clamp_against_fallback_caps_when_closed) {
    radio::UsrpRadio r;

    /* With no device the fallback B200 limits apply, so out-of-range requests
     * come back clamped rather than accepted. */
    CHECK_NEAR(r.set_rx_frequency(1e6), 70e6, 1.0);
    CHECK_NEAR(r.set_rx_frequency(7e9), 6e9, 1.0);
    CHECK_NEAR(r.set_rx_frequency(433.92e6), 433.92e6, 1.0);

    CHECK_NEAR(r.set_rx_gain(-10.0), 0.0, 1e-6);
    CHECK_NEAR(r.set_rx_gain(500.0), 76.0, 1e-6);
    CHECK_NEAR(r.set_rx_gain(30.0), 30.0, 1e-6);
}

TEST(starting_streams_without_device_fails_cleanly) {
    radio::UsrpRadio r;
    CHECK(!r.start_rx());
    CHECK(!r.start_tx());
    CHECK(!r.last_error().empty());

    /* Stopping something that never started must be a no-op, not a hang. */
    r.stop_rx();
    r.stop_tx();
    r.close();
}

TEST(stream_stats_reset) {
    radio::StreamStats s;
    s.overflows.store(5);
    s.rx_samples.store(1000);
    s.reset();
    CHECK_EQ(s.overflows.load(), uint32_t{0});
    CHECK_EQ(s.rx_samples.load(), uint64_t{0});
}

/* --- ReceiverModel --------------------------------------------------------- */

TEST(receiver_mode_names_match_mayhem_labels) {
    radio::UsrpRadio r;
    audio::AudioOut a;
    radio::ReceiverModel rx{r, a};

    rx.set_mode(radio::ReceiverModel::Mode::NarrowbandFMAudio);
    rx.set_nfm_configuration(radio::ReceiverModel::NfmConfig::Medium11k);
    CHECK_STR_EQ(rx.mode_name(), "NFM 11k");

    rx.set_mode(radio::ReceiverModel::Mode::AMAudio);
    rx.set_am_configuration(radio::ReceiverModel::AmConfig::USB);
    CHECK_STR_EQ(rx.mode_name(), "AM USB");

    rx.set_mode(radio::ReceiverModel::Mode::WidebandFMAudio);
    CHECK_STR_EQ(rx.mode_name(), "WFM 200k");

    rx.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
    CHECK_STR_EQ(rx.mode_name(), "SPEC");
}

TEST(receiver_channel_bandwidth_tracks_configuration) {
    radio::UsrpRadio r;
    audio::AudioOut a;
    radio::ReceiverModel rx{r, a};

    rx.set_mode(radio::ReceiverModel::Mode::AMAudio);
    rx.set_am_configuration(radio::ReceiverModel::AmConfig::DSB9k);
    CHECK_NEAR(rx.channel_bandwidth(), 9000.0, 1.0);

    rx.set_am_configuration(radio::ReceiverModel::AmConfig::CW);
    CHECK_NEAR(rx.channel_bandwidth(), 1400.0, 1.0);

    rx.set_mode(radio::ReceiverModel::Mode::NarrowbandFMAudio);
    rx.set_nfm_configuration(radio::ReceiverModel::NfmConfig::Wide16k);
    CHECK_NEAR(rx.channel_bandwidth(), 16000.0, 1.0);

    rx.set_mode(radio::ReceiverModel::Mode::WidebandFMAudio);
    CHECK_NEAR(rx.channel_bandwidth(), 200000.0, 1.0);
}

TEST(receiver_squelch_level_is_bounded) {
    radio::UsrpRadio r;
    audio::AudioOut a;
    radio::ReceiverModel rx{r, a};

    rx.set_squelch_level(200);
    CHECK_EQ(static_cast<int>(rx.squelch_level()), 99);

    rx.set_squelch_level(0);
    CHECK_EQ(static_cast<int>(rx.squelch_level()), 0);
}

TEST(receiver_start_without_device_does_not_hang) {
    radio::UsrpRadio r;
    audio::AudioOut a;
    radio::ReceiverModel rx{r, a};

    /* start() should fail because the radio cannot stream, and stop() must
     * still be safe afterwards. */
    CHECK(!rx.start());
    CHECK(!rx.running());
    rx.stop();
}

TEST(receiver_capture_rejects_unwritable_path) {
    radio::UsrpRadio r;
    audio::AudioOut a;
    radio::ReceiverModel rx{r, a};

    CHECK(!rx.start_capture("Z:/definitely/not/a/real/path/capture"));
    CHECK(!rx.capturing());
    CHECK(!rx.capture_error().empty());
}
