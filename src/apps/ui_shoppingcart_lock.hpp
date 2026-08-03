/*
 * mayhem-b200 — Shopping-cart wheel lock tone.
 *
 * Ported from firmware/application/external/shoppingcart_lock/ (RocketGod's
 * "Cart Lock", built on jimilinuxguy's captures).
 *
 * What the signal actually is
 * ---------------------------
 * Store anti-theft cart wheels (Gatekeeper Systems "Smart Wheels" and similar)
 * lock and unlock in response to a very-low-frequency magnetic tone radiated by
 * a wire loop buried at the store boundary. The control tone sits in the audio
 * band — reverse-engineering of the Gatekeeper wheels documents an ~7.8 kHz
 * carrier (sources variously quote 7.8-8 kHz; the task brief says ~7.6 kHz) that
 * is on/off-keyed with an 8-bit code:
 *
 *     LOCK    = 0x8E = 1000 1110      UNLOCK   = 0x71 = 0111 0001
 *     LOCK2   = 0xC7 = 1100 0111      UNLOCK2  = 0x78 = 0111 1000
 *
 * (Codes from the cra0/Gatekeeper-Systems-SmartWheel research repository.)
 *
 * Because the control frequency is in the audible range, the field can be
 * produced by simply playing the tone through a speaker held near the wheel —
 * which is exactly what upstream does: it replays two recorded .wav files
 * (shopping_cart_lock.wav / shopping_cart_unlock.wav) through the PortaPack's
 * audio-TX path.
 *
 * How this port differs, and why (honesty)
 * -----------------------------------------
 * A USRP B200 tunes 70 MHz - 6 GHz. It physically CANNOT emit a ~7.8 kHz signal
 * over RF, so the PortaPack's "transmit the audio" path does not map to the
 * radio here at all. The faithful host equivalent of "play the tone through a
 * speaker" is to play it through the PC sound card — the same near-field,
 * acoustic/magnetic coupling the phone-speaker method relies on.
 *
 * So this app:
 *   - GENERATES the coded ~7.8 kHz tone itself (upstream shipped .wav files; we
 *     have the documented codes, so we synthesise the OOK waveform);
 *   - plays it through host audio on an explicit button press only (no auto-send);
 *   - states plainly on screen that the B200 RF path is not used and that whether
 *     the sound card actually couples to a wheel is unverified.
 *
 * The bit *timing* is not documented in the sources, so the bit period is an
 * explicit, labelled assumption (kBitSeconds) — the carrier frequency and the
 * code bytes are the verified parts.
 *
 * Legal note (shown on screen too): deliberately locking or unlocking retail
 * cart wheels you do not own may be unlawful interference with property. This is
 * provided for research and interoperability testing on equipment you control.
 *
 * Copyright (C) 2023 RocketGod (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SHOPPINGCART_LOCK_H__
#define __MB200_UI_SHOPPINGCART_LOCK_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace app {
namespace cartlock {

/* --- Verified protocol constants ------------------------------------------- */

/* 8-bit OOK codes (MSB first), from the Gatekeeper Smart Wheel teardown. */
constexpr uint8_t kLockCode = 0x8E;    /* 1000 1110 */
constexpr uint8_t kUnlockCode = 0x71;  /* 0111 0001 */
constexpr uint8_t kLock2Code = 0xC7;   /* 1100 0111 */
constexpr uint8_t kUnlock2Code = 0x78; /* 0111 1000 */

/* Documented control-tone carrier. Sources cite 7.8-8 kHz; 7800 Hz is used. */
constexpr float kCarrierHz = 7800.0f;

/* ASSUMPTION (not documented): one bit lasts this long. Chosen so an 8-bit code
 * is ~80 ms, comfortably long for a coil to detect. Change here if a real
 * capture ever pins it down. */
constexpr float kBitSeconds = 0.010f;

/* --- Pure tone generation (unit-tested) ------------------------------------ */

/* Whole samples in one code bit at a given output rate. Always at least 1. */
inline size_t samples_per_bit(float sample_rate_hz, float bit_seconds) {
    const double s = static_cast<double>(sample_rate_hz) *
                     static_cast<double>(bit_seconds);
    if (s <= 1.0) return 1;
    return static_cast<size_t>(s + 0.5);
}

/* Extracts bit `index` (0 == MSB) of an 8-bit code. */
inline bool code_bit(uint8_t code, size_t index) {
    return (code >> (7 - index)) & 0x1u;
}

/* Synthesises one transmission of `code` as OOK of a sine carrier: a '1' bit is
 * the carrier on for one bit period, a '0' bit is silence. Returns mono float
 * samples in [-amplitude, amplitude] at sample_rate_hz. The carrier phase runs
 * continuously across bits (it is only gated), so on-bits stay phase-coherent —
 * the closest thing to a clean recovered tone. Total length is exactly
 * 8 * samples_per_bit(...). */
std::vector<float> generate_ook(uint8_t code, float carrier_hz,
                                float sample_rate_hz, float bit_seconds,
                                float amplitude = 0.9f);

/* --- The app view ---------------------------------------------------------- */

class ShoppingCartLockView : public ui::View {
   public:
    ShoppingCartLockView();
    ~ShoppingCartLockView() override;

    std::string title() const override { return "Cart Lock"; }

    void focus() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void log(std::string_view line);
    void begin(const char* what, uint8_t code);
    void stop();
    void refill_audio();

    /* The waveform currently being looped out, and our read position in it. */
    std::vector<float> waveform_{};
    size_t play_pos_{0};
    bool active_{false};
    bool audio_started_{false};

    /* --- widgets --- */
    ui::Labels labels_{
        {{0, 8}, "Carrier", ui::Color::light_grey()},
        {{160, 8}, "Hz", ui::Color::light_grey()},
        {{0, 26}, "Code", ui::Color::light_grey()},
    };

    ui::NumberField field_carrier_{{72, 8}, 5, {5000, 9000}, 100, ' '};

    ui::OptionsField options_code_{
        {40, 26},
        14,
        {{"Std 8E/71", 0}, {"Alt C7/78", 1}}};

    ui::Text text_note1_{
        {0, 46, 240, 16},
        STR_COLOR_YELLOW "B200 cannot TX 7.8kHz."};
    ui::Text text_note2_{
        {0, 62, 240, 16},
        STR_COLOR_LIGHT_GREY "Tone plays via PC audio."};
    ui::Text text_note3_{
        {0, 78, 240, 16},
        STR_COLOR_LIGHT_GREY "Coupling to a cart unverified."};
    ui::Text text_legal_{
        {0, 94, 240, 16},
        STR_COLOR_DARK_YELLOW "Only on gear you own. May be illegal."};

    ui::Console console_{{0, 116, 240, 128}};

    ui::Button button_lock_{{4, 248, 74, 48}, "Lock"};
    ui::Button button_unlock_{{83, 248, 74, 48}, "Unlock"};
    ui::Button button_stop_{{162, 248, 74, 48}, "Stop"};
};

}  // namespace cartlock
}  // namespace app

#endif /*__MB200_UI_SHOPPINGCART_LOCK_H__*/
