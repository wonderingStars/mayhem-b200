/*
 * mayhem-b200 — transmit chain tests.
 *
 * These run without hardware, so they cover the parts of TransmitterModel that
 * are pure decisions: which deviation and bandwidth a mode implies, when the LO
 * is moved rather than the NCO, how the source callbacks are held, and that
 * starting with nothing plugged in fails cleanly instead of hanging. Anything
 * that needs samples to flow is unverified until a B200 is attached — the DSP
 * blocks themselves are covered in tests/test_modulate.cpp.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "transmitter_model.hpp"
#include "usrp_radio.hpp"

#include <cstddef>

using Mode = radio::TransmitterModel::Mode;
using NfmConfig = radio::TransmitterModel::NfmConfig;
using SubTone = radio::TransmitterModel::SubTone;

TEST(transmitter_mode_names_match_mayhem_labels) {
    radio::UsrpRadio r;
    radio::TransmitterModel tx{r};

    tx.set_mode(Mode::NarrowbandFM);
    tx.set_nfm_configuration(NfmConfig::Medium11k);
    CHECK_STR_EQ(tx.mode_name(), "NFM 11k");

    tx.set_nfm_configuration(NfmConfig::Narrow8k5);
    CHECK_STR_EQ(tx.mode_name(), "NFM 8k5");

    tx.set_nfm_configuration(NfmConfig::Wide16k);
    CHECK_STR_EQ(tx.mode_name(), "NFM 16k");

    tx.set_mode(Mode::WidebandFM);
    CHECK_STR_EQ(tx.mode_name(), "WFM");

    tx.set_mode(Mode::USB);
    CHECK_STR_EQ(tx.mode_name(), "USB");

    tx.set_mode(Mode::Raw);
    CHECK_STR_EQ(tx.mode_name(), "RAW");

    CHECK_STR_EQ(radio::TransmitterModel::mode_label(Mode::DSB), "DSB");
    CHECK_STR_EQ(radio::TransmitterModel::mode_label(Mode::CW), "CW");
}

TEST(transmitter_channel_bandwidth_matches_the_receive_side_pairings) {
    /* These must agree with ReceiverModel's channel filters, or a transmission
     * lands outside the receiver's passband. */
    radio::UsrpRadio r;
    radio::TransmitterModel tx{r};

    tx.set_mode(Mode::NarrowbandFM);
    tx.set_nfm_configuration(NfmConfig::Narrow8k5);
    CHECK_NEAR(tx.channel_bandwidth(), 8500.0, 1.0);

    tx.set_nfm_configuration(NfmConfig::Medium11k);
    CHECK_NEAR(tx.channel_bandwidth(), 11000.0, 1.0);

    tx.set_nfm_configuration(NfmConfig::Wide16k);
    CHECK_NEAR(tx.channel_bandwidth(), 16000.0, 1.0);

    tx.set_mode(Mode::WidebandFM);
    CHECK_NEAR(tx.channel_bandwidth(), 200000.0, 1.0);

    tx.set_mode(Mode::AM);
    CHECK_NEAR(tx.channel_bandwidth(), 9000.0, 1.0);

    tx.set_mode(Mode::LSB);
    CHECK_NEAR(tx.channel_bandwidth(), 2800.0, 1.0);

    /* A raw stream occupies whatever is being streamed. */
    tx.set_mode(Mode::Raw);
    CHECK_NEAR(tx.channel_bandwidth(), tx.sampling_rate(), 1.0);
}

TEST(transmitter_deviation_follows_the_mode_until_overridden) {
    radio::UsrpRadio r;
    radio::TransmitterModel tx{r};

    tx.set_mode(Mode::NarrowbandFM);
    tx.set_nfm_configuration(NfmConfig::Narrow8k5);
    CHECK_NEAR(tx.deviation(), 2500.0, 1e-6);

    tx.set_nfm_configuration(NfmConfig::Medium11k);
    CHECK_NEAR(tx.deviation(), 3500.0, 1e-6);

    tx.set_nfm_configuration(NfmConfig::Wide16k);
    CHECK_NEAR(tx.deviation(), 5000.0, 1e-6);

    tx.set_mode(Mode::WidebandFM);
    CHECK_NEAR(tx.deviation(), 75000.0, 1e-6);

    tx.set_deviation(12500.0);
    CHECK_NEAR(tx.deviation(), 12500.0, 1e-6);

    /* Zero restores the mode's own figure. */
    tx.set_deviation(0.0);
    CHECK_NEAR(tx.deviation(), 75000.0, 1e-6);

    /* Amplitude modes have no deviation. */
    tx.set_mode(Mode::AM);
    CHECK_NEAR(tx.deviation(), 0.0, 1e-9);
}

TEST(transmitter_bandwidth_request_is_twice_the_occupied_width) {
    radio::UsrpRadio r;
    radio::TransmitterModel tx{r};

    tx.set_mode(Mode::NarrowbandFM);
    tx.set_nfm_configuration(NfmConfig::Medium11k);
    CHECK_NEAR(tx.bandwidth(), 22000.0, 1.0);

    tx.set_mode(Mode::WidebandFM);
    CHECK_NEAR(tx.bandwidth(), 400000.0, 1.0);

    tx.set_bandwidth(1'500'000.0);
    CHECK_NEAR(tx.bandwidth(), 1'500'000.0, 1.0);

    /* Zero hands control back to the mode. */
    tx.set_bandwidth(0.0);
    CHECK_NEAR(tx.bandwidth(), 400000.0, 1.0);
}

TEST(transmitter_amplitude_and_depth_are_bounded) {
    radio::UsrpRadio r;
    radio::TransmitterModel tx{r};

    tx.set_amplitude(5.0f);
    CHECK_NEAR(tx.amplitude(), 1.0, 1e-6);
    tx.set_amplitude(-1.0f);
    CHECK_NEAR(tx.amplitude(), 0.0, 1e-6);
    tx.set_amplitude(0.5f);
    CHECK_NEAR(tx.amplitude(), 0.5, 1e-6);

    tx.set_am_depth(3.0f);
    CHECK_NEAR(tx.am_depth(), 1.0, 1e-6);
    tx.set_am_depth(-0.5f);
    CHECK_NEAR(tx.am_depth(), 0.0, 1e-6);

    /* A negative audio gain would invert the modulation rather than quieten
     * it, so it is floored at zero. */
    tx.set_audio_gain(-2.0f);
    CHECK_NEAR(tx.audio_gain(), 0.0, 1e-6);
    tx.set_audio_gain(4.0f);
    CHECK_NEAR(tx.audio_gain(), 4.0, 1e-6);
}

TEST(transmitter_sub_tone_selection_is_exclusive) {
    radio::UsrpRadio r;
    radio::TransmitterModel tx{r};

    CHECK(tx.sub_tone() == SubTone::None);

    tx.set_ctcss(100.0f, 0.2f);
    CHECK(tx.sub_tone() == SubTone::Ctcss);
    CHECK_NEAR(tx.ctcss_frequency(), 100.0, 1e-6);
    CHECK_NEAR(tx.sub_tone_mix(), 0.2, 1e-6);

    tx.set_dcs(23, 0.1f);
    CHECK(tx.sub_tone() == SubTone::Dcs);
    CHECK_EQ(static_cast<int>(tx.dcs_code()), 23);

    /* Codes are nine bits, as the DCS word format requires. */
    tx.set_dcs(1000, 0.1f);
    CHECK_EQ(static_cast<int>(tx.dcs_code()), 1000 & 0x1FF);

    tx.set_sub_tone_none();
    CHECK(tx.sub_tone() == SubTone::None);
}

TEST(transmitter_sources_can_be_attached_and_cleared) {
    radio::UsrpRadio r;
    radio::TransmitterModel tx{r};

    CHECK(!tx.has_audio_source());
    CHECK(!tx.has_iq_source());

    tx.set_audio_source([](float* out, size_t count) {
        for (size_t i = 0; i < count; i++) out[i] = 0.0f;
        return count;
    });
    CHECK(tx.has_audio_source());
    CHECK(!tx.has_iq_source());

    tx.set_iq_source([](radio::cfloat* out, size_t count) {
        for (size_t i = 0; i < count; i++) out[i] = radio::cfloat{0.0f, 0.0f};
        return count;
    });
    CHECK(tx.has_iq_source());

    tx.set_audio_source({});
    CHECK(!tx.has_audio_source());
    CHECK(tx.has_iq_source());

    tx.set_iq_source({});
    CHECK(!tx.has_iq_source());
}

TEST(transmitter_moves_the_lo_only_when_the_target_leaves_the_window) {
    /* Same policy as ReceiverModel: the signal is kept inside the middle 80% of
     * the streamed band, and small steps are made by the NCO so the AD936x is
     * not retuned — a retune costs milliseconds and a settling transient, which
     * on transmit is radiated. */
    radio::UsrpRadio r;
    radio::TransmitterModel tx{r};

    CHECK_NEAR(tx.sampling_rate(), 2'000'000.0, 1.0);  /* window = +/-800 kHz */

    tx.set_target_frequency(446'000'000);
    CHECK_NEAR(r.tx_frequency(), 446e6, 1.0);

    /* 500 kHz away: inside the window, LO must not move. */
    tx.set_target_frequency(446'500'000);
    CHECK_EQ(tx.target_frequency(), uint64_t{446'500'000});
    CHECK_NEAR(r.tx_frequency(), 446e6, 1.0);

    /* 1 MHz away: outside, so the LO follows. */
    tx.set_target_frequency(447'000'000);
    CHECK_NEAR(r.tx_frequency(), 447e6, 1.0);

    /* Back down by 700 kHz: inside the new window, LO stays put. */
    tx.set_target_frequency(446'300'000);
    CHECK_NEAR(r.tx_frequency(), 447e6, 1.0);
}

TEST(transmitter_frequency_step_is_stored_verbatim) {
    radio::UsrpRadio r;
    radio::TransmitterModel tx{r};

    CHECK_EQ(tx.frequency_step(), uint64_t{12'500});
    tx.set_frequency_step(25'000);
    CHECK_EQ(tx.frequency_step(), uint64_t{25'000});
}

TEST(transmitter_start_without_device_does_not_hang) {
    radio::UsrpRadio r;
    radio::TransmitterModel tx{r};

    /* No device means no TX stream, so start() must report failure rather than
     * spinning up a DSP thread that produces into nothing. */
    CHECK(!tx.start());
    CHECK(!tx.running());
    CHECK_EQ(tx.samples_generated(), uint64_t{0});

    /* stop() has to be safe whether or not start() got anywhere. */
    tx.stop();
    tx.stop();
    CHECK(!tx.running());
}

TEST(transmitter_sampling_rate_is_clamped_to_the_device_range) {
    /* With nothing attached the model falls back to the published B200 limits
     * (200 kSps to 61.44 MSps), so the UI still shows achievable numbers. */
    radio::UsrpRadio r;
    radio::TransmitterModel tx{r};

    tx.set_sampling_rate(1.0);
    CHECK_NEAR(tx.sampling_rate(), r.caps().tx_rate.min, 1.0);

    tx.set_sampling_rate(1e9);
    CHECK_NEAR(tx.sampling_rate(), r.caps().tx_rate.max, 1.0);

    tx.set_sampling_rate(4'000'000.0);
    CHECK_NEAR(tx.sampling_rate(), 4'000'000.0, 1.0);
}
