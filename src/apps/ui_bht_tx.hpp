/*
 * mayhem-b200 — BHT TX: Xhouse/BHT remote-control relay transmitter.
 *
 * Ported from firmware/application/external/bht_tx/ (bht.cpp, ui_bht_tx.*).
 * Two systems:
 *
 *   Xylos — a 20-symbol CCIR selective-calling tone sequence, FM-modulated (the
 *           upstream tones baseband FM-modulates each tone; this host port does
 *           the same). Header, city, family, subfamily, receiver ID and four
 *           relay states, with wildcards and the "repeat becomes E" rule.
 *   EPAR  — a 12-bit UM3750 OOK word (bit-reversed city, group, relay), sent as
 *           two halves (R2 then R1), each repeated 26 times.
 *
 * The tone-index array and the UM3750 fragment string are reproduced exactly
 * (checked against the upstream encoders in tests/test_bht_tx.cpp).
 *
 * LEGALITY: this drives someone else's gate/relay receivers; transmitting is
 * illegal to radiate in most places. TX happens only on an explicit Start press
 * and shows a warning; RF output needs a USRP B200 and is unverified without it.
 *
 * Copyright (C) 2015 Jared Boone; Copyright (C) 2016 Furrtek (original)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_BHT_TX_H__
#define __MB200_UI_BHT_TX_H__

#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <array>
#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* EPAR: 12-bit UM3750 word as an OOK fragment string. City is bit-reversed
 * (bit c = (city >> c) & 1), then group (2 bits) and relay number/state. The
 * word format is "S" + 12 data symbols; sync "001", a 0 bit "011", a 1 bit
 * "001". */
std::string bht_gen_message_ep(uint8_t city_code, size_t family_code_ep,
                               uint32_t relay_number, uint32_t relay_state);

/* Xylos: the 20-entry CCIR tone-index message (values 0..0xF). */
std::array<uint8_t, 20> bht_gen_message_xy(size_t header_a, size_t header_b,
                                           size_t city, size_t family,
                                           bool subfamily_wc, size_t subfamily_code,
                                           bool id_wc, size_t receiver,
                                           size_t relay_a, size_t relay_b,
                                           size_t relay_c, size_t relay_d);

/* Renders a tone-index message to its display string ('0'..'9','A'..'F'). */
std::string bht_ccir_to_ascii(const std::array<uint8_t, 20>& msg);

class BHTView : public ui::View {
   public:
    BHTView();
    ~BHTView() override;

    BHTView(const BHTView&) = delete;
    BHTView& operator=(const BHTView&) = delete;

    std::string title() const override { return "BHT TX"; }
    void focus() override;
    void on_frame_sync() override;

   private:
    enum class System : uint8_t { Xylos = 0, Epar = 1 };

    void update_system_visibility();
    void refresh_preview();
    void start_tx();
    void stop_tx();

    System system_{System::Xylos};

    bool transmitting_{false};
    std::vector<std::complex<float>> tx_iq_{};
    std::atomic<size_t> tx_pos_{0};

    ui::Labels labels_{
        {{2 * 8, 1 * 16}, "System:", ui::Color::light_grey()},
        {{2 * 8, 8 * 16}, "Freq:", ui::Color::light_grey()},
        {{2 * 8, 9 * 16}, "Msg:", ui::Color::light_grey()},
    };

    ui::OptionsField field_system_{{10 * 8, 1 * 16}, 6, {{"Xylos", 0}, {"EPAR", 1}}};

    /* --- Xylos fields --- */
    ui::Labels xy_labels_{
        {{2 * 8, 2 * 16}, "Header:", ui::Color::light_grey()},
        {{2 * 8, 3 * 16}, "City:", ui::Color::light_grey()},
        {{2 * 8, 4 * 16}, "Family:", ui::Color::light_grey()},
        {{2 * 8, 5 * 16}, "Subfam:", ui::Color::light_grey()},
        {{2 * 8, 6 * 16}, "Rx ID:", ui::Color::light_grey()},
        {{2 * 8, 7 * 16}, "Relays:", ui::Color::light_grey()},
    };
    ui::NumberField xy_header_a_{{10 * 8, 2 * 16}, 2, {0, 99}, 1, '0'};
    ui::NumberField xy_header_b_{{13 * 8, 2 * 16}, 2, {0, 99}, 1, '0'};
    ui::NumberField xy_city_{{10 * 8, 3 * 16}, 2, {0, 99}, 1, ' '};
    ui::NumberField xy_family_{{10 * 8, 4 * 16}, 1, {0, 9}, 1, ' '};
    ui::NumberField xy_subfamily_{{10 * 8, 5 * 16}, 1, {0, 9}, 1, ' '};
    ui::Checkbox xy_sub_all_{{14 * 8, 5 * 16}, 3, "All"};
    ui::NumberField xy_receiver_{{10 * 8, 6 * 16}, 2, {0, 99}, 1, '0'};
    ui::Checkbox xy_id_all_{{14 * 8, 6 * 16}, 3, "All"};
    ui::OptionsField xy_relay_a_{{10 * 8, 7 * 16}, 3, {{"--", 0}, {"Of", 1}, {"On", 2}}};
    ui::OptionsField xy_relay_b_{{14 * 8, 7 * 16}, 3, {{"--", 0}, {"Of", 1}, {"On", 2}}};
    ui::OptionsField xy_relay_c_{{18 * 8, 7 * 16}, 3, {{"--", 0}, {"Of", 1}, {"On", 2}}};
    ui::OptionsField xy_relay_d_{{22 * 8, 7 * 16}, 3, {{"--", 0}, {"Of", 1}, {"On", 2}}};

    /* --- EPAR fields --- */
    ui::Labels ep_labels_{
        {{2 * 8, 3 * 16}, "City:", ui::Color::light_grey()},
        {{2 * 8, 4 * 16}, "Group:", ui::Color::light_grey()},
        {{2 * 8, 5 * 16}, "Relays:", ui::Color::light_grey()},
    };
    ui::NumberField ep_city_{{10 * 8, 3 * 16}, 3, {0, 255}, 1, '0'};
    ui::OptionsField ep_group_{{10 * 8, 4 * 16}, 2, {{"A", 2}, {"B", 1}, {"C", 0}, {"TP", 3}}};
    ui::OptionsField ep_relay_a_{{10 * 8, 5 * 16}, 3, {{"Of", 0}, {"On", 1}}};
    ui::OptionsField ep_relay_b_{{14 * 8, 5 * 16}, 3, {{"Of", 0}, {"On", 1}}};

    ui::FrequencyField field_freq_{{10 * 8, 8 * 16}};
    ui::Text text_msg_{{10 * 8, 9 * 16, 21 * 8, 16}, ""};

    ui::Console console_{{0, 11 * 16, 240, 5 * 16}};
    ui::Button button_tx_{{2 * 8, 17 * 16, 26 * 8, 2 * 16}, "Start"};
};

}  // namespace app

#endif /*__MB200_UI_BHT_TX_H__*/
