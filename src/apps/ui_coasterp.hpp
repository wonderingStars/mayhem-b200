/*
 * mayhem-b200 — Coaster / Syscall restaurant-pager TX ("BurgerPgrTX").
 *
 * Ported from firmware/application/external/coasterp/ui_coasterp.* . The 19-byte
 * 2-FSK frame is reproduced exactly: an 8-byte 0x55 preamble, the 0x2D 0xD4 sync
 * word, a data-length byte (0x08) and eight data bytes taken big-endian from the
 * entered 64-bit value. On air it is 1000 baud, +/-5 kHz deviation, MSB first,
 * a 1 bit shifting the carrier up.
 *
 * LEGALITY: buzzing someone else's restaurant coaster/pager is a misuse of a
 * licensed band in most places. TX happens only on an explicit Start press and
 * shows a warning; RF output needs a USRP B200 and is unverified without one.
 *
 * Copyright (C) 2023 Bernd Herzog (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_COASTERP_H__
#define __MB200_UI_COASTERP_H__

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

/* The 19-byte frame: 8x 0x55, 0x2D, 0xD4, 0x08, then 8 data bytes big-endian. */
std::array<uint8_t, 19> coasterp_build_frame(uint64_t data);

/* The frame as 152 bits, MSB first (one uint8_t of 0/1 per bit) — the order
 * proc_fsk clocks them out. */
std::vector<uint8_t> coasterp_frame_bits(const std::array<uint8_t, 19>& frame);

class CoasterPagerView : public ui::View {
   public:
    CoasterPagerView();
    ~CoasterPagerView() override;

    CoasterPagerView(const CoasterPagerView&) = delete;
    CoasterPagerView& operator=(const CoasterPagerView&) = delete;

    std::string title() const override { return "BurgerPgrTX"; }
    void focus() override;
    void on_frame_sync() override;

   private:
    void start_tx();
    void stop_tx();

    bool transmitting_{false};
    bool scanning_{false};
    std::vector<std::complex<float>> tx_iq_{};
    std::atomic<size_t> tx_pos_{0};

    ui::Labels labels_{
        {{1 * 8, 1 * 16}, "Syscall pager TX", ui::Color::light_grey()},
        {{1 * 8, 3 * 16}, "Data:", ui::Color::light_grey()},
        {{1 * 8, 5 * 16}, "Freq:", ui::Color::light_grey()},
    };

    ui::SymField sym_data_{{7 * 8, 3 * 16}, 16, ui::SymField::Type::Hex};
    ui::FrequencyField field_freq_{{7 * 8, 5 * 16}};
    ui::Checkbox check_scan_{{1 * 8, 7 * 16}, 4, "Scan"};

    ui::Console console_{{0, 9 * 16, 240, 6 * 16}};
    ui::Button button_tx_{{2 * 8, 17 * 16, 26 * 8, 2 * 16}, "Start"};
};

}  // namespace app

#endif /*__MB200_UI_COASTERP_H__*/
