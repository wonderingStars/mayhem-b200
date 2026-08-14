/*
 * mayhem-b200 — Morse panel payload + browser-driven transmit refusals.
 *
 * The encode/keyer math is covered thoroughly in test_morse_tx.cpp; this file
 * covers the NEW web surface: the panel's omit rules and the transmit
 * endpoint's refusal path. The transmit endpoint is the one panel path that
 * keys the radio, so its refusals matter as much as its successes. The actual
 * keying against a real B200 is in test_usrp_hardware.cpp.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_context.hpp"
#include "app_data.hpp"
#include "morse_tx.hpp"

#include <string>

using remote::MorseData;

TEST(morse_payload_omits_wpm_and_tone_until_the_decoder_has_them) {
    MorseData m;
    m.decoded_text = "CQ ";
    m.receiving = true;
    /* wpm and tone default 0 => omitted; a 0 wpm shown would read as a real
     * reading. decoded_text is always present (may legitimately be empty). */
    const std::string j = remote::to_json(m).dump();
    CHECK(j.find("\"decoded_text\":\"CQ \"") != std::string::npos);
    CHECK(j.find("\"receiving\":true") != std::string::npos);
    CHECK(j.find("wpm") == std::string::npos);
    CHECK(j.find("tone_hz") == std::string::npos);
}

TEST(morse_payload_emits_wpm_and_tone_once_present) {
    MorseData m;
    m.decoded_text = "";
    m.wpm = 18;
    m.tone_hz = 700;
    m.receiving = true;
    const std::string j = remote::to_json(m).dump();
    CHECK(j.find("\"wpm\":18") != std::string::npos);
    CHECK(j.find("\"tone_hz\":700") != std::string::npos);
    CHECK(j.find("\"decoded_text\":\"\"") != std::string::npos); /* empty is honest data */
}

TEST(morse_transmit_is_refused_when_no_radio_is_wired) {
    /* Globals are null in the test process — the request must refuse rather
     * than dereference a null transmitter. This is the "(no B200)" path made
     * honest: it names a real reason and keys nothing. */
    radio::TransmitterModel* saved_tx = app::globals().transmitter;
    radio::RadioDevice* saved_radio = app::globals().radio;
    app::globals().transmitter = nullptr;
    app::globals().radio = nullptr;

    const remote::MorseTxResult r = remote::morse_tx_request("CQ CQ", 18);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(!remote::morse_tx_active());

    app::globals().transmitter = saved_tx;
    app::globals().radio = saved_radio;
}
