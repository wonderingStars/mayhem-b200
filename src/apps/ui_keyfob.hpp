/*
 * mayhem-b200 — Key fob TX: Subaru remote keyless-entry transmitter.
 *
 * Ported from firmware/application/external/keyfob/ui_keyfob.* . The 10-byte
 * Subaru frame (0x55 header, command in the two nibbles of bytes 5 and 6,
 * rolling code in bytes 7-9, XOR-of-nibbles checksum in the low nibble of byte
 * 9), the 128x "01" preamble, the two inter-payload spaces and the "10"/"01"
 * per-bit OOK mapping are reproduced exactly so the waveform matches upstream.
 *
 * LEGALITY: this impersonates a car remote and is illegal to radiate almost
 * everywhere. It transmits only on an explicit Start press and shows a warning;
 * RF output needs a USRP B200 and is unverified without one.
 *
 * Copyright (C) 2023 Bernd Herzog (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_KEYFOB_H__
#define __MB200_UI_KEYFOB_H__

#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* The 10-byte frame, source of truth for the encoder. */
using SubaruFrame = std::array<uint8_t, 10>;

/* XOR of every nibble of bytes 0..8 plus the high nibble of byte 9, incremented
 * and masked to 4 bits — the check nibble that goes in frame[9] & 0x0F. */
uint8_t subaru_checksum(const SubaruFrame& frame);

/* frame[0] == 0x55 and a correct checksum. */
bool subaru_is_valid(const SubaruFrame& frame);

/* Sets the command into the low nibble of bytes 5 and 6 (upstream duplicates
 * it there). */
void subaru_set_command(SubaruFrame& frame, uint8_t command);

/* Builds a frame from two 40-bit halves (each big-endian into frame[0..4] and
 * frame[5..9]) and fixes up the checksum, exactly as generate_frame() does. */
SubaruFrame subaru_build_frame(uint64_t half_a, uint64_t half_b);

/* One transmission unit: 128x "01" preamble, 4x "0" space, the 80-bit payload
 * ("10" for a 1 bit, "01" for a 0 bit, MSB first), 8x "0" space, the payload
 * again. Each character is one OOK symbol. */
std::string keyfob_encode_bitstream(const SubaruFrame& frame);

class KeyfobView : public ui::View {
   public:
    KeyfobView();
    ~KeyfobView() override;

    KeyfobView(const KeyfobView&) = delete;
    KeyfobView& operator=(const KeyfobView&) = delete;

    std::string title() const override { return "Key fob TX"; }
    void focus() override;
    void on_frame_sync() override;

   private:
    SubaruFrame frame_{};

    void frame_from_fields();
    void fields_from_frame();
    void start_tx();
    void stop_tx();

    bool transmitting_{false};
    std::vector<std::complex<float>> tx_iq_{};
    std::atomic<size_t> tx_pos_{0};

    ui::Labels labels_{
        {{2 * 8, 1 * 16}, "Make:", ui::Color::light_grey()},
        {{2 * 8, 2 * 16}, "Command:", ui::Color::light_grey()},
        {{2 * 8, 3 * 16}, "Freq:", ui::Color::light_grey()},
        {{2 * 8, 4 * 16}, "Payload:", ui::Color::light_grey()},
        {{2 * 8, 7 * 16}, "Checksum is fixed at TX.", ui::Color::light_grey()},
    };

    ui::OptionsField field_make_{{10 * 8, 1 * 16}, 8, {{"Subaru", 0}}};

    ui::OptionsField field_command_{
        {10 * 8, 2 * 16},
        6,
        {{"Lock", 1}, {"Unlock", 2}, {"Trunk", 11}, {"Panic", 10}}};

    ui::FrequencyField field_freq_{{10 * 8, 3 * 16}};

    ui::SymField field_payload_a_{{2 * 8, 5 * 16}, 10, ui::SymField::Type::Hex};
    ui::SymField field_payload_b_{{13 * 8, 5 * 16}, 10, ui::SymField::Type::Hex};

    ui::Console console_{{0, 9 * 16, 240, 6 * 16}};

    ui::Button button_tx_{{2 * 8, 17 * 16, 26 * 8, 2 * 16}, "Start"};
};

}  // namespace app

#endif /*__MB200_UI_KEYFOB_H__*/
