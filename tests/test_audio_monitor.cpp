/*
 * mayhem-b200 — the speaker monitor's default and reset logic.
 *
 * Operator request (2026-08-14): "we only want sounds from the ones that need
 * it." Data-decoder apps (ACARS, FLEX, POCSAG, APRS, AFSK, the image and
 * two-tone decoders) set an audio demod mode only to configure the receiver;
 * they decode from their own tap and the demodulated audio is modem tones
 * nobody needs. They call set_audio_monitor(false).
 *
 * The load-bearing correctness is not "false means silent" (that is the DSP
 * loop, verified on hardware) but the RESET rule: set_mode re-asserts the
 * monitor ON, unconditionally, so a decoder that muted it cannot leave a
 * later listening app (FM radio, Audio) silent — including the app-to-app
 * switch that never passes through the menu and may even keep the same demod
 * mode. These pin exactly that.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "audio_out.hpp"
#include "counter_radio.hpp"
#include "receiver_model.hpp"

using mb200test::CounterRadio;
using Mode = radio::ReceiverModel::Mode;

TEST(audio_monitor_is_on_by_default) {
    CounterRadio radio;
    audio::AudioOut audio;
    radio::ReceiverModel rx{radio, audio};
    CHECK(rx.audio_monitor()); /* a listening app that only sets a mode is audible */
}

TEST(a_decoder_can_switch_the_monitor_off) {
    CounterRadio radio;
    audio::AudioOut audio;
    radio::ReceiverModel rx{radio, audio};
    rx.set_mode(Mode::NarrowbandFMAudio);
    rx.set_audio_monitor(false);
    CHECK(!rx.audio_monitor());
}

TEST(set_mode_re_enables_the_monitor_even_for_the_same_mode) {
    /* The switch a decoder->listening-app transition makes: the decoder left
     * the monitor off, the next app sets its mode, and it must come back
     * audible. Same mode is the hard case — set_mode early-returns on the
     * chain rebuild, so the reset has to happen BEFORE that return. */
    CounterRadio radio;
    audio::AudioOut audio;
    radio::ReceiverModel rx{radio, audio};

    rx.set_mode(Mode::NarrowbandFMAudio);
    rx.set_audio_monitor(false);
    CHECK(!rx.audio_monitor());

    rx.set_mode(Mode::NarrowbandFMAudio); /* same mode, as a NFM->NFM app switch */
    CHECK(rx.audio_monitor());
}

TEST(set_mode_re_enables_the_monitor_across_a_mode_change) {
    CounterRadio radio;
    audio::AudioOut audio;
    radio::ReceiverModel rx{radio, audio};

    rx.set_mode(Mode::NarrowbandFMAudio);
    rx.set_audio_monitor(false);
    rx.set_mode(Mode::WidebandFMAudio); /* e.g. a pager decoder -> FM radio */
    CHECK(rx.audio_monitor());
}
