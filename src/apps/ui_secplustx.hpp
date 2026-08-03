/*
 * mayhem-b200 — Security+ TX: Chamberlain/LiftMaster garage-door transmitter.
 *
 * Ported from firmware/application/external/secplustx/ (ui_secplustx.* and
 * secplustx.cpp, itself Clayton Smith's secplus). The Security+ 2.0 encoder —
 * base-3 rolling-code expansion, fixed/data parity, the ORDER/INVERT scramble
 * and the three-way bit interleave into two 8-byte packet halves — is a verbatim
 * port, as is the Manchester OOK framing (20-bit preamble, 2-bit frame
 * indicator, 5 or 8 packet bytes, 24-bit blank; a 1 bit is "01", a 0 bit "10").
 *
 * Security+ 1.0 is present in the upstream file format but its encoder is
 * unimplemented upstream (read_secplus_file rejects V1); this port keeps that
 * behaviour rather than inventing a v1 encoder the firmware does not have.
 *
 * LEGALITY: transmitting a garage rolling code impersonates someone's opener and
 * is illegal to radiate in most places. TX happens only on an explicit Start
 * press and shows a warning; RF output needs a USRP B200 and is unverified
 * without one.
 *
 * Copyright (C) 2022 Clayton Smith (secplus)
 * Copyright (C) 2026 Synray (original app)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __MB200_UI_SECPLUSTX_H__
#define __MB200_UI_SECPLUSTX_H__

#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <atomic>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* Security+ 2.0 encoder (secplustx.cpp, verbatim). Fills packet1[8] and
 * packet2[8] with the two scrambled packet halves. frame_type is 0 for a
 * 5-byte (no-data) frame, 1 for an 8-byte (data) frame. Returns 0 on success,
 * -1 if rolling >= 2^28 or fixed >= 2^40. */
int8_t secplus_encode_v2(uint32_t rolling, uint64_t fixed, uint32_t data,
                         uint8_t frame_type, uint8_t* packet1, uint8_t* packet2);

/* Manchester OOK string for one packet half: 20-bit preamble, 2-bit indicator,
 * then 5 (no data) or 8 (data) packet bytes, each bit as "01"/"10", followed by
 * 24 blank ('0') symbols. */
std::string secplus_encode_packet(uint8_t indicator, const uint8_t* packet, bool has_data);

/* The full one-repeat bitstream: packet1 (indicator 0b00) then packet2
 * (indicator 0b01). Empty if the codes are out of range. */
std::string secplus_encode_bitstream(uint32_t rolling, uint64_t fixed, uint32_t data,
                                     bool has_data);

class SecplusTXView : public ui::View {
   public:
    SecplusTXView();
    ~SecplusTXView() override;

    SecplusTXView(const SecplusTXView&) = delete;
    SecplusTXView& operator=(const SecplusTXView&) = delete;

    std::string title() const override { return "Security+"; }
    void focus() override;
    void on_frame_sync() override;

   private:
    void start_tx();
    void stop_tx();

    bool transmitting_{false};
    std::vector<std::complex<float>> tx_iq_{};
    std::atomic<size_t> tx_pos_{0};

    ui::Labels labels_{
        {{2 * 8, 1 * 16}, "Fixed:", ui::Color::light_grey()},
        {{2 * 8, 2 * 16}, "Rolling:", ui::Color::light_grey()},
        {{2 * 8, 4 * 16}, "Freq:", ui::Color::light_grey()},
    };

    ui::SymField field_fixed_{{10 * 8, 1 * 16}, 10, ui::SymField::Type::Hex, true};
    ui::SymField field_rolling_{{10 * 8, 2 * 16}, 7, ui::SymField::Type::Hex, true};
    ui::Checkbox check_data_{{2 * 8, 3 * 16}, 5, "Data:"};
    ui::SymField field_data_{{10 * 8, 3 * 16}, 8, ui::SymField::Type::Hex, true};

    ui::FrequencyField field_freq_{{10 * 8, 4 * 16}};
    ui::Checkbox check_learn_{{2 * 8, 5 * 16}, 6, "Learn"};

    ui::Console console_{{0, 7 * 16, 240, 8 * 16}};

    ui::Button button_tx_{{2 * 8, 17 * 16, 26 * 8, 2 * 16}, "Start"};
};

}  // namespace app

#endif /*__MB200_UI_SECPLUSTX_H__*/
